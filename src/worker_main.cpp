// ============================================================
// worker_main.cpp - chat_translator.dll エントリポイント
// ホットリロード可能なワーカーDLL
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "hooks.h"

extern "C" {

// version.dll から呼ばれる。初期化成功時はコールバック関数ポインタを返す。
__declspec(dllexport) void* WorkerInit() {
    if (!hooks::Init()) {
        return nullptr;
    }
    return reinterpret_cast<void*>(&hooks::OnProcessEvent);
}

__declspec(dllexport) void WorkerShutdown() {
    hooks::Shutdown();
}

__declspec(dllexport) void WorkerSetPEAddress(uintptr_t addr) {
    hooks::SetHookedPEAddress(addr);
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
