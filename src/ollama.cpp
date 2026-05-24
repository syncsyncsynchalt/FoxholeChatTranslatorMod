// ============================================================
// ollama.cpp - Ollama プロセス管理・ヘルス監視
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winhttp.h>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>

#include "ollama.h"
#include "translate.h"
#include "log.h"

// ============================================================
// 内部状態
// ============================================================

static bool              g_managed         = false; // エンドポイントが localhost 系なら自動管理
static std::string       g_ollamaDir;               // 同梱 ollama.exe のディレクトリ
static HANDLE            g_ollamaProcess   = nullptr;
static HANDLE            g_ollamaJobObject = nullptr;
static INTERNET_PORT     g_ollamaPort      = 11435; // 同梱 Ollama がバインドするポート

static std::atomic<RadioState> g_radioState{RadioState::ON};
static std::atomic<bool>       g_userEnabled{true};
static std::atomic<bool>       g_running{false};
static std::atomic<bool>       g_restartRequested{false};

static std::thread             g_healthThread;
static std::mutex              g_healthMutex;
static std::condition_variable g_healthCv;

static std::atomic<bool>       g_installRunning{false};
static std::atomic<bool>       g_installCancel{false};
static std::thread             g_installThread;

// ============================================================
// ユーティリティ
// ============================================================

static INTERNET_PORT ParsePortFromEndpoint(const std::string& endpoint) {
    std::string s = endpoint;
    if (s.size() > 7 && s.substr(0, 7) == "http://") s = s.substr(7);
    size_t colonPos = s.find(':');
    if (colonPos == std::string::npos) return 80;
    size_t slashPos = s.find('/', colonPos);
    std::string portStr = (slashPos != std::string::npos)
        ? s.substr(colonPos + 1, slashPos - colonPos - 1)
        : s.substr(colonPos + 1);
    try { return static_cast<INTERNET_PORT>(std::stoi(portStr)); }
    catch (...) { return 80; }
}

static bool IsLocalEndpoint(const std::string& endpoint) {
    return endpoint.find("localhost") != std::string::npos
        || endpoint.find("127.0.0.1") != std::string::npos
        || endpoint.find("::1")       != std::string::npos;
}

// DLL のベースディレクトリを取得
static std::string GetDllBaseDir() {
    char dllPath[MAX_PATH];
    HMODULE hSelf;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&GetDllBaseDir), &hSelf);
    GetModuleFileNameA(hSelf, dllPath, MAX_PATH);
    std::string dir(dllPath);
    size_t pos = dir.rfind('\\');
    if (pos != std::string::npos) dir = dir.substr(0, pos + 1);
    return dir;
}

// 同梱 ollama.exe のパスを探す
static std::string FindBundledOllama() {
    if (!g_ollamaDir.empty()) {
        std::string exe = g_ollamaDir + "\\ollama.exe";
        if (GetFileAttributesA(exe.c_str()) != INVALID_FILE_ATTRIBUTES) return exe;
    }
    std::string baseDir = GetDllBaseDir();
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);
    std::string cwdStr = cwd;
    if (!cwdStr.empty() && cwdStr.back() != '\\') cwdStr += '\\';

    // Mod 固有パスのみ検索。システムインストール (LOCALAPPDATA/ProgramFiles) は除外して
    // 環境にインストール済みの Ollama と衝突しないようにする。
    std::string candidates[] = {
        baseDir + "tools\\ollama\\ollama.exe",
        baseDir + "..\\..\\Mods\\ChatTranslator\\tools\\ollama\\ollama.exe",
        cwdStr  + "tools\\ollama\\ollama.exe",
    };
    for (auto& path : candidates) {
        if (!path.empty() && GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            return path;
    }
    return "";
}

