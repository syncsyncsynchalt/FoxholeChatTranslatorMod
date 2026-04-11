// ============================================================
// translate.cpp - Ollama 翻訳モジュール実装
// WinHTTP で Ollama REST API (localhost) にリクエストを送信し、
// バックグラウンドスレッドで非同期翻訳を行う
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
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>

#include "translate.h"
#include "log.h"

// ============================================================
// 内部状態
// ============================================================

static HINTERNET    g_hSession   = nullptr;
static std::wstring g_host;
static INTERNET_PORT g_port      = 11434;
static std::wstring g_path;
static std::string  g_model;
static std::string  g_targetLang;
static std::string  g_ollamaDir;       // 同梱 ollama.exe のディレクトリ
static HANDLE       g_ollamaProcess = nullptr; // 自前起動した ollama serve プロセス

// ワーカースレッド
struct QueueItem {
    std::string channel;
    std::string sender;
    std::string message;
};

static std::thread              g_thread;
static std::mutex               g_mutex;
static std::condition_variable  g_cv;
static std::queue<QueueItem>    g_queue;
static bool                     g_running = false;

static constexpr size_t MAX_QUEUE_SIZE = 32;

// ============================================================
// URL パース (http://host:port/path)
// ============================================================

static bool ParseUrl(const std::string& url) {
    std::string s = url;
    if (s.size() > 7 && s.substr(0, 7) == "http://") {
        s = s.substr(7);
    }

    size_t slashPos = s.find('/');
    std::string hostPort = (slashPos != std::string::npos) ? s.substr(0, slashPos) : s;
    std::string path     = (slashPos != std::string::npos) ? s.substr(slashPos) : "/";

    size_t colonPos = hostPort.find(':');
    std::string host;
    if (colonPos != std::string::npos) {
        host   = hostPort.substr(0, colonPos);
        g_port = static_cast<INTERNET_PORT>(std::stoi(hostPort.substr(colonPos + 1)));
    } else {
        host   = hostPort;
        g_port = 80;
    }

    g_host.assign(host.begin(), host.end());
    g_path.assign(path.begin(), path.end());
    return true;
}

// ============================================================
// JSON 文字列エスケープ
// ============================================================

static std::string JsonEscape(const std::string& s) {
    std::string result;
    result.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n";  break;
        case '\r': result += "\\r";  break;
        case '\t': result += "\\t";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                result += buf;
            } else {
                result += static_cast<char>(c);
            }
            break;
        }
    }
    return result;
}

// ============================================================
// Ollama JSON レスポンスから "response" フィールドを抽出
// ============================================================

static std::string ExtractResponse(const std::string& json) {
    size_t pos = json.find("\"response\"");
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + 10);
    if (pos == std::string::npos) return "";
    pos++;

    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++; // skip opening quote

    std::string result;
    while (pos < json.size()) {
        if (json[pos] == '"') break;
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;
            switch (json[pos]) {
            case '"':  result += '"';  break;
            case '\\': result += '\\'; break;
            case '/':  result += '/';  break;
            case 'n':  result += '\n'; break;
            case 'r':  result += '\r'; break;
            case 't':  result += '\t'; break;
            case 'b':  result += '\b'; break;
            case 'f':  result += '\f'; break;
            case 'u': {
                if (pos + 4 < json.size()) {
                    unsigned int cp = 0;
                    try { cp = std::stoul(json.substr(pos + 1, 4), nullptr, 16); }
                    catch (...) { break; }
                    // UTF-8 エンコード
                    if (cp < 0x80) {
                        result += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        result += static_cast<char>(0xC0 | (cp >> 6));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        result += static_cast<char>(0xE0 | (cp >> 12));
                        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    pos += 4;
                }
                break;
            }
            default: result += json[pos]; break;
            }
        } else {
            result += json[pos];
        }
        pos++;
    }
    return result;
}

// ============================================================
// HTTP POST (WinHTTP)
// ============================================================

