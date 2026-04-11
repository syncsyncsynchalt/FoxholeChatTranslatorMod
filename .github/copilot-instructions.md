# Foxhole Chat Translator - Copilot Instructions

## プロジェクト概要

FoxholeのゲームチャットをリアルタイムOllama連携で自動翻訳するC++ DLLモジュール。
version.dllプロキシ方式でUE4ゲームに自動ロードされ、ProcessEventをフックしてチャットメッセージを傍受する。

## アーキテクチャ

### 2-DLL構成
- **version.dll** (永続DLL): システムのversion.dllを偽装しゲーム起動時に自動ロード
  - 17個のエクスポート関数をオリジナルDLL (System32\version.dll) に転送
  - `version.def` でエクスポート名を `Proxy_*` にマッピング
  - .rdataセクション内の全vtableを走査し、ProcessEvent (vtable[66]) の全実装をMinHookでフック
  - DEFINE_PE_HOOK(N)マクロで0-63の個別detour関数を生成
  - chat_translator.dllの管理: ロード/アンロード/ホットリロード/自動リロード(2秒FILETIME監視)/整合性チェック(3秒JMPバイト検証)
- **chat_translator.dll** (ホットリロード可能ワーカー): GNames検出 + OnProcessEventコールバック
  - GNames: FNamePool Block[0]をメモリ走査で検出し、モジュール内逆引きでBlocks[]配列を特定
  - FName CI照合でチャット関連UFunctionを判別 (FUNC_Net 0x40検証で偽陽性除去)
  - 重複排除 (500ms窓) で正確にチャットメッセージを判定
  - 検出メッセージをコンソール表示 + chat_log.txt に記録

### 対象ゲーム
- Foxhole (War-Win64-Shipping.exe)
- **UE4 4.24.3** (`4.24.3-0+++UE4+Release-4.24-War`)
- ゲームパス: `C:\Program Files (x86)\Steam\steamapps\common\Foxhole\`

## ファイル構成

```
Mods/ChatTranslator/
├── .github/copilot-instructions.md  # この知見ファイル
├── .gitignore
├── CMakeLists.txt          # ビルド構成 (version.dll + chat_translator.dll + test_loader)
├── version.def             # DLLエクスポート定義 (17関数)
├── config.ini              # ランタイム設定
├── README.md
├── docs/
│   ├── project-notes.md    # プロジェクトメモ
│   └── ue4-reversing-foxhole.md  # UE4リバースエンジニアリング知見
├── test/
│   └── test_loader.cpp     # DLL動作確認テスト
└── src/
    ├── dllmain.cpp         # version.dll: プロキシ + vtableフック + ワーカーローダー
    ├── scanner.h/cpp       # パターンスキャナー (.text/.rdataセクション走査)
    ├── ue4.h               # UE4内部型定義 (Dumper-7 SDK準拠)
    ├── hooks.h/cpp         # chat_translator.dll: GNames検出 + チャットキャプチャ
    └── worker_main.cpp     # chat_translator.dll エントリポイント
```

## ビルド方法

```powershell
# 前提: Visual Studio 2022 Community (C++ デスクトップ開発ワークロード)
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

cd "C:\Program Files (x86)\Steam\steamapps\common\Foxhole\Mods\ChatTranslator"

# 初回: CMake構成
& $cmake -B build -G "Visual Studio 17 2022" -A x64

# フルビルド (version.dll + chat_translator.dll + test_loader)
& $cmake --build build --config Release

# ワーカーのみリビルド (ゲーム起動中でもOK、F9でホットリロード)
& $cmake --build build --config Release --target chat_translator

# → version.dll, chat_translator.dll, config.ini が War/Binaries/Win64/ に自動コピー
# → ゲーム起動中は version.dll のコピーが失敗する (ファイルロック) → 正常動作
```

### ビルド注意点
- MSVC `/utf-8` フラグ必須（CMakeLists.txtで設定済み）
- MinHookはFetchContentで自動取得（Git必要）
- `WIN32_LEAN_AND_MEAN` はCMake側で定義済み。ソースでは `#ifndef` ガード使用

## config.ini (実際にコードが読み込む設定のみ)

| セクション | キー | 説明 | 読み込み元 |
|-----------|------|------|-----------|
| General | EnableConsole | デバッグコンソールの表示 (1/0) | hooks.cpp |
| General | LogFilePath | チャットログ出力先パス (空=DLL同階層) | hooks.cpp |
| General | InitDelayMs | UE4初期化待機時間 (ms, デフォルト: 10000) | dllmain.cpp |
| Addresses | ProcessEventVtableIndex | ProcessEvent vtableインデックス (デフォルト: 66) | dllmain.cpp |

> Discovery, Patterns, Stage2, Stage3 セクションはconfig.iniに存在するが、
> 現在のコードでは読み込まれない（将来のStage 2/3実装用プレースホルダー）。

## 開発ロードマップ (3段階)

### Stage 1: チャットキャプチャ + テキストファイル出力 ✅ 完了
- ProcessEventフックで3種のチャットRPCを検出:
  - ClientChatMessage, ClientChatMessageWithTag, ClientWorldChatMessage
- Dumper-7 SDK準拠の構造体でparms直接キャスト（FString探索不要）
- 検出メッセージをコンソール表示 + chat_log.txt にUTF-8で記録

### Stage 2: Ollama連携による自動翻訳 (未着手)
- Ollama API (localhost:11434/api/generate) でローカルLLM翻訳
- モデル: gemma3:4b、ターゲット言語: ja
- WinHTTP でHTTPクライアント実装 (chat_translator.dll内)
- 非同期リクエスト + キャッシュで遅延を最小化する設計が必要

