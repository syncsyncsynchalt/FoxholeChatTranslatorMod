// ============================================================
// hooks.cpp - UE4 ProcessEvent フック + チャットログ
// Stage 1: チャットメッセージをテキストファイルに記録
// ============================================================

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

#include <MinHook.h>
#include "scanner.h"
#include "ue4.h"
#include "hooks.h"

// ============================================================
// 設定
// ============================================================
struct Config {
    bool   enableConsole   = true;
    bool   dumpAllEvents   = false;
    int    initDelayMs     = 10000;
    std::string logFilePath;

    // 関数名フィルタ
    std::vector<std::string> nameFilters;

    // 手動アドレス
    uintptr_t processEventAddr = 0;
    uintptr_t gnamesAddr       = 0;

    // パターン候補
    std::vector<std::string> pePatterns;
    std::vector<std::string> gnPatterns;
};

static Config g_config;

static void LoadConfig() {
    // config.ini のパスを取得 (DLLと同じディレクトリ)
    char dllPath[MAX_PATH];
    HMODULE hSelf;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&LoadConfig),
        &hSelf
    );
    GetModuleFileNameA(hSelf, dllPath, MAX_PATH);

    // DLLディレクトリに config.ini を探す
    std::string configPath(dllPath);
    size_t lastSlash = configPath.rfind('\\');
    if (lastSlash != std::string::npos) {
        configPath = configPath.substr(0, lastSlash + 1);
    }

    // ログファイルのデフォルトパスもここに設定
    g_config.logFilePath = configPath + "chat_log.txt";

    configPath += "config.ini";

    // INI読み込み
    auto readIniStr = [&](const char* section, const char* key, const char* def) -> std::string {
        char buf[1024];
        GetPrivateProfileStringA(section, key, def, buf, sizeof(buf), configPath.c_str());
        return buf;
    };
    auto readIniInt = [&](const char* section, const char* key, int def) -> int {
        return GetPrivateProfileIntA(section, key, def, configPath.c_str());
    };

    g_config.enableConsole = readIniInt("General", "EnableConsole", 1) != 0;
    g_config.initDelayMs   = readIniInt("General", "InitDelayMs", 10000);
    g_config.dumpAllEvents = readIniInt("Discovery", "DumpAllEvents", 0) != 0;

    std::string logPath = readIniStr("General", "LogFilePath", "");
    if (!logPath.empty()) {
        g_config.logFilePath = logPath;
    }

    // フィルタ
    std::string filters = readIniStr("Discovery", "FunctionNameFilter", "Chat,Message,Say,Broadcast");
    {
        std::istringstream ss(filters);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (!token.empty()) g_config.nameFilters.push_back(token);
        }
    }

    // アドレス
    std::string peAddrStr = readIniStr("Addresses", "ProcessEventAddress", "0");
    std::string gnAddrStr = readIniStr("Addresses", "GNamesAddress", "0");
    g_config.processEventAddr = strtoull(peAddrStr.c_str(), nullptr, 16);
    g_config.gnamesAddr       = strtoull(gnAddrStr.c_str(), nullptr, 16);

    // パターン
    for (int i = 1; i <= 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "Pattern%d", i);
        std::string pat = readIniStr("Patterns", key, "");
        if (!pat.empty()) g_config.pePatterns.push_back(pat);
    }
    for (int i = 1; i <= 5; i++) {
        char key[32];
        snprintf(key, sizeof(key), "GNamesPattern%d", i);
        std::string pat = readIniStr("Patterns", key, "");
        if (!pat.empty()) g_config.gnPatterns.push_back(pat);
    }
}

// ============================================================
// ログ出力
// ============================================================
static std::mutex g_logMutex;
static FILE*      g_logFile = nullptr;

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    // コンソール出力
    if (g_config.enableConsole) {
        printf("[ChatTranslator] ");
        vprintf(fmt, args);
        printf("\n");
    }

    va_end(args);
}

static void LogChat(const char* funcName, const char* message) {
    std::lock_guard<std::mutex> lock(g_logMutex);

    if (!g_logFile) {
        g_logFile = fopen(g_config.logFilePath.c_str(), "a");
        if (!g_logFile) return;
    }

    // タイムスタンプ
    time_t now = time(nullptr);
    struct tm local;
    localtime_s(&local, &now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);

    fprintf(g_logFile, "[%s] [%s] %s\n", timestamp, funcName, message);
    fflush(g_logFile);
}

// ============================================================
// GNames
// ============================================================
static uintptr_t g_gnamesAddr = 0;
static bool      g_gnamesChunked = true; // UE4 4.23+ はチャンクベース

