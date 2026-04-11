# Foxhole Chat Translator - Copilot Instructions

## プロジェクト概要

FoxholeのゲームチャットをリアルタイムOllama連携で自動翻訳するC++ DLLモジュール。
version.dllプロキシ方式でUE4ゲームに自動ロードされ、ProcessEventをフックしてチャットメッセージを傍受・改変する。

## アーキテクチャ

### DLLインジェクション方式
- **version.dll プロキシ**: システムの version.dll を偽装し、ゲーム起動時に自動ロード
- 17個のエクスポート関数はすべてオリジナルDLL (System32\version.dll) に転送
- `version.def` でエクスポート名を `Proxy_*` にマッピング

### UE4フック
- **ProcessEvent**: MinHookでフック。シグネチャ: `void __thiscall(void* this, void* Function, void* Parms)`
- **GNames**: FNameからの文字列解決に使用。chunked (UE4 4.23+) / flat (4.0-4.22) の両モードに対応
- **FString探索**: parms構造体内を8バイト刻み・256バイトまでスキャンし、VirtualQueryで有効性検証
- パターンスキャン失敗時は config.ini で手動アドレス指定可能

### 対象ゲーム
- Foxhole (War-Win64-Shipping.exe, ~76MB)
- UE4 4.2x系 Shippingビルド
- PhysX3/VS2015, Steamv151, Vivox, Discord RPC, CEF3 使用
- ゲームパス: Steam\steamapps\common\Foxhole\

## ファイル構成

```
Mods/ChatTranslator/
├── .github/copilot-instructions.md  # この知見ファイル
├── .gitignore
├── CMakeLists.txt          # ビルド構成 (MinHook FetchContent, /utf-8 必須)
├── version.def             # DLLエクスポート定義 (17関数)
├── config.ini              # ランタイム設定
├── README.md
├── src/
│   ├── dllmain.cpp         # エントリポイント + version.dllプロキシ + InitThread
│   ├── scanner.h/cpp       # バイトパターンスキャナー (.text セクション優先)
│   ├── ue4.h               # UE4型定義 (FName, FString, FNameEntry, UObject)
│   └── hooks.h/cpp         # ProcessEventフック + config.ini読込 + チャットログ
├── test/
│   └── test_loader.cpp     # DLL動作確認用テスト (ゲーム不要で単体検証可能)
└── build/                  # ビルド出力 (.gitignore対象)
```

## ビルド方法

```powershell
# 前提: Visual Studio 2022 + C++デスクトップ開発ワークロード
# CMakeはVS2022内蔵版を使用:
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

cd Mods/ChatTranslator
mkdir build; cd build
& $cmake .. -G "Visual Studio 17 2022" -A x64 -Wno-dev
& $cmake --build . --config Release

# → version.dll と config.ini が War/Binaries/Win64/ に自動コピーされる
# → test_loader.exe が build/Release/ に生成される
```

### ビルド注意点
- MSVC `/utf-8` フラグが必須（日本語コメント・文字列のために CMakeLists.txt で設定済み）
- MinHook は FetchContent で自動取得される（Git必要）
- `WIN32_LEAN_AND_MEAN` は CMake側で定義済み。ソースでは `#ifndef` ガード使用

## 開発ロードマップ (3段階)

### Stage 1: チャットメッセージ取得 ✅
- ProcessEventフックでチャット関連イベントを検出
- FunctionNameFilter (config.ini) で `Chat,Message,Say,Broadcast` をフィルタ
- 検出メッセージを chat_log.txt に UTF-8 で記録
- DumpAllEvents=1 で全ProcessEvent呼び出しをログ出力 (探索モード)

### Stage 2: 接頭辞追加 (未着手)
- チャット表示時にFStringを書き換えて先頭に接頭辞(★)を追加
- config.ini [Stage2] Prefix で設定可能
- HookedProcessEvent内でparms.FStringのDataポインタを新バッファに差し替える想定

### Stage 3: Ollama翻訳 (未着手)
- Ollama API (localhost:11434/api/generate) でローカルLLM翻訳
- モデル: gemma3:4b、ターゲット言語: ja
- チャット表示テキストを翻訳結果に直接置き換え
- 非同期リクエスト + キャッシュで遅延を最小化する設計が必要

## 技術詳細

### パターンスキャン
- .text セクション内のみスキャン（高速化＋誤検出防止）
- IDA形式パターン: `"48 8D 05 ?? ?? ?? ??"` (?? = ワイルドカード)
- GNames: RIP相対アドレス解決 (lea命令: offset=3, instructionSize=7)
- GNames検証: FName index 0 = "None" であることを確認

### UE4 メモリレイアウト (x64)
- UObject::NamePrivate: offset 0x18 (FName)
- UObject::ClassPrivate: offset 0x10 (UClass*)
- UObject::OuterPrivate: offset 0x20 (UObject*)
- FNameEntry::Header: 2 bytes (bit0=IsWide, bits1-15=Length)
- FNamePool chunked: Block = ComparisonIndex >> 16, Offset = ComparisonIndex & 0xFFFF

### DLL初期化フロー
1. DllMain(DLL_PROCESS_ATTACH) → LoadOriginalDll() → CreateThread(InitThread)
2. InitThread: AllocConsole → SetConsoleOutputCP(UTF8) → Sleep(10s) → hooks::Init()
3. hooks::Init: LoadConfig → MH_Initialize → FindGNames → FindAndHookProcessEvent

## テスト方法

### 単体テスト (ゲーム不要)
```powershell
cd build/Release
.\test_loader.exe
# version.dllのロード、プロキシ転送、InitThread起動を検証
# ProcessEvent/GNamesは見つからない (正常動作)
```

### ゲームテスト
1. ビルド後、Steamからゲームを起動
2. 「Foxhole Chat Translator」コンソールウィンドウが表示される
3. パターンスキャン結果とチャットログを確認

### アンインストール
```powershell
Remove-Item "path\to\Foxhole\War\Binaries\Win64\version.dll"
Remove-Item "path\to\Foxhole\War\Binaries\Win64\config.ini"
```

## 既知の課題・注意事項
- EasyAntiCheat等のアンチチートによりBAN対象の可能性あり（自己責任）
- パターンスキャンはゲームアップデートで破壊される可能性あり → config.iniで手動アドレス指定
- ゲーム実行テストは未実施（Stage 1の次のアクション）
