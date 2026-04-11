// ============================================================
// dllmain.cpp - version.dll プロキシ + ProcessEvent永続フック + ワーカーローダー
// Foxhole Chat Translator
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>
#include "scanner.h"

// ============================================================
// オリジナル version.dll のロードと転送
// ============================================================

static HMODULE g_hOriginalDll = nullptr;

// オリジナル関数ポインタ格納用
// proxy.asm から参照、または .def 経由でエクスポート
extern "C" FARPROC g_origProcs[17] = {};

static const char* g_exportNames[17] = {
    "GetFileVersionInfoA",
    "GetFileVersionInfoByHandle",
    "GetFileVersionInfoExA",
    "GetFileVersionInfoExW",
    "GetFileVersionInfoSizeA",
    "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW",
    "GetFileVersionInfoSizeW",
    "GetFileVersionInfoW",
    "VerFindFileA",
    "VerFindFileW",
    "VerInstallFileA",
    "VerInstallFileW",
    "VerLanguageNameA",
    "VerLanguageNameW",
    "VerQueryValueA",
    "VerQueryValueW"
};

static bool LoadOriginalDll() {
    char systemDir[MAX_PATH];
    GetSystemDirectoryA(systemDir, MAX_PATH);
    strcat_s(systemDir, "\\version.dll");

    g_hOriginalDll = LoadLibraryA(systemDir);
    if (!g_hOriginalDll) return false;

    for (int i = 0; i < 17; i++) {
        g_origProcs[i] = GetProcAddress(g_hOriginalDll, g_exportNames[i]);
    }
    return true;
}

// ============================================================
// プロキシ関数定義
// .def ファイルで元の名前にマッピング
// ============================================================

// 関数ポインタ型定義
typedef BOOL  (WINAPI *PFN_GetFileVersionInfoA)(LPCSTR, DWORD, DWORD, LPVOID);
typedef BOOL  (WINAPI *PFN_GetFileVersionInfoByHandle)(DWORD, HANDLE, LPVOID, DWORD);
typedef BOOL  (WINAPI *PFN_GetFileVersionInfoExA)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
typedef BOOL  (WINAPI *PFN_GetFileVersionInfoExW)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
typedef DWORD (WINAPI *PFN_GetFileVersionInfoSizeA)(LPCSTR, LPDWORD);
typedef DWORD (WINAPI *PFN_GetFileVersionInfoSizeExA)(DWORD, LPCSTR, LPDWORD);
typedef DWORD (WINAPI *PFN_GetFileVersionInfoSizeExW)(DWORD, LPCWSTR, LPDWORD);
typedef DWORD (WINAPI *PFN_GetFileVersionInfoSizeW)(LPCWSTR, LPDWORD);
typedef BOOL  (WINAPI *PFN_GetFileVersionInfoW)(LPCWSTR, DWORD, DWORD, LPVOID);
typedef DWORD (WINAPI *PFN_VerFindFileA)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT);
typedef DWORD (WINAPI *PFN_VerFindFileW)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT);
typedef DWORD (WINAPI *PFN_VerInstallFileA)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT);
typedef DWORD (WINAPI *PFN_VerInstallFileW)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT);
typedef DWORD (WINAPI *PFN_VerLanguageNameA)(DWORD, LPSTR, DWORD);
typedef DWORD (WINAPI *PFN_VerLanguageNameW)(DWORD, LPWSTR, DWORD);
typedef BOOL  (WINAPI *PFN_VerQueryValueA)(LPCVOID, LPCSTR, LPVOID*, PUINT);
typedef BOOL  (WINAPI *PFN_VerQueryValueW)(LPCVOID, LPCWSTR, LPVOID*, PUINT);

