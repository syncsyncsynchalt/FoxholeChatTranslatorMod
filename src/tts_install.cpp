// ============================================================
// tts_install.cpp - TTS コンポーネント自動インストーラー
//
// Sherpa-ONNX DLL / 音声モデル / VOICEVOX Core を GitHub から
// バックグラウンドスレッドでダウンロード・展開する
// ============================================================

#include "tts_install.h"
#include "log.h"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ============================================================
// 内部型・定数
// ============================================================

struct ModelDef {
    const char* lang;
    const char* name;      // アーカイブ名 (拡張子なし)
    bool        supertonic; // true=Supertonic, false=VITS
};

static const ModelDef kModels[] = {
    { "ja", "sherpa-onnx-supertonic-3-tts-int8-2026-05-11", true  },
    { "en", "vits-piper-en_US-lessac-medium",                false },
    { "ru", "vits-piper-ru_RU-ruslan-medium",                false },
    { "zh", "sherpa-onnx-vits-zh-ll",                        false },
    { "ko", "vits-mimic3-ko_KO-kss_low",                     false },
};
static const char kModelBaseUrl[] =
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models";

// Sherpa-ONNX DLL: バージョン固定 (GitHub API レートリミット回避)
static const char kSherpaOnnxVersion[]    = "v1.13.2";
static const char kSherpaOnnxArchiveUrl[] =
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.13.2/"
    "sherpa-onnx-v1.13.2-win-x64-shared-MD-Release.tar.bz2";

// ============================================================
// グローバル状態
// ============================================================

static std::atomic<bool> g_running{false};
static std::thread        g_thread;
static std::mutex         g_statusMutex;
static std::string        g_statusText;

static void SetStatus(const char* fmt, ...) {
    char buf[512];
    va_list va; va_start(va, fmt); vsnprintf(buf, sizeof(buf), fmt, va); va_end(va);
    logging::Debug("[TTS-Install] %s", buf);
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_statusText = buf;
}

static void SetSherpaStatus(const char* fmt, ...) {
    char buf[512];
    va_list va; va_start(va, fmt); vsnprintf(buf, sizeof(buf), fmt, va); va_end(va);
    logging::Debug("[Sherpa] %s", buf);
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_statusText = buf;
}

static void SetVoicevoxStatus(const char* fmt, ...) {
    char buf[512];
    va_list va; va_start(va, fmt); vsnprintf(buf, sizeof(buf), fmt, va); va_end(va);
    logging::Debug("[VOICEVOX] %s", buf);
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_statusText = buf;
}

// ============================================================
// WinHTTP ユーティリティ
// ============================================================

// urlA を GET して response を文字列で返す。失敗時は空文字列
static std::string HttpGetString(const char* urlA) {
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

// urlA のファイルを destPath にダウンロード。label 非 null の場合は進捗を表示。成功で true
static bool HttpDownloadToFile(const char* urlA, const char* destPath, const char* label = nullptr) {
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
    // ダウンロードは大容量なので送受信タイムアウトを長く設定
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

    size_t totalSize = 0;
    if (ok && label) {
        DWORD cl = 0, clSize = sizeof(cl);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                nullptr, &cl, &clSize, nullptr)) {
            totalSize = cl;
        }
    }

    FILE* f = nullptr;
    if (ok) {
        f = fopen(destPath, "wb");
        ok = (f != nullptr);
    }

    if (ok) {
        char buf[65536];
        DWORD avail = 0, read = 0;
        size_t downloaded = 0;
        int lastPct    = -1;
        int lastLogPct = -1;
        while (ok) {
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
                    if (pct != lastPct) {
                        lastPct = pct;
                        if (totalSize > 0) {
                            logging::Progress("[TTS-DL] %s: %.1f / %.1f MB (%d%%)",
                                label, downloaded / 1048576.0, totalSize / 1048576.0, pct);
                        } else {
                            logging::Progress("[TTS-DL] %s: %.1f MB", label, downloaded / 1048576.0);
                        }
                    }
                    int logStep = (pct >= 0) ? (pct / 10) : -1;
                    if (logStep != lastLogPct) {
                        lastLogPct = logStep;
                        if (totalSize > 0) {
                            logging::Debug("[TTS-DL] %s: %.1f / %.1f MB (%d%%)",
                                label, downloaded / 1048576.0, totalSize / 1048576.0, pct);
                        } else {
                            logging::Debug("[TTS-DL] %s: %.1f MB", label, downloaded / 1048576.0);
                        }
                    }
                }
            }
        }
        if (label && ok) {
            logging::Debug("[TTS-DL] %s: 完了 (%.1f MB)", label, downloaded / 1048576.0);
        }
    }

    if (f) fclose(f);
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(sess);
    return ok;
}