// ollama serve を起動し、Job Object に割り当てる
static bool StartProcess(const std::string& exePath) {
    std::string dir = exePath.substr(0, exePath.rfind('\\'));
    std::string modelsDir = dir + "\\models";
    CreateDirectoryA(modelsDir.c_str(), nullptr);
    SetEnvironmentVariableA("OLLAMA_MODELS", modelsDir.c_str());

    // OLLAMA_HOST を設定してシステム Ollama と異なるポートにバインドさせる
    char ollamaHost[64];
    snprintf(ollamaHost, sizeof(ollamaHost), "127.0.0.1:%u", static_cast<unsigned>(g_ollamaPort));
    SetEnvironmentVariableA("OLLAMA_HOST", ollamaHost);
    logging::Debug("[Ollama] OLLAMA_HOST=%s", ollamaHost);

    // Job Object を作成 (初回のみ): KILL_ON_JOB_CLOSE でゲーム終了時に自動終了
    if (!g_ollamaJobObject) {
        g_ollamaJobObject = CreateJobObject(nullptr, nullptr);
        if (g_ollamaJobObject) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(g_ollamaJobObject,
                JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
    }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::string cmdLine = "\"" + exePath + "\" serve";

    // CREATE_BREAKAWAY_FROM_JOB: Steam の Job Object から切り離し、Steam フリーズを防ぐ
    // Job Object が breakaway を禁止している場合 (ERROR_ACCESS_DENIED) はフラグなしで再試行
    BOOL ok = CreateProcessA(nullptr, &cmdLine[0], nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB,
                             nullptr, dir.c_str(), &si, &pi);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
        ok = CreateProcessA(nullptr, &cmdLine[0], nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi);
    }
    if (!ok) {
        logging::Debug("[Ollama] ollama serve 起動失敗: %u", GetLastError());
        return false;
    }

    CloseHandle(pi.hThread);

    // Job Object に割り当て: 当 Job Object のハンドルを閉じると OS がプロセスを終了
    if (g_ollamaJobObject) {
        AssignProcessToJobObject(g_ollamaJobObject, pi.hProcess);
    }

    if (g_ollamaProcess) {
        CloseHandle(g_ollamaProcess);
    }
    g_ollamaProcess = pi.hProcess;

    logging::Debug("[Ollama] ollama serve 起動 (PID=%u, exe=%s)", pi.dwProcessId, exePath.c_str());
    logging::Debug("[Ollama] OLLAMA_MODELS=%s", modelsDir.c_str());

    // サーバー起動を待機 (最大30秒)
    for (int i = 0; i < 60; i++) {
        Sleep(500);
        if (translate::IsHealthy()) {
            logging::Debug("[Ollama] ollama serve 応答確認 (%d ms)", (i + 1) * 500);
            return true;
        }
        // 5秒ごとに待機状況をログ
        if ((i + 1) % 10 == 0) {
            logging::Debug("[Ollama] ollama serve 起動待機中... (%d秒)", (i + 1) / 2);
        }
    }
    logging::Debug("[Ollama] ollama serve 応答タイムアウト (30秒)");
    return false;
}

static void InstallOllamaWorker(); // 前方宣言

// Ollama 起動確認 + モデル確認/DL
static bool EnsureRunning() {
    // 自分が起動したプロセスが生きていればヘルスチェックのみ
    // (システム Ollama が同ポートで動作していても使用しない)
    if (g_ollamaProcess != nullptr) {
        DWORD code = STILL_ACTIVE;
        GetExitCodeProcess(g_ollamaProcess, &code);
        if (code == STILL_ACTIVE && translate::IsHealthy()) {
            return translate::EnsureModel();
        }
    }
    // 同梱 Ollama のみ使用 (システムインストールは無視)
    std::string exe = FindBundledOllama();
    if (exe.empty()) {
        // バイナリ未検出: バックグラウンドインストールを開始
        if (!g_installRunning.exchange(true)) {
            g_installCancel.store(false);
            if (g_installThread.joinable()) g_installThread.join();
            g_installThread = std::thread(InstallOllamaWorker);
            logging::Debug("[Ollama] インストールをバックグラウンドで開始");
        }
        return false;
    }
    if (!StartProcess(exe)) return false;
    return translate::EnsureModel();
}

// ============================================================
// インストーラー - WinHTTP ユーティリティ
// ============================================================

static std::string OllamaHttpGet(const char* urlA) {
    wchar_t urlW[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, urlA, -1, urlW, 2048)) return "";

    URL_COMPONENTSW uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = 2048;
    if (!WinHttpCrackUrl(urlW, 0, 0, &uc)) return "";

    HINTERNET sess = WinHttpOpen(L"FoxholeChatTranslator/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess) return "";

    HINTERNET conn = WinHttpConnect(sess, host, uc.nPort, 0);
    if (!conn) { WinHttpCloseHandle(sess); return ""; }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(sess); return ""; }

    WinHttpSetTimeouts(req, 10000, 30000, 30000, 60000);
    WinHttpAddRequestHeaders(req, L"User-Agent: FoxholeChatTranslator-Setup",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    std::string result;
    if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr))
    {
        char buf[8192];
        DWORD avail = 0, read = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
            DWORD toRead = (avail < sizeof(buf)) ? avail : (DWORD)sizeof(buf);
            if (!WinHttpReadData(req, buf, toRead, &read) || read == 0) break;
            result.append(buf, read);
        }
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(sess);
    return result;
}

