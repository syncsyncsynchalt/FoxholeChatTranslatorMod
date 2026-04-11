# Foxhole Chat Translator

DLLインジェクションによるUE4ゲーム内チャット自動翻訳。EAC BAN リスクあり、自己責任。

## アーキテクチャ

2つのDLL構成。version.dll(ローダー)がゲーム起動時にロードされ、chat_translator.dll(ワーカー)を動的ロードする。

version.dll: DLLプロキシ(17関数転送) → .rdataセクションvtable走査 → ProcessEvent(vtable[66])をMinHookでフック(最大64枠) → chat_translator.dllをLoadLibrary → コールバック登録
chat_translator.dll: FNamePool検出(メモリスキャン) → ProcessEventコールバックでUFunction名を解決 → チャット関連関数を判定 → ChatMessage構造体に変換 → ログ出力

ホットリロード: F9キーでchat_translator.dllを再読み込み。ファイル変更の自動検出にも対応。version.dllはゲーム起動中は変更不可。

## ソースファイル

### version.dll (ローダー)
- src/dllmain.cpp: DLLプロキシ + vtableフック + ワーカー管理 + ホットリロード
- src/scanner.cpp, src/scanner.h: PEセクション走査(.text/.rdataパターンマッチング)

### chat_translator.dll (ワーカー)
- src/worker_main.cpp: エントリポイント。exports: WorkerInit, WorkerShutdown, WorkerSetPEAddress
- src/hooks.cpp, src/hooks.h: ProcessEventコールバック + チャット判定 + ChatMessage構築。hooks::Init(), hooks::Shutdown(), hooks::OnProcessEvent(), hooks::SetHookedPEAddress()
- src/gnames.cpp, src/gnames.h: FNamePool検出エンジン。gnames::Find(), gnames::ResolveFName(), gnames::FindFNameIndex(), gnames::ResolveFNameWithShift(), gnames::GetBlockOffsetBits(), gnames::SetBlockOffsetBits()
- src/config.cpp, src/config.h: config.ini読み込み。config::Load(), config::Get() → Config構造体
- src/log.cpp, src/log.h: スレッドセーフログ。logging::Init(), logging::Shutdown(), logging::Debug(), logging::Chat(), logging::SetChatLogPath()

### 共有ヘッダー (chat_translator.dllのみ使用)
- src/ue4.h: UE4内部型(FName, TArray, FString, EChatChannel, EChatLanguage) + inline関数(IsReadableMemory, FStringToUtf8, ChannelName) + RPC Parms構造体(Parms_ClientChatMessage, Parms_ClientChatMessageWithTag, Parms_ClientWorldChatMessage)
- src/chat_message.h: ChatMessage{channel, sender, message, channelEnum, timestamp}。チャットキャプチャの出力型。

### その他
- version.def: DLLエクスポート定義(17関数)
- test/test_loader.cpp: version.dll + chat_translator.dllの動作確認用ローダー
- config.ini: ランタイム設定
- docs/project-notes.md: 開発状態・実装メモ
- docs/ue4-reversing-foxhole.md: UE4リバースエンジニアリング知見

## ビルド

前提: CMake 3.20+, Visual Studio 2019/2022 (C++), Git
CMakeパス: "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
コンパイラフラグ: MSVC /utf-8 必須

```
cd "C:\Program Files (x86)\Steam\steamapps\common\Foxhole\Mods\ChatTranslator"
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release                          # 全体
cmake --build . --config Release --target chat_translator # ワーカーのみ
```

ポストビルド: version.dll, chat_translator.dll, config.iniが ../../War/Binaries/Win64/ に自動コピーされる。

手動コピー:
```
copy build\Release\version.dll "..\..\War\Binaries\Win64\version.dll"
copy config.ini "..\..\War\Binaries\Win64\config.ini"
```

アンインストール: War\Binaries\Win64\version.dll を削除。

## config.ini

Config構造体(config.h)のフィールドと対応:

| セクション | キー | Config フィールド | デフォルト | 読み込み元 |
|---|---|---|---|---|
| General | EnableConsole | enableConsole | true | config.cpp |
| General | LogFilePath | logFilePath | "" | config.cpp |
| General | InitDelayMs | initDelayMs | 10000 | dllmain.cpp (直接読み) |
| Discovery | DumpAllEvents | dumpAllEvents | false | config.cpp |
| Discovery | FunctionNameFilter | functionNameFilter | "Chat,Message,..." | config.cpp |
| Addresses | ProcessEventVtableIndex | - | 66 | dllmain.cpp (直接読み) |
| Stage2 | Prefix | prefix | "★" (UTF-8: \xe2\x98\x85) | config.cpp |
| Stage3 | OllamaEndpoint | ollamaEndpoint | "http://localhost:11434/api/generate" | config.cpp |

Addresses.ProcessEventAddress, Addresses.GNamesAddress, Patterns.*: 現在未使用。パターンスキャンベースのアドレス検出用予約。

## チャットキャプチャ仕様

監視対象UFunction: ClientChatMessage, ClientChatMessageWithTag, ClientWorldChatMessage
除外: ServerChat系(送信RPC)
判定条件: FName ComparisonIndex一致 + FUNC_Net(0x01000000)フラグ検証
重複排除: 同一sender+message+channelの組み合わせを500msウィンドウで排除
出力: chat_log.txt(チャットのみ), debug_log.txt(デバッグ全般), コンソール

## ロードマップ

- Stage 1: チャットキャプチャ + ログ出力 [完了]
- Stage 2: ラジオオーバーレイ表示 (画面右上に32×32pxアイコン) [未着手]
- Stage 3: ショートカットキーでラジオON/OFF (Pキーでトグル) [未着手]
- Stage 4: ラジオON/OFFボイス再生 [未着手]
- Stage 5: ローカル自動翻訳テスト (英/露/韓/中/日, GPU不使用) [未着手]
- Stage 6: ローカル多言語読み上げテスト (TTS, インターネット不要) [未着手]