static std::string HttpPost(const std::string& body) {
    if (!g_hSession) return "";

    HINTERNET hConnect = WinHttpConnect(g_hSession, g_host.c_str(), g_port, 0);
    if (!hConnect) {
        logging::Debug("[Translate] WinHttpConnect 失敗: %u", GetLastError());
        return "";
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", g_path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        logging::Debug("[Translate] WinHttpOpenRequest 失敗: %u", GetLastError());
        return "";
    }

    // タイムアウト: resolve 60s, connect 60s, send 60s, receive 300s (初回モデルロード対応)
    WinHttpSetTimeouts(hRequest, 60000, 60000, 60000, 300000);

    const wchar_t* headers = L"Content-Type: application/json";
    BOOL sent = WinHttpSendRequest(hRequest, headers, (DWORD)-1,
        (LPVOID)body.c_str(), (DWORD)body.size(), (DWORD)body.size(), 0);
    if (!sent) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        logging::Debug("[Translate] WinHttpSendRequest 失敗: %u (Ollama未起動?)", err);
        return "";
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        logging::Debug("[Translate] WinHttpReceiveResponse 失敗: %u", err);
        return "";
    }

    std::string response;
    DWORD bytesAvailable = 0;
    DWORD bytesRead = 0;
    do {
        bytesAvailable = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) break;
        if (bytesAvailable == 0) break;

        std::string chunk(bytesAvailable, '\0');
        if (!WinHttpReadData(hRequest, &chunk[0], bytesAvailable, &bytesRead)) break;
        response.append(chunk.data(), bytesRead);
    } while (bytesAvailable > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    return response;
}

// ============================================================
// 翻訳コア
// ============================================================

static std::string BuildRequestBody(const std::string& text) {
    std::string prompt =
        "You are a translator. The user sends a chat message in any language."
        " Translate it to " + g_targetLang + "."
        " Output ONLY the translated text, nothing else. No explanations."
        " If the message is already in " + g_targetLang + ", output it unchanged."
        "\n\n" + text;

    return "{\"model\":\"" + JsonEscape(g_model) +
           "\",\"prompt\":\"" + JsonEscape(prompt) +
           "\",\"stream\":false}";
}

static std::string DoTranslate(const std::string& text) {
    std::string body = BuildRequestBody(text);
    std::string response = HttpPost(body);
    if (response.empty()) {
        logging::Debug("[Translate] HTTP レスポンスが空");
        return "";
    }
    // デバッグ: レスポンスの先頭を出力
    std::string preview = response.substr(0, 200);
    logging::Debug("[Translate] レスポンス (先頭200): %s", preview.c_str());
    std::string result = ExtractResponse(response);
    if (result.empty()) {
        logging::Debug("[Translate] ExtractResponse: response フィールド未検出");
    }
    return result;
}

// 末尾の空白・改行を除去
static void TrimTrailing(std::string& s) {
    while (!s.empty()) {
        char c = s.back();
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            s.pop_back();
        } else {
            break;
        }
    }
}

// ============================================================
// ワーカースレッド
// ============================================================

static void WorkerThread() {
    logging::Debug("[Translate] ワーカースレッド開始");
    while (true) {
        QueueItem item;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait(lock, [] { return !g_queue.empty() || !g_running; });
            if (!g_running && g_queue.empty()) break;
            item = std::move(g_queue.front());
            g_queue.pop();
        }

        std::string translated = DoTranslate(item.message);
        if (translated.empty()) continue;

        TrimTrailing(translated);
        logging::Debug("[Translate] [%s] %s: %s -> %s",
            item.channel.c_str(), item.sender.c_str(),
            item.message.c_str(), translated.c_str());
    }
    logging::Debug("[Translate] ワーカースレッド終了");
}

// ============================================================
// Ollama プロセス管理
// ============================================================

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

