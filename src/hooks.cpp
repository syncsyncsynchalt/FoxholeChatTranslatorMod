// ============================================================
// hooks.cpp - SDK ベースのチャットキャプチャ (ワーカーDLL側)
// Dumper-7 で生成した正確な構造体定義を使用
// Stage 1: チャットメッセージをテキストファイルに記録
// ============================================================

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#include "ue4.h"
#include "hooks.h"

// ============================================================
// 設定
// ============================================================
struct Config {
    bool        enableConsole = true;
    std::string logFilePath;
};

static Config g_config;

static void LoadConfig() {
    char dllPath[MAX_PATH];
    HMODULE hSelf;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&LoadConfig), &hSelf);
    GetModuleFileNameA(hSelf, dllPath, MAX_PATH);

    std::string dir(dllPath);
    size_t lastSlash = dir.rfind('\\');
    if (lastSlash != std::string::npos) dir = dir.substr(0, lastSlash + 1);

    g_config.logFilePath = dir + "chat_log.txt";

    std::string configPath = dir + "config.ini";

    auto readIniStr = [&](const char* section, const char* key, const char* def) -> std::string {
        char buf[1024];
        GetPrivateProfileStringA(section, key, def, buf, sizeof(buf), configPath.c_str());
        return buf;
    };
    auto readIniInt = [&](const char* section, const char* key, int def) -> int {
        return GetPrivateProfileIntA(section, key, def, configPath.c_str());
    };

    g_config.enableConsole = readIniInt("General", "EnableConsole", 1) != 0;

    std::string logPath = readIniStr("General", "LogFilePath", "");
    if (!logPath.empty()) g_config.logFilePath = logPath;
}

// ============================================================
// ログ出力
// ============================================================
static std::mutex g_logMutex;
static FILE*      g_chatLogFile  = nullptr;
static FILE*      g_debugLogFile = nullptr;
static std::string g_debugLogPath;

static void EnsureDebugLogFile() {
    if (g_debugLogFile) return;
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

static void Log(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_logMutex);

    if (g_config.enableConsole) {
        printf("[ChatTranslator] %s\n", buf);
    }

    EnsureDebugLogFile();
    if (g_debugLogFile) {
        fprintf(g_debugLogFile, "[ChatTranslator] %s\n", buf);
        fflush(g_debugLogFile);
    }
}

static void LogChat(const char* channel, const char* sender, const char* message) {
    std::lock_guard<std::mutex> lock(g_logMutex);

    if (!g_chatLogFile) {
        g_chatLogFile = fopen(g_config.logFilePath.c_str(), "a");
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

// ============================================================
// ヘルパー
// ============================================================

static bool IsReadableMemory(const void* ptr, size_t size = 8) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_WRITECOPY)) != 0;
}

// FString → UTF-8 変換
static std::string FStringToUtf8(const FString& fstr) {
    if (!fstr.IsValid()) return "";
    if (!IsReadableMemory(fstr.Data)) return "";

    int len = fstr.Count - 1; // null terminator 除く
    if (len <= 0) return "";

    char buf[2048];
    int written = WideCharToMultiByte(CP_UTF8, 0, fstr.Data, len, buf, sizeof(buf) - 1, nullptr, nullptr);
    if (written <= 0) return "";
    buf[written] = 0;
    return std::string(buf);
}

// EChatChannel → 文字列
static const char* ChannelName(EChatChannel ch) {
    switch (ch) {
    case EChatChannel::Default:       return "Default";
    case EChatChannel::RegionTeam:    return "Team";
    case EChatChannel::RegionTeamAir: return "TeamAir";
    case EChatChannel::WorldTeam:     return "World";
    case EChatChannel::Logistics:     return "Logistics";
    case EChatChannel::Intel:         return "Intel";
    case EChatChannel::LocalAll:      return "Local";
    case EChatChannel::Squad:         return "Squad";
    case EChatChannel::Regiment:      return "Regiment";
    case EChatChannel::Whisper:       return "Whisper";
    case EChatChannel::Admin:         return "Admin";
    default:                          return "Unknown";
    }
}

