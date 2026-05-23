# install.ps1 調査記録 — 既知の落とし穴

## 1. VOICEVOX ダウンローダー BOM 混入問題

### 症状
```
Error: the stdin is not a TTY but received invalid input: "\u{feff}y"
[!!] VOICEVOX Core not found after download.
```

### 原因
`chcp 65001` (コードページを UTF-8 に切り替え) を呼んだ後、Windows が
`[Console]::InputEncoding` を `System.Text.Encoding.UTF8` (BOM 付き) に自動更新する。
PowerShell の `"y" | & exe` パイプは内部で `Console.InputEncoding` を使って文字列をバイト変換するため、
先頭に UTF-8 BOM (`0xEF 0xBB 0xBF`) が付与されて downloader に送信される。
Rust 製の voicevox_downloader は BOM を不正入力として拒否する。

### 試したが効果がなかった対策
| 対策 | 結果 |
|---|---|
| `[Console]::InputEncoding` 変更を削除 | `$OutputEncoding`/`$OutputEncoding` 経由で BOM は残った |
| `$OutputEncoding = New-Object System.Text.UTF8Encoding $false` | まだ BOM が混入 (PS 内部状態の問題) |
| `System.Diagnostics.Process` + `BaseStream.Write(ASCII bytes)` | まだ BOM が混入 (原因不明、おそらく PS の内部パイプ管理) |

### 正しい解決策
`cmd.exe` バッチファイル経由で `<` stdin リダイレクトを使う:

```powershell
$vvStdinFile = Join-Path $tmpDir "vv_stdin.txt"
$vvBatchFile = Join-Path $tmpDir "vv_run.bat"
[System.IO.File]::WriteAllBytes($vvStdinFile, [byte[]]@(0x79, 0x0A))  # ASCII "y\n"
$vvBatch  = "@echo off`r`n"
$vvBatch += "`"$vvDownloaderPath`" --devices cpu --models-pattern 0.vvm --output `"$vvDir`" < `"$vvStdinFile`"`r`n"
[System.IO.File]::WriteAllText($vvBatchFile, $vvBatch, [System.Text.Encoding]::ASCII)
& cmd.exe /c $vvBatchFile
```

`cmd.exe` の `<` リダイレクトはファイルをバイトとして処理するため、
PowerShell のエンコーディング変換が介在しない。

---

## 2. Rust pager (minus crate) のパニック

### 症状
```
thread 'main' panicked at minus-5.6.1\src\state.rs:322:24:
byte index 1 is not a char boundary; ...
ERROR something went wrong with the pager
```

### 原因
VOICEVOX downloader が `minus` クレートでライセンス文を表示しようとするが、
日本語テキストのバイト境界計算にバグがある。

### 対応
`$env:TERM = "dumb"` を設定するとページャが無効化されることがあるが、今バージョンでは
完全には止まらない。ただしパニック後もプログラムは継続し、同意プロンプトが表示される。
**ダウンロード自体には影響しない。**

---

## 3. VOICEVOX モデルダウンロードの GitHub レートリミット

### 症状
`--devices cpu` で全 VVM モデルをダウンロードすると、GitHub API の匿名レートリミット
(60回/時) に到達してダウンロードが途中で失敗する。

### 解決策
```
--models-pattern 0.vvm
```
ずんだもんノーマル (style_id=3) は `0.vvm` に収録されている。
このフラグで必要最小限のモデルのみダウンロードする。

---

## 4. Ollama インストール後にウィンドウが表示される問題

### 症状
`winget install Ollama.Ollama --silent` 完了後、Ollama の GUI ウィンドウが自動起動する。

### 原因
Ollama の Inno Setup インストーラは `[Run]` セクションに `skipifsilent` フラグが付いておらず、
`--silent` フラグを渡しても `ollama app.exe` が必ず起動する。

### 解決策
Ollama 公式の `install.ps1` と同じ手法: インストール前に
`%LOCALAPPDATA%\Ollama\upgraded` マーカーファイルを作成する。
Ollama app はこのファイルを検出するとウィンドウを非表示 (タスクトレイ常駐のみ) で起動する。

```powershell
$markerDir  = Join-Path $env:LOCALAPPDATA "Ollama"
$markerFile = Join-Path $markerDir "upgraded"
New-Item -ItemType Directory -Force -Path $markerDir | Out-Null
New-Item -ItemType File      -Force -Path $markerFile | Out-Null
```

### 関連
winget が stale レジストリを持っている場合は `--force` が必要:
```powershell
winget install Ollama.Ollama --silent --disable-interactivity --force
```

---

## 5. -Uninstall が削除すべきファイル一覧

### 背景
ゲーム実行中に mod が生成するファイルが複数あり、当初の uninstall リストは静的な配布ファイルしか含んでいなかった。

### 削除すべきファイル (全て `War\Binaries\Win64\` 直下)

| ファイル/ディレクトリ | 種別 | 生成タイミング |
|---|---|---|
| `version.dll` | 配布 | インストール時 |
| `chat_translator.dll` | 配布 | インストール時 |
| `chat_translator_live.dll` | 実行時生成 | ゲーム起動時 (ホットリロード用コピー) |
| `config.ini` | 配布 | インストール時 |
| `term_protection.txt` | 配布 | インストール時 |
| `chat_log.txt` | 実行時生成 | ゲーム起動後 (翻訳ログ) |
| `loader_log.txt` | 実行時生成 | ゲーム起動後 (ローダーログ) |
| `debug_log.txt` | 実行時生成 | ゲーム起動後 (デバッグログ) |
| `imgui.ini` | 実行時生成 | ゲーム起動後 (ImGui ウィンドウ設定) |
| `assets\` | 配布ディレクトリ | インストール時 |
| `tools\` | 配布ディレクトリ | インストール時 (Ollama/TTS ツール) |

### 確認方法
```powershell
# インストール後に何が置かれているか確認する
Get-ChildItem "War\Binaries\Win64" -File | Where-Object { $_.Name -notlike "FoxholeClient*" -and $_.Name -notlike "EasyAntiCheat*" }
```

---

## 6. install.ps1 の UTF-8 BOM 必須

### 症状
PS5 + 日本語 Windows で実行するとスクリプトが正しくパースされない。例:
- マルチライン `-or` 演算子の継続が無視され、2行目が独立した式として評価される
- 日本語コメントの文字化け

### 原因
BOM なしの UTF-8 ファイルを PS5 (Windows PowerShell 5.1) が CP932 として読み込む。
日本語 Windows では PS5 がデフォルトで CP932 を使用する。

### 解決策
install.ps1 を編集した後は必ず UTF-8 BOM 付きで保存する:

```powershell
$path = "c:\src\FoxholeChatTranslatorMod\install.ps1"
$content = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
$utf8Bom = New-Object System.Text.UTF8Encoding $true
[System.IO.File]::WriteAllText($path, $content, $utf8Bom)
```

`Edit` / `Write` ツール (Claude Code) は BOM なしで保存するため、毎回この手順が必要。
