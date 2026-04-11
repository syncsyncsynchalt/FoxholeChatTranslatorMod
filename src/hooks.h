#pragma once
// ============================================================
// hooks.h - UE4 フック管理
// ============================================================

#include <cstdint>

namespace hooks {

// フックの初期化 (ProcessEvent等のフック設定)
bool Init();

// フックの解除
void Shutdown();

// GNamesが使用可能かどうか
bool IsGNamesAvailable();

// FNameを文字列に解決
// buf: 出力バッファ, bufSize: バッファサイズ
// 成功時 true
bool ResolveFName(int32_t comparisonIndex, char* buf, int bufSize);

} // namespace hooks
