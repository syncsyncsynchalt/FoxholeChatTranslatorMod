# ChatTranslator — AI Dev Guide

Foxhole (UE4 4.24.3) チャット傍受 → Ollama 翻訳 → Sherpa-ONNX / VOICEVOX TTS の C++ DLL Mod。
日本語で応答すること。

---

## ルール (絶対禁止)

- ゲームメモリへの**書き込みコードを生成するな** (読み取り専用)
- `DllMain` 内で `LoadLibrary` を呼ぶな (ローダーロック)
- `WIN32_LEAN_AND_MEAN` / `NOMINMAX` をソースで定義するな (CMake 定義済み)
- C++ 例外を使うな → `bool` 返却 + `logging::Debug()` で報告
- iostream を使うな → `fopen`/`fprintf`/`fflush`
- コミット・プッシュ・PR を勝手に行うな

## ルール (必須)

- UE4 メモリ読み取りは `__try/__except` (SEH) でガード
- スレッドフラグは `std::atomic<bool>`、ロックは `std::mutex` + `std::lock_guard`
- UE4 `FString` は即座に `FStringToUtf8()` で変換
- HTTP は WinHTTP のみ。JSON は手書き (`JsonEscape()` / `ExtractResponse()`)
- INI 読込は `GetPrivateProfileStringA` のみ
- MinHook フックは `MH_EnableHook` / `MH_DisableHook` でペア管理
- ProcessEvent コールバック内は最小処理のみ。重い処理は別スレッドへ
- 翻訳処理は描画スレッドをブロックするな

---

## ファイル構成

| ファイル | namespace / 役割 |
|---|---|
| `src/dllmain.cpp` | — / version.dll エントリ、vtable スキャン、ProcessEvent フック、Present フック、ワーカー DLL ロード |
| `src/worker_main.cpp` | — / chat_translator.dll エントリ、`WorkerInit()` |
| `src/hooks.h/cpp` | `hooks` / ProcessEvent コールバック、チャット判定、重複排除 |
| `src/gnames.h/cpp` | `gnames` / FNamePool 検出、`FindFNameIndex()` |
| `src/overlay.h/cpp` | `overlay` / DX11 ImGui オーバーレイ、マーキー、`OnChatMessage()` |
| `src/translate.h/cpp` | `translate` / Ollama WinHTTP 翻訳、保護語、`Sync()` / `AsyncTranslate()` |
| `src/tts.h/cpp` | `tts` / Sherpa-ONNX + VOICEVOX + XAudio2、`Speak()` |
| `src/ollama.h/cpp` | `ollama` / プロセス管理、ヘルス監視 |
| `src/config.h/cpp` | `config` / config.ini → `Config` 構造体、`Load()` |
| `src/scanner.h/cpp` | `scanner` / .text/.rdata セクションスキャン |
| `src/log.h/cpp` | `logging` / スレッドセーフログ |
| `src/ue4.h` | — / UE4 内部型定義 (Dumper-7 SDK 準拠) |
| `src/chat_message.h` | — / `ChatMessage` 構造体 |
| `src/radio_icon.h` | — / 埋め込み RGBA (32×32px) |

---

## config.ini キー一覧

| セクション | キー | デフォルト | 説明 |
|-----------|------|-----------|------|
| General | EnableConsole | true | デバッグコンソール |
| General | LogFilePath | "chat_log.txt" | ログ出力先 (DLL 同階層) |
| General | InitDelayMs | 10000 | UE4 初期化待機 (ms) |
| Discovery | DumpAllEvents | false | 全 ProcessEvent 出力 |
| Discovery | FunctionNameFilter | "" | 関数名フィルタ (カンマ区切り) |
| Stage2 | Prefix | "★" | チャット接頭辞 |
| Translation | Enabled | true | 翻訳機能 |
| Translation | OllamaEndpoint | "http://localhost:11435/api/generate" | Ollama API |
| Translation | TargetLanguage | "Japanese" | 翻訳先言語 |
| Translation | PerformancePreset | "Medium" | Low / Medium / High |
| Overlay | DemoMode | true | false = 実チャット駆動 |
| TTS | Language | "auto" | auto = テキストから自動判定 |
| TTS | VoicevoxStyleId | 3 | VOICEVOX スタイル ID (3 = ずんだもんノーマル) |
| TTS | SpeakTranslated | true | true = 翻訳後を読み上げ / false = 原文 |
| Addresses | ProcessEventVtableIndex | 66 | ProcessEvent vtable インデックス |