### Stage 3: 翻訳結果のゲーム内表示 (未着手)
- チャット表示テキストを翻訳結果に直接置き換え、またはparms書き換えで接頭辞追加

## 技術詳細

### UE4オフセット (Foxhole UE4 4.24.3 x64, Dumper-7 SDK確認済み)
- ProcessEvent vtableインデックス: **66 (0x42)**
- UObject::ClassPrivate: +0x10
- UObject::NamePrivate: +0x18 (FName = ComparisonIndex:int32 + Number:int32)
- UObject::OuterPrivate: +0x20
- UFunction::FunctionFlags: +0x98 (FUNC_Net = 0x40)
- APlayerState::PlayerNamePrivate: +0x328

### GNames / FNamePool
- FNameEntryHeader: `(Len<<1) | bIsWide` → headerShift = **1** (UE4 4.24系)
- FNameBlockOffsetBits = **16** (自動検出済み)
- ComparisonIndex: `(block << 16) | (offset / stride)`, stride=2
- Block[0]検出: メモリ全域走査で "None" + gap(2/8) + "ByteProperty" の複合パターン
- FNamePool検出: Block[0]ポインタをモジュール内(.data)で逆引き → Blocks[]配列

### チャットparms構造体 (Dumper-7 SDK: War_parameters.hpp)
- ClientChatMessage (0x28): Channel+0x00, SenderPlayerState+0x08, MsgString+0x10
- ClientChatMessageWithTag (0x38): Channel+0x00, SenderPlayerState+0x08, RegTag+0x10, MsgString+0x20
- ClientWorldChatMessage (0x48): Message+0x00, SenderName+0x10, RegTag+0x20, Channel+0x41
- ServerChat (0x18): 送信RPCのため監視不要。除外済み（不正parmsからゴミデータが出力される）

### DLL初期化フロー
1. DllMain(DLL_PROCESS_ATTACH) → LoadOriginalDll(17関数) → CreateThread(InitThread)
2. InitThread: AllocConsole → SetConsoleOutputCP(UTF8) → Sleep(InitDelayMs)
3. MH_Initialize → vtableパターンスキャン → .rdata全vtable走査でユニークPE収集 → MinHookフック
4. LoadWorker: chat_translator.dllをコピー→ロード→WorkerInit()→コールバック設定
5. ホットキーループ: F9=リロード, F10=アンロード, F11=ステータス + 自動リロード + 整合性チェック

### vtableスキャナーの重要ポイント
- **vtable先頭境界検出が必須**: addr[-1]がモジュール内コードを指さない位置 = vtable先頭
  - これがないとスライディングウィンドウで大量の偽陽性が出る
- vtable候補判定: [0],[1],[2]がモジュール内コードを指すか確認
- 全ユニークPEアドレスに個別MinHookフック (最大64個)

## テスト方法

### 単体テスト (ゲーム不要)
```powershell
cd build/Release
.\test_loader.exe
```

### ゲームテスト
```powershell
Start-Process "steam://rungameid/505460"
# コンソールに [ChatTranslator] [Team] sender: message 形式で出力される
# F9でワーカーホットリロード
```

### アンインストール
```powershell
Remove-Item "C:\Program Files (x86)\Steam\steamapps\common\Foxhole\War\Binaries\Win64\version.dll"
Remove-Item "C:\Program Files (x86)\Steam\steamapps\common\Foxhole\War\Binaries\Win64\config.ini"
Remove-Item "C:\Program Files (x86)\Steam\steamapps\common\Foxhole\War\Binaries\Win64\chat_translator*.dll"
```

## Git
- author: Yohei Ikata <ikata-yohei@jsdnet.co.jp>
- リポジトリルート: Foxholeインストールディレクトリ

## 学んだ教訓

### SDK出力を最初に確認すること
- HWBPやランタイムデバッグより、Dumper-7等のSDK出力が信頼性高い
- HWBPでvtable[77]と測定 → 実際はDumper-7 SDKの通りvtable[66]だった
- 間違ったインデックスの症状: コールバックは大量に来るがプロパティ名しか見えない

### ServerChat (送信RPC) を監視してはいけない
- ServerChatはクライアント→サーバーの送信RPC (NetServer)
- UE4のRPC処理中に同じFName CIを持つ別のProcessEvent呼び出しが発生し、不正parmsからゴミデータが読まれる
- プレイヤー自身のメッセージはサーバーからClientChatMessage/WithTagとして折り返されるので機能的に不要

### FNameBlockOffsetBits自動検出の順序
- 降順 (16→14) で試行すること。昇順だとblock=0のCIがどのshiftでも同じ結果になり小さいshiftを誤検出
- CI >= 65536 のフィルタが必須 (block>0の場合のみ有効な判別可能)

### UE4 RPC関数の重複
- ClientChatMessage + ClientChatMessageWithTag が同一メッセージで両方発火する
- 500ms重複排除ウィンドウ (channel + sender + message をキーに) で対処

### 偽陽性UPropertyマッチへの対処
- FName CIの偶然一致でUPropertyなどがUFunctionと誤認される
- UFunction::FunctionFlags (offset 0x98) の FUNC_Net (0x40) 検証で偽陽性を除去

### ゲーム起動中のversion.dllコピー失敗は正常
- ゲームがversion.dllをロック中のためコピーが失敗するが、chat_translator.dllは正常にコピー・リロード可能

## 既知の課題・注意事項
- EasyAntiCheat (EAC) によりBAN対象の可能性あり（自己責任）
- ゲームアップデートでオフセットが変更される可能性あり → TryDetectShiftで一部自動対応
