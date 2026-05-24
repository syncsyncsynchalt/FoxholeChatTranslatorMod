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
#include "ollama.h"
#include "tts.h"
#include "tts_install.h"
#include "config.h"

extern "C" {

__declspec(dllexport) void* WorkerInit() {
    if (!hooks::Init()) {
        return nullptr;
    }

    const Config& cfg = config::Get();

    // 1. translate: WinHTTP セッション + ワーカースレッド起動
    translate::TranslateConfig tcfg;
    tcfg.endpoint          = cfg.ollamaEndpoint;
    tcfg.performancePreset = cfg.performancePreset;
    translate::Init(tcfg);

    // 2. ollama: プロセス管理 + ヘルスワーカー起動
    // (translate::Init 後に呼ぶ: EnsureModel が g_model を参照するため)
    ollama::Init("", cfg.ollamaEndpoint);

    // 3. TTS: 音声合成ワーカー起動 (言語は自動判定、エンジン選択はTranslationModeに委ねる)
    tts::Init("auto", cfg.ttsVoicevoxStyleId);

    // 4. overlay: ImGui 遅延初期化登録 + 翻訳コールバック設定
    overlay::Init();

    return reinterpret_cast<void*>(&hooks::OnProcessEvent);
}

__declspec(dllexport) void WorkerShutdown() {
    // overlay を先に止める: HealthWorker が translate::IsHealthy() を呼んでいる可能性があるため
    // ollama::Shutdown() より前に描画ループを止める
    overlay::Shutdown();
    ollama::Shutdown();   // Job Object 閉鎖 → Ollama プロセス終了
    tts_install::Shutdown();
    tts::Shutdown();
    translate::Shutdown();
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

// テストホスト専用: overlay::OnChatMessage を直接呼び出す
__declspec(dllexport) void WorkerOnChatMessage(const char* sender, const char* message) {
    if (sender && message) {
        overlay::OnChatMessage(std::string(sender), std::string(message));
    }
}

// テストホスト専用: hooks をスキップして overlay/translate/tts/ollama のみ初期化
// ゲームなしでオーバーレイ UI を確認するために使用する
__declspec(dllexport) void* WorkerInitTest(const char* baseDir) {
    config::Load(baseDir);
    const Config& cfg = config::Get();

    translate::TranslateConfig tcfg;
    tcfg.endpoint          = cfg.ollamaEndpoint;
    tcfg.performancePreset = cfg.performancePreset;
    translate::Init(tcfg);

    ollama::Init("", cfg.ollamaEndpoint);
    tts::Init("auto", cfg.ttsVoicevoxStyleId);
    overlay::Init();

    return reinterpret_cast<void*>(&overlay::OnPresent);
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        if (lpReserved != nullptr) {
            // プロセス終了: 全スレッドは既に強制終了済み。
            // static デストラクタが走る前に std::thread を detach して
            // ~std::thread() による std::terminate() を防ぐ。
            // ollama::DetachThread() は Job Object も閉じて Ollama を終了させる。
            ollama::DetachThread();
            translate::DetachThread();
            tts_install::DetachThread();
            tts::DetachThread();
        }
        break;
    }
    return TRUE;
}
