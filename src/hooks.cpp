// ============================================================
// hooks.cpp - GNames解決 + チャットログ (ワーカーDLL側)
// ProcessEventフックはversion.dll側で永続管理される。
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
static FILE*      g_debugLogFile = nullptr;
static std::string g_debugLogPath;

// 重複抑制用
static char  g_lastLogMsg[512] = {};
static int   g_lastLogRepeat = 0;

// デバッグログファイルを確保 (ロック取得済み前提)
static void EnsureDebugLogFile() {
    if (!g_debugLogFile && !g_debugLogPath.empty()) {
        g_debugLogFile = fopen(g_debugLogPath.c_str(), "w");
    }
    if (!g_debugLogFile) {
        char dllPath[MAX_PATH];
        HMODULE hSelf;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&EnsureDebugLogFile), &hSelf);
        GetModuleFileNameA(hSelf, dllPath, MAX_PATH);
        g_debugLogPath = std::string(dllPath);
        size_t pos = g_debugLogPath.rfind('\\');
        if (pos != std::string::npos) g_debugLogPath = g_debugLogPath.substr(0, pos + 1);
        g_debugLogPath += "debug_log.txt";
        g_debugLogFile = fopen(g_debugLogPath.c_str(), "w");
    }
}

// 繰り返し行をフラッシュ (ロック取得済み前提)
static void FlushRepeatLine() {
    if (g_lastLogRepeat > 0) {
        char repeatMsg[64];
        snprintf(repeatMsg, sizeof(repeatMsg), "  ... 同上 x%d 回", g_lastLogRepeat);
        if (g_config.enableConsole) {
            printf("[ChatTranslator] %s\n", repeatMsg);
        }
        EnsureDebugLogFile();
        if (g_debugLogFile) {
            fprintf(g_debugLogFile, "[ChatTranslator] %s\n", repeatMsg);
            fflush(g_debugLogFile);
        }
        g_lastLogRepeat = 0;
    }
}

static void Log(const char* fmt, ...) {
    // まずメッセージを組み立て
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_logMutex);

    // 前回と同じ内容なら抑制
    if (strcmp(buf, g_lastLogMsg) == 0) {
        g_lastLogRepeat++;
        return;
    }

    // 新しいメッセージ: 溜まった繰り返しをフラッシュ
    FlushRepeatLine();

    // 今回のメッセージを記録・出力
    strncpy(g_lastLogMsg, buf, sizeof(g_lastLogMsg) - 1);
    g_lastLogMsg[sizeof(g_lastLogMsg) - 1] = '\0';

    if (g_config.enableConsole) {
        printf("[ChatTranslator] %s\n", buf);
    }

    EnsureDebugLogFile();
    if (g_debugLogFile) {
        fprintf(g_debugLogFile, "[ChatTranslator] %s\n", buf);
        fflush(g_debugLogFile);
    }
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
static int       g_gnamesBlockArrayOffset = 0x40;
static int       g_gnamesNameOffset = 2; // FNameEntryHeader(2) の後に名前
static int       g_gnamesStride = 2;
static int       g_gnamesHeaderShift = 1; // Length取得シフト: >>1(4.23) or >>6(4.25+)
static std::vector<uintptr_t> g_gnamesCandidates; // 遅延リトライ用候補
static int       g_gnamesRetryCount = 0;
static const int GNAMES_MAX_RETRIES = 20; // 最大リトライ回数 (約60秒分)
static uintptr_t g_hookedPEAddr = 0; // フック済みProcessEventアドレス

void hooks::SetHookedPEAddress(uintptr_t addr) {
    g_hookedPEAddr = addr;
    Log("フック済みPEアドレス設定: 0x%llX", addr);
}

bool hooks::IsGNamesAvailable() {
    return g_gnamesAddr != 0;
}

// メモリの有効性チェック
static bool IsReadableMemory(const void* ptr, size_t size = 8) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_WRITECOPY)) != 0;
}

