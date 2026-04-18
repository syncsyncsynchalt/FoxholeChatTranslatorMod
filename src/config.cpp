// ============================================================
// config.cpp - 共有設定管理の実装
// ============================================================

#include "config.h"

#include <windows.h>
#include <string>

static Config g_config;

static std::string ReadIniStr(const char* configPath, const char* section, const char* key, const char* def) {
    char buf[1024];
    GetPrivateProfileStringA(section, key, def, buf, sizeof(buf), configPath);
    return buf;
}

void config::Load(const char* baseDir) {
    std::string dir = baseDir ? baseDir : "";
    std::string configPath = dir + "config.ini";

    g_config.enableConsole = GetPrivateProfileIntA("General", "EnableConsole", 1, configPath.c_str()) != 0;
    g_config.initDelayMs = GetPrivateProfileIntA("General", "InitDelayMs", 10000, configPath.c_str());

    std::string logPath = ReadIniStr(configPath.c_str(), "General", "LogFilePath", "");
    g_config.logFilePath = logPath.empty() ? (dir + "chat_log.txt") : logPath;

    g_config.dumpAllEvents = GetPrivateProfileIntA("Discovery", "DumpAllEvents", 0, configPath.c_str()) != 0;
    g_config.functionNameFilter = ReadIniStr(configPath.c_str(), "Discovery", "FunctionNameFilter", "");

    g_config.prefix = ReadIniStr(configPath.c_str(), "Stage2", "Prefix", "\xe2\x98\x85");

    g_config.translationEnabled = GetPrivateProfileIntA("Translation", "Enabled", 1, configPath.c_str()) != 0;
    g_config.ollamaEndpoint = ReadIniStr(configPath.c_str(), "Translation", "OllamaEndpoint", "http://localhost:11434/api/generate");
    g_config.targetLanguage = ReadIniStr(configPath.c_str(), "Translation", "TargetLanguage", "Japanese");
    g_config.performancePreset = ReadIniStr(configPath.c_str(), "Translation", "PerformancePreset", "Medium");

    g_config.demoMode       = GetPrivateProfileIntA("Overlay", "DemoMode", 1, configPath.c_str()) != 0;

    g_config.ttsLanguage = ReadIniStr(configPath.c_str(), "TTS", "Language", "auto");
}

const Config& config::Get() {
    return g_config;
}
