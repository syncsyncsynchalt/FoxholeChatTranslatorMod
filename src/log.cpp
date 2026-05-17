// ============================================================
// log.cpp - 共有ログシステム実装
// ============================================================

#include "log.h"

#include <windows.h>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

static std::mutex g_logMutex;
static FILE*      g_debugLogFile       = nullptr;
static FILE*      g_chatLogFile        = nullptr;
static FILE*      g_translationLogFile = nullptr;
static bool       g_enableConsole = true;
static std::string g_baseDir;
static std::string g_chatLogPath;

void logging::Init(const char* baseDir, bool enableConsole) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_baseDir = baseDir ? baseDir : "";
    g_enableConsole = enableConsole;
    g_chatLogPath = g_baseDir + "chat_log.txt";

    if (!g_debugLogFile) {
        std::string debugPath = g_baseDir + "debug_log.txt";
        g_debugLogFile = fopen(debugPath.c_str(), "w");
    }
}

void logging::Shutdown() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_translationLogFile) { fclose(g_translationLogFile); g_translationLogFile = nullptr; }
    if (g_chatLogFile)        { fclose(g_chatLogFile);        g_chatLogFile        = nullptr; }
    if (g_debugLogFile)       { fclose(g_debugLogFile);       g_debugLogFile       = nullptr; }
}

void logging::SetChatLogPath(const char* path) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (path && path[0]) {
        g_chatLogPath = path;
    }
}

void logging::Debug(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_logMutex);

    if (g_enableConsole) {
        printf("[ChatTranslator] %s\n", buf);
    }

    if (g_debugLogFile) {
        fprintf(g_debugLogFile, "[ChatTranslator] %s\n", buf);
        fflush(g_debugLogFile);
    }
}

void logging::Chat(const char* channel, const char* sender, const char* message) {
    std::lock_guard<std::mutex> lock(g_logMutex);

    if (!g_chatLogFile) {
        g_chatLogFile = fopen(g_chatLogPath.c_str(), "a");
        if (!g_chatLogFile) return;
    }

    time_t now = time(nullptr);
    struct tm local;
    localtime_s(&local, &now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);

    fprintf(g_chatLogFile, "[%s] [%s] %s: %s\n", timestamp, channel, sender, message);
    fflush(g_chatLogFile);
}

// CSV フィールドをダブルクォートで囲み、内部の " を "" にエスケープする
static std::string CsvField(const char* s) {
    std::string out = "\"";
    for (; *s; ++s) {
        if (*s == '"') out += "\"\"";
        else           out += *s;
    }
    out += '"';
    return out;
}

void logging::Translation(const char* channel, const char* sender,
                          const char* original, const char* translated) {
    std::lock_guard<std::mutex> lock(g_logMutex);

    if (!g_translationLogFile) {
        std::string path = g_baseDir + "translation_log.csv";
        g_translationLogFile = fopen(path.c_str(), "a");
        if (!g_translationLogFile) return;
        // ファイルが空のときだけヘッダを書く
        fseek(g_translationLogFile, 0, SEEK_END);
        if (ftell(g_translationLogFile) == 0) {
            fprintf(g_translationLogFile, "timestamp,channel,sender,original,translated\n");
        }
    }

    time_t now = time(nullptr);
    struct tm local;
    localtime_s(&local, &now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);

    fprintf(g_translationLogFile, "%s,%s,%s,%s,%s\n",
        CsvField(timestamp).c_str(),
        CsvField(channel).c_str(),
        CsvField(sender).c_str(),
        CsvField(original).c_str(),
        CsvField(translated).c_str());
    fflush(g_translationLogFile);
}
