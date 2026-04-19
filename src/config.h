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

    // [Stage2]
    std::string prefix = "\xe2\x98\x85"; // ★ (UTF-8)

    // [Translation]
    bool        translationEnabled = true;
    std::string ollamaEndpoint = "http://localhost:11434/api/generate";
    std::string targetLanguage = "Japanese";
    std::string performancePreset = "Medium"; // "Low" / "Medium" / "High"

    // [Overlay]
    bool        demoMode       = true;

    // [TTS]
    // "auto" = テキスト内容から自動判定
    // "ja", "en", "ru", "zh", "ko" = 強制指定
    std::string ttsLanguage    = "auto";
};

namespace config {

// config.ini を読み込んで Config を構築
// baseDir: DLLのあるディレクトリ (末尾 '\\' 付き)
void Load(const char* baseDir);

// 現在のグローバル設定を取得
const Config& Get();

} // namespace config
