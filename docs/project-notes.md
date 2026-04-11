# 開発状態・実装メモ

## 現在の状態

Stage 1-7 完了。

## 初期化フロー

1. ゲーム起動 → Windows が War/Binaries/Win64/version.dll をロード (DLLプロキシ)
2. dllmain.cpp DLL_PROCESS_ATTACH → config.ini から InitDelayMs, ProcessEventVtableIndex 読み込み → Sleep(InitDelayMs)
3. .rdataセクション走査 → vtable[ProcessEventVtableIndex] の全実装を MinHook でフック (最大64枠)
4. chat_translator.dll を LoadLibrary → WorkerInit() 呼び出し
5. worker_main.cpp WorkerInit() → hooks::Init()
6. hooks::Init() → config::Load() → logging::Init() → gnames::Find()
7. gnames::Find() → ヒープメモリスキャンで FNamePool 検出 → FNameBlockOffsetBits 自動判定
8. チャット関連UFunctionの ComparisonIndex を gnames::FindFNameIndex() で逆引き・キャッシュ
9. ProcessEvent コールバック(hooks::OnProcessEvent)がフックから呼ばれる状態に

## データフロー (チャットキャプチャ)

ProcessEvent呼び出し → hooks::OnProcessEvent(thisObj, function, parms)
→ function->FunctionFlags & FUNC_Net(0x01000000) チェック
→ function->NamePrivate.ComparisonIndex をキャッシュ済みCIと照合
→ 一致 → parms を対応する Parms_* 構造体にキャスト
→ FStringToUtf8 で sender/message 抽出
→ 重複排除 (sender+message+channel, 500ms窓)
→ ChatMessage{channel, sender, message, channelEnum, timestamp} 構築
→ logging::Chat() + logging::Debug() で出力

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

## Stage 8 実装計画 — デモメッセージのリアルタイム翻訳 + Ollamaメモリ削減

目的: Stage 6 のハードコード翻訳をやめ、Stage 5 の Ollama 翻訳で実際に翻訳する。
合わせて gemma3:4b のメモリ使用量を削減する。
実装内容:
- デモメッセージ切替時に `translate::Sync()` (または非同期キュー) で原文を翻訳し、
  2行目「翻訳:」の行に結果を表示する
- DemoMessage 構造体から `translated` フィールドを削除し、`original` のみにする
- 翻訳中は「翻訳中...」を表示
- 翻訳完了後に `g_translatedText` を更新
- 翻訳は別スレッド (translate ワーカー) で行い、描画スレッドをブロックしない
Ollamaメモリ削減:
- Ollama API `options` パラメータで以下を指定:
  - `num_ctx=256` — チャットは短文のためコンテキスト長を最小化 (デフォルト2048の1/8)
  - `num_gpu=0` — GPU VRAM を一切使わない (ゲームが3-4GB VRAM消費するため)
  - `num_thread=2` — CPU スレッド数を制限しゲーム描画への影響を最小化
- メモリ見積もり (16GB RAM / RTX 3050 Ti 4GB VRAM):
  - gemma3:4b Q4: ~2.5GB RAM (CPU-only)
  - KVキャッシュ (num_ctx=256): ~50MB
  - Foxhole: ~4-6GB RAM + 3-4GB VRAM
  - OS: ~3GB → 合計 ~12GB / 16GB で余裕あり
- config.ini [Translation] セクションに `NumCtx=256`, `NumGpu=0`, `NumThread=2` 追加
制約:
- Python 不使用、純粋 C++ (WinHTTP)
- ゲーム描画を妨げない (翻訳は非同期)
Ollamaヘルスチェック & 自動復旧:
- 定期的に Ollama の死活を監視 (GET /api/tags または /api/version, 数秒間隔)
- ラジオアイコンの3状態:
  - 通常ON (不透明) — Ollama正常稼働中
  - 通常OFF (半透明) — ユーザーがOFFにした状態
  - 赤色 (エラー) — Ollamaがダウンしている状態
- 赤色アイコン用の radio_icon テクスチャを追加 (赤色版、32×32px)
- 赤色アイコンをクリック → Ollama再起動 (`translate::RestartOllama()` 等)
  - 再起動中はアイコンを点滅させるなどフィードバックを表示
  - 再起動成功 → 通常アイコンに戻る
  - 再起動失敗 → 赤色のまま + ログに警告
- translate.h/cpp に `translate::IsHealthy()` と `translate::Restart()` API 追加

## トラブルシューティング

- チャット未検出: コンソールで "GNames: FNamePool 発見!" 確認 → "ProcessEvent フック: N/M 成功" 確認 → ProcessEventVtableIndex=66 確認 → InitDelayMs を増やす
- ホットリロード: F9キー or chat_translator.dll ファイル変更で自動検出