// Ollama API に疎通確認 (GET /api/version)
static bool IsOllamaReachable() {
    if (!g_hSession) return false;
    HINTERNET hConnect = WinHttpConnect(g_hSession, g_host.c_str(), g_port, 0);
    if (!hConnect) return false;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/api/version",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); return false; }
    WinHttpSetTimeouts(hRequest, 2000, 2000, 2000, 2000);
    BOOL ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    return ok != FALSE;
}

// 同梱 ollama.exe のパスを探す
static std::string FindBundledOllama() {
    if (!g_ollamaDir.empty()) {
        std::string exe = g_ollamaDir + "\\ollama.exe";
        if (GetFileAttributesA(exe.c_str()) != INVALID_FILE_ATTRIBUTES) return exe;
    }
    // DLL ディレクトリからの相対パスで探す
    std::string baseDir = GetDllBaseDir();
    // カレントディレクトリ
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);
    std::string cwdStr = cwd;
    if (!cwdStr.empty() && cwdStr.back() != '\\') cwdStr += '\\';

    std::string candidates[] = {
        baseDir + "tools\\ollama\\ollama.exe",
        baseDir + "..\\..\\Mods\\ChatTranslator\\tools\\ollama\\ollama.exe",
        cwdStr + "tools\\ollama\\ollama.exe",
    };
    for (auto& path : candidates) {
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }
    return "";
}

// ollama serve をバックグラウンドで起動
static bool StartOllamaServe(const std::string& exePath) {
    // OLLAMA_MODELS を同梱ディレクトリ内の models/ に設定
    std::string dir = exePath;
    size_t pos = dir.rfind('\\');
    if (pos != std::string::npos) dir = dir.substr(0, pos);
    std::string modelsDir = dir + "\\models";
    CreateDirectoryA(modelsDir.c_str(), nullptr);
    SetEnvironmentVariableA("OLLAMA_MODELS", modelsDir.c_str());

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::string cmdLine = "\"" + exePath + "\" serve";

    if (!CreateProcessA(nullptr, &cmdLine[0], nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        logging::Debug("[Translate] ollama serve 起動失敗: %u", GetLastError());
        return false;
    }

    CloseHandle(pi.hThread);
    g_ollamaProcess = pi.hProcess;
    logging::Debug("[Translate] ollama serve 起動 (PID=%u, exe=%s)", pi.dwProcessId, exePath.c_str());
    logging::Debug("[Translate] OLLAMA_MODELS=%s", modelsDir.c_str());

    // サーバー起動を待機 (最大30秒)
    for (int i = 0; i < 60; i++) {
        Sleep(500);
        if (IsOllamaReachable()) {
            logging::Debug("[Translate] ollama serve 応答確認 (%d ms)", (i + 1) * 500);
            return true;
        }
    }
    logging::Debug("[Translate] ollama serve 応答タイムアウト (30秒)");
    return false;
}

// モデルが利用可能か確認 (POST /api/show)
static bool IsModelAvailable() {
    std::string body = "{\"name\":\"" + JsonEscape(g_model) + "\"}";
    HINTERNET hConnect = WinHttpConnect(g_hSession, g_host.c_str(), g_port, 0);
    if (!hConnect) return false;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/api/show",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); return false; }
    WinHttpSetTimeouts(hRequest, 5000, 5000, 5000, 10000);
    const wchar_t* headers = L"Content-Type: application/json";
    BOOL ok = WinHttpSendRequest(hRequest, headers, (DWORD)-1,
        (LPVOID)body.c_str(), (DWORD)body.size(), (DWORD)body.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);
    DWORD statusCode = 0;
    DWORD size = sizeof(statusCode);
    if (ok) WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    return statusCode == 200;
}

// モデルを pull (POST /api/pull, stream=false)
static bool PullModel() {
    logging::Debug("[Translate] モデル '%s' をダウンロード中...", g_model.c_str());
    std::string body = "{\"name\":\"" + JsonEscape(g_model) + "\",\"stream\":false}";
    HINTERNET hConnect = WinHttpConnect(g_hSession, g_host.c_str(), g_port, 0);
    if (!hConnect) return false;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/api/pull",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); return false; }
    // モデルダウンロードは長時間かかる (最大30分)
    WinHttpSetTimeouts(hRequest, 60000, 60000, 60000, 1800000);
    const wchar_t* headers = L"Content-Type: application/json";
    BOOL ok = WinHttpSendRequest(hRequest, headers, (DWORD)-1,
        (LPVOID)body.c_str(), (DWORD)body.size(), (DWORD)body.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);
    DWORD statusCode = 0;
    DWORD size = sizeof(statusCode);
    if (ok) WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX);
    // レスポンスボディを読み捨て (コネクション解放のため)
    if (ok) {
        DWORD avail = 0, read = 0;
        do {
            if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
            std::string buf(avail, '\0');
            WinHttpReadData(hRequest, &buf[0], avail, &read);
        } while (avail > 0);
    }
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    if (statusCode == 200) {
        logging::Debug("[Translate] モデル '%s' ダウンロード完了", g_model.c_str());
        return true;
    }
    logging::Debug("[Translate] モデル pull 失敗 (HTTP %u)", statusCode);
    return false;
}

