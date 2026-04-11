#pragma once
// ============================================================
// scanner.h - メモリパターンスキャナー
// UE4関数アドレスの自動検出に使用
// ============================================================

#include <windows.h>
#include <cstdint>
#include <vector>
#include <string>

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

// モジュール全体からパターンに一致するすべてのアドレスを返す
std::vector<uintptr_t> FindAllPatternsInModule(const char* moduleName, const char* pattern);

// RIP相対アドレスを解決 (lea/mov命令用)
// instructionAddr: 命令のアドレス
// offset: RIP相対オフセットの位置 (命令先頭からのバイト数)
// instructionSize: 命令全体のサイズ
uintptr_t ResolveRIPRelative(uintptr_t instructionAddr, int offset, int instructionSize);

// パターン文字列をバイト配列とマスクに変換
bool ParsePattern(const char* patternStr, std::vector<uint8_t>& bytes, std::vector<bool>& mask);

} // namespace scanner
