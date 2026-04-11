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
#include "overlay.h"
#include "translate.h"
#include "config.h"

extern "C" {

// version.dll から呼ばれる。初期化成功時はコールバック関数ポインタを返す。
__declspec(dllexport) void* WorkerInit() {
    if (!hooks::Init()) {
        return nullptr;
    }
    overlay::Init();

    // 翻訳モジュール初期化
    const Config& cfg = config::Get();
    if (cfg.translationEnabled) {
        translate::TranslateConfig tcfg;
        tcfg.endpoint   = cfg.ollamaEndpoint;
        tcfg.model      = cfg.ollamaModel;
        tcfg.targetLang = cfg.targetLanguage;
        translate::Init(tcfg);
    }

    return reinterpret_cast<void*>(&hooks::OnProcessEvent);
}

__declspec(dllexport) void WorkerShutdown() {
    translate::Shutdown();
    overlay::Shutdown();
    hooks::Shutdown();
}

__declspec(dllexport) void WorkerSetPEAddress(uintptr_t addr) {
    hooks::SetHookedPEAddress(addr);
}

__declspec(dllexport) void* WorkerGetRenderCallback() {
    return reinterpret_cast<void*>(&overlay::OnPresent);
}

__declspec(dllexport) void* WorkerGetWndProcCallback() {
    return reinterpret_cast<void*>(&overlay::OnWndProc);
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
