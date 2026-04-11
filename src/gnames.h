#pragma once
// ============================================================
// gnames.h - UE4 FNamePool (GNames) 解決エンジン
// メモリスキャンによりFNamePoolを検出し、ComparisonIndex → 文字列を解決
// ============================================================

#include <cstdint>

namespace gnames {

// FNamePool を検出する (メモリスキャン)
bool Find();

// FNamePool が利用可能か
bool IsAvailable();

// ComparisonIndex → 文字列を解決
bool ResolveFName(int32_t comparisonIndex, char* buf, int bufSize);

// 指定の shift ビットで CI を解決 (FNameBlockOffsetBits 自動検出用)
bool ResolveFNameWithShift(int32_t comparisonIndex, int shift, char* buf, int bufSize);

// FNamePool 内を線形走査し、名前 → ComparisonIndex を逆引き
int32_t FindFNameIndex(const char* targetName);

// 現在の FNameBlockOffsetBits を取得/設定
int GetBlockOffsetBits();
void SetBlockOffsetBits(int bits);

} // namespace gnames
