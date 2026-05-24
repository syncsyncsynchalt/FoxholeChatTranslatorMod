// ============================================================
// overlay.cpp - DX11 オーバーレイ描画 (ImGui)
// ゲームの Present フック内で呼ばれ、ラジオアイコン + 翻訳テキストを表示する
// ============================================================

#include "overlay.h"
#include "ollama.h"
#include "log.h"
#include "radio_icon.h"
#include "tts.h"
#include "translate.h"
#include "config.h"

#include <d3d11.h>
#include <dxgi.h>
#include <mmsystem.h>
#include <atomic>
#include <mutex>
#include <string>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ============================================================
// 内部状態
// ============================================================

static bool                       g_initialized    = false;
static ID3D11Device*              g_device          = nullptr;
static ID3D11DeviceContext*       g_context         = nullptr;
static HWND                       g_hwnd            = nullptr;
static ID3D11ShaderResourceView*  g_radioTextureSRV = nullptr;
static std::string                g_radioOnWav;
static std::string                g_radioOffWav;
static std::string                g_assetsDir;
static ImFont*                    g_cjkFont         = nullptr;

static const ImWchar g_cjkGlyphRanges[] = {
    0x0020, 0x00FF,
    0x0400, 0x04FF,
    0x2000, 0x206F,
    0x3000, 0x30FF,
    0x31F0, 0x31FF,
    0x4E00, 0x9FFF,
    0xAC00, 0xD7AF,
    0xFF00, 0xFFEF,
    0, 0
};

static std::mutex   g_textMutex;
static std::string  g_originalText;
static std::string  g_translatedText;

static float g_scrollOrig  = 0.0f;
static float g_scrollTrans = 0.0f;

// デモモード: 5言語自動切替
static const char* g_demoMessages[] = {
    u8"We need more supplies at the front line!",
    u8"Нам нужно больше танков на фронте!",
    u8"우리 팀에 보급이 더 필요해요!",
    u8"我们需要在前线部署更多坦克！",
    u8"弾薬が尽きそうだ、補給を頼む！",
    u8"Enemy tanks spotted near Jade Cove, we need anti-tank weapons and reinforcements immediately! All available units please respond!",
    u8"Противник прорвал нашу оборону на восточном фланге, срочно нужны подкрепления и боеприпасы!",
};
static const int   g_demoCount    = sizeof(g_demoMessages) / sizeof(g_demoMessages[0]);
static int         g_demoIndex    = 0;
static float       g_demoTimer    = 0.0f;
static const float g_demoInterval = 10.0f;

static std::atomic<bool> g_textChanged{false};

// ============================================================
// テクスチャ作成
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
    srvDesc.Format              = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    hr = g_device->CreateShaderResourceView(texture, &srvDesc, &g_radioTextureSRV);
    texture->Release();

    if (FAILED(hr)) {
        logging::Debug("[Overlay] SRV 作成失敗: 0x%08X", hr);
        return false;
    }
    return true;
}

// ============================================================
// ImGui 初期化
// ============================================================

static bool InitImGui(IDXGISwapChain* swapChain) {
    HRESULT hr = swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_device);
    if (FAILED(hr)) {
        logging::Debug("[Overlay] GetDevice 失敗: 0x%08X", hr);
        return false;
    }
    g_device->GetImmediateContext(&g_context);

    DXGI_SWAP_CHAIN_DESC desc;
    swapChain->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    std::string fontPath = g_assetsDir + "NotoSansCJKjp-Regular.otf";
    if (GetFileAttributesA(fontPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_cjkFont = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 14.0f, nullptr, g_cjkGlyphRanges);
        if (g_cjkFont) {
            logging::Debug("[Overlay] CJK フォント読み込み成功: %s", fontPath.c_str());
        } else {
            logging::Debug("[Overlay] CJK フォント読み込み失敗: %s", fontPath.c_str());
        }
    } else {
        logging::Debug("[Overlay] CJK フォントが見つかりません (後でロード試行): %s", fontPath.c_str());
    }

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    if (!CreateRadioTexture()) {
        logging::Debug("[Overlay] ラジオテクスチャ作成失敗");
    }

    logging::Debug("[Overlay] ImGui 初期化完了 (HWND=0x%p)", g_hwnd);
    return true;
}

// ============================================================
// マーキースクロール描画ヘルパー
// ============================================================

