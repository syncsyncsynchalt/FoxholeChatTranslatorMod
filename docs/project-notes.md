# 開発状態・実装メモ

## 現在の状態

Stage 1 完了。Stage 2 未着手。リファクタリング完了済み。

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

目的: ゲームクライアントのメモリ書き換えを避け、翻訳結果を音声(ラジオ)で流す方式に変更。Stage 2 ではまずオーバーレイの基盤を作る。
実装内容:
- 画面右上にラジオアイコン (32×32px) を常時オーバーレイ表示する
- ゲームウインドウに対して DirectX/GDI オーバーレイ、または透明ウインドウを重ねる方式を検討
- ゲームクライアントのメモリは書き換えない

## Stage 3 実装計画 — ショートカットキーでラジオON/OFF

目的: Pキーでラジオの有効/無効を切り替えるUI実験。
実装内容:
- Pキー押下でラジオアイコンを半透明にする (ラジオOFFを表現)
- もう一度Pキーを押すと半透明を解除 (ラジオONを表現)
- トグル状態を内部で管理する

## Stage 4 実装計画 — ラジオON/OFFボイス再生

目的: ラジオの状態切り替え時に音声フィードバックを与える。
実装内容:
- ラジオON時: "Radio ON" のボイスを再生
- ラジオOFF時: "Radio OFF" のボイスを再生
- 音声ファイル (WAV等) をアセットとして同梱し、Windows Audio API で再生

## Stage 5 実装計画 — ローカル自動翻訳テスト

目的: インターネット接続なしで多言語翻訳が動作することを検証する。
対象言語: 英語、ロシア語、韓国語、中国語、日本語
実装内容:
- 固定で与えたメッセージを指定言語に翻訳し、テキストとして得られればよい（UI統合は不要）
- 翻訳はすべてローカルで完結すること（インターネットアクセス不可）
制約:
- CPU・メモリを過剰に消費しないこと
- GPU に依存しないこと（VRAM を大量に使わないこと）
- 軽量な翻訳モデル・ライブラリを選定すること

## Stage 6 実装計画 — ローカル多言語読み上げテスト

目的: インターネット接続なしで多言語テキストの音声読み上げ (TTS) が動作することを検証する。
対象言語: 英語、ロシア語、韓国語、中国語、日本語
実装内容:
- 固定で与えたメッセージを自動的に読み上げられればよい（UI統合は不要）
- 読み上げはすべてローカルで完結すること（インターネットアクセス不可）

## トラブルシューティング

- チャット未検出: コンソールで "GNames: FNamePool 発見!" 確認 → "ProcessEvent フック: N/M 成功" 確認 → ProcessEventVtableIndex=66 確認 → InitDelayMs を増やす
- ホットリロード: F9キー or chat_translator.dll ファイル変更で自動検出