// Ollama の確保: 疎通確認 → 未起動なら同梱版を起動 → モデル確認 → 未DLなら pull
static bool EnsureOllama() {
    if (!IsOllamaReachable()) {
        std::string exe = FindBundledOllama();
        if (exe.empty()) {
            logging::Debug("[Translate] Ollama 未起動かつ同梱バイナリ未検出");
            return false;
        }
        if (!StartOllamaServe(exe)) return false;
    }
    if (!IsModelAvailable()) {
        if (!PullModel()) return false;
    }
    return true;
}

// ============================================================
// 公開 API
// ============================================================

bool translate::Init(const TranslateConfig& cfg) {
    if (!ParseUrl(cfg.endpoint)) {
        logging::Debug("[Translate] URL パース失敗: %s", cfg.endpoint.c_str());
        return false;
    }
    g_model      = cfg.model;
    g_targetLang = cfg.targetLang;
    g_ollamaDir  = cfg.ollamaDir;

    g_hSession = WinHttpOpen(L"FoxholeChatTranslator/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_hSession) {
        logging::Debug("[Translate] WinHttpOpen 失敗: %u", GetLastError());
        return false;
    }

    // Ollama 自動起動 + モデル確認/DL
    if (!EnsureOllama()) {
        logging::Debug("[Translate] Ollama 準備失敗 - 翻訳は利用不可");
        // セッションは維持 (後でリトライ可能)
    }

    g_running = true;
    g_thread  = std::thread(WorkerThread);

    logging::Debug("[Translate] 初期化完了 (model=%s, target=%s, endpoint=%s)",
        g_model.c_str(), g_targetLang.c_str(), cfg.endpoint.c_str());
    return true;
}

void translate::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_running = false;
    }
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();

    if (g_hSession) {
        WinHttpCloseHandle(g_hSession);
        g_hSession = nullptr;
    }

    // 自前起動した ollama プロセスを終了
    if (g_ollamaProcess) {
        TerminateProcess(g_ollamaProcess, 0);
        WaitForSingleObject(g_ollamaProcess, 5000);
        CloseHandle(g_ollamaProcess);
        g_ollamaProcess = nullptr;
        logging::Debug("[Translate] ollama プロセス終了");
    }

    logging::Debug("[Translate] シャットダウン完了");
}

std::string translate::Sync(const std::string& text) {
    std::string result = DoTranslate(text);
    TrimTrailing(result);
    return result;
}

void translate::Queue(const std::string& channel, const std::string& sender, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_queue.size() >= MAX_QUEUE_SIZE) {
        g_queue.pop(); // 最古のメッセージを破棄
    }
    g_queue.push({channel, sender, message});
    g_cv.notify_one();
}
