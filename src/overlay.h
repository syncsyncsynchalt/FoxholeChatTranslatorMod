#pragma once
// ============================================================
// overlay.h - DX11 オーバーレイ描画 (ワーカーDLL側)
// Present フックは version.dll 側で永続管理される。
// このモジュールは ImGui 初期化とフレーム描画を担当。
// ============================================================

namespace overlay {

// 初期化 (遅延: 実際の ImGui 初期化は最初の OnPresent で行う)
bool Init();

// Present コールバック (version.dll のフックから呼ばれる)
void OnPresent(void* swapChain);

// ImGui + DX11 リソース解放
void Shutdown();

} // namespace overlay
