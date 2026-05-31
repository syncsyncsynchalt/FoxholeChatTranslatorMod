// ============================================================
// log.cpp - ログシステム（ダブルバッファ + バックグラウンドスレッド）
//
// ゲームスレッドは vsnprintf → mutex → push_back → return のみ。
// ファイル I/O とコンソール出力は専用ログスレッドが 100ms ごとに処理する。
// ============================================================

#include "log.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

// -------- 内部状態 --------

static std::vector<std::string> g_debugBuf, g_chatBuf, g_translBuf;
static std::mutex               g_mu;
static HANDLE                   g_thread  = nullptr;
static std::atomic<bool>        g_running{false};

static FILE*       g_debugFile  = nullptr;
static FILE*       g_chatFile   = nullptr;
static FILE*       g_translFile = nullptr;
static bool        g_console    = false;
static std::string g_baseDir;

// -------- 内部ヘルパー --------

static void OpenChatLog() {
    g_chatFile = fopen((g_baseDir + "chat_log.txt").c_str(), "a");
}

static void OpenTranslLog() {
    std::string path = g_baseDir + "translation_log.csv";
    g_translFile = fopen(path.c_str(), "a");
    if (!g_translFile) return;
    fseek(g_translFile, 0, SEEK_END);
    if (ftell(g_translFile) == 0)
        fputs("timestamp,channel,sender,original,translated\n", g_translFile);
}

// CSV フィールドをスタックバッファに書き込む。戻り値: 書いたバイト数。
static int CsvFieldInto(char* out, int cap, const char* s) {
    int n = 0;
    if (n < cap) out[n++] = '"';
    for (; *s && n < cap - 3; ++s) {
        if (*s == '"') { out[n++] = '"'; out[n++] = '"'; }
        else           out[n++] = *s;
    }
    if (n < cap) out[n++] = '"';
    if (n < cap) out[n]   = '\0';
    return n;
}

static void Drain(std::vector<std::string>& debug,
                  std::vector<std::string>& chat,
                  std::vector<std::string>& transl) {
    // swap は O(1)。mutex の保護範囲を最小化する。
    {
        std::lock_guard<std::mutex> lk(g_mu);
        debug.swap(g_debugBuf);
        chat.swap(g_chatBuf);
        transl.swap(g_translBuf);
    }

    for (auto& s : debug) {
        if (g_console)   fputs(s.c_str(), stdout);
        if (g_debugFile) fputs(s.c_str(), g_debugFile);
    }
    for (auto& s : chat) {
        if (!g_chatFile) OpenChatLog();
        if (g_chatFile)  fputs(s.c_str(), g_chatFile);
    }
    for (auto& s : transl) {
        if (!g_translFile) OpenTranslLog();
        if (g_translFile)  fputs(s.c_str(), g_translFile);
    }

    // 書き込みがあったファイルだけ 1 回フラッシュ
    if (g_console   && !debug.empty())   fflush(stdout);
    if (g_debugFile && !debug.empty())   fflush(g_debugFile);
    if (g_chatFile  && !chat.empty())    fflush(g_chatFile);
    if (g_translFile&& !transl.empty())  fflush(g_translFile);

    debug.clear(); chat.clear(); transl.clear();
}

static DWORD WINAPI LogWorker(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::vector<std::string> debug, chat, transl;
    while (g_running.load()) {
        Sleep(100);
        Drain(debug, chat, transl);
    }
    Drain(debug, chat, transl);  // シャットダウン時の残りをドレイン
    return 0;
}

// -------- 公開 API --------

void logging::Init(const char* baseDir, bool enableConsole) {
    if (g_running.load()) return;  // 二重 Init 対策

    g_baseDir = baseDir ? baseDir : "";
    g_console = enableConsole;
    g_debugFile = fopen((g_baseDir + "debug_log.txt").c_str(), "w");

    g_running.store(true);
    g_thread = CreateThread(nullptr, 0, LogWorker, nullptr, 0, nullptr);
}

void logging::Shutdown() {
    if (!g_running.exchange(false)) return;
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    // LogWorker が最後の Drain を済ませているのでここでは fclose のみ
    if (g_translFile) { fclose(g_translFile); g_translFile = nullptr; }
    if (g_chatFile)   { fclose(g_chatFile);   g_chatFile   = nullptr; }
    if (g_debugFile)  { fclose(g_debugFile);  g_debugFile  = nullptr; }
}

void logging::Debug(const char* fmt, ...) {
    char buf[600];
    int n = snprintf(buf, sizeof(buf), "[ChatTranslator] ");
    va_list a; va_start(a, fmt);
    n += vsnprintf(buf + n, (int)sizeof(buf) - n, fmt, a);
    va_end(a);
    if (n < (int)sizeof(buf) - 1) { buf[n++] = '\n'; buf[n] = '\0'; }
    std::lock_guard<std::mutex> lk(g_mu);
    g_debugBuf.emplace_back(buf, n);
}

void logging::Progress(const char* fmt, ...) {
    // インストール中のみ使用（ゲームプレイ中は呼ばれない）→ 同期書き込みで可
    char buf[256];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    printf("\r[ChatTranslator] %-70s", buf);
    fflush(stdout);
}

void logging::Chat(const char* channel, const char* sender, const char* message) {
    char ts[32], line[512];
    time_t now = time(nullptr);
    struct tm tm;
    localtime_s(&tm, &now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    int n = snprintf(line, sizeof(line), "[%s] [%s] %s: %s\n",
                     ts, channel, sender, message);
    std::lock_guard<std::mutex> lk(g_mu);
    g_chatBuf.emplace_back(line, n > 0 ? n : 0);
}

void logging::Translation(const char* channel, const char* sender,
                           const char* original, const char* translated) {
    char ts[32];
    time_t now = time(nullptr);
    struct tm tm;
    localtime_s(&tm, &now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    char buf[1024];
    int pos = 0;
    pos += CsvFieldInto(buf + pos, (int)sizeof(buf) - pos, ts);
    if (pos < (int)sizeof(buf)) buf[pos++] = ',';
    pos += CsvFieldInto(buf + pos, (int)sizeof(buf) - pos, channel);
    if (pos < (int)sizeof(buf)) buf[pos++] = ',';
    pos += CsvFieldInto(buf + pos, (int)sizeof(buf) - pos, sender);
    if (pos < (int)sizeof(buf)) buf[pos++] = ',';
    pos += CsvFieldInto(buf + pos, (int)sizeof(buf) - pos, original);
    if (pos < (int)sizeof(buf)) buf[pos++] = ',';
    pos += CsvFieldInto(buf + pos, (int)sizeof(buf) - pos, translated);
    if (pos < (int)sizeof(buf) - 1) { buf[pos++] = '\n'; buf[pos] = '\0'; }
    std::lock_guard<std::mutex> lk(g_mu);
    g_translBuf.emplace_back(buf, pos);
}
