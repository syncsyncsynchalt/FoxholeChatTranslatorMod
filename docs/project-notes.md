# 開発状態・実装メモ

## Stage 実装状態

Stage 1-9 完了。

## Stage 1 実装計画 — チャットキャプチャ

目的: ゲーム内チャットメッセージをリアルタイムに傍受・抽出する。
実装内容:
- DLLプロキシ方式でゲームプロセスに自動注入する
- UE4 の ProcessEvent をフックしてチャット関連イベントを捕捉する
- UE4 の FName テーブルを自動検出し、チャット関連関数を名前で特定する
- チャットメッセージから送信者名・本文・チャンネルを抽出する
- 同一メッセージの重複を排除する (500ms窓)
- 抽出したメッセージをログに出力する
- フック対象やタイミングを config.ini で調整可能にする

## Stage 2 実装計画 — ラジオオーバーレイ表示

目的: オーバーレイの基盤を作る。
実装内容:
- 画面右上にラジオアイコン (32×32px) を常時オーバーレイ表示する
- DX11 Present フック + ImGui でゲームウインドウ上にオーバーレイ

## Stage 3 実装計画 — アイコンクリックでラジオON/OFF

目的: ラジオアイコンをクリックして有効/無効を切り替えるUI。
実装内容:
- 画面右上のラジオアイコンをマウスクリックでトグル
- ラジオOFF時はアイコンを半透明にする
- ラジオON時はアイコンを不透明にする
- ゲーム内キーバインドと競合しないこと
- トグル状態を内部で管理する

## Stage 4 実装計画 — ラジオON/OFFボイス再生

目的: ラジオの状態切り替え時に音声フィードバックを与える。
実装内容:
- ラジオON時: "Radio ON" のボイスを再生
- ラジオOFF時: "Radio OFF" のボイスを再生
- 音声ファイル (WAV等) をアセットとして同梱し、Windows Audio API で再生

## Stage 5 実装計画 — Ollama ローカル翻訳

目的: Ollama (gemma3:4b) を使ってチャットメッセージを自動翻訳する。
対象言語: 英語、ロシア語、韓国語、中国語、日本語 → 日本語 (configurable)
実装内容:
- C++ WinHTTP で Ollama REST API (localhost:11434) にリクエスト
- translate.h/cpp: 同期 (Sync) + 非同期 (Queue + ワーカースレッド) API
- translate_test.exe: スタンドアロンテストアプリ (対話モード / 単文 / ファイル一括)
- hooks.cpp: ラジオON時に自動翻訳キュー投入
- config.ini [Translation] セクションで設定 (Endpoint, Model, TargetLanguage, Enabled)
Ollamaバンドル:
- tools/ollama/ に CPU-only バイナリ同梱 (ollama.exe + ggml-*.dll, 約47.5MB)
- モデル (gemma3:4b) は初回起動時に自動ダウンロード → tools/ollama/models/
- translate.cpp: FindBundledOllama → StartOllamaServe → EnsureOllama で全自動管理
- OLLAMA_MODELS 環境変数でモデル保存先をローカルに固定
テスト:
- `translate_test.exe "Hello world"` → 日本語翻訳結果を表示
- `translate_test.exe --file chat_log.txt` → ログ一括翻訳
- `translate_test.exe` → 対話モード
制約:
- Python 不使用、純粋 C++ (WinHTTP)
- Ollama バイナリは同梱済み、モデルは初回自動ダウンロード

## Stage 6 実装計画 — 翻訳表示領域（画面右下）

目的: 最終的に翻訳結果を表示するための領域を画面右下に作る。
実装内容:
- ラジオアイコン (右下) の左側に翻訳テキスト2行を横並びで表示:
  - 上の行: 原文: XXXXXXXXXXXXXXXXXXXXXX
  - 下の行: 翻訳: XXXXXXXXXXXXXXXXXXXXXX
  - 2行とも同じ幅で表示する
- テキスト領域の幅は固定 400px
- テキストが長い場合は自動で水平スクロール (マーキー) して全文を表示する
- テキスト領域に半透明の黒背景を付ける (ラジオON時のみ表示、OFF時は非表示)
- この段階では固定メッセージのデモ表示とする:
  - EN/RU/KO/ZH/JA の5言語ペア (原文+翻訳) を数秒ごとに自動切替
  - 各言語が正しく表示できることを確認するためのテスト