// プロキシ関数: オリジナルDLLの関数に転送
extern "C" {

BOOL WINAPI Proxy_GetFileVersionInfoA(LPCSTR a, DWORD b, DWORD c, LPVOID d) {
    return reinterpret_cast<PFN_GetFileVersionInfoA>(g_origProcs[0])(a, b, c, d);
}
BOOL WINAPI Proxy_GetFileVersionInfoByHandle(DWORD a, HANDLE b, LPVOID c, DWORD d) {
    return reinterpret_cast<PFN_GetFileVersionInfoByHandle>(g_origProcs[1])(a, b, c, d);
}
BOOL WINAPI Proxy_GetFileVersionInfoExA(DWORD a, LPCSTR b, DWORD c, DWORD d, LPVOID e) {
    return reinterpret_cast<PFN_GetFileVersionInfoExA>(g_origProcs[2])(a, b, c, d, e);
}
BOOL WINAPI Proxy_GetFileVersionInfoExW(DWORD a, LPCWSTR b, DWORD c, DWORD d, LPVOID e) {
    return reinterpret_cast<PFN_GetFileVersionInfoExW>(g_origProcs[3])(a, b, c, d, e);
}
DWORD WINAPI Proxy_GetFileVersionInfoSizeA(LPCSTR a, LPDWORD b) {
    return reinterpret_cast<PFN_GetFileVersionInfoSizeA>(g_origProcs[4])(a, b);
}
DWORD WINAPI Proxy_GetFileVersionInfoSizeExA(DWORD a, LPCSTR b, LPDWORD c) {
    return reinterpret_cast<PFN_GetFileVersionInfoSizeExA>(g_origProcs[5])(a, b, c);
}
DWORD WINAPI Proxy_GetFileVersionInfoSizeExW(DWORD a, LPCWSTR b, LPDWORD c) {
    return reinterpret_cast<PFN_GetFileVersionInfoSizeExW>(g_origProcs[6])(a, b, c);
}
DWORD WINAPI Proxy_GetFileVersionInfoSizeW(LPCWSTR a, LPDWORD b) {
    return reinterpret_cast<PFN_GetFileVersionInfoSizeW>(g_origProcs[7])(a, b);
}
BOOL WINAPI Proxy_GetFileVersionInfoW(LPCWSTR a, DWORD b, DWORD c, LPVOID d) {
    return reinterpret_cast<PFN_GetFileVersionInfoW>(g_origProcs[8])(a, b, c, d);
}
DWORD WINAPI Proxy_VerFindFileA(DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPSTR e, PUINT f, LPSTR g, PUINT h) {
    return reinterpret_cast<PFN_VerFindFileA>(g_origProcs[9])(a, b, c, d, e, f, g, h);
}
DWORD WINAPI Proxy_VerFindFileW(DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPWSTR e, PUINT f, LPWSTR g, PUINT h) {
    return reinterpret_cast<PFN_VerFindFileW>(g_origProcs[10])(a, b, c, d, e, f, g, h);
}
DWORD WINAPI Proxy_VerInstallFileA(DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPCSTR e, LPCSTR f2, LPSTR g, PUINT h) {
    return reinterpret_cast<PFN_VerInstallFileA>(g_origProcs[11])(a, b, c, d, e, f2, g, h);
}
DWORD WINAPI Proxy_VerInstallFileW(DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPCWSTR e, LPCWSTR f2, LPWSTR g, PUINT h) {
    return reinterpret_cast<PFN_VerInstallFileW>(g_origProcs[12])(a, b, c, d, e, f2, g, h);
}
DWORD WINAPI Proxy_VerLanguageNameA(DWORD a, LPSTR b, DWORD c) {
    return reinterpret_cast<PFN_VerLanguageNameA>(g_origProcs[13])(a, b, c);
}
DWORD WINAPI Proxy_VerLanguageNameW(DWORD a, LPWSTR b, DWORD c) {
    return reinterpret_cast<PFN_VerLanguageNameW>(g_origProcs[14])(a, b, c);
}
BOOL WINAPI Proxy_VerQueryValueA(LPCVOID a, LPCSTR b, LPVOID* c, PUINT d) {
    return reinterpret_cast<PFN_VerQueryValueA>(g_origProcs[15])(a, b, c, d);
}
BOOL WINAPI Proxy_VerQueryValueW(LPCVOID a, LPCWSTR b, LPVOID* c, PUINT d) {
    return reinterpret_cast<PFN_VerQueryValueW>(g_origProcs[16])(a, b, c, d);
}

} // extern "C"

// ============================================================
// ProcessEvent 永続フック (version.dll に常駐)
// ============================================================

typedef void (__fastcall *ProcessEventFn)(void*, void*, void*);
typedef void (*WorkerCallbackFn)(void*, void*, void*);

// 複数ProcessEvent実装のフック管理
static const int MAX_PE_HOOKS = 64;
static ProcessEventFn   g_originalPEs[MAX_PE_HOOKS] = {};
static uintptr_t        g_hookedPeAddrs[MAX_PE_HOOKS] = {};
static int              g_peHookCount = 0;

static volatile WorkerCallbackFn g_workerCallback = nullptr;
static FILE* g_loaderLogFile = nullptr;

