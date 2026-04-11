// ============================================================
// dllmain.cpp - version.dll プロキシ + エントリポイント
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

#include "hooks.h"

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
// 初期化スレッド
// ============================================================

static DWORD WINAPI InitThread(LPVOID param) {
    // デバッグコンソールを開く
    AllocConsole();
    SetConsoleTitleA("Foxhole Chat Translator");

    // コンソールの出力コードページをUTF-8に設定
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    FILE* conOut = nullptr;
    freopen_s(&conOut, "CONOUT$", "w", stdout);
    freopen_s(&conOut, "CONOUT$", "w", stderr);
    // UTF-8 の setvbuf
    setvbuf(stdout, nullptr, _IOFBF, 4096);

    printf("[ChatTranslator] DLL ロード成功\n");
    printf("[ChatTranslator] UE4 初期化を待機中...\n");

    // UE4の初期化完了を待つ
    HMODULE gameModule = GetModuleHandleA(nullptr);
    if (gameModule) {
        // PEヘッダーからモジュール情報を取得
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(gameModule);
        auto nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(
            reinterpret_cast<uintptr_t>(gameModule) + dos->e_lfanew);
        printf("[ChatTranslator] メインモジュール: 0x%p (size=0x%X)\n",
               gameModule, nt->OptionalHeader.SizeOfImage);
    }

    // 設定ファイルから待機時間を取得する前にデフォルト値で待機
    Sleep(10000);

    printf("[ChatTranslator] 初期化開始...\n");

    // フックを初期化
    hooks::Init();

    printf("[ChatTranslator] チャット監視中 (Ctrl+C で停止)\n");

    return 0;
}

// ============================================================
// DLL エントリポイント
// ============================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);

        // オリジナル version.dll をロード
        if (!LoadOriginalDll()) {
            return FALSE;
        }

        // 初期化スレッドを作成 (ローダーロック回避)
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        hooks::Shutdown();
        if (g_hOriginalDll) {
            FreeLibrary(g_hOriginalDll);
            g_hOriginalDll = nullptr;
        }
        FreeConsole();
        break;
    }

    return TRUE;
}