多言語フォント:
- NotoSansCJKjp-Regular.otf (~16MB) を Google Fonts から直接ダウンロードして assets/ に配置
- EN/RU/KO/ZH/JA すべてのスクリプトをカバー (Latin, Cyrillic, Hangul, CJK, かな/カナ)
- overlay.cpp の InitImGui() で io.Fonts->AddFontFromFileTTF() により登録
- グリフ範囲: Latin + Cyrillic + CJK Unified Ideographs + Hangul Syllables + Full-width

## Stage 7 実装計画 — 多言語TTS読み上げ

目的: チャット原文を音声で読み上げる。
対象言語: 英語、ロシア語、韓国語、中国語、日本語
TTSエンジン: Windows OneCore (Neural) — SAPI5 経由
実装内容:
- Stage 6 のデモメッセージの「原文」を、表示が切り替わるたびにTTSで読み上げる
- 5言語すべてに対応した音声合成
- テキストの言語を自動判定し、対応するOneCore音声を選択
- ラジオON時のみ読み上げ
- Python 不使用、純粋 C++ (SAPI5 COM API)
前提条件 (事前インストール必須):
- Windows OneCore 音声パックを管理者権限でインストールする必要がある:
  ```powershell
  # 管理者PowerShellで実行
  Add-WindowsCapability -Online -Name "Language.Speech~~~en-US~0.0.1.0"
  Add-WindowsCapability -Online -Name "Language.Speech~~~zh-CN~0.0.1.0"
  Add-WindowsCapability -Online -Name "Language.TextToSpeech~~~ru-RU~0.0.1.0"
  Add-WindowsCapability -Online -Name "Language.TextToSpeech~~~ko-KR~0.0.1.0"
  ```
- 日本語 (ja-JP) はWindows日本語版に標準搭載
- 音声が見つからない言語はスキップしてログに警告出力
制約:
- ゲームプロセスのメモリは書き換えない

## Stage 8 実装計画 — リアルタイム翻訳 + メモリ削減 + ヘルスチェック

目的: デモメッセージのハードコード翻訳をやめ、Ollama でリアルタイム翻訳する。
合わせてメモリ使用量の削減と、Ollama の死活監視・自動復旧を行う。
要件:
- デモメッセージ切替時に Ollama で非同期翻訳を実行する
- 翻訳完了後に原文+翻訳を同時表示し、TTS 読み上げを開始する
- 翻訳中は前のメッセージ表示を維持する (「翻訳中...」のような中間表示は出さない)
- 翻訳処理は描画スレッドをブロックしない
メモリ・パフォーマンス最適化:
- コンテキスト長とCPUスレッド数を設定で制限し、ゲーム描画への影響を最小化する
- GPU レイヤー割り当ては Ollama に委任する (明示的に 0 指定しない)
- 接続タイムアウトを短縮し、Ollama 停止時にゲームがフリーズしないようにする
ヘルスチェック & 自動復旧:
- 定期的に Ollama の生死を非同期で確認する (描画スレッドで HTTP を待たない)
- Ollama が停止している場合は翻訳リクエストを送らずスキップする
- ラジオアイコンの状態を3つに拡張する:
  - ON (不透明) — 正常稼働中
  - OFF (半透明) — ユーザーが無効化した状態、テキスト領域非表示
  - FAULT (赤色) — Ollama ダウン、テキスト領域は表示し原文+TTSは再生
- FAULT 状態でアイコンをクリックすると Ollama の再起動を試みる

## Stage 9 実装計画 — 実チャットメッセージの翻訳・読み上げ

目的: デモメッセージ翻訳を卒業し、ゲーム内の実チャットメッセージを翻訳・TTS再生する。
要件:
- フックで取得した実際のチャットメッセージをオーバーレイに渡して翻訳・表示する
- 翻訳・TTS 再生の対象はメッセージ本文のみ (送信者名は表示のみ、翻訳・読み上げしない)
- 翻訳完了後に原文+翻訳を同時表示し、TTS で読み上げる
- ラジオ ON 時のみ翻訳・再生する (OFF/FAULT 時の動作は Stage 8 と同様)
TTS ボイス割り当て:
- 送信者ごとに固定の声色を割り当てる (同じ送信者は常に同じ声色で読み上げる)
- 送信者名から決定論的に声色を選択する (ランダムにしない)
デモ/実チャット切替:
- 設定でデモモードと実チャットモードを切り替えられるようにする
  - デモモード: Stage 8 までの自動切替デモメッセージ (開発・テスト用)
  - 実チャットモード: ゲーム内チャット駆動 (本番モード)
