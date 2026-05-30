// ============================================================
// translate.cpp - Ollama 翻訳モジュール実装
// WinHTTP で Ollama REST API にリクエストを送信し、
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
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <regex>
#include <utility>

#include "translate.h"
#include "config.h"
#include "log.h"
#include <nlohmann/json.hpp>

// ============================================================
// 内部状態
// ============================================================

static HINTERNET     g_hSession  = nullptr;
static std::wstring  g_host;
static INTERNET_PORT g_port      = 11434;
static std::wstring  g_path;
static std::string   g_model;
static int           g_numCtx    = 256;
static int           g_numThread = 2;
static float         g_temperature = 0.1f;
static int           g_numPredict  = 120;

static translate::SyncStats g_lastSyncStats;

// ワーカースレッド
struct QueueItem {
    std::string     channel;
    std::string     sender;
    std::string     message;
    TranslationMode translationMode = TranslationMode::JA;
    TtsMode         ttsMode         = TtsMode::Off;
};

static std::thread             g_thread;
static std::mutex              g_mutex;
static std::condition_variable g_cv;
static std::queue<QueueItem>   g_queue;
static bool                    g_running = false;
static std::atomic<bool>       g_activeWork{false};

static constexpr size_t MAX_QUEUE_SIZE = 4;

static translate::ResultCallback g_resultCallback;

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

static std::string ClassifyError(const std::string& errMsg) {
    if (errMsg.find("memory") != std::string::npos ||
        errMsg.find("Memory") != std::string::npos) {
        return u8"(メモリ不足: config.ini の PerformancePreset を Low に変更してください)";
    }
    if (errMsg.find("not found") != std::string::npos) {
        return u8"(モデル未インストール: ollama pull で取得してください)";
    }
    return u8"(Ollamaエラー: " + errMsg + ")";
}

// ============================================================
// HTTP 共通処理 (WinHTTP)
// ============================================================

static const char* WinHttpErrorName(DWORD err) {
    switch (err) {
    case 12002: return "TIMEOUT";
    case 12007: return "NAME_NOT_RESOLVED";
    case 12029: return "CANNOT_CONNECT";
    case 12030: return "CONNECTION_ERROR";
    case 12152: return "INVALID_SERVER_RESPONSE";
    default:    return "(unknown)";
    }
}

static std::string HttpPost(const std::string& body) {
    if (!g_hSession) return "";

    HINTERNET hConnect = WinHttpConnect(g_hSession, g_host.c_str(), g_port, 0);
    if (!hConnect) {
        DWORD err = GetLastError();
        logging::Debug("[Translate] WinHttpConnect 失敗: %u (%s)", err, WinHttpErrorName(err));
        return "";
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", g_path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hConnect);
        logging::Debug("[Translate] WinHttpOpenRequest 失敗: %u (%s)", err, WinHttpErrorName(err));
        return "";
    }

    WinHttpSetTimeouts(hRequest, 3000, 3000, 5000, 10000);

    const wchar_t* headers = L"Content-Type: application/json";
    BOOL sent = WinHttpSendRequest(hRequest, headers, (DWORD)-1,
        (LPVOID)body.c_str(), (DWORD)body.size(), (DWORD)body.size(), 0);
    if (!sent) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        logging::Debug("[Translate] WinHttpSendRequest 失敗: %u (%s)", err, WinHttpErrorName(err));
        return "";
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        logging::Debug("[Translate] WinHttpReceiveResponse 失敗: %u (%s)", err, WinHttpErrorName(err));
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
// 固有名詞プレースホルダー保護
// ============================================================

using ReplacementMap = std::vector<std::pair<std::string, std::string>>;

static std::vector<std::regex> g_termRegexes;

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

static void InitTermRegexes(const std::string& baseDir) {
    std::string path = baseDir + "term_protection.txt";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        logging::Debug("[Translate] term_protection.txt が見つかりません: %s", path.c_str());
        return;
    }

    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        int len = static_cast<int>(strlen(line));
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'
                           || line[len-1] == ' ' || line[len-1] == '\t'))
            line[--len] = '\0';

        if (len == 0 || line[0] == '#') continue;

        bool icase = false;
        if (len >= 2 && line[len-1] == 'i' && line[len-2] == ' ') {
            line[len-2] = '\0';
            icase = true;
        }

        std::string pat(line);
        std::string fullPat = (pat.find("\\b") == std::string::npos)
            ? ("\\b" + pat + "\\b")
            : pat;

        try {
            auto flags = std::regex::ECMAScript;
            if (icase) flags |= std::regex::icase;
            g_termRegexes.emplace_back(fullPat, flags);
            count++;
        } catch (const std::regex_error& e) {
            logging::Debug("[Translate] 正規表現エラー '%s': %s", line, e.what());
        }
    }
    fclose(f);
    logging::Debug("[Translate] term_protection.txt: %d 件読み込み (%s)", count, path.c_str());
}