// label が非 null の場合、コンソールにダウンロード進捗を表示する
static bool OllamaHttpDownload(const char* urlA, const char* destPath,
                                const char* label = nullptr) {
    wchar_t urlW[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, urlA, -1, urlW, 2048)) return false;

    URL_COMPONENTSW uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = 2048;
    if (!WinHttpCrackUrl(urlW, 0, 0, &uc)) return false;

    HINTERNET sess = WinHttpOpen(L"FoxholeChatTranslator/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess) return false;

    HINTERNET conn = WinHttpConnect(sess, host, uc.nPort, 0);
    if (!conn) { WinHttpCloseHandle(sess); return false; }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(sess); return false; }

    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));
    WinHttpSetTimeouts(req, 10000, 30000, 60000, 600000);
    WinHttpAddRequestHeaders(req, L"User-Agent: FoxholeChatTranslator-Setup",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    bool ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);

    if (ok) {
        DWORD status = 0, slen = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            nullptr, &status, &slen, nullptr);
        ok = (status == 200);
    }

    // Content-Length を取得して進捗計算に使用
    size_t totalSize = 0;
    if (ok && label) {
        DWORD cl = 0, clSize = sizeof(cl);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                nullptr, &cl, &clSize, nullptr)) {
            totalSize = cl;
        }
    }

    FILE* f = nullptr;
    if (ok) { f = fopen(destPath, "wb"); ok = (f != nullptr); }

    if (ok) {
        char buf[65536];
        DWORD avail = 0, read = 0;
        size_t downloaded = 0;
        int lastPct    = -1; // コンソール更新 (1% ごと)
        int lastLogPct = -1; // ファイル記録 (10% ごと)
        while (ok && !g_installCancel.load()) {
            avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; break; }
            if (avail == 0) break;
            DWORD toRead = (avail < sizeof(buf)) ? avail : (DWORD)sizeof(buf);
            if (!WinHttpReadData(req, buf, toRead, &read)) { ok = false; break; }
            if (read > 0) {
                fwrite(buf, 1, read, f);
                downloaded += read;
                if (label) {
                    int pct = (totalSize > 0)
                        ? static_cast<int>(100.0 * downloaded / totalSize)
                        : -1;
                    // コンソール: 1% ごとに同一行を上書き
                    if (pct != lastPct) {
                        lastPct = pct;
                        if (totalSize > 0) {
                            logging::Progress("[Ollama-DL] %s: %.1f / %.1f MB (%d%%)",
                                label,
                                downloaded / 1048576.0,
                                totalSize  / 1048576.0,
                                pct);
                        } else {
                            logging::Progress("[Ollama-DL] %s: %.1f MB",
                                label, downloaded / 1048576.0);
                        }
                    }
                    // ファイル: 10% ごとにチェックポイントを記録
                    int logStep = (pct >= 0) ? (pct / 10) : -1;
                    if (logStep != lastLogPct) {
                        lastLogPct = logStep;
                        if (totalSize > 0) {
                            logging::Debug("[Ollama-DL] %s: %.1f / %.1f MB (%d%%)",
                                label,
                                downloaded / 1048576.0,
                                totalSize  / 1048576.0,
                                pct);
                        } else {
                            logging::Debug("[Ollama-DL] %s: %.1f MB",
                                label, downloaded / 1048576.0);
                        }
                    }
                }
            }
        }
        if (g_installCancel.load()) ok = false;
        if (label && ok) {
            // \r 行を確定して改行
            logging::Debug("[Ollama-DL] %s: 完了 (%.1f MB)", label, downloaded / 1048576.0);
        }
    }

    if (f) fclose(f);
    if (!ok) DeleteFileA(destPath);
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(sess);
    return ok;
}

// ============================================================
// インストーラー - GitHub API
// ============================================================

