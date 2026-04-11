// ============================================================
// test_loader.cpp - DLL動作確認テスト
// version.dll プロキシの基本動作をゲーム外で検証
// ============================================================

#include <windows.h>
#include <cstdio>

int main() {
    printf("=== Foxhole Chat Translator - DLL Load Test ===\n\n");

    // 1. テスト: version.dll のプロキシ関数が動くか
    printf("[Test 1] version.dll proxy functions\n");

    // ゲームディレクトリにある version.dll を明示的にロード
    HMODULE hVersion = LoadLibraryA("version.dll");
    if (!hVersion) {
        printf("  FAIL: LoadLibrary(version.dll) failed, error=%lu\n", GetLastError());
        printf("  This test must be run from the game directory.\n");
        return 1;
    }
    printf("  OK: version.dll loaded at 0x%p\n", hVersion);

    // プロキシ関数の存在確認
    const char* exports[] = {
        "GetFileVersionInfoA",
        "GetFileVersionInfoW",
        "GetFileVersionInfoSizeA",
        "GetFileVersionInfoSizeW",
        "VerQueryValueA",
        "VerQueryValueW"
    };

    bool allOk = true;
    for (const char* name : exports) {
        FARPROC proc = GetProcAddress(hVersion, name);
        if (proc) {
            printf("  OK: %s = 0x%p\n", name, proc);
        } else {
            printf("  FAIL: %s not found\n", name);
            allOk = false;
        }
    }

    // 2. テスト: プロキシ経由で実際に GetFileVersionInfoSizeA を呼べるか
    printf("\n[Test 2] Proxy function call\n");
    typedef DWORD(WINAPI* PFN_GetFileVersionInfoSizeA)(LPCSTR, LPDWORD);
    auto pfn = reinterpret_cast<PFN_GetFileVersionInfoSizeA>(
        GetProcAddress(hVersion, "GetFileVersionInfoSizeA"));

    if (pfn) {
        DWORD handle = 0;
        DWORD size = pfn("kernel32.dll", &handle);
        if (size > 0) {
            printf("  OK: GetFileVersionInfoSizeA(kernel32.dll) = %lu bytes\n", size);
        } else {
            printf("  WARN: GetFileVersionInfoSizeA returned 0 (error=%lu)\n", GetLastError());
        }
    } else {
        printf("  SKIP: Function not available\n");
    }

    // 3. コンソールが開いたか確認 (DLLのInitThreadが動いていれば別コンソールが出る)
    printf("\n[Test 3] Check console output\n");
    printf("  If a separate console window titled 'Foxhole Chat Translator' appeared,\n");
    printf("  the DLL initialization thread is working correctly.\n");
    printf("  (It will try to find ProcessEvent in THIS process and fail - that's expected.)\n");

    // 少し待ってDLLの初期化スレッドの出力を見る
    printf("\n  Waiting 15 seconds for DLL init thread...\n");
    printf("  Check the OTHER console window for output.\n");
    Sleep(15000);

    printf("\n[Done] Press Enter to unload DLL and exit.\n");
    getchar();

    FreeLibrary(hVersion);
    return allOk ? 0 : 1;
}