---

## アーキテクチャ

### 2-DLL 構成

```
version.dll (永続)
  ├─ system version.dll へ 17 関数フォワード
  ├─ ProcessEvent フック: vtable[66]、最大 64 枠 (MinHook)
  ├─ DX11 Present フック: vtable[8]
  └─ chat_translator.dll ロード/ホットリロード (F9 or ファイル変更)
        └─ WorkerInit() → hooks / overlay / translate / tts / ollama
```

ワーカー DLL だけを再ロード可能。フックアドレスは version.dll 側に維持。

### 翻訳・TTS パイプライン

```
ProcessEvent → hooks::OnProcessEvent
  → ChatMessage 構築 → 重複排除 (sender+message+channel, 500ms 窓)
  → overlay::OnChatMessage
    → [ラジオ ON] AsyncTranslate スレッド
      → 保護語マスク ({{T0}}…{{Tn}})
      → translate::Sync() [WinHTTP → Ollama]
      → 保護語復元 (2パス: 完全一致 → \bTn\b フォールバック)
      → tts::Speak(text, sender)
```

翻訳キュー MAX 32。TTS キュー MAX 8。

### パフォーマンスプリセット (`translate::ApplyPreset`)

| Preset | model | num_ctx | num_thread | temperature | num_predict |
|--------|-------|--------:|-----------:|------------:|------------:|
| Low | gemma3:1b | 128 | 2 | 0.1 | 120 |
| Medium | gemma3:4b | 256 | 0 | 0.1 | 120 |
| High | gemma3:4b | 512 | 0 | 0.1 | 120 |

`num_thread=0` = 全 CPU コア使用。

### ラジオアイコン状態

| 状態 | 動作 | クリック |
|---|---|---|
| ON | 翻訳 + TTS + 表示 | → OFF |
| OFF | 停止・非表示 | → ON |
| FAULT | 原文 + TTS のみ | Ollama 再起動試行 |
| RESTARTING | 再起動中 | — |

---

## TTS エンジン

### エンジン選択 (tts.cpp)

1. VOICEVOX Core — 言語=JA かつ初期化済み
2. Sherpa-ONNX — その他言語 / JA フォールバック
3. 英語モデルフォールバック — 対象モデル未インストール時

### Sherpa-ONNX モデル検出 (`War/Binaries/Win64/tools/tts/models/<lang>/`)

- `duration_predictor.int8.onnx` 存在 → Supertonic エンジン
- `model.onnx` + `tokens.txt` 存在 → VITS (Piper/mimic3) エンジン

日本語・韓国語は Piper 系モデルが存在しない:
- JA → Supertonic 3 (`sherpa-onnx-supertonic-3-tts-int8-2026-05-11`)
- KO → `vits-mimic3-ko_KO-kss_low`

### VOICEVOX パス (`War/Binaries/Win64/tools/tts/voicevox/`)

```
c_api/lib/voicevox_core.dll              ← lib/ サブディレクトリ (c_api/ 直下ではない)
onnxruntime/lib/voicevox_onnxruntime.dll ← lib/ サブディレクトリ
models/vvms/*.vvm                        ← vvms/ サブディレクトリ
dict/open_jtalk_dic_utf_8-1.11/
```

### TTS 初期化ルール

- VOICEVOX 初期化成功 + 言語=JA → VOICEVOX (style_id=3 = ずんだもんノーマル)
- VOICEVOX 失敗または非 JA → Sherpa-ONNX にフォールバック
- 両方とも使用不可 → TTS スレッドを起動しない (スキップ)

---

## 保護語システム (term_protection.txt)

- 1行1正規表現。行末 ` i` で icase。`\b` 未指定なら自動補完
- 翻訳前: マッチ語 → `{{T0}}`, `{{T1}}` … に置換
- 翻訳後: 2パス復元 — `{{Tn}}` 完全一致 → `\bTn\b` フォールバック
- マッチ語のみシステムプロンプトに列挙 ("keep exactly as-is")