struct OllamaGHAsset { std::string name, url; };

static std::string OllamaJStr(const std::string& json, size_t from, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k, from);
    if (p == std::string::npos) return "";
    p = json.find('"', p + k.size());
    if (p == std::string::npos || json[p] != '"') return "";
    p++;
    std::string v;
    while (p < json.size() && json[p] != '"') {
        if (json[p] == '\\') { p++; if (p < json.size()) v += json[p++]; }
        else v += json[p++];
    }
    return v;
}

static std::vector<OllamaGHAsset> OllamaParseAssets(const std::string& json) {
    std::vector<OllamaGHAsset> assets;
    size_t p = json.find("\"assets\"");
    if (p == std::string::npos) return assets;
    p = json.find('[', p);
    if (p == std::string::npos) return assets;

    int depth = 0;
    size_t objStart = std::string::npos;
    for (p++; p < json.size(); p++) {
        char c = json[p];
        if (c == '{') {
            if (depth == 0) objStart = p;
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0 && objStart != std::string::npos) {
                std::string obj = json.substr(objStart, p - objStart + 1);
                OllamaGHAsset a;
                a.name = OllamaJStr(obj, 0, "name");
                a.url  = OllamaJStr(obj, 0, "browser_download_url");
                if (!a.name.empty() && !a.url.empty()) assets.push_back(std::move(a));
                objStart = std::string::npos;
            }
        } else if (c == ']' && depth == 0) {
            break;
        } else if (c == '"') {
            for (p++; p < json.size() && json[p] != '"'; p++)
                if (json[p] == '\\') p++;
        }
    }
    return assets;
}

// ollama/ollama latest release からフィルター名のアセット URL を取得
static bool FindOllamaAsset(const char* nameFilter, std::string& outUrl) {
    std::string body = OllamaHttpGet(
        "https://api.github.com/repos/ollama/ollama/releases/latest");
    if (body.empty()) {
        logging::Debug("[Ollama-Install] GitHub API 取得失敗");
        return false;
    }
    auto assets = OllamaParseAssets(body);
    for (auto& a : assets) {
        if (a.name.find(nameFilter) != std::string::npos) {
            outUrl = a.url;
            return true;
        }
    }
    logging::Debug("[Ollama-Install] アセット未検出: %s", nameFilter);
    return false;
}

// ============================================================
// インストーラー - ファイルユーティリティ
// ============================================================

static std::string OllamaTempDir() {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string dir = std::string(tmp) + "ollama_inst_" + std::to_string(GetTickCount());
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

static void OllamaRmDirR(const std::string& dir) {
    std::string pattern = dir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        std::string p = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) OllamaRmDirR(p);
        else DeleteFileA(p.c_str());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    RemoveDirectoryA(dir.c_str());
}

// コマンドを実行し stdout+stderr の各行に対してコールバックを呼ぶ。戻り値: 終了コード (-1=起動失敗)
static int RunPiped(const std::string& cmd,
                    void (*onLine)(const char*, void*), void* ctx) {
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return -1;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0); // 読み取り端は子に継承しない

    STARTUPINFOA si = {};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput  = hWrite;
    si.hStdError   = hWrite;
    si.hStdInput   = INVALID_HANDLE_VALUE;

    PROCESS_INFORMATION pi = {};
    std::string cmdCopy = cmd;
    BOOL ok = CreateProcessA(nullptr, &cmdCopy[0], nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite); // 書き込み端は親では不要。子終了でパイプが閉じる
    if (!ok) { CloseHandle(hRead); return -1; }
    CloseHandle(pi.hThread);

    char rawBuf[8192];
    char lineBuf[4096] = {};
    int  lineLen = 0;
    DWORD bytesRead = 0;
    while (ReadFile(hRead, rawBuf, sizeof(rawBuf), &bytesRead, nullptr) && bytesRead > 0) {
        for (DWORD i = 0; i < bytesRead; i++) {
            char c = rawBuf[i];
            if (c == '\n' || c == '\r') {
                if (lineLen > 0) {
                    lineBuf[lineLen] = '\0';
                    if (onLine) onLine(lineBuf, ctx);
                    lineLen = 0;
                }
            } else if (lineLen < static_cast<int>(sizeof(lineBuf)) - 1) {
                lineBuf[lineLen++] = c;
            }
        }
    }
    if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        if (onLine) onLine(lineBuf, ctx);
    }

    CloseHandle(hRead);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