static uintptr_t g_hookedPeAddr = 0;  // フックしたProcessEventのアドレス

// ============================================================
// DXGI Present フック (version.dll に常駐)
// ============================================================

typedef HRESULT (WINAPI *PFN_Present)(IDXGISwapChain*, UINT, UINT);
static PFN_Present g_originalPresent = nullptr;

typedef void (*RenderCallbackFn)(void*);
static volatile RenderCallbackFn g_renderCallback = nullptr;

// WndProc サブクラス化 (マウス入力を ImGui に転送)
typedef LRESULT (*WndProcCallbackFn)(HWND, UINT, WPARAM, LPARAM);
static volatile WndProcCallbackFn g_wndProcCallback = nullptr;
static WNDPROC g_originalWndProc = nullptr;
static HWND    g_gameHwnd = nullptr;

static LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WndProcCallbackFn cb = g_wndProcCallback;
    if (cb) {
        LRESULT result = cb(hwnd, msg, wParam, lParam);
        if (result) return result;  // ImGui が入力を消費した
    }
    return CallWindowProc(g_originalWndProc, hwnd, msg, wParam, lParam);
}

static void InstallWndProcHook(IDXGISwapChain* swapChain) {
    if (g_gameHwnd) return;  // 既にインストール済み
    DXGI_SWAP_CHAIN_DESC desc;
    if (SUCCEEDED(swapChain->GetDesc(&desc)) && desc.OutputWindow) {
        g_gameHwnd = desc.OutputWindow;
        g_originalWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtr(g_gameHwnd, GWLP_WNDPROC,
                             reinterpret_cast<LONG_PTR>(HookedWndProc)));
    }
}

static HRESULT WINAPI HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    InstallWndProcHook(swapChain);
    RenderCallbackFn cb = g_renderCallback;
    if (cb) {
        cb(swapChain);
    }
    return g_originalPresent(swapChain, syncInterval, flags);
}

// HookDXGIPresent() は LogLoader の後に定義 (前方参照回避)
static bool HookDXGIPresent();

// ============================================================
// ProcessEvent フック (MinHook 版 - 診断後に使用)
// 複数PE実装のフック: 各フックは同じコールバック + 個別のオリジナル呼び出し
// ============================================================

// 共通のコールバック処理
static void InvokeWorkerCallback(void* thisObj, void* function, void* parms) {
    WorkerCallbackFn cb = g_workerCallback;
    if (cb) {
        cb(thisObj, function, parms);
    }
}

// フック関数テンプレート: 各PE実装に対応
#define DEFINE_PE_HOOK(N) \
static void __fastcall HookedPE_##N(void* thisObj, void* function, void* parms) { \
    InvokeWorkerCallback(thisObj, function, parms); \
    if (g_originalPEs[N]) g_originalPEs[N](thisObj, function, parms); \
}

DEFINE_PE_HOOK(0)  DEFINE_PE_HOOK(1)  DEFINE_PE_HOOK(2)  DEFINE_PE_HOOK(3)
DEFINE_PE_HOOK(4)  DEFINE_PE_HOOK(5)  DEFINE_PE_HOOK(6)  DEFINE_PE_HOOK(7)
DEFINE_PE_HOOK(8)  DEFINE_PE_HOOK(9)  DEFINE_PE_HOOK(10) DEFINE_PE_HOOK(11)
DEFINE_PE_HOOK(12) DEFINE_PE_HOOK(13) DEFINE_PE_HOOK(14) DEFINE_PE_HOOK(15)
DEFINE_PE_HOOK(16) DEFINE_PE_HOOK(17) DEFINE_PE_HOOK(18) DEFINE_PE_HOOK(19)
DEFINE_PE_HOOK(20) DEFINE_PE_HOOK(21) DEFINE_PE_HOOK(22) DEFINE_PE_HOOK(23)
DEFINE_PE_HOOK(24) DEFINE_PE_HOOK(25) DEFINE_PE_HOOK(26) DEFINE_PE_HOOK(27)
DEFINE_PE_HOOK(28) DEFINE_PE_HOOK(29) DEFINE_PE_HOOK(30) DEFINE_PE_HOOK(31)
DEFINE_PE_HOOK(32) DEFINE_PE_HOOK(33) DEFINE_PE_HOOK(34) DEFINE_PE_HOOK(35)
DEFINE_PE_HOOK(36) DEFINE_PE_HOOK(37) DEFINE_PE_HOOK(38) DEFINE_PE_HOOK(39)
DEFINE_PE_HOOK(40) DEFINE_PE_HOOK(41) DEFINE_PE_HOOK(42) DEFINE_PE_HOOK(43)
DEFINE_PE_HOOK(44) DEFINE_PE_HOOK(45) DEFINE_PE_HOOK(46) DEFINE_PE_HOOK(47)
DEFINE_PE_HOOK(48) DEFINE_PE_HOOK(49) DEFINE_PE_HOOK(50) DEFINE_PE_HOOK(51)
DEFINE_PE_HOOK(52) DEFINE_PE_HOOK(53) DEFINE_PE_HOOK(54) DEFINE_PE_HOOK(55)
DEFINE_PE_HOOK(56) DEFINE_PE_HOOK(57) DEFINE_PE_HOOK(58) DEFINE_PE_HOOK(59)
DEFINE_PE_HOOK(60) DEFINE_PE_HOOK(61) DEFINE_PE_HOOK(62) DEFINE_PE_HOOK(63)