- ホットリロードで切替可能
制約:
- デモモードと実チャットモードで翻訳・TTS の処理フローは共通とする
- 翻訳は描画スレッドをブロックしない

## Stage 10 実装計画 — Ollama 翻訳速度最適化

目的: Ollama の翻訳が遅い問題を解決するため、パラメータのベンチマークを行い、PC スペックに応じた 3 段階のプリセットを提供する。
背景:
- 現状の設定 (num_ctx=256, num_thread=2) は省リソース寄りで翻訳レイテンシが大きい
- スレッド数・コンテキスト長を調整することで大幅な高速化が見込める
- ただし高速設定は CPU/メモリに負荷がかかるため、環境に応じた使い分けが必要
- Ollama REST API の options で per-request に制御可能な速度関連パラメータは **num_ctx** と **num_thread** のみ (api/types.go Runner struct より確認)
- モデルサイズも速度に大きく影響する。ベンチマーク対象: gemma3:4b (現行), gemma3:1b, gemma3:270m

ベンチマーク:
- ゲームを起動せずに Ollama 単独で実行できるベンチマークスクリプト (PowerShell) を作成する
- 直接 Ollama REST API を叩いて計測する
- テスト文は実チャットログの長さ分布に基づき、3段階で用意する

テスト文:
- **英語 → 日本語** の 3 段階
  - 短文 (~10文字): 1文
  - 中文 (~40文字): 1文
  - 長文 (~80文字): 1文
- ベンチマーク前に `ollama pull gemma3:1b` / `ollama pull gemma3:270m` でモデルを事前ダウンロードする

ベンチマークパターン一覧 (全 13 パターン):
  1. **4b-Baseline**: gemma3:4b, num_ctx=256, num_thread=2 (現行設定)
  2. **4b-Thread4**: gemma3:4b, num_ctx=256, num_thread=4
  3. **4b-ThreadMax**: gemma3:4b, num_ctx=256, num_thread=<物理コア数>
  4. **4b-ThreadAuto**: gemma3:4b, num_ctx=256, num_thread=0 (Ollama デフォルト、ランタイムが自動決定)
  5. **4b-Ctx128**: gemma3:4b, num_ctx=128, num_thread=4 (※システムプロンプト ~55トークン+入出力で128を超える可能性あり。切り詰め発生時はパターンから除外)
  6. **4b-Ctx512**: gemma3:4b, num_ctx=512, num_thread=4
  7. **4b-MinCtx+MaxThread**: gemma3:4b, num_ctx=128, num_thread=<物理コア数> (Ctx128と同様の切り詰めリスクあり)
  8. **1b-Thread4**: gemma3:1b, num_ctx=256, num_thread=4
  9. **1b-ThreadAuto**: gemma3:1b, num_ctx=256, num_thread=0
  10. **1b-ThreadMax**: gemma3:1b, num_ctx=256, num_thread=<物理コア数>
  11. **270m-Thread4**: gemma3:270m, num_ctx=256, num_thread=4
  12. **270m-ThreadAuto**: gemma3:270m, num_ctx=256, num_thread=0
  13. **270m-ThreadMax**: gemma3:270m, num_ctx=256, num_thread=<物理コア数>

各パターンで以下を記録する:
  - 平均応答時間 (ms)
  - tokens/s (Ollama レスポンスの eval_count / eval_duration から算出)
  - 翻訳出力テキスト (品質の目視確認用にそのまま CSV に保存)
  - メモリ使用量: ベンチマーク前後の ollama プロセスの WorkingSet64 差分を Get-Process で取得
  - CPU/GPU 使用率: 自動計測はしない。プリセット候補が絞れた後に手動で Task Manager 確認