static std::string ProtectTerms(const std::string& text, ReplacementMap& out) {
    out.clear();
    if (g_termRegexes.empty()) return text;

    struct MatchSpan { size_t start, end; std::string original; };
    std::vector<MatchSpan> spans;

    for (const auto& re : g_termRegexes) {
        auto it  = std::sregex_iterator(text.begin(), text.end(), re);
        auto end = std::sregex_iterator();
        for (; it != end; ++it) {
            spans.push_back({
                static_cast<size_t>(it->position()),
                static_cast<size_t>(it->position() + it->length()),
                it->str()
            });
        }
    }
    if (spans.empty()) return text;

    std::sort(spans.begin(), spans.end(),
        [](const MatchSpan& a, const MatchSpan& b) { return a.start < b.start; });
    std::vector<MatchSpan> deduped;
    size_t lastEnd = 0;
    for (const auto& s : spans) {
        if (s.start >= lastEnd) {
            deduped.push_back(s);
            lastEnd = s.end;
        }
    }

    std::reverse(deduped.begin(), deduped.end());
    std::string result = text;
    for (const auto& s : deduped) {
        // {{T0}} 形式: LLM がテンプレート変数として認識し除去しにくい
        std::string ph = "{{T" + std::to_string(out.size()) + "}}";
        out.emplace_back(ph, s.original);
        result.replace(s.start, s.end - s.start, ph);
    }
    return result;
}

static std::string RestoreTerms(std::string translated, const ReplacementMap& replacements) {
    for (size_t i = 0; i < replacements.size(); ++i) {
        const std::string& ph   = replacements[i].first;   // {{Tn}}
        const std::string& orig = replacements[i].second;

        // 1st pass: 完全一致
        size_t pos = 0;
        bool found = false;
        while ((pos = translated.find(ph, pos)) != std::string::npos) {
            translated.replace(pos, ph.size(), orig);
            pos += orig.size();
            found = true;
        }
        if (found) continue;

        // 2nd pass: LLM が外側の {{ }} を除去して Tn だけ残した場合のフォールバック
        std::string bare = "T" + std::to_string(i);
        pos = 0;
        while ((pos = translated.find(bare, pos)) != std::string::npos) {
            // 単語境界チェック: 前後が英数字でなければ置換
            bool prevOk = (pos == 0)                       || !isalnum((unsigned char)translated[pos - 1]);
            bool nextOk = (pos + bare.size() >= translated.size()) || !isalnum((unsigned char)translated[pos + bare.size()]);
            if (prevOk && nextOk) {
                translated.replace(pos, bare.size(), orig);
                pos += orig.size();
            } else {
                pos++;
            }
        }
    }
    return translated;
}

// ============================================================
// 翻訳コア
// ============================================================

// マッチした語だけを列挙した system プロンプトを組み立てる
static std::string BuildSystemPrompt(const ReplacementMap& replacements) {
    if (replacements.empty()) return "";
    std::string s =
        "You are a Foxhole game chat translator."
        " NEVER translate these game-specific terms — keep them exactly as-is: ";
    for (size_t i = 0; i < replacements.size(); ++i) {
        if (i > 0) s += ", ";
        s += replacements[i].second;  // 実際にマッチした原語
    }
    return s + ".";
}

static std::string BuildPrompt(TranslationMode mode) {
    switch (mode) {
    case TranslationMode::JA:
        return "Translate the following war game chat message into Japanese accurately."
               " Keep the original meaning."
               " Output ONLY the translated text. No explanations, no extra sentences.";
    case TranslationMode::JAZ:
        return "Translate the following war game chat message into Japanese."
               " Use Zundamon's speech style: end sentences with なのだ or のだ occasionally."
               " Keep it concise and natural."
               " Output ONLY the translated text. No explanations, no extra sentences.";
    case TranslationMode::EN:
        return "Translate the following war game chat message into English accurately."
               " Keep the original meaning."
               " Output ONLY the translated text. No explanations, no extra sentences.";
    case TranslationMode::RU:
        return "Translate the following war game chat message into Russian accurately."
               " Keep the original meaning."
               " Output ONLY the translated text. No explanations, no extra sentences.";
    case TranslationMode::ZH:
        return "Translate the following war game chat message into Chinese (Simplified) accurately."
               " Keep the original meaning."
               " Output ONLY the translated text. No explanations, no extra sentences.";
    case TranslationMode::KO:
        return "Translate the following war game chat message into Korean accurately."
               " Keep the original meaning."
               " Output ONLY the translated text. No explanations, no extra sentences.";
    default:
        return "Translate the following war game chat message into Japanese accurately."
               " Output ONLY the translated text. No explanations, no extra sentences.";
    }
}

