// ============================================================
// overlay_test.cpp - オーバーレイ UI テストホスト
// ゲームなしで chat_translator.dll のオーバーレイを確認する
//
// 操作:
//   TL ボタン  : クリックで翻訳モード循環 (TL:-- -> TL:JA -> ... -> TL:KO)
//   TTS ボタン : クリックで TTS モード循環 (TTS:-- -> TTS:Src -> TTS:Tr)
//   F1         : デモメッセージを手動送信
//   ESC        : 終了
//
// 注意: version.dll と同ディレクトリに置くと D3D11 初期化がクラッシュする。
//       CMake が build/overlay_test/ に出力するのはこのため。
// ============================================================

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ============================================================
// DLL 関数型
// ============================================================

typedef void*   (*PFN_WorkerInitTest)(const char* baseDir);
typedef void    (*PFN_WorkerShutdown)();
typedef void*   (*PFN_WorkerGetRenderCallback)();
typedef void*   (*PFN_WorkerGetWndProcCallback)();
typedef void    (*PFN_OnPresent)(void* swapChain);
typedef LRESULT (*PFN_OnWndProc)(HWND, UINT, WPARAM, LPARAM);
typedef void    (*PFN_OnChatMessage)(const char* sender, const char* message);

// ============================================================
// グローバル状態
// ============================================================

static HMODULE           g_hDll        = nullptr;
static PFN_OnPresent     g_fnPresent   = nullptr;
static PFN_OnWndProc     g_fnWndProc   = nullptr;
static PFN_OnChatMessage g_fnChatMsg   = nullptr;
static PFN_WorkerShutdown g_fnShutdown = nullptr;

static ID3D11Device*           g_device    = nullptr;
static ID3D11DeviceContext*    g_context   = nullptr;
static IDXGISwapChain*         g_swapChain = nullptr;
static ID3D11RenderTargetView* g_rtv       = nullptr;
static HWND                    g_hwnd      = nullptr;

// ============================================================
// デモメッセージ (F1 キーで順番に送信)
// ============================================================

static const char* kTestMessages[] = {
    "We need more supplies at the front line!",
    "Enemy tanks spotted near Jade Cove, need anti-tank weapons!",
    "Противник прорвал оборону на восточном фланге!",
    "우리 팀에 보급이 더 필요해요!",
    "我们需要在前线部署更多坦克！",
    "弾薬が尽きそうだ、補給を頼む！",
};
static const int kTestMessageCount = sizeof(kTestMessages) / sizeof(kTestMessages[0]);
static int g_msgIndex = 0;

// ============================================================
// DX11 初期化
// ============================================================

static bool CreateDeviceAndSwapChain(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount          = 2;
    scd.BufferDesc.Width     = 0;
    scd.BufferDesc.Height    = 0;
    scd.BufferDesc.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage          = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow         = hwnd;
    scd.SampleDesc.Count     = 1;
    scd.Windowed             = TRUE;
    scd.SwapEffect           = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &g_swapChain, &g_device, &featureLevel, &g_context);
    if (FAILED(hr)) {
        printf("[DX11] D3D11CreateDeviceAndSwapChain 失敗: 0x%08X\n", hr);
        return false;
    }
    return true;
}

static void CreateRTV() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    ID3D11Texture2D* backBuffer = nullptr;
    g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (backBuffer) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
        backBuffer->Release();
    }
}