static void RenderMarqueeText(const char* label, const char* text, float areaWidth,
                              float& scrollPos, float deltaTime) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine();

    float labelWidth = ImGui::GetCursorPosX();
    float childWidth = areaWidth - labelWidth;
    if (childWidth < 10.0f) childWidth = 10.0f;

    char childId[64];
    snprintf(childId, sizeof(childId), "##marquee_%s", label);

    ImGui::BeginChild(childId, ImVec2(childWidth, ImGui::GetTextLineHeightWithSpacing()),
                      false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImVec2 textSize = ImGui::CalcTextSize(text);
    if (textSize.x > childWidth) {
        scrollPos += 40.0f * deltaTime;
        float maxScroll = textSize.x - childWidth + 20.0f;
        if (scrollPos > maxScroll + 60.0f) scrollPos = -60.0f;
        ImGui::SetCursorPosX(-scrollPos);
    } else {
        scrollPos = 0.0f;
    }
    ImGui::TextUnformatted(text);

    ImGui::EndChild();
}

// ============================================================
// フレーム描画
// ============================================================

static void RenderFrame(IDXGISwapChain* swapChain) {
    // フォントが未ロードの場合、300フレームごとにファイル出現を確認してホットロード
    if (g_cjkFont == nullptr) {
        static int s_fontCheckFrame = 0;
        if (++s_fontCheckFrame >= 300) {
            s_fontCheckFrame = 0;
            std::string fontPath = g_assetsDir + "NotoSansCJKjp-Regular.otf";
            if (GetFileAttributesA(fontPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                ImGuiIO& io = ImGui::GetIO();
                g_cjkFont = io.Fonts->AddFontFromFileTTF(
                    fontPath.c_str(), 14.0f, nullptr, g_cjkGlyphRanges);
                if (g_cjkFont) {
                    ImGui_ImplDX11_InvalidateDeviceObjects();
                    ImGui_ImplDX11_CreateDeviceObjects();
                    logging::Debug("[Overlay] CJK フォント ホットロード完了");
                }
            }
        }
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    float iconSize      = 32.0f;
    float margin        = 12.0f;
    float textAreaWidth = 297.0f;
    float textAreaHeight = 33.0f;
    float gap  = 4.0f;
    float padX = 6.0f;
    float padY = 2.0f;

    float totalWidth  = padX + textAreaWidth + padX + gap + iconSize + padX;
    float totalHeight = padY + textAreaHeight + padY;

    float winX = io.DisplaySize.x - totalWidth - margin;
    float winY = io.DisplaySize.y - totalHeight - margin;

    if (g_textChanged.exchange(false)) {
        g_scrollOrig  = 0.0f;
        g_scrollTrans = 0.0f;
    }

    RadioState state = ollama::GetRadioState();

    ImGui::SetNextWindowPos(ImVec2(winX, winY));
    ImGui::SetNextWindowSize(ImVec2(totalWidth, totalHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX, padY));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

    if (state == RadioState::ON || state == RadioState::RESTARTING) {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.4f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    }

    ImGui::Begin("##radio_overlay", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

    // --- テキスト領域 (ラジオ ON / FAULT / RESTARTING 時) ---
    if (state != RadioState::OFF) {
        if (config::Get().demoMode) {
            g_demoTimer += io.DeltaTime;
            if (g_demoTimer >= g_demoInterval) {
                g_demoTimer = 0.0f;
                g_demoIndex = (g_demoIndex + 1) % g_demoCount;
                translate::Queue("", "", g_demoMessages[g_demoIndex]);
            }
        }

        std::string origText, transText;
        {
            std::lock_guard<std::mutex> lock(g_textMutex);
            origText  = g_originalText;
            transText = g_translatedText;
        }

        if (g_cjkFont) ImGui::PushFont(g_cjkFont);

        ImGui::BeginChild("##text_area", ImVec2(textAreaWidth, textAreaHeight),
                          false, ImGuiWindowFlags_NoScrollbar);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1));

        float contentWidth = textAreaWidth;
        RenderMarqueeText(u8"原文: ", origText.c_str(), contentWidth,
                          g_scrollOrig, io.DeltaTime);
        RenderMarqueeText(u8"翻訳: ", transText.c_str(), contentWidth,
                          g_scrollTrans, io.DeltaTime);

        ImGui::PopStyleVar();
        ImGui::EndChild();

        if (g_cjkFont) ImGui::PopFont();

        ImGui::SameLine(0, gap);
    } else {
        ImGui::SetCursorPosX(textAreaWidth + gap);
    }

    // --- ラジオアイコン ---
    float iconPadY = (totalHeight - iconSize) * 0.5f;
    ImGui::SetCursorPosY(iconPadY);
    if (g_radioTextureSRV) {
        ImVec4 tint;
        switch (state) {
        case RadioState::ON:         tint = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); break;
        case RadioState::OFF:        tint = ImVec4(1.0f, 1.0f, 1.0f, 0.3f); break;
        case RadioState::FAULT:      tint = ImVec4(1.0f, 0.3f, 0.3f, 1.0f); break;
        case RadioState::RESTARTING: tint = ImVec4(1.0f, 0.8f, 0.0f, 1.0f); break;
        }
        ImGui::Image((ImTextureID)g_radioTextureSRV, ImVec2(iconSize, iconSize),
                     ImVec2(0, 0), ImVec2(1, 1), tint);
        if (ImGui::IsItemClicked()) {
            if (state == RadioState::FAULT) {
                logging::Debug("[Overlay] Ollama 再起動クリック");
                ollama::RequestRestart();
            } else if (state == RadioState::ON || state == RadioState::OFF) {
                bool wasOn = (state == RadioState::ON);
                ollama::SetUserEnabled(!wasOn);
                logging::Debug("[Overlay] ラジオ %s", wasOn ? "OFF" : "ON");
                const char* wav = wasOn ? g_radioOffWav.c_str() : g_radioOnWav.c_str();
                PlaySoundA(wav, nullptr, SND_FILENAME | SND_ASYNC);
            }
        }
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    ImGui::Render();

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
    char dllPath[MAX_PATH];
    HMODULE hSelf;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&overlay::Init), &hSelf);
    GetModuleFileNameA(hSelf, dllPath, MAX_PATH);
    std::string dir(dllPath);
    size_t lastSlash = dir.rfind('\\');
    if (lastSlash != std::string::npos) dir = dir.substr(0, lastSlash + 1);
    g_assetsDir   = dir + "assets\\";
    g_radioOnWav  = g_assetsDir + "radio_on.wav";
    g_radioOffWav = g_assetsDir + "radio_off.wav";

    translate::SetResultCallback([](const translate::TranslateResult& r) {
        {
            std::lock_guard<std::mutex> lock(g_textMutex);
            g_originalText   = r.original;
            g_translatedText = r.translated;
        }
        g_textChanged.store(true);
        logging::Translation(r.channel.c_str(), r.sender.c_str(),
                             r.original.c_str(), r.translated.c_str());
        const char* ttsText = config::Get().ttsSpeakTranslated ? r.translated.c_str() : r.original.c_str();
        tts::Speak(ttsText, r.sender.empty() ? nullptr : r.sender.c_str());
    });

    if (config::Get().demoMode) {
        translate::Queue("", "", g_demoMessages[0]);
    }

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

