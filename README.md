# Foxhole Chat Translator

Foxholeのゲーム内チャットを自動翻訳するDLLモジュール。

## ⚠️ 警告

- このツールはDLLインジェクションを使用します
- EasyAntiCheat (EAC) によりBANされる可能性があります
- **自己責任でご使用ください**
- テスト目的のみで使用を推奨します

## 動作原理

1. `version.dll` プロキシとしてゲームに自動ロードされる（17関数転送）
2. .rdataセクション内の全vtableを走査し、`ProcessEvent`(vtable[66]) の全実装をMinHookでフック
3. `chat_translator.dll`（ホットリロード対応）がGNamesを検出し、チャット関連UFunctionのFName CIを照合
4. 重複排除（500ms窓）+ FUNC_Net検証で正確にチャットメッセージを判定
5. 検出したメッセージをコンソール表示 + `chat_log.txt` に記録

## ビルド方法

### 前提条件

- **CMake** 3.20以上
- **Visual Studio 2019/2022** (C++ デスクトップ開発ワークロード)
- **Git** (MinHookの自動ダウンロードに使用)

### ビルド手順

```powershell
cd "C:\Program Files (x86)\Steam\steamapps\common\Foxhole\Mods\ChatTranslator"

# ビルドディレクトリ作成
mkdir build
cd build

# CMake構成 (Visual Studio 2022の場合)
cmake .. -G "Visual Studio 17 2022" -A x64

# ビルド (Release)
cmake --build . --config Release
```

ビルド成功後、`version.dll` が自動的にゲームディレクトリ
(`War\Binaries\Win64\`) にコピーされます。

## インストール

ビルド後に自動コピーされますが、手動の場合：

```powershell
copy build\Release\version.dll "..\..\War\Binaries\Win64\version.dll"
copy config.ini "..\..\War\Binaries\Win64\config.ini"
```

## アンインストール

```powershell
del "C:\Program Files (x86)\Steam\steamapps\common\Foxhole\War\Binaries\Win64\version.dll"
```

## 使い方

### Stage 1: チャットログ収集

1. ビルドしてインストール
2. `config.ini` の設定を確認
3. ゲームを起動
4. デバッグコンソールが表示される
5. チャットメッセージが `chat_log.txt` に記録される

### トラブルシューティング

チャットが検出されない場合：
1. コンソールに `GNames: FNamePool 発見!` と表示されているか確認
2. `ProcessEvent フック: N/M 成功` の数を確認
3. `config.ini` の `ProcessEventVtableIndex` が正しいか確認 (デフォルト: 66)
4. `InitDelayMs` を増やしてUE4初期化完了を待つ時間を延長

## 設定ファイル (config.ini)

| セクション | キー | 説明 | 読み込み元 |
|-----------|------|------|----------|
| General | EnableConsole | デバッグコンソールの表示 (1/0) | hooks.cpp |
| General | LogFilePath | チャットログ出力先パス (空=DLL同階層) | hooks.cpp |
| General | InitDelayMs | UE4初期化待機時間 (ms) | dllmain.cpp |
| Addresses | ProcessEventVtableIndex | ProcessEvent vtableインデックス (デフォルト: 66) | dllmain.cpp |

> **注**: config.ini には Discovery, Patterns, Stage2, Stage3 セクションもありますが、
> 現在のコードでは読み込まれていません（将来のStage 2/3実装用プレースホルダー）。

## 開発ロードマップ

- [x] **Stage 1**: チャットメッセージのキャプチャ + テキストファイル出力
- [ ] **Stage 2**: Ollama連携による自動翻訳
- [ ] **Stage 3**: 翻訳結果のゲーム内表示

## ファイル構成

```
Mods/ChatTranslator/
├── CMakeLists.txt          # ビルド構成 (version.dll + chat_translator.dll + test_loader)
├── version.def             # DLLエクスポート定義 (17関数)
├── config.ini              # ランタイム設定
├── README.md               # このファイル
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
    ├── worker_main.cpp     # chat_translator.dll エントリポイント
    └── (Stage 2/3 で追加予定)
```