// ============================================================
// GitHub API - アセット URL 取得
// ============================================================

struct GHAsset { std::string name, url; };

// json の from 以降で key の文字列値を返す
static std::string JStr(const std::string& json, size_t from, const char* key) {
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

static std::vector<GHAsset> ParseAssets(const std::string& json) {
    std::vector<GHAsset> assets;
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
                GHAsset a;
                a.name = JStr(obj, 0, "name");
                a.url  = JStr(obj, 0, "browser_download_url");
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

// repo (例: "k2-fsa/sherpa-onnx") の latest release から nameFilter を満たすアセット URL を取得
static bool GetGitHubAssetUrl(const char* repo,
                              bool (*filter)(const std::string&),
                              std::string& outUrl) {
    std::string apiUrl = std::string("https://api.github.com/repos/") + repo + "/releases/latest";
    std::string json = HttpGetString(apiUrl.c_str());
    if (json.empty()) return false;

    for (auto& a : ParseAssets(json)) {
        if (filter(a.name)) { outUrl = a.url; return true; }
    }
    return false;
}

// ============================================================
// ファイル / ディレクトリ操作
// ============================================================

static std::string MakeTempDir() {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    char dir[MAX_PATH];
    _snprintf(dir, sizeof(dir), "%sfct_tts_%lu", tmp, GetCurrentProcessId());
    CreateDirectoryA(dir, nullptr);
    return dir;
}

static void RemoveDirRecursive(const char* path) {
    char pattern[MAX_PATH];
    _snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { RemoveDirectoryA(path); return; }
    do {
        if (fd.cFileName[0] == '.') continue;
        char child[MAX_PATH];
        _snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            RemoveDirRecursive(child);
        else
            DeleteFileA(child);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    RemoveDirectoryA(path);
}

// src ディレクトリ以下で filter(filename)==true のファイルを destDir にコピー
static void CopyFilesIf(const char* srcDir, const char* destDir,
                        bool (*filter)(const char*)) {
    char pattern[MAX_PATH];
    _snprintf(pattern, sizeof(pattern), "%s\\*", srcDir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        char src[MAX_PATH], dst[MAX_PATH];
        _snprintf(src, sizeof(src), "%s\\%s", srcDir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CopyFilesIf(src, destDir, filter);
        } else if (filter(fd.cFileName)) {
            _snprintf(dst, sizeof(dst), "%s\\%s", destDir, fd.cFileName);
            CopyFileA(src, dst, FALSE);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// src 以下で name==target のディレクトリを探して destDir にコピー (見つかったら true)
static bool CopyNamedDir(const char* srcRoot, const char* targetName, const char* destDir) {
    char pattern[MAX_PATH];
    _snprintf(pattern, sizeof(pattern), "%s\\*", srcRoot);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (fd.cFileName[0] == '.' || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        char child[MAX_PATH];
        _snprintf(child, sizeof(child), "%s\\%s", srcRoot, fd.cFileName);
        if (_stricmp(fd.cFileName, targetName) == 0) {
            // 再帰コピー
            CreateDirectoryA(destDir, nullptr);
            char pat2[MAX_PATH];
            _snprintf(pat2, sizeof(pat2), "%s\\*", child);
            WIN32_FIND_DATAA fd2;
            HANDLE h2 = FindFirstFileA(pat2, &fd2);
            if (h2 != INVALID_HANDLE_VALUE) {
                do {
                    if (fd2.cFileName[0] == '.') continue;
                    char s[MAX_PATH], d[MAX_PATH];
                    _snprintf(s, sizeof(s), "%s\\%s", child, fd2.cFileName);
                    _snprintf(d, sizeof(d), "%s\\%s", destDir, fd2.cFileName);
                    if (fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        CopyNamedDir(s, fd2.cFileName, d);
                    else
                        CopyFileA(s, d, FALSE);
                } while (FindNextFileA(h2, &fd2));
                FindClose(h2);
            }
            found = true; break;
        }
        if (!found) found = CopyNamedDir(child, targetName, destDir);
        if (found) break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

// 指定ファイル名をディレクトリツリーから検索して destPath にコピー (見つかったら true)
static bool FindAndCopyFile(const char* srcRoot, const char* filename, const char* destPath) {
    char pattern[MAX_PATH];
    _snprintf(pattern, sizeof(pattern), "%s\\*", srcRoot);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (fd.cFileName[0] == '.') continue;
        char child[MAX_PATH];
        _snprintf(child, sizeof(child), "%s\\%s", srcRoot, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            found = FindAndCopyFile(child, filename, destPath);
        } else if (_stricmp(fd.cFileName, filename) == 0) {
            CopyFileA(child, destPath, FALSE);
            found = true;
        }
        if (found) break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

// ============================================================
// プロセス起動ユーティリティ
// ============================================================

// cmdline を実行して終了を待つ。stdinData が非 null の場合パイプ経由で送る
static bool RunProcess(const char* cmdline, const char* stdinData, size_t stdinLen,
                       const char* extraEnvVar) {
    HANDLE hStdinR = nullptr, hStdinW = nullptr;
    if (stdinData) {
        SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
        if (!CreatePipe(&hStdinR, &hStdinW, &sa, 0)) return false;
        SetHandleInformation(hStdinW, HANDLE_FLAG_INHERIT, 0);
        DWORD written;
        WriteFile(hStdinW, stdinData, (DWORD)stdinLen, &written, nullptr);
        CloseHandle(hStdinW);
    }

    // 環境変数ブロック構築 (既存環境 + extraEnvVar)
    std::string envBlock;
    if (extraEnvVar) {
        char* env = GetEnvironmentStringsA();
        for (char* p = env; *p; ) {
            std::string v(p);
            p += v.size() + 1;
            // extraEnvVar と同じキーは除去
            std::string key = extraEnvVar;
            size_t eq = key.find('=');
            if (eq != std::string::npos) {
                key = key.substr(0, eq + 1);
                if (v.substr(0, key.size()) == key) continue;
            }
            envBlock += v; envBlock += '\0';
        }
        FreeEnvironmentStringsA(env);
        envBlock += extraEnvVar; envBlock += '\0';
        envBlock += '\0';
    }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = hStdinR ? hStdinR : INVALID_HANDLE_VALUE;
    // 出力は捨てる
    HANDLE hNull = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    si.hStdOutput = hNull;
    si.hStdError  = hNull;

    char cmd[4096];
    strncpy(cmd, cmdline, sizeof(cmd) - 1);

    PROCESS_INFORMATION pi = {};
    bool ok = CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE,
                  CREATE_NO_WINDOW,
                  extraEnvVar ? (LPVOID)envBlock.c_str() : nullptr,
                  nullptr, &si, &pi) != 0;
    if (hStdinR) CloseHandle(hStdinR);
    if (hNull != INVALID_HANDLE_VALUE) CloseHandle(hNull);

    if (ok) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return ok;
}

// tar.exe でアーカイブを destDir に展開
static bool ExtractTar(const char* archive, const char* destDir) {
    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, sizeof(sysDir));
    char cmd[4096];
    _snprintf(cmd, sizeof(cmd), "\"%s\\tar.exe\" -xf \"%s\" -C \"%s\"",
              sysDir, archive, destDir);
    CreateDirectoryA(destDir, nullptr);
    return RunProcess(cmd, nullptr, 0, nullptr);
}

// ============================================================
// インストール済み判定
// ============================================================

static bool FontInstalled(const std::string& assetsDir) {
    return GetFileAttributesA((assetsDir + "NotoSansCJKjp-Regular.otf").c_str())
           != INVALID_FILE_ATTRIBUTES;
}

static bool SherpaInstalled(const std::string& ttsDir) {
    return GetFileAttributesA((ttsDir + "sherpa-onnx.dll").c_str()) != INVALID_FILE_ATTRIBUTES ||
           GetFileAttributesA((ttsDir + "sherpa-onnx-c-api.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool ModelInstalled(const std::string& modelsDir, const ModelDef& m) {
    std::string langDir = modelsDir + m.lang + "\\";
    if (m.supertonic)
        return GetFileAttributesA((langDir + "duration_predictor.int8.onnx").c_str()) != INVALID_FILE_ATTRIBUTES;
    return GetFileAttributesA((langDir + "model.onnx").c_str()) != INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesA((langDir + "tokens.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool VoicevoxInstalled(const std::string& ttsDir) {
    std::string vvDir = ttsDir + "voicevox\\";
    if (GetFileAttributesA((vvDir + "c_api\\lib\\voicevox_core.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    std::string vvmsDir = vvDir + "models\\vvms\\";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((vvmsDir + "*.vvm").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return true;
}

// ============================================================
// 各コンポーネントのインストール
// ============================================================

static bool InstallFont(const std::string& assetsDir, const std::string& tmpDir) {
    SetStatus("フォントをダウンロード中...");
    CreateDirectoryA(assetsDir.c_str(), nullptr);
    // 一時ファイルにダウンロードしてから移動 (中断時の壊れたファイルを避ける)
    std::string tmpPath = tmpDir + "\\NotoSansCJKjp-Regular.otf";
    std::string destPath = assetsDir + "NotoSansCJKjp-Regular.otf";
    const char* url =
        "https://github.com/googlefonts/noto-cjk/raw/main/Sans/OTF/Japanese/NotoSansCJKjp-Regular.otf";
    if (!HttpDownloadToFile(url, tmpPath.c_str(), "フォント")) {
        logging::Debug("[TTS-Install] フォント: ダウンロード失敗");
        return false;
    }
    if (!MoveFileExA(tmpPath.c_str(), destPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        CopyFileA(tmpPath.c_str(), destPath.c_str(), FALSE);
        DeleteFileA(tmpPath.c_str());
    }
    logging::Debug("[TTS-Install] フォント: インストール完了");
    return true;
}

static bool InstallSherpaOnnxDll(const std::string& ttsDir, const std::string& tmpDir) {
    SetSherpaStatus("DLL をダウンロード中... (%s)", kSherpaOnnxVersion);
    std::string archiveName = std::string("sherpa-onnx-") + kSherpaOnnxVersion +
                              "-win-x64-shared-MD-Release.tar.bz2";
    std::string archivePath = tmpDir + "\\" + archiveName;
    if (!HttpDownloadToFile(kSherpaOnnxArchiveUrl, archivePath.c_str(), "Sherpa-ONNX DLL")) {
        logging::Debug("[Sherpa] DLL: ダウンロード失敗");
        return false;
    }

    std::string extractDir = tmpDir + "\\dll_extract";
    if (!ExtractTar(archivePath.c_str(), extractDir.c_str())) {
        logging::Debug("[Sherpa] DLL: 展開失敗");
        return false;
    }

    // sherpa-onnx / onnxruntime / espeak 系 DLL をコピー
    CopyFilesIf(extractDir.c_str(), ttsDir.c_str(), [](const char* n) {
        std::string s(n);
        return s.size() > 4 && _stricmp(s.c_str() + s.size() - 4, ".dll") == 0 &&
               (s.find("sherpa") != std::string::npos ||
                s.find("onnxruntime") != std::string::npos ||
                s.find("espeak") != std::string::npos);
    });

    // espeak-ng-data ディレクトリをコピー
    std::string espeakDest = ttsDir + "espeak-ng-data";
    if (GetFileAttributesA(espeakDest.c_str()) == INVALID_FILE_ATTRIBUTES)
        CopyNamedDir(extractDir.c_str(), "espeak-ng-data", espeakDest.c_str());

    RemoveDirRecursive(extractDir.c_str());
    logging::Debug("[Sherpa] DLL: インストール完了");
    return true;
}

static bool InstallModel(const ModelDef& m, const std::string& modelsDir,
                         const std::string& ttsDir, const std::string& tmpDir) {
    SetSherpaStatus("[%s] モデルをダウンロード中...", m.lang);
    std::string archiveName = std::string(m.name) + ".tar.bz2";
    std::string url = std::string(kModelBaseUrl) + "/" + archiveName;
    std::string archivePath = tmpDir + "\\" + archiveName;

    char modelLabel[32];
    snprintf(modelLabel, sizeof(modelLabel), "Sherpa [%s]", m.lang);
    if (!HttpDownloadToFile(url.c_str(), archivePath.c_str(), modelLabel)) {
        logging::Debug("[Sherpa] [%s] モデルダウンロード失敗", m.lang);
        return false;
    }

    std::string extractDir = tmpDir + "\\" + std::string(m.lang) + "_extract";
    if (!ExtractTar(archivePath.c_str(), extractDir.c_str())) {
        logging::Debug("[Sherpa] [%s] 展開失敗", m.lang);
        return false;
    }

    std::string langDir = modelsDir + m.lang + "\\";
    CreateDirectoryA(langDir.c_str(), nullptr);

    if (m.supertonic) {
        const char* files[] = {
            "duration_predictor.int8.onnx", "text_encoder.int8.onnx",
            "vector_estimator.int8.onnx",   "vocoder.int8.onnx",
            "tts.json", "unicode_indexer.bin", "voice.bin", nullptr
        };
        for (int i = 0; files[i]; i++)
            FindAndCopyFile(extractDir.c_str(), files[i], (langDir + files[i]).c_str());
    } else {
        // VITS
        FindAndCopyFile(extractDir.c_str(), "tokens.txt", (langDir + "tokens.txt").c_str());
        FindAndCopyFile(extractDir.c_str(), "lexicon.txt", (langDir + "lexicon.txt").c_str());
        // *.onnx を model.onnx としてコピー (最初に見つかったもの)
        CopyFilesIf(extractDir.c_str(), langDir.c_str(), [](const char* n) {
            size_t l = strlen(n);
            return l > 5 && _stricmp(n + l - 5, ".onnx") == 0;
        });

        // espeak-ng-data (まだなければ)
        std::string espeakDest = ttsDir + "espeak-ng-data";
        if (GetFileAttributesA(espeakDest.c_str()) == INVALID_FILE_ATTRIBUTES)
            CopyNamedDir(extractDir.c_str(), "espeak-ng-data", espeakDest.c_str());

        // dict (中国語)
        if (strcmp(m.lang, "zh") == 0) {
            std::string dictDest = langDir + "dict";
            if (GetFileAttributesA(dictDest.c_str()) == INVALID_FILE_ATTRIBUTES)
                CopyNamedDir(extractDir.c_str(), "dict", dictDest.c_str());
        }
    }

    RemoveDirRecursive(extractDir.c_str());
    logging::Debug("[Sherpa] [%s] インストール完了", m.lang);
    return true;
}

static bool InstallVoicevox(const std::string& ttsDir, const std::string& tmpDir) {
    SetVoicevoxStatus("ダウンローダーを取得中...");
    auto filter = [](const std::string& n) {
        return n == "download-windows-x64.exe";
    };
    std::string url;
    if (!GetGitHubAssetUrl("VOICEVOX/voicevox_core", filter, url)) {
        logging::Debug("[VOICEVOX] ダウンローダー URL 取得失敗");
        return false;
    }

    std::string downloaderPath = tmpDir + "\\voicevox_downloader.exe";
    if (!HttpDownloadToFile(url.c_str(), downloaderPath.c_str(), "VOICEVOX ダウンローダー")) {
        logging::Debug("[VOICEVOX] ダウンローダーダウンロード失敗");
        return false;
    }

    std::string vvDir = ttsDir + "voicevox";
    CreateDirectoryA(vvDir.c_str(), nullptr);

    SetVoicevoxStatus("インストール中 (数分かかります)...");
    char cmd[4096];
    _snprintf(cmd, sizeof(cmd),
        "\"%s\" --devices cpu --models-pattern 0.vvm --output \"%s\"",
        downloaderPath.c_str(), vvDir.c_str());

    // ライセンス同意に "y\n" を stdin で送る。TERM=dumb でページャを抑制
    const char stdinData[] = "y\n";
    if (!RunProcess(cmd, stdinData, sizeof(stdinData) - 1, "TERM=dumb")) {
        logging::Debug("[VOICEVOX] ダウンローダー実行失敗");
        return false;
    }

    bool ok = VoicevoxInstalled(ttsDir);
    logging::Debug("[VOICEVOX] %s", ok ? "インストール完了" : "インストール失敗");
    return ok;
}

// ============================================================
// ワーカースレッド
// ============================================================

static bool ShouldInstallLang(const std::string& ttsLang, const char* modelLang) {
    if (ttsLang == "auto" || ttsLang.empty()) return true;
    return ttsLang == modelLang;
}

// ttsDir (.../tools/tts/) から 2階層上に遡って assets\ を得る
static std::string AssetsDir(const std::string& ttsDir) {
    std::string base = ttsDir;
    if (!base.empty() && base.back() == '\\') base.pop_back();
    size_t p = base.rfind('\\');
    if (p != std::string::npos) base = base.substr(0, p);
    p = base.rfind('\\');
    if (p != std::string::npos) base = base.substr(0, p + 1);
    return base + "assets\\";
}

static void Worker(std::string ttsDir, std::string ttsLang) {
    std::string assetsDir = AssetsDir(ttsDir);
    std::string modelsDir = ttsDir + "models\\";
    std::string tmpDir    = MakeTempDir();

    CreateDirectoryA(ttsDir.c_str(), nullptr);
    CreateDirectoryA(modelsDir.c_str(), nullptr);

    // 結果追跡: インストール済みのものは最初から OK
    const int kN = static_cast<int>(sizeof(kModels) / sizeof(kModels[0]));
    bool fontOk   = FontInstalled(assetsDir);
    bool sherpaOk = SherpaInstalled(ttsDir);
    bool vvOk     = false;
    bool modelOk[5] = {};
    for (int i = 0; i < kN; i++) modelOk[i] = ModelInstalled(modelsDir, kModels[i]);

    // ステップ 1/4: フォント
    if (!fontOk) {
        logging::Debug("[TTS-Install] ステップ 1/4: フォントをインストール中...");
        fontOk = InstallFont(assetsDir, tmpDir);
        if (!fontOk) logging::Debug("[TTS-Install] フォント失敗、続行");
    } else {
        logging::Debug("[TTS-Install] ステップ 1/4: フォント - インストール済み");
    }

    // ステップ 2/4: Sherpa-ONNX DLL
    if (!sherpaOk) {
        logging::Debug("[TTS-Install] ステップ 2/4: Sherpa-ONNX DLL をインストール中...");
        sherpaOk = InstallSherpaOnnxDll(ttsDir, tmpDir);
        if (!sherpaOk) logging::Debug("[Sherpa] DLL 失敗、続行");
    } else {
        logging::Debug("[TTS-Install] ステップ 2/4: Sherpa-ONNX DLL - インストール済み");
    }

    // ステップ 3/4: Sherpa-ONNX 音声モデル
    logging::Debug("[TTS-Install] ステップ 3/4: Sherpa-ONNX 音声モデルをインストール中...");
    bool needsVoicevox = (ttsLang == "auto" || ttsLang.empty());
    for (int i = 0; i < kN; i++) {
        const auto& m = kModels[i];
        if (!ShouldInstallLang(ttsLang, m.lang)) continue;
        if (strcmp(m.lang, "ja") == 0) needsVoicevox = true;
        if (!modelOk[i]) {
            modelOk[i] = InstallModel(m, modelsDir, ttsDir, tmpDir);
            if (!modelOk[i]) logging::Debug("[Sherpa] [%s] モデル失敗、続行", m.lang);
        } else {
            logging::Debug("[Sherpa] [%s] モデル - インストール済み", m.lang);
        }
    }

    // ステップ 4/4: VOICEVOX Core (日本語用)
    if (needsVoicevox) {
        vvOk = VoicevoxInstalled(ttsDir);
        if (!vvOk) {
            logging::Debug("[TTS-Install] ステップ 4/4: VOICEVOX Core をインストール中...");
            vvOk = InstallVoicevox(ttsDir, tmpDir);
            if (!vvOk) logging::Debug("[VOICEVOX] インストール失敗、続行");
        } else {
            logging::Debug("[TTS-Install] ステップ 4/4: VOICEVOX Core - インストール済み");
        }
    }

    // インストール結果サマリー
    logging::Debug("[TTS-Install] === インストール結果 ===");
    logging::Debug("[TTS-Install]   フォント        : %s", fontOk   ? "OK" : "失敗");
    logging::Debug("[TTS-Install]   Sherpa-ONNX DLL : %s", sherpaOk ? "OK" : "失敗");
    for (int i = 0; i < kN; i++) {
        if (!ShouldInstallLang(ttsLang, kModels[i].lang)) continue;
        logging::Debug("[TTS-Install]   Sherpa [%-2s]     : %s",
                       kModels[i].lang, modelOk[i] ? "OK" : "失敗");
    }
    if (needsVoicevox)
        logging::Debug("[TTS-Install]   VOICEVOX Core   : %s", vvOk ? "OK" : "失敗");

    RemoveDirRecursive(tmpDir.c_str());
    SetStatus("TTS インストール完了");
    g_running.store(false);
}

// ============================================================
// 公開 API
// ============================================================

void tts_install::StartIfNeeded(const std::string& ttsDir, const std::string& ttsLang) {
    if (g_running.load()) return;

    bool needSherpa   = !SherpaInstalled(ttsDir);
    bool needVoicevox = !VoicevoxInstalled(ttsDir);
    bool needModel    = false;
    std::string modelsDir = ttsDir + "models\\";
    for (const auto& m : kModels) {
        if (ShouldInstallLang(ttsLang, m.lang) && !ModelInstalled(modelsDir, m)) {
            needModel = true; break;
        }
    }

    std::string assetsDir = AssetsDir(ttsDir);
    bool needFont = !assetsDir.empty() && !FontInstalled(assetsDir);

    if (!needFont && !needSherpa && !needVoicevox && !needModel) return;

    logging::Debug("[TTS-Install] 不足コンポーネントを検出: font=%d sherpa=%d voicevox=%d model=%d",
                   needFont, needSherpa, needVoicevox, needModel);
    g_running.store(true);
    g_thread = std::thread(Worker, ttsDir, ttsLang);
}

bool tts_install::IsRunning() { return g_running.load(); }

std::string tts_install::GetStatusText() {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    return g_statusText;
}

void tts_install::Shutdown() {
    g_running.store(false);
    if (g_thread.joinable()) g_thread.join();
}
