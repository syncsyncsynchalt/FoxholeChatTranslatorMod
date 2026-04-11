#pragma once
// ============================================================
// scanner.h - メモリパターンスキャナー
// UE4関数アドレスの自動検出に使用
// ============================================================

#include <windows.h>
#include <cstdint>
#include <vector>

namespace scanner {

// モジュールのメモリ情報
struct ModuleInfo {
    uintptr_t base;
    size_t    size;
};

// モジュール情報を取得
bool GetModuleInfo(const char* moduleName, ModuleInfo& info);

// メイン実行ファイルの .text セクション情報を取得
bool GetTextSection(uintptr_t moduleBase, uintptr_t& textBase, size_t& textSize);

// パターンスキャン (IDA形式: "48 8D 05 ?? ?? ?? ??")
// ??はワイルドカード
uintptr_t FindPattern(uintptr_t base, size_t size, const char* pattern);

// モジュール全体からパターンを検索
uintptr_t FindPatternInModule(const char* moduleName, const char* pattern);

// パターン文字列をバイト配列とマスクに変換
bool ParsePattern(const char* patternStr, std::vector<uint8_t>& bytes, std::vector<bool>& mask);

} // namespace scanner
