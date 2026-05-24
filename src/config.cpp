// ============================================================
// config.cpp - 共有設定管理の実装
// ============================================================

#include "config.h"

#include <windows.h>
#include <atomic>
#include <string>

static Config g_config;
static std::atomic<int> g_translationMode{static_cast<int>(TranslationMode::JA)};
static std::atomic<int> g_ttsMode{static_cast<int>(TtsMode::Translated)};

static std::string ReadIniStr(const char* configPath, const char* section, const char* key, const char* def) {
    char buf[1024];
    GetPrivateProfileStringA(section, key, def, buf, sizeof(buf), configPath);
    return buf;
}

static TranslationMode ParseTranslationMode(const std::string& s) {
    if (s == "Off")      return TranslationMode::Off;
    if (s == "Japanese") return TranslationMode::JA;
    if (s == "Zundamon") return TranslationMode::JAZ;
    if (s == "English")  return TranslationMode::EN;
    if (s == "Russian")  return TranslationMode::RU;
    if (s == "Chinese")  return TranslationMode::ZH;
    if (s == "Korean")   return TranslationMode::KO;
    return TranslationMode::JA;
}

static TtsMode ParseTtsMode(const std::string& s) {
    if (s == "Off")        return TtsMode::Off;
    if (s == "Original")   return TtsMode::Original;
    if (s == "Translated") return TtsMode::Translated;
    return TtsMode::Translated;
}

void config::Load(const char* baseDir) {
    std::string dir = baseDir ? baseDir : "";
    std::string configPath = dir + "config.ini";

    g_config.enableConsole    = GetPrivateProfileIntA("General", "EnableConsole", 1, configPath.c_str()) != 0;
    g_config.demoMode         = GetPrivateProfileIntA("General", "DemoMode",      1, configPath.c_str()) != 0;

    g_config.ollamaEndpoint   = ReadIniStr(configPath.c_str(), "Translation", "OllamaEndpoint",   "http://localhost:11435/api/generate");
    g_config.performancePreset = ReadIniStr(configPath.c_str(), "Translation", "PerformancePreset", "Medium");

    std::string transMode = ReadIniStr(configPath.c_str(), "Translation", "Mode", "Japanese");
    g_translationMode.store(static_cast<int>(ParseTranslationMode(transMode)));

    g_config.ttsVoicevoxStyleId = static_cast<uint32_t>(
        GetPrivateProfileIntA("TTS", "VoicevoxStyleId", 3, configPath.c_str()));

    std::string ttsMode = ReadIniStr(configPath.c_str(), "TTS", "Mode", "Translated");
    g_ttsMode.store(static_cast<int>(ParseTtsMode(ttsMode)));
}

const Config& config::Get() {
    return g_config;
}

TranslationMode config::GetTranslationMode() {
    return static_cast<TranslationMode>(g_translationMode.load());
}

void config::CycleTranslationMode() {
    static const TranslationMode kCycle[] = {
        TranslationMode::Off, TranslationMode::JA, TranslationMode::JAZ,
        TranslationMode::EN,  TranslationMode::RU, TranslationMode::ZH,
        TranslationMode::KO
    };
    static const int kCount = static_cast<int>(sizeof(kCycle) / sizeof(kCycle[0]));
    int cur = g_translationMode.load();
    for (int i = 0; i < kCount; i++) {
        if (static_cast<int>(kCycle[i]) == cur) {
            g_translationMode.store(static_cast<int>(kCycle[(i + 1) % kCount]));
            return;
        }
    }
    g_translationMode.store(static_cast<int>(TranslationMode::JA));
}

TtsMode config::GetTtsMode() {
    return static_cast<TtsMode>(g_ttsMode.load());
}

void config::CycleTtsMode() {
    static const TtsMode kCycle[] = {
        TtsMode::Off, TtsMode::Original, TtsMode::Translated
    };
    static const int kCount = static_cast<int>(sizeof(kCycle) / sizeof(kCycle[0]));
    int cur = g_ttsMode.load();
    for (int i = 0; i < kCount; i++) {
        if (static_cast<int>(kCycle[i]) == cur) {
            g_ttsMode.store(static_cast<int>(kCycle[(i + 1) % kCount]));
            return;
        }
    }
    g_ttsMode.store(static_cast<int>(TtsMode::Off));
}
