# Foxhole Chat Translator - Copilot Instructions

## Role

C++、Windows、バイナリリバースエンジニアリング、UE4内部構造（4.24）、MinHook、DX11/ImGui、
Ollama API（WinHTTP）、WinRT OneCore TTS + XAudio2 の専門家として開発支援を行うこと。
日本語で応答すること。

## プロジェクト概要

Foxhole (UE4 4.24.3) のゲームチャットを傍受し、ローカルAI翻訳＋オーバーレイ表示＋多言語TTS読み上げを行うC++ DLLモジュール。
version.dllプロキシ方式でゲームに自動ロードされ、ProcessEvent (vtable[66]) をフックしてチャットメッセージを傍受する。
Stage 1-9 全完了。基本機能は実装済み。

### 2-DLL構成
- **version.dll** (永続DLL): DLLプロキシ + .rdata vtableスキャン + ProcessEventフック (MinHook, 最大64枠) + DX11 Presentフック + chat_translator.dll管理 (ホットリロード/自動リロード/整合性チェック)
- **chat_translator.dll** (ホットリロード可能ワーカー): GNames自動検出 + FName CI照合 + チャットキャプチャ + Ollama翻訳 + ImGuiオーバーレイ + WinRT OneCore TTS

### 対象ゲーム
- Foxhole (War-Win64-Shipping.exe), UE4 4.24.3, ゲームパス: `C:\Program Files (x86)\Steam\steamapps\common\Foxhole\`

## ビルド

```powershell
cd "C:\Program Files (x86)\Steam\steamapps\common\Foxhole\Mods\ChatTranslator"
cmake -B build -G "Visual Studio 17 2022" -A x64              # 初回構成
cmake --build build --config Release                          # フルビルド
cmake --build build --config Release --target chat_translator  # ワーカーのみ (F9でホットリロード)
```

- MinHook, Dear ImGui v1.91.6 は FetchContent で自動取得
- `WIN32_LEAN_AND_MEAN` はCMake側で定義済み。ソースでは `#ifndef` ガード使用
- 成果物は War/Binaries/Win64/ に自動コピー

## Coding Conventions

### 命名規則
- 関数: `PascalCase` (`Init()`, `OnProcessEvent()`, `FindBundledOllama()`)
- ローカル変数: `camelCase` (`hostPort`, `slashPos`)
- グローバル/static変数: `g_` prefix + `camelCase` (`g_hSession`, `g_mutex`)
- namespace: `snake_case` (`hooks`, `config`, `logging`, `translate`, `gnames`, `overlay`, `tts`)
- 構造体/enum: `PascalCase` (`Config`, `ChatMessage`, `RadioState`)
- 定数/マクロ: `ALL_CAPS` (`MAX_PE_HOOKS`, `MAX_QUEUE_SIZE`)
- UE4由来の定数名はUE4の命名をそのまま使用する (`FUNC_Net`, `EObjectFlags` 等、正規化しない)

### スタイルルール
- C++17 (`CMAKE_CXX_STANDARD 17`)
- `#pragma once` (全ヘッダ統一)
- インクルード順序: `<windows.h>` → C標準 (`<cstdio>`) → C++標準 (`<string>`, `<mutex>`) → 外部 (`<MinHook.h>`, `"imgui.h"`) → プロジェクト内 (`"log.h"`)
- C++ 例外は使用しない。エラーは `bool` 返却 + `logging::Debug()` で報告
- UE4メモリ読み取りには `__try/__except` (SEH) でクラッシュ保護
- スレッド同期: `std::mutex` + `std::lock_guard` / `std::unique_lock`。フラグは `std::atomic<bool>`
- 内部文字列は `std::string` (UTF-8)。UE4 `FString` は即座に `FStringToUtf8()` で変換
- JSON: 手書き (`JsonEscape()`, `ExtractResponse()`)。外部ライブラリ不使用
- ファイルI/O: `fopen`/`fprintf`/`fflush` (iostream未使用)
- HTTP通信: `WinHTTP` (libcurl等の外部ライブラリ不使用)
- INI読込: `GetPrivateProfileStringA` (自前パーサー不使用)
- TTS: WinRT COM API (`RoInitialize`, `ComPtr<>`, `HStringReference`)
- モジュール構成: namespace + free function + static 内部状態 (クラスベースではない)
- 内部関数/変数は `static` (無名namespace未使用)
- コメント・ログ文字列は日本語

## Rules

### 絶対禁止
- ゲームクライアントのメモリを**書き換えるコードを生成してはならない**（読み取りのみ）
- `DllMain` 内で `LoadLibrary` 等のローダーロック違反操作を行わない
- `WIN32_LEAN_AND_MEAN` / `NOMINMAX` をソースで再定義しない（CMake定義済み）
- 勝手にコミットしない、プッシュしない、PRを作成しない。コード生成が必要な場合は必ずユーザーに確認すること。

### 必須ルール
- UE4構造体のフィールドオフセットは Dumper-7 SDK 準拠とする (SDK: `War_parameters.hpp`, 出力先: `C:\Dumper-7\4.24.3-0+++UE4+Release-4.24-War\CppSDK\`)
- MinHookフックは `MH_EnableHook` / `MH_DisableHook` でペア管理する
- ProcessEvent コールバック内は最小限の処理のみ。重い処理は別スレッドに委譲
- 翻訳処理は描画スレッドをブロックしない（非同期実行）

## Key Design Decisions

- **vtable先頭境界検出**: `addr[-1]` がモジュール内コードを指さない位置を検出しないとスライディングウィンドウで大量の偽陽性が出る。vtable候補は `[0],[1],[2]` がモジュール内コードを指すか確認
- **GNames検出**: FNamePool Block[0]をヒープメモリ走査で検出し、モジュール内逆引きでBlocks[]配列を特定。FNameBlockOffsetBits は 16 にハードコード、TryDetectShift() で自動補正
- **チャットキャプチャ**: 監視対象は ClientChatMessage / ClientChatMessageWithTag / ClientWorldChatMessage のみ (ServerChat系は除外)。判定は FName ComparisonIndex一致 + FUNC_Net (0x40) フラグ検証。重複排除は sender+message+channel, 500ms窓

## Known Pitfalls

- EasyAntiCheat (EAC) によりBAN対象の可能性あり（自己責任）
- ゲームアップデートでオフセット変更の可能性あり → TryDetectShift で一部自動対応
- Ollama がダウンすると FAULT 状態 → 原文のみ表示 + TTS再生、アイコンクリックで Ollama 再起動試行
- UE4オフセット・構造体レイアウト・parms解析の詳細は `docs/ue4-reversing-foxhole.md` を参照
- 初期化フロー・パイプライン・config.ini 詳細は `docs/project-notes.md` を参照
