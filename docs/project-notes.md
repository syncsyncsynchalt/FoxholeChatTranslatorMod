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
→ // TODO: Stage 2 - chatMsg を翻訳パイプラインに渡す

## Stage 2 実装計画

開始点: hooks.cpp OnProcessEvent() 末尾の `// TODO: Stage 2` マーカー
入力: ChatMessage 構造体 (chat_message.h)
設定値: config::Get().ollamaEndpoint, config::Get().prefix (読み込み済み)
実装予定: translator.h/cpp を新規作成 → ChatMessage を受け取り Ollama API (gemma3:4b) で翻訳
CMake: chat_translator ターゲットに src/translator.cpp を追加

## Stage 3 実装計画

翻訳結果をゲーム内チャットUIに表示する。UE4内部のチャット送信関数を呼び出す方式。詳細未定。

## トラブルシューティング

- チャット未検出: コンソールで "GNames: FNamePool 発見!" 確認 → "ProcessEvent フック: N/M 成功" 確認 → ProcessEventVtableIndex=66 確認 → InitDelayMs を増やす
- ホットリロード: F9キー or chat_translator.dll ファイル変更で自動検出