bool hooks::IsGNamesAvailable() {
    return g_gnamesAddr != 0;
}

bool hooks::ResolveFName(int32_t comparisonIndex, char* buf, int bufSize) {
    if (!g_gnamesAddr || bufSize <= 0) return false;

    __try {
        const FNameEntry* entry = nullptr;

        if (g_gnamesChunked) {
            entry = ResolveFNameChunked(g_gnamesAddr, comparisonIndex);
        } else {
            entry = ResolveFNameFlat(g_gnamesAddr, comparisonIndex);
        }

        if (entry) {
            return entry->GetName(buf, bufSize);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // メモリアクセス違反 - GNamesアドレスまたはレイアウトが不正
    }

    return false;
}

static bool FindGNames() {
    // 1. config.ini で手動指定されたアドレスを使用
    if (g_config.gnamesAddr != 0) {
        g_gnamesAddr = g_config.gnamesAddr;
        Log("GNames: config指定アドレス 0x%llX を使用", g_gnamesAddr);
        return true;
    }

    // 2. パターンスキャン
    for (const auto& pattern : g_config.gnPatterns) {
        uintptr_t addr = scanner::FindPatternInModule(nullptr, pattern.c_str());
        if (addr) {
            // パターンが見つかった場合、RIP相対アドレスを解決
            // 典型的には "48 8D 05 XX XX XX XX" (lea rax, [rip+XXXX])
            // オフセット3の位置に相対アドレス、命令サイズは7
            uintptr_t resolved = scanner::ResolveRIPRelative(addr, 3, 7);
            if (resolved) {
                g_gnamesAddr = resolved;
                Log("GNames: パターンスキャンで発見 0x%llX", g_gnamesAddr);
                return true;
            }
        }
    }

    Log("GNames: 検出失敗 - config.ini で手動アドレスを指定してください");
    return false;
}

// ============================================================
// ProcessEvent フック
// ============================================================
static ue4::ProcessEventFn g_originalProcessEvent = nullptr;

// フック関数: 全 ProcessEvent 呼び出しを監視
static void __fastcall HookedProcessEvent(void* thisObj, void* function, void* parms) {
    // 関数名を取得
    if (function && g_gnamesAddr) {
        __try {
            FName funcName = ue4::GetObjectFName(function);
            char nameStr[256] = {};

            if (hooks::ResolveFName(funcName.ComparisonIndex, nameStr, sizeof(nameStr))) {
                bool shouldLog = false;

                if (g_config.dumpAllEvents) {
                    // 探索モード: 全イベントをログ
                    shouldLog = true;
                } else {
                    // フィルタモード: 指定キーワードに一致するもののみ
                    for (const auto& filter : g_config.nameFilters) {
                        if (strstr(nameStr, filter.c_str())) {
                            shouldLog = true;
                            break;
                        }
                    }
                }

                if (shouldLog) {
                    // thisObj のクラス名も取得を試みる
                    char className[256] = "Unknown";
                    void* classObj = *reinterpret_cast<void**>(
                        reinterpret_cast<uintptr_t>(thisObj) + ue4::UOBJECT_CLASS_OFFSET
                    );
                    if (classObj) {
                        FName classNameFN = ue4::GetObjectFName(classObj);
                        hooks::ResolveFName(classNameFN.ComparisonIndex, className, sizeof(className));
                    }

                    // parms からテキストデータの抽出を試みる
                    char textContent[1024] = "";
                    if (parms) {
                        // FString を探す: parms 内の最初の有効な FString を探索
                        // FString は { wchar_t* Data, int32 Count, int32 Max } の構造
                        // Data が有効なポインタで Count > 0 かつ Count < 10000 なら有効
                        for (int offset = 0; offset < 256; offset += 8) {
                            __try {
                                auto fstr = reinterpret_cast<FString*>(
                                    reinterpret_cast<uintptr_t>(parms) + offset
                                );
                                if (fstr->Data && fstr->Count > 0 && fstr->Count < 10000 &&
                                    fstr->Max >= fstr->Count) {
                                    // Data ポインタの有効性チェック
                                    MEMORY_BASIC_INFORMATION mbi;
                                    if (VirtualQuery(fstr->Data, &mbi, sizeof(mbi)) &&
                                        mbi.State == MEM_COMMIT &&
                                        (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
                                        // ワイド文字をマルチバイトに変換
                                        WideCharToMultiByte(CP_UTF8, 0,
                                            fstr->Data, fstr->Count - 1,
                                            textContent, sizeof(textContent) - 1,
                                            nullptr, nullptr);
                                        break;
                                    }
                                }
                            }
                            __except (EXCEPTION_EXECUTE_HANDLER) {
                                continue;
                            }
                        }
                    }

                    // コンソールに出力
                    if (textContent[0]) {
                        Log("[%s::%s] %s", className, nameStr, textContent);
                        LogChat(nameStr, textContent);
                    } else if (g_config.dumpAllEvents) {
                        Log("[%s::%s] (no text data)", className, nameStr);
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // アクセス違反を安全に無視
        }
    }

    // オリジナル関数を呼び出し
    g_originalProcessEvent(thisObj, function, parms);
}

static bool FindAndHookProcessEvent() {
    uintptr_t peAddr = 0;

    // 1. config.ini で手動指定されたアドレスを使用
    if (g_config.processEventAddr != 0) {
        peAddr = g_config.processEventAddr;
        Log("ProcessEvent: config指定アドレス 0x%llX を使用", peAddr);
    }

    // 2. パターンスキャン
    if (peAddr == 0) {
        for (const auto& pattern : g_config.pePatterns) {
            peAddr = scanner::FindPatternInModule(nullptr, pattern.c_str());
            if (peAddr) {
                Log("ProcessEvent: パターン '%s' で発見 0x%llX", pattern.c_str(), peAddr);
                break;
            }
        }
    }

    if (peAddr == 0) {
        Log("ProcessEvent: 検出失敗");
        Log("  対処法:");
        Log("  1. x64dbg でゲームを解析し ProcessEvent のアドレスを特定");
        Log("  2. config.ini の ProcessEventAddress にアドレスを記入");
        Log("  ヒント: 文字列 'Script Msg:' や 'ProcessEvent' を検索");
        return false;
    }

    // MinHook でフック
    MH_STATUS status = MH_CreateHook(
        reinterpret_cast<void*>(peAddr),
        reinterpret_cast<void*>(&HookedProcessEvent),
        reinterpret_cast<void**>(&g_originalProcessEvent)
    );

    if (status != MH_OK) {
        Log("ProcessEvent: フック作成失敗 (MH_STATUS=%d)", status);
        return false;
    }

    status = MH_EnableHook(reinterpret_cast<void*>(peAddr));
    if (status != MH_OK) {
        Log("ProcessEvent: フック有効化失敗 (MH_STATUS=%d)", status);
        return false;
    }

    Log("ProcessEvent: フック成功 (アドレス=0x%llX)", peAddr);
    return true;
}

// ============================================================
// 公開インターフェース
// ============================================================
bool hooks::Init() {
    LoadConfig();

    Log("===================================");
    Log("Foxhole Chat Translator - Stage 1");
    Log("チャットメッセージロガー");
    Log("===================================");

    // MinHook 初期化
    MH_STATUS mhStatus = MH_Initialize();
    if (mhStatus != MH_OK) {
        Log("MinHook 初期化失敗 (status=%d)", mhStatus);
        return false;
    }

    // GNames を検出
    FindGNames();

    // GNames の検証 (FName index 0 = "None")
    if (g_gnamesAddr) {
        char testName[64] = {};
        if (ResolveFName(0, testName, sizeof(testName))) {
            Log("GNames 検証: index 0 = '%s' (%s)",
                testName,
                strcmp(testName, "None") == 0 ? "正常" : "異常 - レイアウト要調整");
        } else {
            Log("GNames 検証失敗 - アドレスまたはレイアウトが不正");
            // チャンクベースとフラットの切り替えを試す
            g_gnamesChunked = !g_gnamesChunked;
            if (ResolveFName(0, testName, sizeof(testName))) {
                Log("GNames 検証リトライ (%s モード): index 0 = '%s'",
                    g_gnamesChunked ? "chunked" : "flat", testName);
            } else {
                Log("GNames 両方のモードで失敗 - 手動調査が必要");
                g_gnamesAddr = 0;
            }
        }
    }

    // ProcessEvent をフック
    if (!FindAndHookProcessEvent()) {
        Log("ProcessEvent のフックに失敗しました");
        Log("チャットログ機能は無効です");
        return false;
    }

    Log("初期化完了 - ログ出力先: %s", g_config.logFilePath.c_str());
    if (g_config.dumpAllEvents) {
        Log("警告: 探索モード有効 - パフォーマンス低下に注意");
    }

    return true;
}

void hooks::Shutdown() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }

    Log("シャットダウン完了");
}