各パターンを 3 回ずつ実行し、初回 (モデルロード込み) と 2〜3 回目 (ウォームキャッシュ) を分けて記録する
推定実行時間: 13 パターン × 3 テスト文 × 3 回 = 117 リクエスト。**約 15 分** (小モデルは高速のため実際はもっと短い)

環境変数テスト (Ollama 再起動が必要、手動実施):
- 上記ベンチマークで最速パターンが判明した後、そのパターンで以下を追加テストする:
  - **OLLAMA_FLASH_ATTENTION=1**: Ollama をこの環境変数付きで再起動し、最速パターンを再計測
  - **OLLAMA_KV_CACHE_TYPE=q8_0**: KV キャッシュを q8 量子化して VRAM 削減効果を確認
- これらは per-request で制御できないため、プリセットには含めず README に推奨環境変数として記載する

3 段階プリセット:
- ベンチマーク結果を元に以下の 3 プリセットを config.ini の [Translation] セクションに追加する
- config.ini に PerformancePreset キーを追加し、値で切り替える

| プリセット | 目的 | 方針 |
|-----------|------|------|
| Low | ローエンド PC | 速度を妥協し最小リソースで動作。ゲーム FPS への影響を最小化 |
| Medium | ミドルレンジ PC | リソースと速度のバランスが最も良い実用的な設定 |
| High | ハイエンド PC | 最大限リソースを使用して最速で動作 |

- プリセット選択時、内部でモデル名 / num_ctx / num_thread を自動設定する
- num_thread はマシンごとにコア数が異なるため、プリセットでは num_thread=0 (Ollama が実行時に自動決定) を基本とする。ベンチマーク結果で固定値の方が明確に優れる場合のみ物理コア数ベースの計算式 (例: cores/2) を採用する
- Low プリセットでは、翻訳品質が実用レベルなら gemma3:1b や gemma3:270m を使用してリソース消費を最小化する可能性がある


実装内容:
- test/benchmark_ollama.ps1: ベンチマークスクリプト (PowerShell)
  - Ollama REST API に直接 HTTP リクエストを送信して計測
  - 結果を CSV + コンソールサマリーで出力
  - 実行前に Ollama の起動確認、未起動なら同梱バイナリから自動起動
- config.h/cpp: PerformancePreset フィールド追加 ("Low" / "Medium" / "High")
- config.ini: [Translation] に PerformancePreset キー追加 (デフォルト: "Low")
- translate.cpp: Init() でプリセットに応じたパラメータを適用
- docs/benchmark-results.md: ベンチマーク結果のまとめドキュメント
  - benchmark_ollama.ps1 の出力 CSV を整形して記載する
  - 以下の構成で記録する:
    1. 実行環境 (CPU型番, コア数, RAM容量, GPU型番, VRAM容量, OS, Ollamaバージョン, モデル)
    2. テスト文一覧 (言語, 文字数, 内容)
    3. パターン別結果テーブル (パターン名, 平均応答時間ms, tokens/s, 初回/ウォーム別, メモリMB)
    4. モデル別翻訳出力一覧 (原文 / 翻訳結果テキストをそのまま掲載、品質の目視確認用)
    5. 考察 (ボトルネック分析, 適切なパラメータの選択)
    6. プリセット決定根拠 (Low/Medium/High 各プリセットに選んだパラメータとその理由)
  - 異なるマシンで再実行した場合は結果を追記できる構造にする

config.ini [Translation] セクション (整理後):

| キー | Config フィールド | デフォルト | 説明 |
|------|------------------|-----------|------|
| Enabled | translationEnabled | true | 翻訳機能の有効/無効 |
| OllamaEndpoint | ollamaEndpoint | "http://localhost:11434/api/generate" | Ollama APIエンドポイント |
| TargetLanguage | targetLanguage | "Japanese" | 翻訳先言語 |
| PerformancePreset | performancePreset | "Low" | プリセット (Low/Medium/High) |

- NumCtx / NumThread / OllamaModel は削除し、PerformancePreset が内部で model / num_ctx / num_thread を一括管理する
- デフォルトは "Low" (どんな PC でも動作することを最優先)

テスト:
- `powershell -ExecutionPolicy Bypass -File test/benchmark_ollama.ps1` でベンチマーク実行
- 出力された CSV を元にプリセット値を決定
- 各プリセットでの translate_test.exe 動作確認
制約:
- ベンチマークスクリプトはゲーム不要、Ollama 単独で動作する
- Python 不使用、PowerShell + C++ のみ
- プリセット値はベンチマーク結果確定後に最終決定する


