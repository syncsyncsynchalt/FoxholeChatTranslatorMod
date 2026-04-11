# Foxhole Chat Translator

Foxholeのゲーム内チャットを自動翻訳するDLLモジュール。

## ⚠️ 警告

- このツールはDLLインジェクションを使用します
- EasyAntiCheat (EAC) によりBANされる可能性があります
- **自己責任でご使用ください**
- テスト目的のみで使用を推奨します

## 動作原理

1. `version.dll` プロキシとしてゲームに自動ロードされる
2. UE4の `ProcessEvent` をフックしてチャットメッセージを検出
3. 検出したメッセージをテキストファイルに記録

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

### ProcessEvent アドレスの調査

パターンスキャンが失敗した場合、手動でアドレスを特定する必要があります：

1. **x64dbg** でゲームを起動
2. モジュール `War-Win64-Shipping.exe` を開く
3. 文字列検索で以下を探す：
   - `"Script Msg:"`
   - `"ProcessEvent"`
   - `"BlueprintVMError"`
4. 見つかった文字列の参照元にブレークポイントを設定
5. チャットを送信/受信してブレークポイントがヒットする関数を特定
6. その関数のアドレスを `config.ini` の `ProcessEventAddress` に設定

### GNames アドレスの調査

1. x64dbg でメモリパターンを検索
2. または文字列 `"None"` を検索し、その参照元から GNames 構造体を追跡
3. アドレスを `config.ini` の `GNamesAddress` に設定

### 探索モードの使用

`config.ini` で `DumpAllEvents=1` に設定すると、
すべての ProcessEvent 呼び出しをコンソールに出力します。

⚠️ 大量の出力が発生するため、チャット関連の関数名を特定したら
すぐに `DumpAllEvents=0` に戻してください。

## 設定ファイル (config.ini)

| セクション | キー | 説明 |
|-----------|------|------|
| General | EnableConsole | デバッグコンソールの表示 |
| General | LogFilePath | ログ出力先パス |
| General | InitDelayMs | 初期化待機時間(ms) |
| Discovery | DumpAllEvents | 全イベントログモード |
| Discovery | FunctionNameFilter | 関数名フィルタ |
| Addresses | ProcessEventAddress | 手動アドレス(16進) |
| Addresses | GNamesAddress | 手動アドレス(16進) |
| Patterns | Pattern1-10 | ProcessEventパターン |
| Patterns | GNamesPattern1-5 | GNamesパターン |

## 開発ロードマップ

- [x] **Stage 1**: チャットメッセージのテキストファイル出力
- [ ] **Stage 2**: チャット表示に接頭辞を追加
- [ ] **Stage 3**: Ollama連携による自動翻訳

## ファイル構成

```
Mods/ChatTranslator/
├── CMakeLists.txt          # ビルド構成
├── version.def             # DLLエクスポート定義
├── config.ini              # ランタイム設定
├── README.md               # このファイル
└── src/
    ├── dllmain.cpp         # エントリポイント + プロキシ
    ├── scanner.h/cpp       # パターンスキャナー
    ├── ue4.h               # UE4内部型定義
    ├── hooks.h/cpp         # ProcessEventフック + チャットログ
    └── (Stage 2/3 で追加予定)
```
