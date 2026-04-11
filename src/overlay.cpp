// ============================================================
// overlay.cpp - DX11 オーバーレイ描画 (ImGui)
// ゲームの Present フック内で呼ばれ、ラジオアイコン + 翻訳テキストを表示する
// ============================================================

#include "overlay.h"
#include "log.h"
#include "radio_icon.h"

#include <d3d11.h>
#include <dxgi.h>
#include <mmsystem.h>
#include <mutex>
#include <string>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

// ImGui Win32 WndProc ハンドラ (外部宣言)
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ============================================================
// 内部状態
// ============================================================

static bool                       g_initialized      = false;
static ID3D11Device*              g_device            = nullptr;
static ID3D11DeviceContext*       g_context           = nullptr;
static HWND                       g_hwnd              = nullptr;
static ID3D11ShaderResourceView*  g_radioTextureSRV   = nullptr;
static bool                       g_radioOn           = true;
static std::string                g_radioOnWav;
static std::string                g_radioOffWav;
static std::string                g_assetsDir;
static ImFont*                    g_cjkFont           = nullptr;

// 翻訳テキスト表示用 (スレッドセーフ)
static std::mutex                 g_textMutex;
static std::string                g_originalText;
static std::string                g_translatedText;

// マーキースクロール用
static float                      g_scrollOrig        = 0.0f;
static float                      g_scrollTrans       = 0.0f;

// デモモード: 5言語自動切替
struct DemoMessage {
    const char* original;
    const char* translated;
};
static const DemoMessage g_demoMessages[] = {
    { u8"We need more supplies at the front line!",
      u8"前線にもっと補給が必要だ！" },
    { u8"\u041d\u0430\u043c \u043d\u0443\u0436\u043d\u043e \u0431\u043e\u043b\u044c\u0448\u0435 \u0442\u0430\u043d\u043a\u043e\u0432 \u043d\u0430 \u0444\u0440\u043e\u043d\u0442\u0435!",
      u8"前線にさらに多くの戦車が必要です！" },
    { u8"\uc6b0\ub9ac \ud300\uc5d0 \ubcf4\uae09\uc774 \ub354 \ud544\uc694\ud574\uc694!",
      u8"チームに補給がもっと必要です！" },
    { u8"\u6211\u4eec\u9700\u8981\u5728\u524d\u7ebf\u90e8\u7f72\u66f4\u591a\u5766\u514b\uff01",
      u8"前線にさらに戦車を配置する必要がある！" },
    { u8"\u5f3e\u85ac\u304c\u5c3d\u304d\u305d\u3046\u3060\u3001\u88dc\u7d66\u3092\u983c\u3080\uff01",
      u8"\u5f3e\u85ac\u304c\u5c3d\u304d\u305d\u3046\u3060\u3001\u88dc\u7d66\u3092\u983c\u3080\uff01" },
    // Long messages for marquee scroll test
    { u8"Enemy tanks spotted near Jade Cove, we need anti-tank weapons and reinforcements immediately! All available units please respond!",
      u8"\u30b8\u30a7\u30a4\u30c9\u30b3\u30fc\u30d6\u4ed8\u8fd1\u3067\u6575\u306e\u6226\u8eca\u3092\u767a\u898b\u3001\u5bfe\u6226\u8eca\u5175\u5668\u3068\u588f\u63f4\u304c\u81f3\u6025\u5fc5\u8981\u3067\u3059\uff01\u5168\u90e8\u968a\u5fdc\u7b54\u3057\u3066\u304f\u3060\u3055\u3044\uff01" },
    { u8"\u041f\u0440\u043e\u0442\u0438\u0432\u043d\u0438\u043a \u043f\u0440\u043e\u0440\u0432\u0430\u043b \u043d\u0430\u0448\u0443 \u043e\u0431\u043e\u0440\u043e\u043d\u0443 \u043d\u0430 \u0432\u043e\u0441\u0442\u043e\u0447\u043d\u043e\u043c \u0444\u043b\u0430\u043d\u0433\u0435, \u0441\u0440\u043e\u0447\u043d\u043e \u043d\u0443\u0436\u043d\u044b \u043f\u043e\u0434\u043a\u0440\u0435\u043f\u043b\u0435\u043d\u0438\u044f \u0438 \u0431\u043e\u0435\u043f\u0440\u0438\u043f\u0430\u0441\u044b!",
      u8"\u6575\u304c\u6771\u5074\u9632\u885b\u7dda\u3092\u7a81\u7834\u3057\u307e\u3057\u305f\u3001\u588f\u63f4\u3068\u5f3e\u85ac\u304c\u81f3\u6025\u5fc5\u8981\u3067\u3059\uff01" },
};
static const int   g_demoCount    = sizeof(g_demoMessages) / sizeof(g_demoMessages[0]);
static int         g_demoIndex    = 0;
static float       g_demoTimer    = 0.0f;
static const float g_demoInterval = 10.0f; // 秒

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

    // CJK フォント読み込み
    std::string fontPath = g_assetsDir + "NotoSansCJKjp-Regular.otf";
    if (GetFileAttributesA(fontPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        // グリフ範囲: Latin + Cyrillic + CJK + Hangul + Full-width
        static const ImWchar glyphRanges[] = {
            0x0020, 0x00FF, // Basic Latin + Latin Supplement
            0x0400, 0x04FF, // Cyrillic
            0x2000, 0x206F, // General Punctuation
            0x3000, 0x30FF, // CJK Symbols, Hiragana, Katakana
            0x31F0, 0x31FF, // Katakana Phonetic Extensions
            0x4E00, 0x9FFF, // CJK Unified Ideographs
            0xAC00, 0xD7AF, // Hangul Syllables
            0xFF00, 0xFFEF, // Halfwidth and Fullwidth Forms
            0, 0
        };
        g_cjkFont = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 14.0f, nullptr, glyphRanges);
        if (g_cjkFont) {
            logging::Debug("[Overlay] CJK フォント読み込み成功: %s", fontPath.c_str());
        } else {
            logging::Debug("[Overlay] CJK フォント読み込み失敗: %s", fontPath.c_str());
        }
    } else {
        logging::Debug("[Overlay] CJK フォントが見つかりません: %s", fontPath.c_str());
    }

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
// マーキースクロール描画ヘルパー
// ============================================================