## テスト方法

### 単体テスト (ゲーム不要)
```powershell
cd build/Release
.\test_loader.exe        # DLL動作確認
.\translate_test.exe "Hello world"  # Ollama翻訳テスト
```

### ゲームテスト
```powershell
Start-Process "steam://rungameid/505460"
# コンソールに [ChatTranslator] [Team] sender: message 形式で出力される
# F9でワーカーホットリロード, F10=アンロード, F11=ステータス
```

### アンインストール
```powershell
$dir = "C:\Program Files (x86)\Steam\steamapps\common\Foxhole\War\Binaries\Win64"
Remove-Item "$dir\version.dll", "$dir\chat_translator*.dll", "$dir\config.ini" -ErrorAction SilentlyContinue
Remove-Item "$dir\assets", "$dir\tools" -Recurse -ErrorAction SilentlyContinue
```

## トラブルシューティング

- チャット未検出: コンソールで "GNames: FNamePool 発見!" 確認 → "ProcessEvent フック: N/M 成功" 確認 → ProcessEventVtableIndex=66 確認 → InitDelayMs を増やす
- ホットリロード: F9キー or chat_translator.dll ファイル変更で自動検出 (2秒ごとFILETIME監視)
- Ollama未起動: ラジオアイコンが赤(FAULT)になる → アイコンクリックで再起動、または tools/ollama/ollama.exe serve を手動実行
- TTS音声なし: Windows OneCore音声パックがインストールされているか確認 (管理者PowerShellで Add-WindowsCapability)
- フォント表示崩れ: assets/NotoSansCJKjp-Regular.otf が War/Binaries/Win64/assets/ にコピーされているか確認


## 技術詳細 (copilot-instructions.md から移動)

### ファイル構成

```
Mods/ChatTranslator/
├── .github/copilot-instructions.md  # Copilot指示書
├── CMakeLists.txt          # ビルド構成 (version.dll + chat_translator.dll + test_loader + translate_test)
├── version.def             # DLLエクスポート定義 (17関数)
├── config.ini              # ランタイム設定
├── assets/                 # フォント、効果音、アイコン
├── docs/                   # 開発メモ、リバースエンジニアリング知見
├── test/                   # test_loader.cpp, translate_test.cpp
├── tools/ollama/           # CPU-only Ollamaバイナリ同梱
└── src/
    ├── dllmain.cpp         # version.dll: プロキシ + vtableフック + Presentフック + ワーカーローダー
    ├── scanner.h/cpp       # パターンスキャナー (.text/.rdataセクション走査)
    ├── worker_main.cpp     # chat_translator.dll エントリポイント
    ├── hooks.h/cpp         # ProcessEventコールバック + チャット判定
    ├── gnames.h/cpp        # FNamePool検出エンジン
    ├── config.h/cpp        # config.ini → Config構造体
    ├── log.h/cpp           # スレッドセーフログ
    ├── overlay.h/cpp       # DX11 ImGuiオーバーレイ
    ├── translate.h/cpp     # Ollama WinHTTP翻訳
    ├── tts.h/cpp           # WinRT OneCore + XAudio2 TTS
    ├── ue4.h               # UE4内部型定義 (Dumper-7 SDK準拠)
    ├── chat_message.h      # ChatMessage構造体
    └── radio_icon.h        # 埋め込みRGBAデータ (32×32px)
```

### config.ini

| セクション | キー | Config フィールド | デフォルト | 説明 |
|-----------|------|------------------|-----------|------|
| General | EnableConsole | enableConsole | true | デバッグコンソール表示 |
| General | LogFilePath | logFilePath | "" | チャットログ出力先 (空=DLL同階層) |
| General | InitDelayMs | initDelayMs | 10000 | UE4初期化待機(ms) |
| Discovery | DumpAllEvents | dumpAllEvents | false | 全ProcessEvent出力(デバッグ用) |
| Discovery | FunctionNameFilter | functionNameFilter | "" | 関数名フィルタ (カンマ区切り) |
| Stage2 | Prefix | prefix | "★" | チャット接頭辞 |
| Translation | Enabled | translationEnabled | true | 翻訳機能の有効/無効 |
| Translation | OllamaEndpoint | ollamaEndpoint | "http://localhost:11434/api/generate" | Ollama APIエンドポイント |
| Translation | TargetLanguage | targetLanguage | "Japanese" | 翻訳先言語 |
| Translation | PerformancePreset | performancePreset | "Low" | プリセット (Low/Medium/High) |
| Overlay | DemoMode | demoMode | true | デモモード |
| Addresses | ProcessEventVtableIndex | - | 66 | ProcessEvent vtableインデックス |

