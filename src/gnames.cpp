// ============================================================
// gnames.cpp - UE4 FNamePool (GNames) 解決エンジン実装
// ============================================================

#include "gnames.h"
#include "ue4.h"
#include "log.h"

#include <windows.h>
#include <cstring>

// ============================================================
// 内部状態
// ============================================================

static uintptr_t g_gnamesAddr = 0;
static int       g_gnamesBlockArrayOffset = 0;
static int       g_gnamesNameOffset = 2;
static int       g_gnamesStride = 2;
static int       g_gnamesHeaderShift = 6;
static int       g_fnameBlockOffsetBits = 16;
static int32_t   g_gnamesCurrentBlock = 0;

// ============================================================
// 公開 API
// ============================================================

bool gnames::IsAvailable() { return g_gnamesAddr != 0; }

int gnames::GetBlockOffsetBits() { return g_fnameBlockOffsetBits; }

void gnames::SetBlockOffsetBits(int bits) { g_fnameBlockOffsetBits = bits; }

bool gnames::ResolveFName(int32_t comparisonIndex, char* buf, int bufSize) {
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

bool gnames::ResolveFNameWithShift(int32_t comparisonIndex, int shift, char* buf, int bufSize) {
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

int32_t gnames::FindFNameIndex(const char* targetName) {
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

// ============================================================
// FNamePool 検出 (メモリスキャン)
// ============================================================

bool gnames::Find() {
    logging::Debug("GNames: FNamePool Block[0] 検索...");

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
                            candidate.nameOffset = (gap == 8) ? 6 : 2;
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
        logging::Debug("GNames: Block[0] 検出失敗");
        return false;
    }

    logging::Debug("GNames: Block[0] 発見 allocBase=0x%llX gap=%d hdr=0x%04X",
        candidate.allocBase, candidate.gap, candidate.noneHeader);

    // ヘッダーシフト判定
    uint16_t h = candidate.noneHeader;
    if (h == 0x0008) {
        g_gnamesHeaderShift = 1;
    } else if ((h >> 6) == 4) {
        g_gnamesHeaderShift = 6;
    } else {
        g_gnamesHeaderShift = 6;
    }
    logging::Debug("GNames: headerShift=%d", g_gnamesHeaderShift);

    // ステップ2: Block[0]ポインタをモジュール内(.data)で逆引き → Blocks[]配列
    uintptr_t target = candidate.allocBase;
    for (uintptr_t s = moduleBase; s < moduleBase + moduleSize - 8; s += 8) {
        __try {
            if (*reinterpret_cast<uintptr_t*>(s) != target) continue;

            int32_t curBlock  = *reinterpret_cast<int32_t*>(s - 8);
            int32_t curCursor = *reinterpret_cast<int32_t*>(s - 4);
            if (curBlock <= 0 || curBlock >= 8192 || curCursor <= 0 || curCursor >= 0x100000) continue;

            uintptr_t block1 = *reinterpret_cast<uintptr_t*>(s + 8);
            if (!block1 || !IsReadableMemory(reinterpret_cast<void*>(block1))) continue;

            uint8_t* verifyPtr = reinterpret_cast<uint8_t*>(target + candidate.nameOffset);
            if (verifyPtr[0] != 'N' || verifyPtr[1] != 'o' || verifyPtr[2] != 'n' || verifyPtr[3] != 'e') {
                candidate.nameOffset = static_cast<int>(candidate.noneAddr - target);
            }

            g_gnamesAddr = s;
            g_gnamesBlockArrayOffset = 0;
            g_gnamesNameOffset = candidate.nameOffset;
            g_gnamesStride = 2;
            g_gnamesCurrentBlock = curBlock;

            logging::Debug("GNames: FNamePool 発見! Blocks=module+0x%llX CurrentBlock=%d nameOffset=%d",
                s - moduleBase, curBlock, g_gnamesNameOffset);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
    }

    logging::Debug("GNames: Blocks[]逆引き失敗");
    return false;
}