static void RenderMarqueeText(const char* label, const char* text, float areaWidth,
                              float& scrollPos, float deltaTime) {
    // "原文: " or "翻訳: " ラベル
    ImGui::TextUnformatted(label);
    ImGui::SameLine();

    float labelWidth = ImGui::GetCursorPosX();
    float childWidth = areaWidth - labelWidth;
    if (childWidth < 10.0f) childWidth = 10.0f;

    // ユニークID生成
    char childId[64];
    snprintf(childId, sizeof(childId), "##marquee_%s", label);

    ImGui::BeginChild(childId, ImVec2(childWidth, ImGui::GetTextLineHeightWithSpacing()),
                      false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImVec2 textSize = ImGui::CalcTextSize(text);
    if (textSize.x > childWidth) {
        // スクロールが必要
        scrollPos += 40.0f * deltaTime; // 40 px/秒
        float maxScroll = textSize.x - childWidth + 20.0f;
        if (scrollPos > maxScroll + 60.0f) scrollPos = -60.0f; // 巻き戻し (余白付き)
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
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    float iconSize = 32.0f;
    float margin   = 12.0f;
    float textAreaWidth = 297.0f;
    float textAreaHeight = 33.0f;
    float gap = 4.0f; // アイコンとテキスト間
    float padX = 6.0f; // 背景-テキスト間パディング
    float padY = 2.0f;

    // ウィンドウ全体サイズ (pad + テキスト + pad + gap + アイコン + pad)
    float totalWidth  = padX + textAreaWidth + padX + gap + iconSize + padX;
    float totalHeight = padY + textAreaHeight + padY;

    float winX = io.DisplaySize.x - totalWidth - margin;
    float winY = io.DisplaySize.y - totalHeight - margin;

    // ラジオON: 黒背景あり / OFF: 背景なし
    ImGui::SetNextWindowPos(ImVec2(winX, winY));
    ImGui::SetNextWindowSize(ImVec2(totalWidth, totalHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX, padY));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

    if (g_radioOn) {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.4f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    }

    ImGui::Begin("##radio_overlay", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

    // --- テキスト領域 (左側、ラジオON時のみ) ---
    if (g_radioOn) {
        // デモ: 自動切替
        g_demoTimer += io.DeltaTime;
        if (g_demoTimer >= g_demoInterval) {
            g_demoTimer = 0.0f;
            g_demoIndex = (g_demoIndex + 1) % g_demoCount;
            g_scrollOrig = 0.0f;
            g_scrollTrans = 0.0f;
            {
                std::lock_guard<std::mutex> lock(g_textMutex);
                g_originalText   = g_demoMessages[g_demoIndex].original;
                g_translatedText = g_demoMessages[g_demoIndex].translated;
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
        RenderMarqueeText(u8"\u539f\u6587: ", origText.c_str(), contentWidth,
                          g_scrollOrig, io.DeltaTime);
        RenderMarqueeText(u8"\u7ffb\u8a33: ", transText.c_str(), contentWidth,
                          g_scrollTrans, io.DeltaTime);

        ImGui::PopStyleVar();
        ImGui::EndChild();

        if (g_cjkFont) ImGui::PopFont();

        ImGui::SameLine(0, gap);
    } else {
        // OFF時: テキスト非表示、アイコンを右端に配置
        ImGui::SetCursorPosX(textAreaWidth + gap);
    }

    // --- ラジオアイコン (右側) ---
    float iconPadY = (totalHeight - iconSize) * 0.5f;
    ImGui::SetCursorPosY(iconPadY);
    if (g_radioTextureSRV) {
        float alpha = g_radioOn ? 1.0f : 0.3f;
        ImGui::Image((ImTextureID)g_radioTextureSRV, ImVec2(iconSize, iconSize),
                     ImVec2(0, 0), ImVec2(1, 1), ImVec4(1, 1, 1, alpha));
        if (ImGui::IsItemClicked()) {
            g_radioOn = !g_radioOn;
            logging::Debug("[Overlay] ラジオ %s", g_radioOn ? "ON" : "OFF");
            const char* wav = g_radioOn ? g_radioOnWav.c_str() : g_radioOffWav.c_str();
            PlaySoundA(wav, nullptr, SND_FILENAME | SND_ASYNC);
        }
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

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
    // DLLベースディレクトリからアセットパスを構築
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

    // デモ初期メッセージ
    g_originalText   = g_demoMessages[0].original;
    g_translatedText = g_demoMessages[0].translated;

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
    // ImGui がマウス入力を使いたい場合のみゲームへの転送をブロック
    if (ImGui::GetIO().WantCaptureMouse) return result;
    return 0;
}

bool overlay::IsRadioOn() {
    return g_radioOn;
}

void overlay::SetDisplayText(const char* original, const char* translated) {
    std::lock_guard<std::mutex> lock(g_textMutex);
    g_originalText   = original  ? original  : "";
    g_translatedText = translated ? translated : "";
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