LRESULT overlay::OnWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!g_initialized) return 0;
    LRESULT result = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
    if (ImGui::GetIO().WantCaptureMouse) return result;
    return 0;
}

bool overlay::IsRadioOn() {
    return ollama::GetRadioState() == RadioState::ON;
}

void overlay::SetDisplayText(const char* original, const char* translated) {
    std::lock_guard<std::mutex> lock(g_textMutex);
    g_originalText   = original   ? original   : "";
    g_translatedText = translated ? translated : "";
}

void overlay::OnChatMessage(const std::string& sender, const std::string& message) {
    if (config::Get().demoMode) return;
    if (ollama::GetRadioState() != RadioState::ON) return;
    if (message.empty()) return;

    {
        size_t a = message.find_first_not_of(" \t\r\n");
        size_t b = message.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) return;
        std::string trimmed = message.substr(a, b - a + 1);
        bool hasSpace = trimmed.find_first_of(" \t") != std::string::npos;
        bool hasMultiByte = false;
        for (unsigned char c : trimmed) {
            if (c >= 0x80) { hasMultiByte = true; break; }
        }
        if (!hasSpace && !hasMultiByte) {
            logging::Debug("[Overlay] 1単語のためスキップ: %s", trimmed.c_str());
            return;
        }
    }

    translate::Queue("", sender, message);
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
