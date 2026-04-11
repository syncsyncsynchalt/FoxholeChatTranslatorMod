#pragma once
// ============================================================
// hooks.h - チャット処理ロジック (ワーカーDLL側)
// ProcessEventフックはversion.dll側で永続管理される。
// このモジュールはGNames解決とイベント処理を担当。
// ============================================================

#include <cstdint>

namespace hooks {

// GNames検出 + 設定読み込み (ProcessEventフックは行わない)
bool Init();

// ログファイル等のクリーンアップ (フック解除は行わない)
void Shutdown();

// ProcessEventから呼ばれるコールバック (version.dll側のフックから呼び出される)
void OnProcessEvent(void* thisObj, void* function, void* parms);

// GNamesが使用可能かどうか
bool IsGNamesAvailable();

// FNameを文字列に解決
bool ResolveFName(int32_t comparisonIndex, char* buf, int bufSize);

// フック済みProcessEventアドレスを設定 (version.dll側から呼ばれる)
void SetHookedPEAddress(uintptr_t addr);

} // namespace hooks