static std::string BuildRequestBody(const std::string& text, const std::string& systemPrompt,
                                    bool hasPlaceholders, TranslationMode translationMode) {
    std::string prompt = BuildPrompt(translationMode);
    if (hasPlaceholders)
        prompt += " IMPORTANT: Keep all {{T0}}, {{T1}}, {{T2}} etc. tokens exactly as-is in your output.";
    prompt += "\n\n" + text;

    nlohmann::json opts;
    opts["num_ctx"]     = g_numCtx;
    opts["num_predict"] = g_numPredict;
    opts["temperature"] = g_temperature;
    if (g_numThread != 0)
        opts["num_thread"] = g_numThread;

    nlohmann::json req{
        {"model",   g_model},
        {"prompt",  prompt},
        {"stream",  false},
        {"options", opts}
    };
    if (!systemPrompt.empty()) req["system"] = systemPrompt;
    return req.dump();
}

// 空白を除いた文字列が単一プレースホルダー {{T0}} だけか判定する
static bool IsSinglePlaceholder(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    if (i + 6 > s.size() || s.substr(i, 2) != "{{") return false;
    size_t end = s.find("}}", i + 2);
    if (end == std::string::npos) return false;
    end += 2;
    while (end < s.size() && s[end] == ' ') end++;
    return end == s.size();
}

// text を Ollama に送り、生の翻訳文字列を返す (エラー時は空文字列)
static std::string RawTranslate(const std::string& text, TranslationMode translationMode,
                                const std::string& systemPrompt = "",
                                bool hasPlaceholders = false) {
    std::string body     = BuildRequestBody(text, systemPrompt, hasPlaceholders, translationMode);
    std::string response = HttpPost(body);
    if (response.empty()) {
        logging::Debug("[Translate] HTTP レスポンスが空 (Ollama未起動?)");
        return "";
    }
    logging::Debug("[Translate] レスポンス (先頭200): %s", response.substr(0, 200).c_str());
    auto j = nlohmann::json::parse(response, nullptr, false);
    if (j.is_discarded()) {
        logging::Debug("[Translate] レスポンス解析失敗 (JSONパースエラー)");
        return "";
    }
    if (j.contains("response") && j["response"].is_string()) {
        g_lastSyncStats = {};
        if (j.contains("eval_count") && j["eval_count"].is_number())
            g_lastSyncStats.evalCount = j["eval_count"].get<int>();
        if (j.contains("eval_duration") && j["eval_duration"].is_number()) {
            double sec = j["eval_duration"].get<long long>() / 1e9;
            if (sec > 0) g_lastSyncStats.tokPerSec = g_lastSyncStats.evalCount / sec;
        }
        return j["response"].get<std::string>();
    }
    if (j.contains("error") && j["error"].is_string()) {
        logging::Debug("[Translate] Ollamaエラー: %s", j["error"].get<std::string>().c_str());
    }
    return "";
}

// result に replacements の原語が何件含まれているかを返す (大文字小文字無視)
static int CountFoundTerms(const std::string& result, const ReplacementMap& replacements) {
    std::string lower = result;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    int found = 0;
    for (const auto& kv : replacements) {
        std::string origLower = kv.second;
        std::transform(origLower.begin(), origLower.end(), origLower.begin(), ::tolower);
        if (lower.find(origLower) != std::string::npos) found++;
    }
    return found;
}