typedef void (__fastcall *PEHookFn)(void*, void*, void*);
static PEHookFn g_peHookFns[] = {
    HookedPE_0,  HookedPE_1,  HookedPE_2,  HookedPE_3,
    HookedPE_4,  HookedPE_5,  HookedPE_6,  HookedPE_7,
    HookedPE_8,  HookedPE_9,  HookedPE_10, HookedPE_11,
    HookedPE_12, HookedPE_13, HookedPE_14, HookedPE_15,
    HookedPE_16, HookedPE_17, HookedPE_18, HookedPE_19,
    HookedPE_20, HookedPE_21, HookedPE_22, HookedPE_23,
    HookedPE_24, HookedPE_25, HookedPE_26, HookedPE_27,
    HookedPE_28, HookedPE_29, HookedPE_30, HookedPE_31,
    HookedPE_32, HookedPE_33, HookedPE_34, HookedPE_35,
    HookedPE_36, HookedPE_37, HookedPE_38, HookedPE_39,
    HookedPE_40, HookedPE_41, HookedPE_42, HookedPE_43,
    HookedPE_44, HookedPE_45, HookedPE_46, HookedPE_47,
    HookedPE_48, HookedPE_49, HookedPE_50, HookedPE_51,
    HookedPE_52, HookedPE_53, HookedPE_54, HookedPE_55,
    HookedPE_56, HookedPE_57, HookedPE_58, HookedPE_59,
    HookedPE_60, HookedPE_61, HookedPE_62, HookedPE_63,
};

// ============================================================
// ワーカーDLL ロード/アンロード
// ============================================================

static HMODULE g_hWorker = nullptr;
typedef void* (*PFN_WorkerInit)();
typedef void (*PFN_WorkerShutdown)();
static char g_workerDir[MAX_PATH] = {};

static void LogLoader(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("[Loader] ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);

    // ファイルにも出力
    va_start(args, fmt);
    if (!g_loaderLogFile) {
        char logPath[MAX_PATH];
        snprintf(logPath, MAX_PATH, "%sloader_log.txt", g_workerDir);
        g_loaderLogFile = fopen(logPath, "w");
    }
    if (g_loaderLogFile) {
        fprintf(g_loaderLogFile, "[Loader] ");
        vfprintf(g_loaderLogFile, fmt, args);
        fprintf(g_loaderLogFile, "\n");
        fflush(g_loaderLogFile);
    }
    va_end(args);
}

// ============================================================
// HookDXGIPresent 実装 (LogLoader の後に配置)
// ============================================================

static bool HookDXGIPresent() {
    // ダミーウインドウ作成
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "DummyDX11Window";
    RegisterClassEx(&wc);
    HWND hwnd = CreateWindow(wc.lpszClassName, "", WS_OVERLAPPED,
                              0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        LogLoader("ダミーウインドウ作成失敗");
        return false;
    }

    // ダミー SwapChain + Device 作成
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* device = nullptr;
    IDXGISwapChain* dummySwapChain = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &desc, &dummySwapChain, &device, &featureLevel, &context);

    if (FAILED(hr)) {
        LogLoader("D3D11 ダミーデバイス作成失敗: 0x%08X", hr);
        DestroyWindow(hwnd);
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return false;
    }

    // SwapChain vtable から Present アドレス取得 (index 8)
    void** vtable = *reinterpret_cast<void***>(dummySwapChain);
    void* presentAddr = vtable[8];

    // ダミーリソース解放
    context->Release();
    dummySwapChain->Release();
    device->Release();
    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);

    // MinHook で Present をフック
    MH_STATUS st = MH_CreateHook(presentAddr, &HookedPresent,
                                  reinterpret_cast<void**>(&g_originalPresent));
    if (st != MH_OK) {
        LogLoader("Present フック作成失敗 (MH=%d)", st);
        return false;
    }
    st = MH_EnableHook(presentAddr);
    if (st != MH_OK) {
        LogLoader("Present フック有効化失敗 (MH=%d)", st);
        return false;
    }

    LogLoader("Present フック成功 (addr=0x%p)", presentAddr);
    return true;
}

