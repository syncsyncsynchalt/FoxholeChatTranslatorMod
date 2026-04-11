#pragma once
// ============================================================
// hooks.h - チャット処理ロジック (ワーカーDLL側)
// ProcessEventフックはversion.dll側で永続管理される。
// このモジュールはイベント処理とチャットキャプチャを担当。
// GNames解決は gnames.h に分離。
// ============================================================

#include <cstdint>

namespace hooks {

// 設定読み込み + GNames検出 + ログ初期化
bool Init();

// ログファイル等のクリーンアップ
void Shutdown();

// ProcessEventから呼ばれるコールバック (version.dll側のフックから呼び出される)
void OnProcessEvent(void* thisObj, void* function, void* parms);

// フック済みProcessEventアドレスを設定 (version.dll側から呼ばれる)
void SetHookedPEAddress(uintptr_t addr);

} // namespace hooks