static bool OllamaExtract(const std::string& archive, const std::string& destDir) {
    CreateDirectoryA(destDir.c_str(), nullptr);
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    std::string tarExe = std::string(sys) + "\\tar.exe";

    // ファイル数を事前カウント (tar -tf): 展開割合を計算するために使用
    std::string listCmd = "\"" + tarExe + "\" -tf \"" + archive + "\"";
    int totalFiles = 0;
    RunPiped(listCmd,
        [](const char*, void* ctx) { (*static_cast<int*>(ctx))++; },
        &totalFiles);
    logging::Debug("[Ollama-展開] %d ファイル検出、展開開始: %s", totalFiles, destDir.c_str());

    // 展開しながら進捗表示 (tar -xvf で1ファイルごとに1行出力)
    struct ExtCtx { int total, extracted, lastLogStep; };
    ExtCtx ectx = { totalFiles, 0, -1 };
    std::string extractCmd = "\"" + tarExe + "\" -xvf \"" + archive + "\" -C \"" + destDir + "\"";
    int code = RunPiped(extractCmd,
        [](const char*, void* ctx) {
            auto* e = static_cast<ExtCtx*>(ctx);
            e->extracted++;
            if (e->total > 0) {
                int pct = 100 * e->extracted / e->total;
                logging::Progress("[Ollama-展開] %d / %d ファイル (%d%%)",
                    e->extracted, e->total, pct);
                // ファイル: 25% ごとにチェックポイントを記録
                int logStep = pct / 25;
                if (logStep != e->lastLogStep) {
                    e->lastLogStep = logStep;
                    logging::Debug("[Ollama-展開] %d / %d ファイル (%d%%)",
                        e->extracted, e->total, pct);
                }
            } else {
                logging::Progress("[Ollama-展開] %d ファイル展開済み", e->extracted);
            }
        },
        &ectx);

    if (code == 0)
        logging::Debug("[Ollama-展開] 完了 (%d ファイル)", ectx.extracted);
    else
        logging::Debug("[Ollama-展開] 失敗 (終了コード: %d)", code);
    return code == 0;
}

// ============================================================
// インストーラー - ワーカー
// ============================================================

static void InstallOllamaWorker() {
    logging::Debug("[Ollama-Install] バックグラウンドインストール開始");
    std::string tmpDir  = OllamaTempDir();
    std::string baseDir = GetDllBaseDir();
    bool installed = false;

    // 方法1: portable ZIP を tools\ollama\ に展開
    std::string zipUrl;
    if (!g_installCancel.load() &&
        FindOllamaAsset("ollama-windows-amd64.zip", zipUrl))
    {
        logging::Debug("[Ollama-Install] ZIP: %s", zipUrl.c_str());
        logging::Debug("[Ollama-Install] ステップ 1/2: バイナリをダウンロード中...");
        std::string zipPath = tmpDir + "\\ollama.zip";
        if (!g_installCancel.load() &&
            OllamaHttpDownload(zipUrl.c_str(), zipPath.c_str(), "Ollama ZIP"))
        {
            logging::Debug("[Ollama-Install] ステップ 2/2: 展開中...");
            std::string destDir = baseDir + "tools\\ollama";
            installed = OllamaExtract(zipPath, destDir);
            if (!installed)
                logging::Debug("[Ollama-Install] ZIP 展開失敗");
        } else {
            logging::Debug("[Ollama-Install] ZIP ダウンロード失敗");
        }
    }

    OllamaRmDirR(tmpDir);

    if (!g_installCancel.load()) {
        if (installed) {
            logging::Debug("[Ollama-Install] インストール完了 - 再起動要求");
            g_restartRequested.store(true);
            g_healthCv.notify_one();
        } else {
            logging::Debug("[Ollama-Install] インストール失敗");
            g_radioState.store(RadioState::FAULT);
        }
    }
    g_installRunning.store(false);
}

// ============================================================
// ヘルスワーカー
// ============================================================

