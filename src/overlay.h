#pragma once
// ============================================================
// overlay.h - DX11 オーバーレイ描画 (ワーカーDLL側)
// Present フックは version.dll 側で永続管理される。
// このモジュールは ImGui 初期化とフレーム描画を担当。
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace overlay {

// 初期化 (遅延: 実際の ImGui 初期化は最初の OnPresent で行う)
bool Init();

// Present コールバック (version.dll のフックから呼ばれる)
void OnPresent(void* swapChain);

// WndProc コールバック (version.dll のサブクラスから呼ばれる)
// ImGui が入力を消費した場合は非ゼロを返す
LRESULT OnWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ラジオの ON/OFF 状態
bool IsRadioOn();

// ImGui + DX11 リソース解放
void Shutdown();

} // namespace overlay
