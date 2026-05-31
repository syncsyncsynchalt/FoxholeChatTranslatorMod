#pragma once
// ============================================================
// log.h - ログシステム（ダブルバッファ + バックグラウンドスレッド）
// ゲームスレッドはキューに積むだけ。I/O は専用スレッドが担う。
// ============================================================

#include <cstdarg>

namespace logging {

// 初期化。ログスレッドを起動する。
// baseDir: 末尾 '\\' 付きディレクトリパス
void Init(const char* baseDir, bool enableConsole);

// シャットダウン。ログスレッドを停止しキューの残りをドレインする。
void Shutdown();

// デバッグログ (debug_log.txt + コンソール)。非ブロッキング。
void Debug(const char* fmt, ...);

// 進捗表示 (コンソール \r インプレースのみ)。インストール専用。
void Progress(const char* fmt, ...);

// チャットログ (chat_log.txt)。非ブロッキング。
void Chat(const char* channel, const char* sender, const char* message);

// 翻訳ペアログ (translation_log.csv)。非ブロッキング。
void Translation(const char* channel, const char* sender,
                 const char* original, const char* translated);

} // namespace logging