static void UnloadWorker() {
    if (!g_hWorker) return;

    // コールバックを先にクリア (アトミック)
    g_wndProcCallback = nullptr;
    g_renderCallback = nullptr;
    g_workerCallback = nullptr;
    // インフライト呼び出しが完了するのを待つ
    Sleep(200);

    auto shutdownFn = reinterpret_cast<PFN_WorkerShutdown>(
        GetProcAddress(g_hWorker, "WorkerShutdown"));
    if (shutdownFn) {
        shutdownFn();
    }

    FreeLibrary(g_hWorker);
    g_hWorker = nullptr;
    LogLoader("ワーカーDLL アンロード完了");
}

static bool LoadWorker() {
    // ワーカーDLLのパス: version.dll と同じディレクトリ
    char workerPath[MAX_PATH];
    snprintf(workerPath, MAX_PATH, "%schat_translator.dll", g_workerDir);

    // ファイル存在確認
    DWORD attrib = GetFileAttributesA(workerPath);
    if (attrib == INVALID_FILE_ATTRIBUTES) {
        LogLoader("ワーカーDLL が見つかりません: %s", workerPath);
        return false;
    }

    // DLLロック回避: コピーしてからロード
    char tempPath[MAX_PATH];
    snprintf(tempPath, MAX_PATH, "%schat_translator_live.dll", g_workerDir);
    CopyFileA(workerPath, tempPath, FALSE);

    g_hWorker = LoadLibraryA(tempPath);
    if (!g_hWorker) {
        LogLoader("ワーカーDLL ロード失敗 (error=%lu)", GetLastError());
        return false;
    }

    auto initFn = reinterpret_cast<PFN_WorkerInit>(
        GetProcAddress(g_hWorker, "WorkerInit"));
    if (!initFn) {
        LogLoader("WorkerInit エクスポートが見つかりません");
        FreeLibrary(g_hWorker);
        g_hWorker = nullptr;
        return false;
    }

    LogLoader("ワーカーDLL ロード成功");
    void* callback = initFn();
    if (!callback) {
        LogLoader("WorkerInit 失敗 (コールバック取得できず)");
        return false;
    }

    // コールバックを設定 (フックから呼ばれるようになる)
    g_workerCallback = reinterpret_cast<WorkerCallbackFn>(callback);
    LogLoader("ワーカーコールバック設定完了");

    // フック済みPEアドレスをワーカーに通知
    typedef void (*PFN_SetPEAddr)(uintptr_t);
    auto setPEAddrFn = reinterpret_cast<PFN_SetPEAddr>(
        GetProcAddress(g_hWorker, "WorkerSetPEAddress"));
    if (setPEAddrFn && g_hookedPeAddr) {
        setPEAddrFn(g_hookedPeAddr);
        LogLoader("PE アドレス 0x%llX をワーカーに通知", g_hookedPeAddr);
    }

    // レンダーコールバックを取得
    typedef void* (*PFN_GetRenderCallback)();
    auto getRenderCb = reinterpret_cast<PFN_GetRenderCallback>(
        GetProcAddress(g_hWorker, "WorkerGetRenderCallback"));
    if (getRenderCb) {
        void* renderCb = getRenderCb();
        if (renderCb) {
            g_renderCallback = reinterpret_cast<RenderCallbackFn>(renderCb);
            LogLoader("レンダーコールバック設定完了");
        }
    }

    // WndProc コールバックを取得
    typedef void* (*PFN_GetWndProcCallback)();
    auto getWndProcCb = reinterpret_cast<PFN_GetWndProcCallback>(
        GetProcAddress(g_hWorker, "WorkerGetWndProcCallback"));
    if (getWndProcCb) {
        void* wndProcCb = getWndProcCb();
        if (wndProcCb) {
            g_wndProcCallback = reinterpret_cast<WndProcCallbackFn>(wndProcCb);
            LogLoader("WndProc コールバック設定完了");
        }
    }

    return true;
}