static std::string DoTranslate(const std::string& text, TranslationMode translationMode) {
    ReplacementMap replacements;
    std::string protected_text = ProtectTerms(text, replacements);

    // メッセージ全体が1つの保護語だけなら翻訳不要 (LLM が無意味な訳を返すのを防ぐ)
    if (!replacements.empty() && IsSinglePlaceholder(protected_text)) {
        return replacements[0].second;
    }

    // このメッセージに出現した語だけで system プロンプトを組む
    std::string systemPrompt = BuildSystemPrompt(replacements);
    bool hasPlaceholders = !replacements.empty();

    // 1st try: プレースホルダー保護あり
    std::string raw = RawTranslate(protected_text, translationMode, systemPrompt, hasPlaceholders);
    if (raw.empty())
        return u8"(接続失敗: Ollamaが起動していません)";

    std::string result = RestoreTerms(raw, replacements);

    // 保護語の半数以上が訳文から消えていたら保護なしで再翻訳
    if (!replacements.empty()) {
        int found    = CountFoundTerms(result, replacements);
        int expected = static_cast<int>(replacements.size());
        if (found * 2 < expected) {
            logging::Debug("[Translate] 保護語失落 (%d/%d) - 保護なしで再翻訳", found, expected);
            std::string fallback = RawTranslate(text, translationMode, systemPrompt, false);
            if (!fallback.empty()) result = fallback;
        }
    }

    if (result.empty()) {
        return u8"(解析失敗)";
    }
    return result;
}

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
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
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

        g_activeWork.store(true);
        std::string translated = DoTranslate(item.message, item.translationMode);
        TrimTrailing(translated);

        logging::Debug("[Translate] [%s] %s: %s -> %s",
            item.channel.c_str(), item.sender.c_str(),
            item.message.c_str(), translated.c_str());

        auto cb = g_resultCallback;
        if (cb) {
            translate::TranslateResult result;
            result.channel    = item.channel;
            result.sender     = item.sender;
            result.original   = item.message;
            result.translated = translated;
            result.ok         = !translated.empty() && translated[0] != '(';
            result.ttsMode    = item.ttsMode;
            cb(result);
        }
        g_activeWork.store(false);
    }
    logging::Debug("[Translate] ワーカースレッド終了");
}

// ============================================================
// モデル管理 (ollama.cpp から呼ばれる)
// ============================================================