---

## 落とし穴

### Sherpa-ONNX v1.13.x ABI 変更

`SherpaOnnxOfflineTtsModelConfig` に以下の新フィールドが追加 (全部定義しないとクラッシュ):
`Matcha` / `Kokoro` / `Kitten` / `Zipvoice` / `Pocket` / `Supertonic` の各 ModelConfig。
`SherpaOnnxGeneratedAudio.num_samples` → `.n` に改名。

DLL 名: `sherpa-onnx-c-api.dll` (新) / `sherpa-onnx.dll` (旧)。両方を順番に試す実装。
パッケージ選択条件: `win-x64-shared` + `MD-Release`、`-lib.` / `no-tts` を含まない。

### VOICEVOX ダウンローダー

- パイプ経由で実行するな → stdin 切断でライセンス同意プロンプトが無限ループ
- CP932 環境で Rust pager が Unicode パニック → `$env:TERM = "dumb"` でページャ抑制 (完全には止まらないが動作は続く)
- **BOM 混入問題**: `chcp 65001` 後に `[Console]::InputEncoding` が BOM 付き UTF-8 になり、PowerShell パイプ `"y" | & exe` が `\u{feff}y` を送信してダウンローダーが拒否する。対策: `cmd.exe` バッチファイル経由の `< stdin_file` リダイレクト (純粋バイト転送) を使う
- `--models-pattern "0.vvm"` で必要最小限 (ずんだもんノーマル = style_id 3) のみダウンロード。全 VVM はGitHub レートリミットに当たる

### チャットキャプチャ対象関数

`ClientChatMessage` / `ClientChatMessageWithTag` / `ClientWorldChatMessage` のみ。
ServerChat 系は除外。判定: FName ComparisonIndex 一致 + `FUNC_Net (0x40)` フラグ。

### GNames 検出

FNameBlockOffsetBits = 16 ハードコード。`TryDetectShift()` で自動補正。

### EAC

EasyAntiCheat により BAN の可能性あり (自己責任)。ゲームアップデートでオフセット変更の可能性あり。

---

## コーディング規約

| 対象 | 規則 |
|---|---|
| 関数 | `PascalCase` |
| ローカル変数 | `camelCase` |
| グローバル/static 変数 | `g_` + `camelCase` |
| namespace | `snake_case` |
| 構造体/enum | `PascalCase` |
| 定数/マクロ | `ALL_CAPS` |
| UE4 定数 | UE4 原名のまま (`FUNC_Net`, `EObjectFlags` 等) |

- C++17 / `#pragma once` / インクルード順: windows → C 標準 → C++ 標準 → 外部 → プロジェクト内
- モジュール構成: namespace + free function + static 内部状態 (クラスベースではない)
- 内部関数/変数は `static` (無名 namespace 未使用)
- コメント・ログ文字列は日本語

---

## ビルド・セットアップ

```powershell
# ビルド
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
cmake --build build --config Release --target chat_translator  # ワーカーのみ (F9 ホットリロード用)

# デプロイ (TTS・Ollama を含むフルインストール)
powershell -ExecutionPolicy Bypass -File .\install.ps1 -Dev

# TTS テスト GUI
python tools\tts_test.py
```

install.ps1 編集時: **UTF-8 BOM 必須** (PS5 + 日本語 Windows は BOM なし UTF-8 を CP932 として読む → マルチライン演算子継続やコメントのパースが壊れる)。Edit/Write ツールは BOM なしで保存するため、編集後に PowerShell で `[System.IO.File]::WriteAllText(path, content, (New-Object System.Text.UTF8Encoding $true))` で BOM を付与すること。`tar` は `$env:SystemRoot\System32\tar.exe` を使うこと (Git tar はドライブレターを解釈できない)。

---

## 参照

- UE4 オフセット・構造体レイアウト・parms 解析: `docs/ue4-reversing-foxhole.md`
- UE4 構造体フィールドオフセット: Dumper-7 SDK (`C:\Dumper-7\4.24.3-0+++UE4+Release-4.24-War\CppSDK\`)
- install.ps1 既知の落とし穴 (BOM/Ollama/VOICEVOX): `docs/installer-pitfalls.md`