static void HealthWorker() {
    logging::Debug("[Ollama] ヘルスワーカー開始");
    while (g_running.load()) {
        {
            std::unique_lock<std::mutex> lock(g_healthMutex);
            g_healthCv.wait_for(lock, std::chrono::seconds(5), [] {
                return g_restartRequested.load() || !g_running.load();
            });
        }
        if (!g_running.load()) break;

        if (g_restartRequested.exchange(false)) {
            if (g_managed) {
                g_radioState.store(RadioState::RESTARTING);
                bool ok = EnsureRunning();
                g_radioState.store(ok ? RadioState::ON : RadioState::FAULT);
                logging::Debug("[Ollama] 再起動 %s", ok ? "成功" : "失敗");
                if (ok) {
                    logging::Debug("[Ollama] 翻訳システム準備完了 - チャットメッセージを翻訳します");
                }
            }
            continue;
        }

        // 定期ヘルスチェック
        if (!g_userEnabled.load()) continue;
        bool ok = translate::IsHealthy();
        RadioState cur = g_radioState.load();
        if (ok) {
            if (cur == RadioState::FAULT || cur == RadioState::OFF)
                g_radioState.store(RadioState::ON);
        } else if (cur == RadioState::ON) {
            g_radioState.store(g_managed ? RadioState::FAULT : RadioState::OFF);
            logging::Debug("[Ollama] ダウン検出 (%s)", g_managed ? "FAULT" : "OFF");
        }
    }
    logging::Debug("[Ollama] ヘルスワーカー終了");
}

// ============================================================
// 公開 API
// ============================================================

void ollama::Init(const std::string& ollamaDir, const std::string& endpoint) {
    // Shutdown を経ずに再 Init された場合の防衛
    if (g_healthThread.joinable()) {
        g_running.store(false);
        g_healthCv.notify_all();
        g_healthThread.join();
    }

    g_ollamaDir   = ollamaDir;
    g_managed     = IsLocalEndpoint(endpoint);
    g_ollamaPort  = ParsePortFromEndpoint(endpoint);

    if (g_managed) {
        logging::Debug("[Ollama] ローカルエンドポイント: 自動管理モード");
        if (!EnsureRunning()) {
            logging::Debug("[Ollama] 初期化失敗 - FAULT 状態で続行");
            g_radioState.store(RadioState::FAULT);
        }
    } else {
        logging::Debug("[Ollama] リモートエンドポイント: 接続のみモード");
    }

    g_running.store(true);
    g_healthThread = std::thread(HealthWorker);
}

void ollama::Shutdown() {
    g_installCancel.store(true);
    g_running.store(false);
    g_healthCv.notify_all();
    if (g_healthThread.joinable()) g_healthThread.join();
    if (g_installThread.joinable()) g_installThread.join();

    // Job Object を閉じる → KILL_ON_JOB_CLOSE により Ollama プロセスが終了
    if (g_ollamaJobObject) {
        CloseHandle(g_ollamaJobObject);
        g_ollamaJobObject = nullptr;
    }

    if (g_ollamaProcess) {
        if (WaitForSingleObject(g_ollamaProcess, 3000) == WAIT_TIMEOUT) {
            TerminateProcess(g_ollamaProcess, 1);
        }
        CloseHandle(g_ollamaProcess);
        g_ollamaProcess = nullptr;
    }

    logging::Debug("[Ollama] シャットダウン完了");
}

void ollama::DetachThread() {
    // DLL_PROCESS_DETACH (プロセス終了) 専用: スレッドは既に OS に終了済み
    g_installCancel.store(true);
    if (g_installThread.joinable()) g_installThread.detach();
    if (g_healthThread.joinable()) g_healthThread.detach();
    // Job Object を閉じて Ollama を終了させる
    if (g_ollamaJobObject) {
        CloseHandle(g_ollamaJobObject);
        g_ollamaJobObject = nullptr;
    }
    if (g_ollamaProcess) {
        CloseHandle(g_ollamaProcess);
        g_ollamaProcess = nullptr;
    }
}

RadioState ollama::GetRadioState() {
    return g_radioState.load();
}

void ollama::SetUserEnabled(bool enabled) {
    g_userEnabled.store(enabled);
    if (!enabled) {
        g_radioState.store(RadioState::OFF);
    } else if (g_radioState.load() == RadioState::OFF) {
        // 再有効化: ヘルスチェックで ON/FAULT に遷移させる
        g_healthCv.notify_one();
    }
}

void ollama::RequestRestart() {
    g_restartRequested.store(true);
    g_healthCv.notify_one();
}
