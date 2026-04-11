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

    // タイムアウト: resolve 60s, connect 60s, send 60s, receive 120s (LLM生成は遅い)
    WinHttpSetTimeouts(hRequest, 60000, 60000, 60000, 120000);

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
        "Translate the following chat message to " + g_targetLang + "."
        " Output ONLY the translated text, nothing else."
        " If the text is already in " + g_targetLang + ", output it unchanged."
        "\n\n" + text;

    return "{\"model\":\"" + JsonEscape(g_model) +
           "\",\"prompt\":\"" + JsonEscape(prompt) +
           "\",\"stream\":false}";
}

static std::string DoTranslate(const std::string& text) {
    std::string body = BuildRequestBody(text);
    std::string response = HttpPost(body);
    if (response.empty()) return "";
    return ExtractResponse(response);
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
// 公開 API
// ============================================================

bool translate::Init(const TranslateConfig& cfg) {
    if (!ParseUrl(cfg.endpoint)) {
        logging::Debug("[Translate] URL パース失敗: %s", cfg.endpoint.c_str());
        return false;
    }
    g_model      = cfg.model;
    g_targetLang = cfg.targetLang;

    g_hSession = WinHttpOpen(L"FoxholeChatTranslator/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_hSession) {
        logging::Debug("[Translate] WinHttpOpen 失敗: %u", GetLastError());
        return false;
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