### DLL初期化フロー
1. ゲーム起動 → Windows が War/Binaries/Win64/version.dll をロード (DLLプロキシ)
2. dllmain.cpp DLL_PROCESS_ATTACH → LoadOriginalDll(17関数) → CreateThread で InitThread を起動 (ローダーロック回避)
3. InitThread → AllocConsole → SetConsoleOutputCP(UTF8) → config.ini から InitDelayMs 読み込み → Sleep(InitDelayMs)
4. MH_Initialize → config.ini から ProcessEventVtableIndex 読み込み → .rdataセクション走査 → vtable[peIndex] の全実装を MinHook でフック (最大64枠)
5. DX11 Presentフック: ダミーデバイスでvtable[8]取得 → MinHookフック → WndProcサブクラス
6. chat_translator.dll を LoadLibrary → WorkerInit() 呼び出し → コールバック設定(ProcessEvent/Render/WndProc)
7. worker_main.cpp WorkerInit() → hooks::Init() → overlay::Init() → translate::Init()
8. hooks::Init() → config::Load() → logging::Init() → gnames::Find()
9. gnames::Find() → ヒープメモリスキャンで FNamePool 検出 (FNameBlockOffsetBits は 16 にハードコード)
10. チャット関連UFunctionの ComparisonIndex を gnames::FindFNameIndex() で逆引き・キャッシュ
11. overlay::Init() → TTS初期化 + デモモードなら初回非同期翻訳開始
12. translate::Init() → Ollama同梱バイナリ自動起動 (FindBundledOllama → StartOllamaServe → EnsureOllama)
13. OnProcessEvent 初回100回で hooks::TryDetectShift() により FNameBlockOffsetBits を自動検証・補正

### 翻訳・表示・TTS パイプライン
1. hooks::OnProcessEvent → ChatMessage構築 → overlay::OnChatMessage(sender, message)
2. overlay::OnChatMessage → ラジオON時のみ AsyncTranslate スレッド起動
3. AsyncTranslate → translate::Sync(text) で Ollama 翻訳 (別スレッド、描画ブロックなし)
4. 翻訳完了 → g_originalText / g_translatedText を更新 → tts::Speak(text, sender)
5. TTS → 言語自動判定 → 送信者名から決定論的にOneCore音声を選択 → 読み上げ
6. RenderFrame → マーキースクロールで原文+翻訳を表示

Ollamaダウン時: 原文のみ表示 + TTS再生、ラジオアイコンが赤色(FAULT)に変化。FAULTアイコンクリックでOllama再起動を試行。

### データフロー (チャットキャプチャ)

ProcessEvent呼び出し → hooks::OnProcessEvent(thisObj, function, parms)
→ function->NamePrivate.ComparisonIndex をキャッシュ済みCIと照合
→ 一致時 function->FunctionFlags & FUNC_Net(0x40) チェック
→ 一致 → parms を対応する Parms_* 構造体にキャスト
→ FStringToUtf8 で sender/message 抽出
→ 重複排除 (sender+message+channel, 500ms窓)
→ ChatMessage{channel, sender, message, channelEnum, timestamp} 構築
→ logging::Chat() + logging::Debug() で出力
→ overlay::OnChatMessage(sender, message) で翻訳・TTS転送

### ラジオアイコン状態

| 状態 | 外観 | 動作 | クリック時 |
|---|---|---|---|
| ON | 不透明 | 翻訳+TTS+テキスト表示 | OFFに切替 |
| OFF | 半透明 | テキスト非表示、翻訳停止 | ONに切替 |
| FAULT | 赤色 | 原文表示+TTS (翻訳なし) | Ollama再起動試行 |