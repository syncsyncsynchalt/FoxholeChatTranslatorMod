#pragma once
// ============================================================
// log.h - 共有ログシステム
// デバッグログとチャットログの書き出しを一元管理
// ============================================================

#include <cstdarg>

namespace logging {

// 初期化 (ログファイルのディレクトリを設定)
// baseDir: 末尾に '\\' を含むディレクトリパス
void Init(const char* baseDir, bool enableConsole);

// シャットダウン (ファイルハンドルを閉じる)
void Shutdown();

// デバッグログ出力 (debug_log.txt + コンソール)
void Debug(const char* fmt, ...);

// チャットログ出力 (chat_log.txt)
// logFilePath が空の場合は baseDir/chat_log.txt に出力
void Chat(const char* channel, const char* sender, const char* message);

// チャットログの出力先パスを設定
void SetChatLogPath(const char* path);

} // namespace logging