// ============================================================
// ウィンドウプロシージャ
// ============================================================

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_fnWndProc) {
        LRESULT r = g_fnWndProc(hwnd, msg, wp, lp);
        if (r) return r;
    }

    switch (msg) {
    case WM_SIZE:
        if (g_swapChain && wp != SIZE_MINIMIZED) {
            if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
            g_swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
            CreateRTV();
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            PostQuitMessage(0);
        } else if (wp == VK_F1 && g_fnChatMsg) {
            const char* msg_text = kTestMessages[g_msgIndex % kTestMessageCount];
            g_fnChatMsg("TestUser", msg_text);
            printf("[F1] メッセージ送信: %s\n", msg_text);
            g_msgIndex++;
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// ============================================================
// エントリポイント
// ============================================================

int main() {
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    SetConsoleOutputCP(CP_UTF8);
    printf("=== Overlay Test Host ===\n");
    printf("操作: TL/TTS ボタンをクリック | F1=テストメッセージ送信 | ESC=終了\n\n");

    // ウィンドウ登録・作成
    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "OverlayTestHost";
    RegisterClassExA(&wc);

    g_hwnd = CreateWindowExA(
        0, "OverlayTestHost",
        "Overlay Test Host  [F1: テストメッセージ送信]  [ESC: 終了]",
        WS_OVERLAPPEDWINDOW,
        100, 100, 1280, 720,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!g_hwnd) {
        printf("FAIL: ウィンドウ作成失敗\n");
        system("pause");
        return 1;
    }

    if (!CreateDeviceAndSwapChain(g_hwnd)) {
        system("pause");
        return 1;
    }
    CreateRTV();
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    printf("OK: DX11 ウィンドウ作成完了\n");

    // DLL ロード (DX11 初期化後にロードすること: version.dll との競合を避けるため)
    g_hDll = LoadLibraryA("chat_translator.dll");
    if (!g_hDll) {
        printf("FAIL: chat_translator.dll が見つかりません (error=%lu)\n", GetLastError());
        printf("build\\overlay_test\\ ディレクトリから実行してください\n");
        system("pause");
        return 1;
    }
    printf("OK: chat_translator.dll ロード完了\n");

    auto fnInitTest  = (PFN_WorkerInitTest)         GetProcAddress(g_hDll, "WorkerInitTest");
    g_fnShutdown     = (PFN_WorkerShutdown)          GetProcAddress(g_hDll, "WorkerShutdown");
    auto fnGetWndCb  = (PFN_WorkerGetWndProcCallback)GetProcAddress(g_hDll, "WorkerGetWndProcCallback");
    g_fnChatMsg      = (PFN_OnChatMessage)           GetProcAddress(g_hDll, "WorkerOnChatMessage");

    if (!fnInitTest) {
        printf("FAIL: WorkerInitTest エクスポートが見つかりません\n");
        system("pause");
        FreeLibrary(g_hDll);
        return 1;
    }

    // DLL 初期化 (baseDir = 実行ファイルのディレクトリ)
    char baseDir[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, baseDir, MAX_PATH);
    char* lastSlash = strrchr(baseDir, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    printf("BaseDir: %s\n", baseDir);

    void* renderCb = fnInitTest(baseDir);
    g_fnPresent = (PFN_OnPresent)renderCb;

    if (fnGetWndCb) {
        g_fnWndProc = (PFN_OnWndProc)fnGetWndCb();
    }

    printf("OK: WorkerInitTest 完了\n");
    printf("\n--- テスト手順 ---\n");
    printf("1. 右下のオーバーレイに Orig:/Tran: の2行と TL/TTS ボタンが見えること\n");
    printf("2. TL ボタンを7回クリックして TL:JAZ->TL:EN->TL:RU->TL:ZH->TL:KO->TL:-->TL:JA->TL:JAZ と循環すること\n");
    printf("3. TL:--> のとき Tran: [Off] が表示されること\n");
    printf("4. TTS ボタンを3回クリックして TTS:Tr->TTS:-->TTS:Src->TTS:Tr と循環すること\n");
    printf("5. F1 キーでテストメッセージが Orig: 行に表示されること\n");
    printf("6. DemoMode=1 の場合は自動でメッセージが流れること\n");
    if (!g_fnChatMsg) {
        printf("INFO: WorkerOnChatMessage 未エクスポート - F1 によるメッセージ送信は無効\n");
    }
    printf("------------------\n\n");

    // メインループ
    MSG winMsg = {};
    while (winMsg.message != WM_QUIT) {
        while (PeekMessageA(&winMsg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&winMsg);
            DispatchMessageA(&winMsg);
            if (winMsg.message == WM_QUIT) goto done;
        }

        if (g_rtv) {
            const float clearColor[4] = {0.08f, 0.08f, 0.12f, 1.0f};
            g_context->ClearRenderTargetView(g_rtv, clearColor);
        }

        if (g_fnPresent) {
            g_fnPresent(g_swapChain);
        }

        g_swapChain->Present(1, 0);
    }

done:
    printf("終了処理中...\n");

    if (g_fnShutdown) g_fnShutdown();
    if (g_rtv)        { g_rtv->Release();       g_rtv       = nullptr; }
    if (g_swapChain)  { g_swapChain->Release();  g_swapChain = nullptr; }
    if (g_context)    { g_context->Release();    g_context   = nullptr; }
    if (g_device)     { g_device->Release();     g_device    = nullptr; }
    if (g_hDll)       { FreeLibrary(g_hDll);     g_hDll      = nullptr; }

    printf("終了\n");
    return 0;
}
