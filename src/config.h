#pragma once
// ============================================================
// config.h - 共有設定管理
// config.ini から読み込んだ設定値を全モジュールに提供
// ============================================================

#include <string>

struct Config {
    // [General]
    bool        enableConsole = true;
    std::string logFilePath;
    int         initDelayMs = 10000;

    // [Discovery]
    bool        dumpAllEvents = false;
    std::string functionNameFilter;

    // [Stage2]
    std::string prefix = "\xe2\x98\x85"; // ★ (UTF-8)

    // [Translation]
    bool        translationEnabled = true;
    std::string ollamaEndpoint = "http://localhost:11434/api/generate";
    std::string ollamaModel    = "gemma3:4b";
    std::string targetLanguage = "Japanese";
};

namespace config {

// config.ini を読み込んで Config を構築
// baseDir: DLLのあるディレクトリ (末尾 '\\' 付き)
void Load(const char* baseDir);

// 現在のグローバル設定を取得
const Config& Get();

} // namespace config