static void ReloadWorker() {
    LogLoader("=== ホットリロード開始 ===");
    UnloadWorker();
    Sleep(100); // DLLアンロード完了を待つ
    if (LoadWorker()) {
        LogLoader("=== ホットリロード完了 ===");
    } else {
        LogLoader("=== ホットリロード失敗 ===");
    }
}

// ============================================================
// 初期化スレッド
// ============================================================

static DWORD WINAPI InitThread(LPVOID param) {
    // デバッグコンソールを開く
    AllocConsole();
    SetConsoleTitleA("Foxhole Chat Translator - Diagnostic Mode");

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    FILE* conOut = nullptr;
    freopen_s(&conOut, "CONOUT$", "w", stdout);
    freopen_s(&conOut, "CONOUT$", "w", stderr);
    setvbuf(stdout, nullptr, _IOFBF, 4096);

    LogLoader("========================================");
    LogLoader("  Foxhole Chat Translator");
    LogLoader("========================================");

    // モジュール情報表示
    HMODULE gameModule = GetModuleHandleA(nullptr);
    uintptr_t moduleBase = reinterpret_cast<uintptr_t>(gameModule);
    DWORD moduleSize = 0;
    if (gameModule) {
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(gameModule);
        auto nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(moduleBase + dos->e_lfanew);
        moduleSize = nt->OptionalHeader.SizeOfImage;
        LogLoader("メインモジュール: base=0x%llX size=0x%X", moduleBase, moduleSize);
    }

    char configPath[MAX_PATH];
    snprintf(configPath, MAX_PATH, "%sconfig.ini", g_workerDir);

    int initDelay = GetPrivateProfileIntA("General", "InitDelayMs", 10000, configPath);
    LogLoader("UE4 初期化を待機中 (%dms)...", initDelay);
    Sleep(initDelay);

    // MinHook 初期化
    MH_STATUS mhStatus = MH_Initialize();
    if (mhStatus != MH_OK) {
        LogLoader("MinHook 初期化失敗 (status=%d)", mhStatus);
        return 1;
    }

    // ============================================================
    // UObject vtable からProcessEventを取得してフック
    // ============================================================
    void** vtable = nullptr;

    {
        const char* patterns[] = {
            "48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01",
            "48 8B 05 ?? ?? ?? ?? 48 3B ?? 48 0F 44",
            "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 48",
            "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 48 89 44 24",
            "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 88 ?? ?? 00 00",
            "48 89 05 ?? ?? ?? ?? 48 85 C0 74",
            nullptr
        };

        for (int i = 0; patterns[i]; i++) {
            uintptr_t match = scanner::FindPatternInModule(nullptr, patterns[i]);
            if (!match) continue;

            int32_t ripOff = *reinterpret_cast<int32_t*>(match + 3);
            uintptr_t resolved = match + 7 + ripOff;

            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(resolved), &mbi, sizeof(mbi)) ||
                mbi.State != MEM_COMMIT) continue;

            uintptr_t obj = *reinterpret_cast<uintptr_t*>(resolved);
            if (obj == 0) continue;

            void** vt = *reinterpret_cast<void***>(obj);
            uintptr_t vtAddr = reinterpret_cast<uintptr_t>(vt);
            if (vtAddr >= moduleBase && vtAddr < moduleBase + moduleSize) {
                vtable = vt;
                LogLoader("UObject vtable: 0x%p (パターン[%d])", vt, i);
                break;
            }
        }
    }

    if (!vtable) {
        LogLoader("vtable 取得失敗");
        MH_Uninitialize();
        return 1;
    }

    int peIndex = GetPrivateProfileIntA("Addresses", "ProcessEventVtableIndex", 66, configPath);
    uintptr_t peAddr = reinterpret_cast<uintptr_t>(vtable[peIndex]);
    LogLoader("ProcessEvent: vtable[%d] = 0x%llX (+0x%llX)",
              peIndex, peAddr, peAddr - moduleBase);

    // ============================================================
    // .rdata セクション内の全vtableから異なるvtable[peIndex]を収集
    // 改良版: vtable先頭を検出して偽陽性を除去
    // ============================================================
    uintptr_t uniquePEs[MAX_PE_HOOKS] = {};
    int uniquePECount = 0;
    uniquePEs[uniquePECount++] = peAddr; // 最初に発見したものを含む

    {
        // .rdata セクションを見つける
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase);
        auto nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(moduleBase + dos->e_lfanew);
        IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
        uintptr_t rdataStart = 0, rdataEnd = 0;
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            if (memcmp(sections[i].Name, ".rdata", 6) == 0) {
                rdataStart = moduleBase + sections[i].VirtualAddress;
                rdataEnd = rdataStart + sections[i].Misc.VirtualSize;
                break;
            }
        }

        if (rdataStart && rdataEnd > rdataStart) {
            LogLoader(".rdata: 0x%llX - 0x%llX (%llu KB)",
                      rdataStart, rdataEnd, (rdataEnd - rdataStart) / 1024);

            // 改良版vtable検出:
            // vtableの先頭を検出するため、直前のエントリがモジュール内コードを
            // 指さない位置を探す（vtableの境界）
            for (uintptr_t addr = rdataStart; addr + (peIndex + 1) * 8 <= rdataEnd; addr += 8) {
                __try {
                    // vtable先頭判定: addr[-1] (直前の8バイト) が
                    // モジュール内コードを指していない場合、vtable先頭の可能性
                    if (addr > rdataStart) {
                        uintptr_t prevVal = *reinterpret_cast<uintptr_t*>(addr - 8);
                        if (prevVal >= moduleBase && prevVal < moduleBase + moduleSize) {
                            // 直前もモジュール内 → vtable途中 → スキップ
                            continue;
                        }
                    }

                    // vtableらしさチェック: [0],[1],[2]がモジュール内コードを指すか
                    uintptr_t v0 = *reinterpret_cast<uintptr_t*>(addr);
                    uintptr_t v1 = *reinterpret_cast<uintptr_t*>(addr + 8);
                    uintptr_t v2 = *reinterpret_cast<uintptr_t*>(addr + 16);
                    if (v0 < moduleBase || v0 >= moduleBase + moduleSize) continue;
                    if (v1 < moduleBase || v1 >= moduleBase + moduleSize) continue;
                    if (v2 < moduleBase || v2 >= moduleBase + moduleSize) continue;

                    // vtable[peIndex]を読む
                    uintptr_t candidate = *reinterpret_cast<uintptr_t*>(addr + peIndex * 8);
                    if (candidate < moduleBase || candidate >= moduleBase + moduleSize) continue;
                    if (candidate == 0) continue;

                    // 既知のアドレスか確認
                    bool known = false;
                    for (int i = 0; i < uniquePECount; i++) {
                        if (uniquePEs[i] == candidate) { known = true; break; }
                    }
                    if (!known && uniquePECount < MAX_PE_HOOKS) {
                        uniquePEs[uniquePECount++] = candidate;
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            }
        }
        LogLoader("ユニークなvtable[%d]アドレス: %d 件発見", peIndex, uniquePECount);
    }

    // ============================================================
    // 全ユニークPEアドレスをフック
    // ============================================================
    int hookedCount = 0;
    for (int i = 0; i < uniquePECount && i < MAX_PE_HOOKS; i++) {
        uintptr_t addr = uniquePEs[i];
        MH_STATUS st = MH_CreateHook(
            reinterpret_cast<void*>(addr),
            reinterpret_cast<void*>(g_peHookFns[i]),
            reinterpret_cast<void**>(&g_originalPEs[i])
        );
        if (st == MH_OK) {
            st = MH_EnableHook(reinterpret_cast<void*>(addr));
            if (st == MH_OK) {
                g_hookedPeAddrs[i] = addr;
                g_peHookCount = i + 1;
                hookedCount++;
                if (i == 0) {
                    g_hookedPeAddr = addr;
                }
                LogLoader("  PE[%d] フック成功: +0x%llX", i, addr - moduleBase);
            } else {
                LogLoader("  PE[%d] 有効化失敗: +0x%llX (MH=%d)", i, addr - moduleBase, st);
            }
        } else {
            LogLoader("  PE[%d] 作成失敗: +0x%llX (MH=%d)", i, addr - moduleBase, st);
        }
    }
    LogLoader("ProcessEvent フック: %d/%d 成功 ✓", hookedCount, uniquePECount);

    // DXGI Present をフック
    if (HookDXGIPresent()) {
        LogLoader("Present フック完了 ✓");
    } else {
        LogLoader("Present フック失敗 (オーバーレイ無効)");
    }

    // ワーカーDLLをロード
    LogLoader("ワーカーDLL を読み込み中...");
    LoadWorker();

    // ワーカーDLLの最終更新時刻を記録 (自動リロード用)
    char workerDllPath[MAX_PATH];
    snprintf(workerDllPath, MAX_PATH, "%schat_translator.dll", g_workerDir);
    FILETIME lastWorkerWriteTime = {};
    {
        HANDLE hf = CreateFileA(workerDllPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
        if (hf != INVALID_HANDLE_VALUE) {
            GetFileTime(hf, nullptr, nullptr, &lastWorkerWriteTime);
            CloseHandle(hf);
        }
    }

    // ホットキー監視ループ
    LogLoader("F9 = リロード, F10 = アンロード, F11 = ステータス");
    LogLoader("自動リロード: chat_translator.dll 変更検知で自動リロードします");

    int integrityCheckCounter = 0;
    while (true) {
        if (GetAsyncKeyState(VK_F9) & 1) {
            ReloadWorker();
            // 更新時刻を再取得
            HANDLE hf = CreateFileA(workerDllPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
            if (hf != INVALID_HANDLE_VALUE) {
                GetFileTime(hf, nullptr, nullptr, &lastWorkerWriteTime);
                CloseHandle(hf);
            }
        }
        if (GetAsyncKeyState(VK_F10) & 1) {
            UnloadWorker();
        }
        if (GetAsyncKeyState(VK_F11) & 1) {
            LogLoader("ワーカー状態: %s", g_hWorker ? "ロード済み" : "未ロード");
            if (g_hookedPeAddr) {
                uint8_t* p = reinterpret_cast<uint8_t*>(g_hookedPeAddr);
                LogLoader("  フック先頭: %02X %02X %02X %02X %02X", p[0], p[1], p[2], p[3], p[4]);
            }
        }

        // 2秒ごとにファイル変更チェック → 自動リロード
        if (integrityCheckCounter % 20 == 0) {
            HANDLE hf = CreateFileA(workerDllPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
            if (hf != INVALID_HANDLE_VALUE) {
                FILETIME ft;
                if (GetFileTime(hf, nullptr, nullptr, &ft)) {
                    if (CompareFileTime(&ft, &lastWorkerWriteTime) != 0) {
                        CloseHandle(hf);
                        LogLoader(">>> chat_translator.dll 変更検知 → 自動リロード <<<");
                        lastWorkerWriteTime = ft;
                        Sleep(500); // ファイル書き込み完了を待つ
                        ReloadWorker();
                        // リロード後に最新の時刻を再取得
                        hf = CreateFileA(workerDllPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         nullptr, OPEN_EXISTING, 0, nullptr);
                        if (hf != INVALID_HANDLE_VALUE) {
                            GetFileTime(hf, nullptr, nullptr, &lastWorkerWriteTime);
                            CloseHandle(hf);
                        }
                        integrityCheckCounter++;
                        Sleep(100);
                        continue;
                    }
                }
                CloseHandle(hf);
            }
        }

        // 3秒ごとにフックバイト整合性チェック (全PEフック)
        integrityCheckCounter++;
        if (g_peHookCount > 0 && (integrityCheckCounter % 30 == 0)) {
            for (int i = 0; i < g_peHookCount; i++) {
                if (!g_hookedPeAddrs[i]) continue;
                uint8_t* p = reinterpret_cast<uint8_t*>(g_hookedPeAddrs[i]);
                if (p[0] != 0xE9 && g_originalPEs[i]) {
                    LogLoader("!!! PE[%d] JMPバイト消失! 先頭=%02X (復元試行)", i, p[0]);
                    MH_STATUS rs = MH_EnableHook(reinterpret_cast<void*>(g_hookedPeAddrs[i]));
                    LogLoader("  再有効化: MH_STATUS=%d, 先頭=%02X", rs, p[0]);
                }
            }
        }

        Sleep(100);
    }

    return 0;
}

// ============================================================
// DLL エントリポイント
// ============================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);

        // 自身のディレクトリを記録
        GetModuleFileNameA(hModule, g_workerDir, MAX_PATH);
        {
            char* lastSlash = strrchr(g_workerDir, '\\');
            if (lastSlash) lastSlash[1] = '\0';
        }

        // オリジナル version.dll をロード
        if (!LoadOriginalDll()) {
            return FALSE;
        }

        // 初期化スレッドを作成 (ローダーロック回避)
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        UnloadWorker();
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        if (g_hOriginalDll) {
            FreeLibrary(g_hOriginalDll);
            g_hOriginalDll = nullptr;
        }
        FreeConsole();
        break;
    }

    return TRUE;
}
