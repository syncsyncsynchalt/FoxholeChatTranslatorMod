// ============================================================
// scanner.cpp - メモリパターンスキャナー実装
// ============================================================

#include "scanner.h"
#include <cstring>
#include <cstdlib>

namespace scanner {

bool GetModuleInfo(const char* moduleName, ModuleInfo& info) {
    HMODULE hModule = GetModuleHandleA(moduleName);
    if (!hModule) return false;

    // PE ヘッダーからモジュールサイズを取得 (psapi.lib 不要)
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hModule);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<uintptr_t>(hModule) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    info.base = reinterpret_cast<uintptr_t>(hModule);
    info.size = nt->OptionalHeader.SizeOfImage;
    return true;
}

bool GetTextSection(uintptr_t moduleBase, uintptr_t& textBase, size_t& textSize) {
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(moduleBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    auto section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
        if (strncmp(reinterpret_cast<const char*>(section->Name), ".text", 5) == 0) {
            textBase = moduleBase + section->VirtualAddress;
            textSize = section->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

bool ParsePattern(const char* patternStr, std::vector<uint8_t>& bytes, std::vector<bool>& mask) {
    bytes.clear();
    mask.clear();

    const char* p = patternStr;
    while (*p) {
        // 空白をスキップ
        while (*p == ' ') p++;
        if (!*p) break;

        if (p[0] == '?' && p[1] == '?') {
            bytes.push_back(0);
            mask.push_back(false); // ワイルドカード
            p += 2;
        } else {
            char hex[3] = { p[0], p[1], 0 };
            bytes.push_back(static_cast<uint8_t>(strtoul(hex, nullptr, 16)));
            mask.push_back(true); // 固定バイト
            p += 2;
        }
    }
    return !bytes.empty();
}

uintptr_t FindPattern(uintptr_t base, size_t size, const char* pattern) {
    std::vector<uint8_t> bytes;
    std::vector<bool> matchMask;
    if (!ParsePattern(pattern, bytes, matchMask)) return 0;

    size_t patternLen = bytes.size();
    if (patternLen == 0 || patternLen > size) return 0;

    const uint8_t* mem = reinterpret_cast<const uint8_t*>(base);
    size_t scanEnd = size - patternLen;

    for (size_t i = 0; i <= scanEnd; i++) {
        bool found = true;
        for (size_t j = 0; j < patternLen; j++) {
            if (matchMask[j] && mem[i + j] != bytes[j]) {
                found = false;
                break;
            }
        }
        if (found) {
            return base + i;
        }
    }
    return 0;
}

uintptr_t FindPatternInModule(const char* moduleName, const char* pattern) {
    ModuleInfo info;
    if (!GetModuleInfo(moduleName, info)) return 0;

    // .text セクション内のみスキャン（高速化＋誤検出防止）
    uintptr_t textBase;
    size_t textSize;
    if (GetTextSection(info.base, textBase, textSize)) {
        return FindPattern(textBase, textSize, pattern);
    }

    // フォールバック: モジュール全体をスキャン
    return FindPattern(info.base, info.size, pattern);
}

std::vector<uintptr_t> FindAllPatternsInModule(const char* moduleName, const char* pattern) {
    std::vector<uintptr_t> results;
    ModuleInfo info;
    if (!GetModuleInfo(moduleName, info)) return results;

    uintptr_t scanBase = info.base;
    size_t scanSize = info.size;

    uintptr_t textBase;
    size_t textSize;
    if (GetTextSection(info.base, textBase, textSize)) {
        scanBase = textBase;
        scanSize = textSize;
    }

    std::vector<uint8_t> bytes;
    std::vector<bool> matchMask;
    if (!ParsePattern(pattern, bytes, matchMask)) return results;

    size_t patternLen = bytes.size();
    if (patternLen == 0 || patternLen > scanSize) return results;

    const uint8_t* mem = reinterpret_cast<const uint8_t*>(scanBase);
    size_t scanEnd = scanSize - patternLen;

    for (size_t i = 0; i <= scanEnd; i++) {
        bool found = true;
        for (size_t j = 0; j < patternLen; j++) {
            if (matchMask[j] && mem[i + j] != bytes[j]) {
                found = false;
                break;
            }
        }
        if (found) {
            results.push_back(scanBase + i);
        }
    }
    return results;
}

uintptr_t ResolveRIPRelative(uintptr_t instructionAddr, int offset, int instructionSize) {
    int32_t ripOffset = *reinterpret_cast<int32_t*>(instructionAddr + offset);
    return instructionAddr + instructionSize + ripOffset;
}

} // namespace scanner