static bool IsModelAvailable() {
    if (!g_hSession) return false;
    std::string body = nlohmann::json{{"name", g_model}}.dump();
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

// ストリーミング JSON から文字列フィールドを抽出 ("key":"値")
static std::string ExtractJsonStr(const std::string& line, const char* key) {
    std::string search = std::string("\"") + key + "\":\"";
    size_t p = line.find(search);
    if (p == std::string::npos) return "";
    p += search.size();
    std::string result;
    while (p < line.size() && line[p] != '"') {
        if (line[p] == '\\') { p++; if (p < line.size()) result += line[p++]; }
        else result += line[p++];
    }
    return result;
}

// ストリーミング JSON から数値フィールドを抽出 ("key":数値)
static size_t ExtractJsonUint(const std::string& line, const char* key) {
    std::string search = std::string("\"") + key + "\":";
    size_t p = line.find(search);
    if (p == std::string::npos) return 0;
    p += search.size();
    while (p < line.size() && line[p] == ' ') p++;
    size_t end = p;
    while (end < line.size() && isdigit(static_cast<unsigned char>(line[end]))) end++;
    if (end == p) return 0;
    try { return std::stoull(line.substr(p, end - p)); }
    catch (...) { return 0; }
}

static bool PullModel() {
    logging::Debug("[Translate] モデル '%s' をダウンロード中...", g_model.c_str());
    if (!g_hSession) return false;
    // stream: true でレスポンスを行単位に受け取り、進捗をリアルタイム表示する
    std::string body = nlohmann::json{{"name", g_model}, {"stream", true}}.dump();
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
    DWORD szLen = sizeof(statusCode);
    if (ok) WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &szLen, WINHTTP_NO_HEADER_INDEX);

    if (ok && statusCode == 200) {
        // ストリーミングレスポンスを行単位で処理して進捗表示
        // 1MB未満のblobは設定ファイル等の小物なので進捗表示しない
        static const size_t MIN_PROGRESS_BYTES = 1 * 1024 * 1024;
        char rawBuf[65536];
        DWORD avail = 0, read = 0;
        std::string lineBuf;
        size_t lastBlobTotal = 0; // blobが変わったらカウンターをリセットするために追跡
        int lastPct    = -1;
        int lastLogPct = -1;
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            DWORD toRead = (avail < sizeof(rawBuf)) ? avail : sizeof(rawBuf);
            if (!WinHttpReadData(hRequest, rawBuf, toRead, &read) || read == 0) break;
            for (DWORD i = 0; i < read; i++) {
                char c = rawBuf[i];
                if (c == '\n' || c == '\r') {
                    if (!lineBuf.empty()) {
                        size_t total     = ExtractJsonUint(lineBuf, "total");
                        size_t completed = ExtractJsonUint(lineBuf, "completed");
                        // 新しいblobが始まったらカウンターをリセット
                        if (total > 0 && total != lastBlobTotal) {
                            lastBlobTotal = total;
                            lastPct    = -1;
                            lastLogPct = -1;
                        }
                        // 1MB以上のblobのみ進捗表示 (小物blobの 0.0/0.0 MB ノイズを抑制)
                        if (total >= MIN_PROGRESS_BYTES) {
                            int pct = static_cast<int>(100.0 * completed / total);
                            // コンソール: 1% ごとに同一行を上書き
                            if (pct != lastPct) {
                                lastPct = pct;
                                logging::Progress("[Model-DL] %s: %.1f / %.1f MB (%d%%)",
                                    g_model.c_str(),
                                    completed / 1048576.0,
                                    total     / 1048576.0,
                                    pct);
                            }
                            // ファイル: 10% ごとにチェックポイント記録
                            int logStep = pct / 10;
                            if (logStep != lastLogPct) {
                                lastLogPct = logStep;
                                logging::Debug("[Model-DL] %s: %.1f / %.1f MB (%d%%)",
                                    g_model.c_str(),
                                    completed / 1048576.0,
                                    total     / 1048576.0,
                                    pct);
                            }
                        }
                        // 検証・書き込みフェーズのステータスメッセージを記録
                        if (total == 0) {
                            std::string status = ExtractJsonStr(lineBuf, "status");
                            if (status.find("verifying sha256") != std::string::npos) {
                                logging::Debug("[Model-DL] %s: SHA256 検証中...", g_model.c_str());
                            } else if (status.find("writing manifest") != std::string::npos) {
                                logging::Debug("[Model-DL] %s: マニフェスト書き込み中...", g_model.c_str());
                            }
                        }
                        lineBuf.clear();
                    }
                } else {
                    lineBuf += c;
                }
            }
        }
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

// ============================================================
// プリセット解決
// ============================================================

static void ApplyPreset(const std::string& preset) {
    if (preset == "High") {
        g_model       = "gemma3:4b";
        g_numCtx      = 512;
        g_numThread   = 0;
        g_temperature = 0.1f;
    } else if (preset == "Medium") {
        g_model       = "phi4-mini";
        g_numCtx      = 256;
        g_numThread   = 0;
        g_temperature = 0.1f;
    } else { // Low
        g_model       = "gemma3:1b";
        g_numCtx      = 128;
        g_numThread   = 2;
        g_temperature = 0.1f;
    }
    logging::Debug("[Translate] プリセット '%s' 適用: model=%s, num_ctx=%d, num_thread=%d, temperature=%.2f, num_predict=%d",
        preset.c_str(), g_model.c_str(), g_numCtx, g_numThread, g_temperature, g_numPredict);
}

// ============================================================
// 公開 API
// ============================================================

bool translate::Init(const TranslateConfig& cfg) {
    InitTermRegexes(GetDllBaseDir());
    if (!ParseUrl(cfg.endpoint)) {
        logging::Debug("[Translate] URL パース失敗: %s", cfg.endpoint.c_str());
        return false;
    }

    ApplyPreset(cfg.performancePreset);

    g_hSession = WinHttpOpen(L"FoxholeChatTranslator/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_hSession) {
        logging::Debug("[Translate] WinHttpOpen 失敗: %u", GetLastError());
        return false;
    }

    g_running = true;
    g_thread  = std::thread(WorkerThread);

    logging::Debug("[Translate] 初期化完了 (model=%s, endpoint=%s)",
        g_model.c_str(), cfg.endpoint.c_str());
    return true;
}

bool translate::IsBusy() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_activeWork.load() || !g_queue.empty();
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

void translate::SetResultCallback(translate::ResultCallback cb) {
    g_resultCallback = std::move(cb);
}

std::string translate::Sync(const std::string& text) {
    std::string result = DoTranslate(text, config::GetTranslationMode());
    TrimTrailing(result);
    return result;
}

translate::SyncStats translate::GetLastSyncStats() {
    return g_lastSyncStats;
}

void translate::Queue(const std::string& channel, const std::string& sender, const std::string& message,
                      TranslationMode translationMode, TtsMode ttsMode) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_queue.size() >= MAX_QUEUE_SIZE) {
        logging::Debug("[Translate] キュー満杯: 最古メッセージを破棄 (size=%zu)", g_queue.size());
        g_queue.pop();
    }
    g_queue.push({channel, sender, message, translationMode, ttsMode});
    g_cv.notify_one();
}

bool translate::IsHealthy() {
    if (!g_hSession) return false;
    HINTERNET hConnect = WinHttpConnect(g_hSession, g_host.c_str(), g_port, 0);
    if (!hConnect) return false;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/api/version",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); return false; }
    WinHttpSetTimeouts(hRequest, 3000, 3000, 3000, 3000);
    BOOL ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    return ok != FALSE;
}

bool translate::EnsureModel() {
    if (IsModelAvailable()) return true;
    return PullModel();
}

void translate::DetachThread() {
    if (g_thread.joinable()) g_thread.detach();
}