// APlayerState から PlayerNamePrivate を取得 (SEH safe)
static bool GetPlayerNameSafe(void* playerState, FString* out) {
    if (!playerState || !IsReadableMemory(playerState)) return false;
    __try {
        auto namePtr = reinterpret_cast<FString*>(
            reinterpret_cast<uintptr_t>(playerState) + PLAYERSTATE_PLAYERNAME_OFFSET);
        *out = *namePtr;
        return out->IsValid() && IsReadableMemory(out->Data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static std::string GetPlayerName(void* playerState) {
    FString fs = {};
    if (GetPlayerNameSafe(playerState, &fs)) return FStringToUtf8(fs);
    return "";
}

// ============================================================
// GNames
// ============================================================
static uintptr_t g_gnamesAddr = 0;
static int       g_gnamesBlockArrayOffset = 0;
static int       g_gnamesNameOffset = 2;
static int       g_gnamesStride = 2;
static int       g_gnamesHeaderShift = 6;
static int       g_fnameBlockOffsetBits = 16; // FNamePool ComparisonIndex encoding shift
static int32_t   g_gnamesCurrentBlock = 0;    // 最大ブロック番号

bool hooks::IsGNamesAvailable() { return g_gnamesAddr != 0; }

void hooks::SetHookedPEAddress(uintptr_t addr) {
    Log("フック済みPEアドレス設定: 0x%llX", addr);
}


bool hooks::ResolveFName(int32_t comparisonIndex, char* buf, int bufSize) {
    if (!g_gnamesAddr || bufSize <= 0) return false;
    __try {
        int mask = (1 << g_fnameBlockOffsetBits) - 1;
        int block  = comparisonIndex >> g_fnameBlockOffsetBits;
        int offset = comparisonIndex & mask;
        uintptr_t* blocks = reinterpret_cast<uintptr_t*>(g_gnamesAddr + g_gnamesBlockArrayOffset);
        if (block < 0 || block > g_gnamesCurrentBlock || !blocks[block]) return false;
        uintptr_t entryAddr = blocks[block] + static_cast<uintptr_t>(offset) * g_gnamesStride;
        const char* namePtr = reinterpret_cast<const char*>(entryAddr + g_gnamesNameOffset);
        uint16_t hdrData = *reinterpret_cast<const uint16_t*>(entryAddr);
        int len = hdrData >> g_gnamesHeaderShift;
        if (len <= 0 || len >= bufSize) return false;
        memcpy(buf, namePtr, len);
        buf[len] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// GNames検出: "None" + "ByteProperty" 複合パターン (FNamePool Block[0])
static bool FindGNames() {
    Log("GNames: FNamePool Block[0] 検索...");

    HMODULE gameModule = GetModuleHandleA(nullptr);
    uintptr_t moduleBase = reinterpret_cast<uintptr_t>(gameModule);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase);
    auto nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(moduleBase + dos->e_lfanew);
    DWORD moduleSize = nt->OptionalHeader.SizeOfImage;

    const uint8_t byteProperty[] = { 'B','y','t','e','P','r','o','p','e','r','t','y' };

    // ステップ1: メモリ全域を走査し、Block[0]を検出
    struct BlockCandidate {
        uintptr_t allocBase;
        uintptr_t noneAddr;
        uint16_t  noneHeader;
        int       gap;
        int       nameOffset;
    };
    BlockCandidate candidate = {};
    bool found = false;

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t scanAddr = 0x10000;
    while (!found && VirtualQuery(reinterpret_cast<void*>(scanAddr), &mbi, sizeof(mbi))) {
        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
            mbi.RegionSize >= 64) {

            uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            bool isModule = (regionStart >= moduleBase && regionStart < moduleBase + moduleSize);
            if (!isModule) {
                uint8_t* base = reinterpret_cast<uint8_t*>(mbi.BaseAddress);
                size_t searchEnd = (mbi.RegionSize < 64) ? mbi.RegionSize : 64;
                for (size_t i = 0; i + 4 <= searchEnd && !found; i += 2) {
                    if (base[i] != 'N' || base[i+1] != 'o' || base[i+2] != 'n' || base[i+3] != 'e') continue;
                    for (int gap : {2, 8}) {
                        size_t bpOff = i + 4 + gap;
                        if (bpOff + 12 > mbi.RegionSize) continue;
                        if (memcmp(base + bpOff, byteProperty, 12) == 0) {
                            candidate.allocBase = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
                            candidate.noneAddr = regionStart + i;
                            candidate.noneHeader = (i >= 2) ? *reinterpret_cast<uint16_t*>(base + i - 2) : 0;
                            candidate.gap = gap;
                            candidate.nameOffset = (gap == 8) ? 6 : 2; // WITH_CASE_PRESERVING_NAME ?
                            found = true;
                            break;
                        }
                    }
                }
            }
        }
        if (regionEnd <= scanAddr) break;
        scanAddr = regionEnd;
    }

    if (!found) {
        Log("GNames: Block[0] 検出失敗");
        return false;
    }

    Log("GNames: Block[0] 発見 allocBase=0x%llX gap=%d hdr=0x%04X",
        candidate.allocBase, candidate.gap, candidate.noneHeader);

    // ヘッダーシフト判定
    uint16_t h = candidate.noneHeader;
    if (h == 0x0008) {
        g_gnamesHeaderShift = 1; // UE4 4.23-4.24
    } else if ((h >> 6) == 4) {
        g_gnamesHeaderShift = 6; // UE4 4.25+
    } else {
        g_gnamesHeaderShift = 6;
    }
    Log("GNames: headerShift=%d", g_gnamesHeaderShift);

    // ステップ2: Block[0]ポインタをモジュール内(.data)で逆引き → Blocks[]配列
    uintptr_t targets[] = { candidate.allocBase };
    for (int ti = 0; ti < 1; ti++) {
        uintptr_t target = targets[ti];
        for (uintptr_t s = moduleBase; s < moduleBase + moduleSize - 8; s += 8) {
            __try {
                if (*reinterpret_cast<uintptr_t*>(s) != target) continue;

                // Blocks[]の前にCurrentBlock + CurrentByteCursorがある
                int32_t curBlock  = *reinterpret_cast<int32_t*>(s - 8);
                int32_t curCursor = *reinterpret_cast<int32_t*>(s - 4);
                if (curBlock <= 0 || curBlock >= 8192 || curCursor <= 0 || curCursor >= 0x100000) continue;

                uintptr_t block1 = *reinterpret_cast<uintptr_t*>(s + 8);
                if (!block1 || !IsReadableMemory(reinterpret_cast<void*>(block1))) continue;

                // 検証: Block[0] + nameOffset に "None" があるか
                uint8_t* verifyPtr = reinterpret_cast<uint8_t*>(target + candidate.nameOffset);
                if (verifyPtr[0] != 'N' || verifyPtr[1] != 'o' || verifyPtr[2] != 'n' || verifyPtr[3] != 'e') {
                    candidate.nameOffset = static_cast<int>(candidate.noneAddr - target);
                }

                g_gnamesAddr = s;
                g_gnamesBlockArrayOffset = 0;
                g_gnamesNameOffset = candidate.nameOffset;
                g_gnamesStride = 2;
                g_gnamesCurrentBlock = curBlock;

                Log("GNames: FNamePool 発見! Blocks=module+0x%llX CurrentBlock=%d nameOffset=%d",
                    s - moduleBase, curBlock, g_gnamesNameOffset);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        }
    }

    Log("GNames: Blocks[]逆引き失敗");
    return false;
}

// ============================================================
// チャットFName CI 検出
// ============================================================

// FNamePool内を線形走査して名前→CIを逆引き
static int32_t FindFNameIndex(const char* targetName) {
    if (!g_gnamesAddr || !targetName) return -1;
    int targetLen = static_cast<int>(strlen(targetName));
    if (targetLen <= 0 || targetLen > 200) return -1;

    __try {
        uintptr_t* blocks = reinterpret_cast<uintptr_t*>(g_gnamesAddr + g_gnamesBlockArrayOffset);
        int32_t currentBlock = *reinterpret_cast<int32_t*>(g_gnamesAddr - 8);
        if (currentBlock <= 0 || currentBlock > 8192) currentBlock = 64;

        for (int block = 0; block <= currentBlock; block++) {
            uintptr_t blockBase = blocks[block];
            if (!blockBase || !IsReadableMemory(reinterpret_cast<void*>(blockBase))) continue;

            int offset = 0;
            int maxOffset = (1 << g_fnameBlockOffsetBits) * g_gnamesStride;
            while (offset < maxOffset) {
                uintptr_t entryAddr = blockBase + offset;
                if (!IsReadableMemory(reinterpret_cast<void*>(entryAddr))) break;

                uint16_t hdr = *reinterpret_cast<uint16_t*>(entryAddr);
                int len = hdr >> g_gnamesHeaderShift;
                if (len <= 0 || len > 1024) break;

                bool isWide = (hdr & 1) != 0;
                int nameBytes = isWide ? (len * 2) : len;
                int entrySize = g_gnamesNameOffset + nameBytes;
                entrySize = (entrySize + 1) & ~1;

                if (!isWide && len == targetLen) {
                    const char* namePtr = reinterpret_cast<const char*>(entryAddr + g_gnamesNameOffset);
                    if (memcmp(namePtr, targetName, len) == 0) {
                        return (block << g_fnameBlockOffsetBits) | (offset / g_gnamesStride);
                    }
                }
                offset += entrySize;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}

// チャット関連UFunction名とそのCI
enum ChatFuncId {
    CHAT_FUNC_CLIENT_CHAT_MESSAGE = 0,
    CHAT_FUNC_CLIENT_CHAT_MESSAGE_WITH_TAG,
    CHAT_FUNC_CLIENT_WORLD_CHAT_MESSAGE,
    CHAT_FUNC_COUNT
};

static const char* g_chatFuncNames[CHAT_FUNC_COUNT] = {
    "ClientChatMessage",
    "ClientChatMessageWithTag",
    "ClientWorldChatMessage",
};

static int32_t g_chatFuncCIs[CHAT_FUNC_COUNT] = { -1, -1, -1 };
static bool    g_chatCIsReady = false;

static void SearchChatFNames() {
    Log("=== チャット関連FName検索 ===");
    int found = 0;
    for (int i = 0; i < CHAT_FUNC_COUNT; i++) {
        g_chatFuncCIs[i] = FindFNameIndex(g_chatFuncNames[i]);
        if (g_chatFuncCIs[i] >= 0) {
            char verify[128] = {};
            hooks::ResolveFName(g_chatFuncCIs[i], verify, sizeof(verify));
            Log("  %s → CI=%d [verify='%s']", g_chatFuncNames[i], g_chatFuncCIs[i], verify);
            found++;
        } else {
            Log("  %s → 未検出", g_chatFuncNames[i]);
        }
    }
    Log("  %d/%d 件検出", found, CHAT_FUNC_COUNT);
    g_chatCIsReady = (found > 0);
}

// ============================================================
// ProcessEvent コールバック
// ============================================================

// GNames遅延リトライ
static int g_gnamesRetryCount = 0;

static void TryLazyGNamesResolve() {
    if (g_gnamesAddr != 0 || g_gnamesRetryCount >= 20) return;
    static int callCounter = 0;
    if (++callCounter % 30000 != 0) return;
    g_gnamesRetryCount++;
    Log("GNames: 遅延リトライ #%d", g_gnamesRetryCount);
    if (FindGNames()) {
        char test[64] = {};
        if (hooks::ResolveFName(0, test, sizeof(test))) {
            Log("GNames: 検出成功! index 0 = '%s'", test);
            SearchChatFNames();
        }
    }
}

// function の FName CI から ChatFuncId を判定
// UFunction検証: FunctionFlags (offset 0x98) に FUNC_Net (0x40) が必要
static int IdentifyChatFunc(void* function) {
    if (!function || !g_chatCIsReady) return -1;
    __try {
        uintptr_t funcAddr = reinterpret_cast<uintptr_t>(function);
        if (funcAddr < 0x10000 || !IsReadableMemory(function)) return -1;
        int32_t funcCI = *reinterpret_cast<int32_t*>(funcAddr + ue4::UOBJECT_NAME_OFFSET);
        for (int i = 0; i < CHAT_FUNC_COUNT; i++) {
            if (g_chatFuncCIs[i] >= 0 && funcCI == g_chatFuncCIs[i]) {
                // UFunction FunctionFlags 検証 (FUNC_Net = 0x40)
                if (IsReadableMemory(reinterpret_cast<void*>(funcAddr + 0x98))) {
                    uint32_t flags = *reinterpret_cast<uint32_t*>(funcAddr + 0x98);
                    if (!(flags & 0x40)) return -1; // FUNC_Net がないなら UFunction ではない
                }
                return i;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}

// SEH安全なパラメータ読み取り (C++オブジェクト禁止)
struct ChatData {
    FString msgStr;
    FString senderStr;
    FString regTagStr;
    EChatChannel channel;
    bool hasSender;
    bool hasRegTag;
    void* senderPlayerState;
};

static bool ReadChatParamsSafe(int chatFunc, void* parms, ChatData* out) {
    __try {
        memset(out, 0, sizeof(*out));
        out->channel = EChatChannel::Default;

        switch (chatFunc) {
        case CHAT_FUNC_CLIENT_CHAT_MESSAGE: {
            auto p = reinterpret_cast<Parms_ClientChatMessage*>(parms);
            out->msgStr = p->MsgString;
            out->senderPlayerState = p->SenderPlayerState;
            out->channel = p->Channel;
            out->hasSender = true;
            break;
        }
        case CHAT_FUNC_CLIENT_CHAT_MESSAGE_WITH_TAG: {
            auto p = reinterpret_cast<Parms_ClientChatMessageWithTag*>(parms);
            out->msgStr = p->MsgString;
            out->senderPlayerState = p->SenderPlayerState;
            out->regTagStr = p->SenderRegimentTag;
            out->channel = p->Channel;
            out->hasSender = true;
            out->hasRegTag = true;
            break;
        }
        case CHAT_FUNC_CLIENT_WORLD_CHAT_MESSAGE: {
            auto p = reinterpret_cast<Parms_ClientWorldChatMessage*>(parms);
            out->msgStr = p->Message;
            out->senderStr = p->SenderName;
            out->regTagStr = p->SenderRegimentTag;
            out->channel = p->Channel;
            out->hasRegTag = true;
            break;
        }
        default:
            return false;
        }
        return out->msgStr.IsValid() && IsReadableMemory(out->msgStr.Data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// 特定の shift ビットで CI を解決 (内部用)
static bool ResolveFNameWithShift(int32_t comparisonIndex, int shift, char* buf, int bufSize) {
    if (!g_gnamesAddr || bufSize <= 0) return false;
    __try {
        int mask = (1 << shift) - 1;
        int block  = comparisonIndex >> shift;
        int offset = comparisonIndex & mask;
        uintptr_t* blocks = reinterpret_cast<uintptr_t*>(g_gnamesAddr + g_gnamesBlockArrayOffset);
        if (block < 0 || block > g_gnamesCurrentBlock) return false;
        if (!blocks[block]) return false;
        uintptr_t entryAddr = blocks[block] + static_cast<uintptr_t>(offset) * g_gnamesStride;
        if (!IsReadableMemory(reinterpret_cast<void*>(entryAddr))) return false;
        uint16_t hdrData = *reinterpret_cast<const uint16_t*>(entryAddr);
        int len = hdrData >> g_gnamesHeaderShift;
        if (len <= 0 || len >= bufSize) return false;
        const char* namePtr = reinterpret_cast<const char*>(entryAddr + g_gnamesNameOffset);
        for (int i = 0; i < len; i++) {
            if (namePtr[i] < 0x20 || namePtr[i] > 0x7E) return false;
        }
        memcpy(buf, namePtr, len);
        buf[len] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// FNameBlockOffsetBits 自動検出 (SEH分離)
static bool g_shiftDetected = false;

static bool TryDetectShift(void* function) {
    __try {
        uintptr_t funcAddr = reinterpret_cast<uintptr_t>(function);
        if (funcAddr < 0x10000 || !IsReadableMemory(function)) return false;

        // UObject基本チェック: +0x10 (ClassPrivate) が有効なポインタか
        uintptr_t classPtr = *reinterpret_cast<uintptr_t*>(funcAddr + 0x10);
        if (classPtr < 0x10000 || !IsReadableMemory(reinterpret_cast<void*>(classPtr))) return false;

        int32_t rawCI = *reinterpret_cast<int32_t*>(funcAddr + ue4::UOBJECT_NAME_OFFSET);
        if (rawCI <= 0) return false; // CI=0 (None) は判別不可

        // block=0の小さいCIではshift判別不可 → block>0になるCIのみ使用
        // shift=16でblock>0 ⇔ CI >= 65536
        if (rawCI < 65536) return false;

        char buf[128];
        // 降順で試行: 大きいshiftが正しい場合、小さいshiftでも
        // block=0なら解決できてしまうので、大きい方から試す
        for (int shift = 16; shift >= 14; shift--) {
            if (ResolveFNameWithShift(rawCI, shift, buf, sizeof(buf))) {
                if (shift != g_fnameBlockOffsetBits) {
                    Log("FNameBlockOffsetBits 検出: %d (旧=%d, CI=%d → '%s')", 
                        shift, g_fnameBlockOffsetBits, rawCI, buf);
                    g_fnameBlockOffsetBits = shift;

                    // chat CI を再計算
                    Log("chat CI 再計算 (shift=%d)...", shift);
                    int found = 0;
                    for (int i = 0; i < CHAT_FUNC_COUNT; i++) {
                        g_chatFuncCIs[i] = FindFNameIndex(g_chatFuncNames[i]);
                        if (g_chatFuncCIs[i] >= 0) {
                            char verify[128] = {};
                            hooks::ResolveFName(g_chatFuncCIs[i], verify, sizeof(verify));
                            Log("  %s → CI=%d [verify='%s']", g_chatFuncNames[i], g_chatFuncCIs[i], verify);
                            found++;
                        }
                    }
                    g_chatCIsReady = (found > 0);
                    Log("  %d/%d 件検出", found, CHAT_FUNC_COUNT);
                } else {
                    Log("FNameBlockOffsetBits 確認OK: %d (CI=%d → '%s')", shift, rawCI, buf);
                }
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}



void hooks::OnProcessEvent(void* thisObj, void* function, void* parms) {
    // GNames 遅延リトライ
    if (!g_gnamesAddr) {
        TryLazyGNamesResolve();
        return;
    }
    if (!g_chatCIsReady) return;

    // FNameBlockOffsetBits 自動検出 (最初の100回のうちから検出)
    if (!g_shiftDetected) {
        static volatile long shiftAttempts = 0;
        long sa = InterlockedIncrement(&shiftAttempts);
        if (sa <= 100) {
            if (TryDetectShift(function)) {
                g_shiftDetected = true;
            }
        } else {
            g_shiftDetected = true;
        }
    }

    // チャット関数か判定
    int chatFunc = IdentifyChatFunc(function);
    if (chatFunc < 0 || !parms) return;

    // SEH安全にパラメータ読み取り
    ChatData data;
    if (!ReadChatParamsSafe(chatFunc, parms, &data)) return;

    // C++文字列変換 (SEH外)
    std::string msg = FStringToUtf8(data.msgStr);
    if (msg.empty()) return;

    std::string sender;
    if (data.hasSender) {
        sender = GetPlayerName(data.senderPlayerState);
    } else {
        sender = FStringToUtf8(data.senderStr);
    }

    std::string regTag;
    if (data.hasRegTag) regTag = FStringToUtf8(data.regTagStr);

    std::string displaySender = sender;
    if (!regTag.empty()) displaySender = "[" + regTag + "] " + sender;
    if (displaySender.empty()) displaySender = "???";

    const char* chName = ChannelName(data.channel);

    // 重複排除: 同一メッセージを500ms以内に2回処理しない
    static std::string s_lastDedupKey;
    static DWORD s_lastDedupTime = 0;
    std::string dedupKey = std::string(chName) + "|" + displaySender + "|" + msg;
    DWORD now = GetTickCount();
    if (dedupKey == s_lastDedupKey && (now - s_lastDedupTime) < 500) {
        return;
    }
    s_lastDedupKey = dedupKey;
    s_lastDedupTime = now;

    Log("[%s] %s: %s", chName, displaySender.c_str(), msg.c_str());
    LogChat(chName, displaySender.c_str(), msg.c_str());
}

// ============================================================
// 公開インターフェース
// ============================================================

bool hooks::Init() {
    LoadConfig();

    Log("===================================");
    Log("Foxhole Chat Translator - Stage 1");
    Log("SDK ベースのチャットキャプチャ");
    Log("===================================");

    if (FindGNames()) {
        char testName[64] = {};
        if (ResolveFName(0, testName, sizeof(testName))) {
            Log("GNames OK: index 0 = '%s' (headerShift=%d)", testName, g_gnamesHeaderShift);
            SearchChatFNames();
        } else {
            Log("GNames: ResolveFName失敗");
            g_gnamesAddr = 0;
        }
    } else {
        Log("GNames 未検出 - 遅延リトライで再試行");
    }

    Log("チャットログ出力先: %s", g_config.logFilePath.c_str());
    return true;
}

void hooks::Shutdown() {
    Log("ワーカーシャットダウン");
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_chatLogFile) { fclose(g_chatLogFile); g_chatLogFile = nullptr; }
    if (g_debugLogFile) { fclose(g_debugLogFile); g_debugLogFile = nullptr; }
}
