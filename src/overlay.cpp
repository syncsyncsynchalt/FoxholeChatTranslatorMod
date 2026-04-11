// ============================================================
// overlay.cpp - DX11 オーバーレイ描画 (ImGui)
// ゲームの Present フック内で呼ばれ、画面右上にラジオアイコンを表示する
// ============================================================

#include "overlay.h"
#include "log.h"
#include "radio_icon.h"

#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

// ============================================================
// 内部状態
// ============================================================

static bool                       g_initialized      = false;
static ID3D11Device*              g_device            = nullptr;
static ID3D11DeviceContext*       g_context           = nullptr;
static HWND                       g_hwnd              = nullptr;
static ID3D11ShaderResourceView*  g_radioTextureSRV   = nullptr;

// ============================================================
// テクスチャ作成 (埋め込み RGBA データから)
// ============================================================

static bool CreateRadioTexture() {
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width            = RADIO_ICON_WIDTH;
    texDesc.Height           = RADIO_ICON_HEIGHT;
    texDesc.MipLevels        = 1;
    texDesc.ArraySize        = 1;
    texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage            = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA subResource = {};
    subResource.pSysMem     = g_radioIconData;
    subResource.SysMemPitch = RADIO_ICON_WIDTH * 4;

    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = g_device->CreateTexture2D(&texDesc, &subResource, &texture);
    if (FAILED(hr)) {
        logging::Debug("[Overlay] テクスチャ作成失敗: 0x%08X", hr);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                    = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels       = 1;

    hr = g_device->CreateShaderResourceView(texture, &srvDesc, &g_radioTextureSRV);
    texture->Release();

    if (FAILED(hr)) {
        logging::Debug("[Overlay] SRV 作成失敗: 0x%08X", hr);
        return false;
    }
    return true;
}

// ============================================================
// ImGui 初期化 (最初の OnPresent で呼ばれる)
// ============================================================

static bool InitImGui(IDXGISwapChain* swapChain) {
    // SwapChain からデバイス取得
    HRESULT hr = swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_device);
    if (FAILED(hr)) {
        logging::Debug("[Overlay] GetDevice 失敗: 0x%08X", hr);
        return false;
    }
    g_device->GetImmediateContext(&g_context);

    // HWND 取得
    DXGI_SWAP_CHAIN_DESC desc;
    swapChain->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;

    // ImGui コンテキスト作成
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    // ラジオアイコンテクスチャ作成
    if (!CreateRadioTexture()) {
        logging::Debug("[Overlay] ラジオテクスチャ作成失敗");
    }

    logging::Debug("[Overlay] ImGui 初期化完了 (HWND=0x%p)", g_hwnd);
    return true;
}

// ============================================================
// フレーム描画
// ============================================================

static void RenderFrame(IDXGISwapChain* swapChain) {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // 画面右上にラジオアイコン表示
    ImGuiIO& io = ImGui::GetIO();
    float iconSize = 32.0f;
    float margin   = 10.0f;
    float x = io.DisplaySize.x - iconSize - margin;
    float y = margin;

    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(iconSize, iconSize));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGui::Begin("##radio_overlay", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);

    if (g_radioTextureSRV) {
        ImGui::Image((ImTextureID)g_radioTextureSRV, ImVec2(iconSize, iconSize));
    }

    ImGui::End();
    ImGui::PopStyleVar(2);

    ImGui::Render();

    // バックバッファに描画
    ID3D11Texture2D* backBuffer = nullptr;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (backBuffer) {
        ID3D11RenderTargetView* rtv = nullptr;
        g_device->CreateRenderTargetView(backBuffer, nullptr, &rtv);
        backBuffer->Release();
        if (rtv) {
            g_context->OMSetRenderTargets(1, &rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            rtv->Release();
        }
    }
}

// ============================================================
// 公開 API
// ============================================================

bool overlay::Init() {
    logging::Debug("[Overlay] Init (遅延初期化モード)");
    return true;
}

void overlay::OnPresent(void* swapChainPtr) {
    auto* swapChain = static_cast<IDXGISwapChain*>(swapChainPtr);

    if (!g_initialized) {
        if (!InitImGui(swapChain)) return;
        g_initialized = true;
    }

    RenderFrame(swapChain);
}

void overlay::Shutdown() {
    if (!g_initialized) return;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (g_radioTextureSRV) { g_radioTextureSRV->Release(); g_radioTextureSRV = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device)  { g_device->Release();  g_device  = nullptr; }

    g_initialized = false;
    g_hwnd = nullptr;

    logging::Debug("[Overlay] シャットダウン完了");
}