// メモリ内容を16進ダンプ
static void DumpMemory(const char* label, uintptr_t addr, int bytes) {
    Log("%s (0x%llX):", label, addr);
    __try {
        if (!IsReadableMemory(reinterpret_cast<void*>(addr))) {
            Log("  (読み取り不可)");
            return;
        }
        for (int row = 0; row < bytes; row += 16) {
            char hex[80] = {};
            char ascii[20] = {};
            int pos = 0;
            for (int col = 0; col < 16 && (row + col) < bytes; col++) {
                uint8_t b = *reinterpret_cast<uint8_t*>(addr + row + col);
                pos += sprintf(hex + pos, "%02X ", b);
                ascii[col] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
            }
            Log("  +0x%03X: %-48s  %s", row, hex, ascii);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("  (アクセス違反)");
    }
}

// "None" 文字列をブロック先頭付近で検索
static bool CheckBlockForNone(uintptr_t block0, int& outNameOffset) {
    if (!block0 || !IsReadableMemory(reinterpret_cast<void*>(block0))) return false;
    __try {
        for (int off = 0; off <= 12; off += 2) {
            const char* ptr = reinterpret_cast<const char*>(block0 + off);
            if (ptr[0] == 'N' && ptr[1] == 'o' && ptr[2] == 'n' && ptr[3] == 'e') {
                outNameOffset = off;
                return true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// GNamesレイアウトを自動検出
// ブロック配列のオフセットを総当たりで試し、entry 0 が "None" になるものを探す
static bool ProbeGNamesLayout(uintptr_t gnamesBase) {
    Log("GNames: レイアウト探索開始 (base=0x%llX)", gnamesBase);

    // 診断: 先頭128バイトをダンプ
    DumpMemory("GNames base", gnamesBase, 128);

    // 方法1: gnamesBase + offset を直接ブロック配列として試す (UE4 4.23+ FNamePool)
    // 範囲: 0x00 ~ 0x400 (FNamePool のハッシュシャードが大きい場合がある)
    for (int blockOff = 0x00; blockOff <= 0x400; blockOff += 8) {
        __try {
            if (!IsReadableMemory(reinterpret_cast<void*>(gnamesBase + blockOff))) continue;

            uintptr_t candidate = *reinterpret_cast<uintptr_t*>(gnamesBase + blockOff);
            if (!candidate) continue;

            int nameOff = 0;
            if (CheckBlockForNone(candidate, nameOff)) {
                g_gnamesBlockArrayOffset = blockOff;
                g_gnamesNameOffset = nameOff;
                g_gnamesStride = 2;
                Log("GNames: レイアウト検出成功 (方法1: 直接ブロック配列)");
                Log("  blockArrayOffset=0x%X, nameOffset=%d", blockOff, nameOff);
                return true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
    }

    // 方法2: gnamesBase をポインタとしてデリファレンス (間接参照)
    // GNames がポインタ変数の場合: *gnamesBase -> 実際のテーブル
    __try {
        if (IsReadableMemory(reinterpret_cast<void*>(gnamesBase))) {
            uintptr_t deref = *reinterpret_cast<uintptr_t*>(gnamesBase);
            if (deref && IsReadableMemory(reinterpret_cast<void*>(deref))) {
                Log("GNames: 間接参照を試行 *0x%llX = 0x%llX", gnamesBase, deref);
                DumpMemory("GNames deref", deref, 128);

                for (int blockOff = 0x00; blockOff <= 0x400; blockOff += 8) {
                    __try {
                        if (!IsReadableMemory(reinterpret_cast<void*>(deref + blockOff))) continue;

                        uintptr_t candidate = *reinterpret_cast<uintptr_t*>(deref + blockOff);
                        if (!candidate) continue;

                        int nameOff = 0;
                        if (CheckBlockForNone(candidate, nameOff)) {
                            g_gnamesBlockArrayOffset = blockOff;
                            g_gnamesNameOffset = nameOff;
                            g_gnamesStride = 2;
                            // 実際のアドレスを更新 (deref をベースとして使う)
                            Log("GNames: レイアウト検出成功 (方法2: ポインタ間接参照)");
                            Log("  実アドレス=0x%llX, blockArrayOffset=0x%X, nameOffset=%d",
                                deref, blockOff, nameOff);
                            // gnamesBase を deref に書き換え (呼び出し元で設定)
                            g_gnamesAddr = deref;
                            return true;
                        }
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    // 方法3: 旧スタイル TNameEntryArray - ポインタ配列
    // *gnamesBase -> FNameEntry** array, array[0] -> FNameEntry for "None"
    __try {
        if (IsReadableMemory(reinterpret_cast<void*>(gnamesBase))) {
            uintptr_t arrayPtr = *reinterpret_cast<uintptr_t*>(gnamesBase);
            if (arrayPtr && IsReadableMemory(reinterpret_cast<void*>(arrayPtr))) {
                uintptr_t entry0Ptr = *reinterpret_cast<uintptr_t*>(arrayPtr);
                if (entry0Ptr && IsReadableMemory(reinterpret_cast<void*>(entry0Ptr))) {
                    int nameOff = 0;
                    if (CheckBlockForNone(entry0Ptr, nameOff)) {
                        Log("GNames: レイアウト検出成功 (方法3: 旧TNameEntryArray)");
                        Log("  array=0x%llX, entry0=0x%llX, nameOffset=%d",
                            arrayPtr, entry0Ptr, nameOff);
                        // この場合は特別なフラグを設定
                        g_gnamesAddr = gnamesBase;
                        g_gnamesChunked = false;
                        g_gnamesNameOffset = nameOff;
                        return true;
                    }
                    // entry0 の中身もダンプ
                    DumpMemory("GNames entry0 (旧スタイル候補)", entry0Ptr, 32);
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    // 方法4: "None" 文字列をgnamesBase付近で直接検索 (最終手段)
    Log("GNames: 'None' 文字列をbase付近で広域検索...");
    __try {
        for (int off = 0; off < 0x1000; off++) {
            if (!IsReadableMemory(reinterpret_cast<void*>(gnamesBase + off))) {
                off = (off + 0x1000) & ~0xFFF; // 次のページへ
                continue;
            }
            const char* p = reinterpret_cast<const char*>(gnamesBase + off);
            if (p[0] == 'N' && p[1] == 'o' && p[2] == 'n' && p[3] == 'e' &&
                (p[4] == '\0' || p[4] < 0x20)) {
                Log("GNames: 'None' 文字列発見 offset=0x%X (base+0x%X)", off, off);
                DumpMemory("None付近", gnamesBase + off - 16, 48);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    return false;
}

bool hooks::ResolveFName(int32_t comparisonIndex, char* buf, int bufSize) {
    if (!g_gnamesAddr || bufSize <= 0) return false;

    __try {
        if (g_gnamesChunked) {
            // 自動検出されたオフセットを使用
            int block  = comparisonIndex >> 16;
            int offset = comparisonIndex & 0xFFFF;

            uintptr_t* blocks = reinterpret_cast<uintptr_t*>(
                g_gnamesAddr + g_gnamesBlockArrayOffset);
            if (!blocks[block]) return false;

            uintptr_t entryAddr = blocks[block] +
                static_cast<uintptr_t>(offset) * g_gnamesStride;

            // nameOffset の位置から名前文字列を読み取る
            const char* namePtr = reinterpret_cast<const char*>(
                entryAddr + g_gnamesNameOffset);

            // g_gnamesNameOffset >= 2 の場合、HeaderからLengthを取得
            int len = 0;
            if (g_gnamesNameOffset >= 2) {
                uint16_t hdrData = *reinterpret_cast<const uint16_t*>(entryAddr);
                len = hdrData >> g_gnamesHeaderShift;
            } else {
                // ヘッダーなし - NUL終端まで読む (最大bufSize-1)
                len = 0;
                while (len < bufSize - 1 && namePtr[len] >= 0x20 && namePtr[len] < 0x7F) {
                    len++;
                }
            }

            if (len <= 0 || len >= bufSize) return false;
            memcpy(buf, namePtr, len);
            buf[len] = 0;
            return true;
        } else {
            const FNameEntry* entry = ResolveFNameFlat(g_gnamesAddr, comparisonIndex);
            if (entry) {
                return entry->GetName(buf, bufSize);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // メモリアクセス違反
    }

    return false;
}

static bool FindGNames() {
    // 1. config.ini で手動指定されたアドレスを使用
    if (g_config.gnamesAddr != 0) {
        uintptr_t candidate = g_config.gnamesAddr;
        Log("GNames: config指定アドレス 0x%llX を使用", candidate);
        if (ProbeGNamesLayout(candidate)) {
            if (g_gnamesAddr == 0) { g_gnamesAddr = candidate; g_gnamesChunked = true; }
            return true;
        }
        return false;
    }

    // 2. FNamePool Block[0] 検索
    // UE4 4.23+のFNamePoolでは:
    //   Entry[0] = "None" (4文字), Entry[1] = "ByteProperty" (12文字)
    //   各エントリ: FNameEntryHeader(2bytes) + 文字列(null無し)
    //   ヘッダー形式はバージョン依存だが、"None"+"ByteProperty"の直列配置は不変
    //
    // 検索パターン: "None" + 2bytes(次ヘッダー) + "ByteProperty"
    //   = 4E 6F 6E 65 ?? ?? 42 79 74 65 50 72 6F 70 65 72 74 79
    // WITH_CASE_PRESERVING_NAMEビルドでは各エントリにComparisonId(4bytes)が付き:
    //   "None" + 2bytes(pad) + 4bytes(CompId) + 2bytes(hdr) + "ByteProperty"
    //   この場合 gap = 8
    Log("GNames: FNamePool Block[0] 検索開始...");

    HMODULE gameModule = GetModuleHandleA(nullptr);
    uintptr_t moduleBase = reinterpret_cast<uintptr_t>(gameModule);
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase);
    IMAGE_NT_HEADERS* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(moduleBase + dos->e_lfanew);
    DWORD moduleSize = nt->OptionalHeader.SizeOfImage;

    const uint8_t byteProperty[] = { 'B','y','t','e','P','r','o','p','e','r','t','y' };
    const int bpLen = 12;

    struct BlockCandidate {
        uintptr_t noneAddr;     // "None" テキストのアドレス
        uintptr_t blockStart;   // Block[0] 先頭推定 (= noneAddr - headerSize)
        uintptr_t allocBase;
        uint16_t  noneHeader;   // "None" の FNameEntryHeader 値
        uint16_t  bpHeader;     // "ByteProperty" の FNameEntryHeader 値
        int       gap;          // "None"末尾から"ByteProperty"ヘッダーまでのバイト数
        int       headerSize;   // FNameEntryHeader のバイト数 (2)
        bool      hasCompId;    // WITH_CASE_PRESERVING_NAME
    };

    BlockCandidate candidates[16];
    int candidateCount = 0;

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t scanAddr = 0x10000;

    while (candidateCount < 16 &&
           VirtualQuery(reinterpret_cast<void*>(scanAddr), &mbi, sizeof(mbi))) {
        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;

        if (mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
            mbi.RegionSize >= 64) {

            uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            bool isModule = (regionStart >= moduleBase && regionStart < moduleBase + moduleSize);

            if (!isModule) {
                uint8_t* base = reinterpret_cast<uint8_t*>(mbi.BaseAddress);
                // Block[0]先頭付近に"None"があるはず (最大offset 16 = CompId含む場合)
                size_t searchEnd = (mbi.RegionSize < 64) ? mbi.RegionSize : 64;

                for (size_t i = 0; i + 4 <= searchEnd; i += 2) {
                    if (base[i] != 'N' || base[i+1] != 'o' ||
                        base[i+2] != 'n' || base[i+3] != 'e') continue;

                    // "None" found at base+i, check for "ByteProperty" after it
                    // gap=2: standard (header 2 bytes only)
                    // gap=8: WITH_CASE_PRESERVING_NAME (header 2 + ComparisonId 4 + pad 2)
                    for (int gap : {2, 8}) {
                        size_t bpTextOff = i + 4 + gap;
                        if (bpTextOff + bpLen > mbi.RegionSize) continue;

                        if (memcmp(base + bpTextOff, byteProperty, bpLen) == 0) {
                            // FNamePool Block[0] 発見!
                            uint16_t noneHdr = (i >= 2) ? *reinterpret_cast<uint16_t*>(base + i - 2) : 0;
                            uint16_t bpHdr = *reinterpret_cast<uint16_t*>(base + bpTextOff - 2);

                            bool hasCompId = (gap == 8);
                            int headerSize = 2;
                            uintptr_t blockStart = regionStart;
                            if (hasCompId) {
                                // ComparisonId(4) + Header(2) + "None"(4) = offset 6
                                blockStart = regionStart + i - 6;
                            } else {
                                // Header(2) + "None"(4) = offset 2
                                blockStart = regionStart + i - 2;
                            }

                            // ログ
                            Log("GNames: Block[0] 発見! allocBase=0x%llX gap=%d",
                                (uintptr_t)mbi.AllocationBase, gap);
                            Log("  'None' at regionBase+0x%X, hdr=0x%04X", (unsigned)i, noneHdr);
                            Log("  'ByteProperty' at regionBase+0x%X, hdr=0x%04X",
                                (unsigned)bpTextOff, bpHdr);
                            Log("  blockStart=0x%llX, hasCompId=%d", blockStart, hasCompId);
                            DumpMemory("Block[0]", regionStart, 128);

                            candidates[candidateCount].noneAddr = regionStart + i;
                            candidates[candidateCount].blockStart = blockStart;
                            candidates[candidateCount].allocBase = (uintptr_t)mbi.AllocationBase;
                            candidates[candidateCount].noneHeader = noneHdr;
                            candidates[candidateCount].bpHeader = bpHdr;
                            candidates[candidateCount].gap = gap;
                            candidates[candidateCount].headerSize = headerSize;
                            candidates[candidateCount].hasCompId = hasCompId;
                            candidateCount++;
                            goto next_region; // 1リージョンにつき1候補
                        }
                    }
                }
            }
        }
        next_region:
        if (regionEnd <= scanAddr) break;
        scanAddr = regionEnd;
    }

    Log("GNames: Block[0] 候補: %d 件", candidateCount);

    // ヘッダー形式を解析 (最初の候補から)
    int detectedShift = 6; // デフォルト: UE4 4.25+ (Len<<6)
    if (candidateCount > 0) {
        uint16_t h = candidates[0].noneHeader;
        // UE4 4.23-4.24: (Len<<1)|bIsWide → "None"=0x0008
        // UE4 4.25+:     (Len<<6)|(ProbeHash<<1)|bIsWide → "None"=0x01XX
        if (h == 0x0008) {
            Log("GNames: ヘッダー形式 = (Len<<1) [UE4 4.23-4.24]");
            detectedShift = 1;
        } else if ((h >> 6) == 4) {
            Log("GNames: ヘッダー形式 = (Len<<6|Hash<<1|Wide) [UE4 4.25+], hash=%d",
                (h >> 1) & 0x1F);
            detectedShift = 6;
        } else {
            // ヘッダー値から推定
            if ((h >> 1) == 4) detectedShift = 1;
            else if ((h >> 6) == 4) detectedShift = 6;
            else detectedShift = 6; // フォールバック
            Log("GNames: ヘッダー形式不明 (h=0x%04X), shift=%d を使用", h, detectedShift);
        }
    }

    // Step 2: 各Block[0]候補のポインタをモジュール内で検索
    for (int ci = 0; ci < candidateCount; ci++) {
        // Block[0]ポインタ候補: allocBase, blockStart  
        uintptr_t targets[] = { candidates[ci].allocBase, candidates[ci].blockStart };
        for (int ti = 0; ti < 2; ti++) {
            uintptr_t target = targets[ti];
            if (target == 0) continue;

            for (uintptr_t s = moduleBase; s < moduleBase + moduleSize - 8; s += 8) {
                __try {
                    if (*reinterpret_cast<uintptr_t*>(s) != target) continue;

                    Log("GNames: モジュール+0x%llX にBlock[0]ポインタ 0x%llX 発見",
                        s - moduleBase, target);

                    // FNameEntryAllocator: [Lock] + CurrentBlock(4) + CurrentByteCursor(4) + Blocks[]
                    // Blocks[0] は s にある。その前にCurrentBlock+Cursorがある。
                    for (int preSize = 8; preSize <= 64; preSize += 8) {
                        uintptr_t poolBase = s - preSize;
                        if (poolBase < moduleBase) continue;

                        int32_t curBlock  = *reinterpret_cast<int32_t*>(s - 8);
                        int32_t curCursor = *reinterpret_cast<int32_t*>(s - 4);

                        if (curBlock > 0 && curBlock < 8192 &&
                            curCursor > 0 && curCursor < 0x100000) {

                            uintptr_t block1 = *reinterpret_cast<uintptr_t*>(s + 8);
                            if (block1 != 0 && IsReadableMemory(reinterpret_cast<void*>(block1))) {
                                Log("GNames: FNamePool 発見!");
                                Log("  Blocks[0]=0x%llX at module+0x%llX",
                                    target, s - moduleBase);
                                Log("  CurrentBlock=%d, CurrentByteCursor=%d", curBlock, curCursor);
                                DumpMemory("Pool structure", s - 16, 64);

                                // ヘッダー形式を確定してResolveFNameの設定
                                g_gnamesAddr = s; // Blocks[] 配列の先頭
                                g_gnamesBlockArrayOffset = 0; // s自体がBlocks[0]へのポインタ
                                g_gnamesChunked = true;
                                g_gnamesHeaderShift = detectedShift;

                                // "None"テキストのBlock[0]内オフセット = headerSize (+ compId if present)
                                int nameOff = candidates[ci].headerSize;
                                if (candidates[ci].hasCompId) nameOff += 4; // ComparisonId分
                                g_gnamesNameOffset = nameOff;

                                // stride: ComparisonIndex の Offset 値 → 実バイトオフセットの変換
                                // UE4標準では stride=2 (2バイトアライメント)
                                g_gnamesStride = 2;

                                // Block[0]のアドレスが allocBase なら blockArrayOffset 調整不要
                                // 検証: Block[0] + nameOff に "None" があるか
                                __try {
                                    uint8_t* verifyPtr = reinterpret_cast<uint8_t*>(target + nameOff);
                                    if (verifyPtr[0]=='N' && verifyPtr[1]=='o' && 
                                        verifyPtr[2]=='n' && verifyPtr[3]=='e') {
                                        Log("  検証OK: Block[0]+%d = 'None'", nameOff);
                                    } else {
                                        Log("  検証NG: Block[0]+%d != 'None', 調整中...", nameOff);
                                        // noneAddr - target で直接計算
                                        nameOff = static_cast<int>(candidates[ci].noneAddr - target);
                                        g_gnamesNameOffset = nameOff;
                                        Log("  再計算 nameOffset=%d", nameOff);
                                    }
                                } __except(EXCEPTION_EXECUTE_HANDLER) {}

                                return true;
                            }
                        }
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            }
        }
    }

    // 3. フォールバック: 旧パターンスキャン方式
    Log("GNames: メモリスキャン失敗、パターンスキャンにフォールバック...");
    for (const auto& pattern : g_config.gnPatterns) {
        uintptr_t matchAddr = scanner::FindPatternInModule(nullptr, pattern.c_str());
        if (!matchAddr) continue;

        g_gnamesCandidates.clear();
        for (int i = 0; i < 24; i++) {
            uint8_t b0 = *reinterpret_cast<uint8_t*>(matchAddr + i);
            uint8_t b1 = *reinterpret_cast<uint8_t*>(matchAddr + i + 1);
            uint8_t b2 = *reinterpret_cast<uint8_t*>(matchAddr + i + 2);

            if ((b0 == 0x48 || b0 == 0x4C) && b1 == 0x8D && (b2 & 0xC7) == 0x05) {
                uintptr_t resolved = scanner::ResolveRIPRelative(matchAddr + i, 3, 7);
                if (resolved) {
                    g_gnamesCandidates.push_back(resolved);
                }
            }
        }

        for (uintptr_t candidate : g_gnamesCandidates) {
            if (ProbeGNamesLayout(candidate)) {
                if (g_gnamesAddr == 0) { g_gnamesAddr = candidate; g_gnamesChunked = true; }
                return true;
            }
        }
    }

    Log("GNames: 全方式で検出失敗");
    return false;
}

// ============================================================
// FNamePool 検索: 名前からComparisonIndexを逆引き
// ============================================================
static int32_t FindFNameIndex(const char* targetName) {
    if (!g_gnamesAddr || !targetName) return -1;
    int targetLen = (int)strlen(targetName);
    if (targetLen <= 0 || targetLen > 200) return -1;

    __try {
        // FNamePool のブロック配列を走査
        uintptr_t* blocks = reinterpret_cast<uintptr_t*>(g_gnamesAddr + g_gnamesBlockArrayOffset);

        // CurrentBlock を取得 (Blocks配列の前にある)
        int32_t currentBlock = *reinterpret_cast<int32_t*>(g_gnamesAddr - 8);
        if (currentBlock <= 0 || currentBlock > 8192) currentBlock = 64;

        for (int block = 0; block <= currentBlock; block++) {
            uintptr_t blockBase = blocks[block];
            if (!blockBase || !IsReadableMemory(reinterpret_cast<void*>(blockBase))) continue;

            // ブロック内エントリを走査 (最大64KB/ブロック)
            int offset = 0;
            int maxOffset = (block == 0) ? 0x10000 : 0x10000; // FNameEntryAllocator block size
            while (offset < maxOffset) {
                uintptr_t entryAddr = blockBase + offset;
                if (!IsReadableMemory(reinterpret_cast<void*>(entryAddr))) break;

                uint16_t hdr = *reinterpret_cast<uint16_t*>(entryAddr);
                int len = hdr >> g_gnamesHeaderShift;
                if (len <= 0 || len > 1024) break; // 無効なヘッダー → 終端

                bool isWide = (hdr & 1) != 0;
                int nameBytes = isWide ? (len * 2) : len;
                int entrySize = g_gnamesNameOffset + nameBytes;
                // 2バイトアライメント
                entrySize = (entrySize + 1) & ~1;

                if (!isWide && len == targetLen) {
                    const char* namePtr = reinterpret_cast<const char*>(entryAddr + g_gnamesNameOffset);
                    if (memcmp(namePtr, targetName, len) == 0) {
                        // ComparisonIndex = (block << 16) | (offset / stride)
                        int32_t ci = (block << 16) | (offset / g_gnamesStride);
                        return ci;
                    }
                }

                offset += entrySize;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}

// チャット関連FName CI を格納
static int32_t g_chatFNameCIs[16] = {};
static int     g_chatFNameCount = 0;
static const char* g_chatFNames[] = {
    "ClientChatMessage", "ClientChatMessageWithTag",
    "ServerChat", "ServerVoteOnChatMessage",
    "OnChatMessage", nullptr
};

static void SearchChatFNames() {
    Log("=== チャット関連FName検索 ===");
    g_chatFNameCount = 0;
    for (int i = 0; g_chatFNames[i] && g_chatFNameCount < 16; i++) {
        int32_t ci = FindFNameIndex(g_chatFNames[i]);
        if (ci >= 0) {
            char verify[128] = {};
            hooks::ResolveFName(ci, verify, sizeof(verify));
            Log("  '%s' → CI=%d (0x%08X) [verify='%s']", g_chatFNames[i], ci, ci, verify);
            g_chatFNameCIs[g_chatFNameCount++] = ci;
        } else {
            Log("  '%s' → 未検出", g_chatFNames[i]);
        }
    }
    Log("  合計: %d 件のチャットFName検出", g_chatFNameCount);
}

// ============================================================
// ProcessEvent コールバック (version.dll のフックから呼ばれる)
// ============================================================

// GNames遅延リトライ
static void TryLazyGNamesResolve() {
    if (g_gnamesAddr != 0 || g_gnamesRetryCount >= GNAMES_MAX_RETRIES) return;

    static int callCounter = 0;
    if (++callCounter % 30000 != 0) return; // ~0.75秒ごと (@40k calls/s)
    g_gnamesRetryCount++;

    Log("GNames: 遅延リトライ #%d (メモリスキャン方式)", g_gnamesRetryCount);

    if (FindGNames()) {
        char testName[64] = {};
        if (hooks::ResolveFName(0, testName, sizeof(testName))) {
            Log("GNames: 遅延リトライで検出成功! index 0 = '%s'", testName);
        }
    }
}

void hooks::OnProcessEvent(void* thisObj, void* function, void* parms) {
    // デバッグカウンタ
    static int peCallCount = 0;
    peCallCount++;
    if (peCallCount == 1 || peCallCount == 1000 || peCallCount == 100000 || peCallCount == 1000000) {
        Log("ProcessEvent コールバック #%d (GNames=%s)",
            peCallCount, g_gnamesAddr ? "有効" : "無効");
    }

    // GNames 遅延リトライ
    if (!g_gnamesAddr) {
        TryLazyGNamesResolve();
    }

    // === チャットCI早期検出 (全フィルター前) ===
    // vtable/ClassPrivateチェックを通過しないRPCも検出するため、
    // function と thisObj の +0x18 を直接チェック
    if (g_chatFNameCount > 0 && function && g_gnamesAddr) {
        __try {
            uintptr_t funcAddr = reinterpret_cast<uintptr_t>(function);
            uintptr_t thisAddr = reinterpret_cast<uintptr_t>(thisObj);
            int32_t funcCI = -1, thisCI = -1;
            if (funcAddr > 0x10000 && IsReadableMemory(function)) {
                funcCI = *reinterpret_cast<int32_t*>(funcAddr + ue4::UOBJECT_NAME_OFFSET);
            }
            if (thisAddr > 0x10000 && IsReadableMemory(thisObj)) {
                thisCI = *reinterpret_cast<int32_t*>(thisAddr + ue4::UOBJECT_NAME_OFFSET);
            }
            for (int i = 0; i < g_chatFNameCount; i++) {
                if (funcCI == g_chatFNameCIs[i] || thisCI == g_chatFNameCIs[i]) {
                    char fn[128] = "?", tn[128] = "?";
                    if (funcCI > 0) hooks::ResolveFName(funcCI, fn, sizeof(fn));
                    if (thisCI > 0) hooks::ResolveFName(thisCI, tn, sizeof(tn));
                    Log("*** CHAT CI HIT *** func='%s'(CI=%d) this='%s'(CI=%d) parms=%s",
                        fn, funcCI, tn, thisCI, parms ? "有" : "無");
                    if (function && IsReadableMemory(function))
                        DumpMemory("chat function obj", funcAddr, 64);
                    if (thisObj && IsReadableMemory(thisObj))
                        DumpMemory("chat thisObj", thisAddr, 64);
                    if (parms) DumpMemory("chat parms", reinterpret_cast<uintptr_t>(parms), 256);
                    break;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // 関数名を取得
    if (function && g_gnamesAddr) {
        __try {
            // UFunction であることを検証: vtableポインタがモジュール内にあるか
            static uintptr_t s_moduleBase = 0, s_moduleEnd = 0;
            if (!s_moduleBase) {
                HMODULE hMod = GetModuleHandleA(nullptr);
                s_moduleBase = reinterpret_cast<uintptr_t>(hMod);
                auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hMod);
                auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(s_moduleBase + dos->e_lfanew);
                s_moduleEnd = s_moduleBase + nt->OptionalHeader.SizeOfImage;
            }
            uintptr_t funcAddr = reinterpret_cast<uintptr_t>(function);
            if (funcAddr < 0x10000 || !IsReadableMemory(function)) goto pe_end;
            uintptr_t vtPtr = *reinterpret_cast<uintptr_t*>(funcAddr);
            // vtableがモジュール内 (.rdata) にない場合は UObject ではない → スキップ
            if (vtPtr < s_moduleBase || vtPtr >= s_moduleEnd) goto pe_end;

            // UFunction ClassPrivate 検証
            // ClassPrivate->NamePrivate を確認し、UFunction系のみ通す
            {
                uintptr_t classPtr = *reinterpret_cast<uintptr_t*>(funcAddr + ue4::UOBJECT_CLASS_OFFSET);
                if (!classPtr || !IsReadableMemory(reinterpret_cast<void*>(classPtr))) goto pe_end;
                int32_t classNameCI = *reinterpret_cast<int32_t*>(classPtr + ue4::UOBJECT_NAME_OFFSET);
                char classCheckBuf[64] = {};
                if (!hooks::ResolveFName(classNameCI, classCheckBuf, sizeof(classCheckBuf))) goto pe_end;
                // 許可: "Function", "DelegateFunction", "SparseDelegateFunction"
                bool isFunc = (strstr(classCheckBuf, "Function") != nullptr);
                // 診断: 最初の30件の異なるクラス名をログ
                static char rejectedClasses[30][64] = {};
                static int rejectedClassCount = 0;
                if (!isFunc && rejectedClassCount < 30) {
                    bool already = false;
                    for (int i = 0; i < rejectedClassCount; i++) {
                        if (strcmp(rejectedClasses[i], classCheckBuf) == 0) { already = true; break; }
                    }
                    if (!already) {
                        // このクラスの関数名も取得
                        char rejFuncName[64] = "?";
                        FName fn = ue4::GetObjectFName(function);
                        hooks::ResolveFName(fn.ComparisonIndex, rejFuncName, sizeof(rejFuncName));
                        strncpy(rejectedClasses[rejectedClassCount], classCheckBuf, 63);
                        rejectedClassCount++;
                        Log("[ClassReject #%d] class='%s' funcName='%s'", rejectedClassCount, classCheckBuf, rejFuncName);
                    }
                }
                if (!isFunc) goto pe_end;
            }

            FName funcName = ue4::GetObjectFName(function);
            char nameStr[256] = {};

            if (hooks::ResolveFName(funcName.ComparisonIndex, nameStr, sizeof(nameStr))) {
                // ユニーク関数名コレクター (即時ログ方式)
                static char uniqueNames[2000][64] = {};
                static int uniqueCount = 0;
                // "None" はスキップ
                if (uniqueCount < 2000 && strcmp(nameStr, "None") != 0) {
                    bool found = false;
                    for (int i = 0; i < uniqueCount; i++) {
                        if (strcmp(uniqueNames[i], nameStr) == 0) { found = true; break; }
                    }
                    if (!found) {
                        strncpy(uniqueNames[uniqueCount], nameStr, 63);
                        uniqueNames[uniqueCount][63] = 0;
                        uniqueCount++;
                        // 新しいユニーク名を即座にログ
                        Log("[NEW #%d @%dk] %s", uniqueCount, peCallCount/1000, nameStr);
                    }
                }

                bool shouldLog = false;

                if (g_config.dumpAllEvents) {
                    shouldLog = true;
                } else {
                    for (const auto& filter : g_config.nameFilters) {
                        if (strstr(nameStr, filter.c_str())) {
                            shouldLog = true;
                            break;
                        }
                    }
                }

                if (shouldLog) {
                    char className[256] = "Unknown";
                    // thisObj → ClassPrivate → FName でクラス名取得
                    __try {
                        void* classObj = *reinterpret_cast<void**>(
                            reinterpret_cast<uintptr_t>(thisObj) + ue4::UOBJECT_CLASS_OFFSET
                        );
                        if (classObj && IsReadableMemory(classObj)) {
                            FName classNameFN = ue4::GetObjectFName(classObj);
                            hooks::ResolveFName(classNameFN.ComparisonIndex, className, sizeof(className));
                        }
                        // classObj から取れなければ function の Outer を試行
                        if (strcmp(className, "Unknown") == 0) {
                            // function → OuterPrivate(+0x20) のFNameを取得
                            void* outer = *reinterpret_cast<void**>(
                                reinterpret_cast<uintptr_t>(function) + ue4::UOBJECT_OUTER_OFFSET
                            );
                            if (outer && IsReadableMemory(outer)) {
                                FName outerName = ue4::GetObjectFName(outer);
                                hooks::ResolveFName(outerName.ComparisonIndex, className, sizeof(className));
                            }
                        }
                        // まだ取れなければ - 診断
                        static int classDiag = 0;
                        if (strcmp(className, "Unknown") == 0 && classDiag++ < 3) {
                            uintptr_t classPtr = *reinterpret_cast<uintptr_t*>(
                                reinterpret_cast<uintptr_t>(thisObj) + ue4::UOBJECT_CLASS_OFFSET);
                            Log("  クラス名解決不可: thisObj=0x%llX classPtr=0x%llX readable=%d",
                                (uintptr_t)thisObj, classPtr,
                                classPtr ? IsReadableMemory(reinterpret_cast<void*>(classPtr)) : 0);
                        }
                    } __except(EXCEPTION_EXECUTE_HANDLER) {}

                    // テキスト抽出: parms内のFStringを探す
                    char textContent[1024] = "";
                    if (parms) {
                        for (int offset = 0; offset < 256; offset += 8) {
                            __try {
                                auto fstr = reinterpret_cast<FString*>(
                                    reinterpret_cast<uintptr_t>(parms) + offset
                                );
                                if (!fstr->Data || fstr->Count <= 1 || fstr->Count > 10000 ||
                                    fstr->Max < fstr->Count || fstr->Max > 100000) continue;

                                MEMORY_BASIC_INFORMATION mbi;
                                if (!VirtualQuery(fstr->Data, &mbi, sizeof(mbi)) ||
                                    mbi.State != MEM_COMMIT ||
                                    !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
                                    continue;

                                // テキスト内容が印字可能なUnicode文字を含むか検証
                                bool hasGarbage = false;
                                int printable = 0;
                                int asciiCount = 0;
                                int totalChars = 0;
                                for (int i = 0; i < fstr->Count - 1 && i < 128; i++) {
                                    wchar_t ch = fstr->Data[i];
                                    totalChars++;
                                    if (ch == 0) break;
                                    if (ch >= 0x20 && ch <= 0x7E) { printable++; asciiCount++; }
                                    else if (ch >= 0x80 && ch < 0xD800) printable++;
                                    else if (ch == '\n' || ch == '\r' || ch == '\t') { /* OK */ }
                                    else { hasGarbage = true; break; }
                                }
                                // 2文字以下 or ASCII比率低い → 非テキスト
                                if (hasGarbage || printable < 3) continue;
                                if (totalChars >= 3 && asciiCount == 0) continue; // ASCII皆無は非テキスト

                                if (true) {
                                    WideCharToMultiByte(CP_UTF8, 0,
                                        fstr->Data, fstr->Count - 1,
                                        textContent, sizeof(textContent) - 1,
                                        nullptr, nullptr);
                                    break;
                                }
                            }
                            __except (EXCEPTION_EXECUTE_HANDLER) {
                                continue;
                            }
                        }
                    }

                    // 最初の10件は詳細ログ
                    static int matchCount = 0;
                    matchCount++;
                    if (matchCount <= 10) {
                        Log("[#%d %s::%s] text='%s' (funcCI=%d, parms=%s)",
                            matchCount, className, nameStr,
                            textContent[0] ? textContent : "(none)",
                            funcName.ComparisonIndex,
                            parms ? "有" : "無");
                    }

                    if (textContent[0]) {
                        if (matchCount > 10) {
                            Log("[%s::%s] %s", className, nameStr, textContent);
                        }
                        LogChat(nameStr, textContent);
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    pe_end:;
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

    // GNames を検出 (レイアウト自動検出含む)
    if (FindGNames()) {
        char testName[64] = {};
        if (ResolveFName(0, testName, sizeof(testName))) {
            Log("GNames 検証OK: index 0 = '%s' (モード: %s)",
                testName, g_gnamesChunked ? "chunked" : "flat");
            // チャット関連FNameを検索
            SearchChatFNames();
        } else {
            Log("GNames: ResolveFName失敗 - 予期せぬエラー");
            g_gnamesAddr = 0;
        }
    } else {
        Log("GNames 無効 - 遅延リトライで再試行します");
    }

    Log("ワーカー初期化完了 - ログ出力先: %s", g_config.logFilePath.c_str());
    if (g_config.dumpAllEvents) {
        Log("警告: 探索モード有効 - パフォーマンス低下に注意");
    }

    return true;
}

void hooks::Shutdown() {
    Log("ワーカーシャットダウン開始");

    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_logFile) {
            fclose(g_logFile);
            g_logFile = nullptr;
        }
        if (g_debugLogFile) {
            fclose(g_debugLogFile);
            g_debugLogFile = nullptr;
        }
    }
}
