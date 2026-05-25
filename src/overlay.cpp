// ============================================================
// overlay.cpp - DX11 オーバーレイ描画 (ImGui)
// ゲームの Present フック内で呼ばれ、翻訳テキストと切り替えボタンを表示する
// ============================================================

#include "overlay.h"
#include "ollama.h"
#include "log.h"
#include "tts.h"
#include "translate.h"
#include "config.h"

#include <d3d11.h>
#include <dxgi.h>
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

static bool                       g_initialized = false;
static ID3D11Device*              g_device      = nullptr;
static ID3D11DeviceContext*       g_context     = nullptr;
static ID3D11RenderTargetView*    g_rtv              = nullptr;
static ID3D11Texture2D*           g_cachedBackBuffer = nullptr; // リサイズ検出用
static HWND                       g_hwnd        = nullptr;
static std::string                g_assetsDir;
static ImFont*                    g_cjkFont     = nullptr;

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

    logging::Debug("[Overlay] ImGui 初期化完了 (HWND=0x%p)", g_hwnd);
    return true;
}

// ============================================================
// マーキースクロール描画ヘルパー
// ============================================================

static void RenderMarqueeText(const char* id, const char* text, float areaWidth,
                              float& scrollPos, float deltaTime) {
    float childWidth = (areaWidth < 10.0f) ? 10.0f : areaWidth;

    ImGui::BeginChild(id, ImVec2(childWidth, ImGui::GetTextLineHeightWithSpacing()),
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
// TL/TTS ボタンラベル
// ============================================================

static const char* TranslationModeLabel(TranslationMode m) {
    switch (m) {
    case TranslationMode::Off: return "TL:--";
    case TranslationMode::JA:  return "TL:JA";
    case TranslationMode::JAZ: return "TL:JAZ";
    case TranslationMode::EN:  return "TL:EN";
    case TranslationMode::RU:  return "TL:RU";
    case TranslationMode::ZH:  return "TL:ZH";
    case TranslationMode::KO:  return "TL:KO";
    default:                   return "TL:??";
    }
}

static const char* TtsModeLabel(TtsMode m) {
    switch (m) {
    case TtsMode::Off:        return "TTS:--";
    case TtsMode::Original:   return "TTS:Src";
    case TtsMode::Translated: return "TTS:Tr";
    default:                  return "TTS:??";
    }
}

// ============================================================
// フレーム描画
// ============================================================

static void RenderFrame() {
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

    // レイアウト定数
    static const float kTextWidth = 270.0f;
    static const float kGap       = 4.0f;
    static const float kPadX      = 6.0f;
    static const float kPadY      = 3.0f;
    static const float kMargin    = 12.0f;
    static const float kFPX       = 4.0f;  // FramePadding.x

    // フォントを先に Push してメトリクスを正確に取得
    if (g_cjkFont) ImGui::PushFont(g_cjkFont);

    float lineH   = ImGui::GetTextLineHeight();
    float lineHWS = ImGui::GetTextLineHeightWithSpacing();

    // ボタン幅 = 全ラベルの最大テキスト幅 + 両側 FramePadding + 余白
    // フォント変更時のみ再計算 (毎フレームの CalcTextSize を避ける)
    static ImFont* s_lastFont   = nullptr;
    static float   s_cachedBtnW = 0.0f;
    if (s_lastFont != ImGui::GetFont()) {
        s_lastFont = ImGui::GetFont();
        static const char* kAllLabels[] = {
            "TL:--","TL:JA","TL:JAZ","TL:EN","TL:RU","TL:ZH","TL:KO",
            "TTS:--","TTS:Src","TTS:Tr", nullptr
        };
        float maxLabelW = 0.0f;
        for (int i = 0; kAllLabels[i]; ++i) {
            float w = ImGui::CalcTextSize(kAllLabels[i]).x;
            if (w > maxLabelW) maxLabelW = w;
        }
        s_cachedBtnW = maxLabelW + 2.0f * kFPX + 4.0f;
    }
    float kBtnWidth = s_cachedBtnW;

    float totalW = kPadX + kBtnWidth + kGap + kTextWidth + kPadX;
    float totalH = kPadY + lineHWS + lineHWS + kPadY;

    float winX = io.DisplaySize.x - totalW - kMargin;
    float winY = io.DisplaySize.y - totalH - kMargin;

    if (g_textChanged.exchange(false)) {
        g_scrollOrig  = 0.0f;
        g_scrollTrans = 0.0f;
    }

    // デモモード: タイマー駆動でメッセージを切り替え
    if (config::Get().demoMode) {
        g_demoTimer += io.DeltaTime;
        if (g_demoTimer >= g_demoInterval) {
            g_demoTimer = 0.0f;
            g_demoIndex = (g_demoIndex + 1) % g_demoCount;
            TranslationMode mode = config::GetTranslationMode();
            if (mode != TranslationMode::Off) {
                translate::Queue("", "", g_demoMessages[g_demoIndex]);
            } else {
                std::lock_guard<std::mutex> lock(g_textMutex);
                g_originalText   = g_demoMessages[g_demoIndex];
                g_translatedText = "";
            }
        }
    }

    static std::string origText, transText;
    {
        std::lock_guard<std::mutex> lock(g_textMutex);
        origText  = g_originalText;
        transText = g_translatedText;
    }

    // 翻訳行のステータステキストを決定
    RadioState radioState = ollama::GetRadioState();
    std::string statusText;
    if (config::GetTranslationMode() == TranslationMode::Off) {
        statusText = "[Off]";
    } else if (radioState == RadioState::RESTARTING) {
        int attempt = 0, maxAttempts = 0;
        ollama::GetRestartProgress(attempt, maxAttempts);
        char buf[64];
        snprintf(buf, sizeof(buf), "[Restarting %d/%d...]", attempt, maxAttempts);
        statusText = buf;
    } else if (radioState == RadioState::FAULT) {
        statusText = "[Error]";
    } else {
        statusText = transText;
    }

    // 初回のみデフォルト位置 (右下) に配置。以降はドラッグで移動した位置を維持
    static ImVec2 s_winPos = ImVec2(-1.f, -1.f);
    if (s_winPos.x < 0.f) s_winPos = ImVec2(winX, winY);

    ImGui::SetNextWindowPos(s_winPos);
    ImGui::SetNextWindowSize(ImVec2(totalW, totalH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPadX, kPadY));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.45f));

    ImGui::Begin("##translator_overlay", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoScrollbar);

    // タイトルバーなしでもドラッグで移動できるようにする
    // IsWindowHovered() はchild windowで誤動作するため、生座標でヒットテスト
    static bool s_dragging = false;
    if (!s_dragging && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImVec2 mp = io.MousePos;
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 ws = ImGui::GetWindowSize();
        bool inWindow = mp.x >= wp.x && mp.x <= wp.x + ws.x &&
                        mp.y >= wp.y && mp.y <= wp.y + ws.y;
        if (inWindow && !ImGui::IsAnyItemHovered()) {
            s_dragging = true;
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        s_dragging = false;
    }
    if (s_dragging) {
        s_winPos.x += io.MouseDelta.x;
        s_winPos.y += io.MouseDelta.y;
        ImGui::SetWindowPos(s_winPos);
    }

    // FramePadding.y=1 でボタン内テキストを縦中央に揃える
    const float kBtnH = lineH + 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kFPX, 1.0f));

    auto RenderRow = [&](const char* marqueeId, const char* text, float& scroll,
                         const char* btnLabel, auto onBtnClick) {
        float rowTopY = ImGui::GetCursorPosY();

        // ボタンを左端・行の縦中央に配置
        float btnOffY = (lineHWS - kBtnH) * 0.5f;
        ImGui::SetCursorPos(ImVec2(0.0f, rowTopY + btnOffY));

        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.2f, 0.2f, 0.2f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.4f, 0.4f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        if (ImGui::Button(btnLabel, ImVec2(kBtnWidth, kBtnH))) {
            onBtnClick();
        }
        ImGui::PopStyleColor(3);

        // テキスト領域をボタン右に配置
        ImGui::SetCursorPos(ImVec2(kBtnWidth + kGap, rowTopY));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        RenderMarqueeText(marqueeId, text, kTextWidth, scroll, io.DeltaTime);
        ImGui::PopStyleVar();

        // 次行の先頭へ
        ImGui::SetCursorPos(ImVec2(0.0f, rowTopY + lineHWS));
    };

    // --- 行1: TTS ボタン + 原文 ---
    RenderRow("##mq_orig", origText.c_str(), g_scrollOrig,
              TtsModeLabel(config::GetTtsMode()), [&]() {
        config::CycleTtsMode();
        logging::Debug("[Overlay] TtsMode -> %s", TtsModeLabel(config::GetTtsMode()));
    });

    // --- 行2: TL ボタン + 訳文/ステータス ---
    RenderRow("##mq_tran", statusText.c_str(), g_scrollTrans,
              TranslationModeLabel(config::GetTranslationMode()), [&]() {
        config::CycleTranslationMode();
        if (config::GetTranslationMode() == TranslationMode::Off) {
            std::lock_guard<std::mutex> lock(g_textMutex);
            g_translatedText = "";
        }
        logging::Debug("[Overlay] TranslationMode -> %s",
            TranslationModeLabel(config::GetTranslationMode()));
    });

    ImGui::PopStyleVar(); // FramePadding

    if (g_cjkFont) ImGui::PopFont();

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    ImGui::Render();

    if (g_rtv) {
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
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
    g_assetsDir = dir + "assets\\";

    translate::SetResultCallback([](const translate::TranslateResult& r) {
        {
            std::lock_guard<std::mutex> lock(g_textMutex);
            g_originalText   = r.original;
            g_translatedText = r.translated;
        }
        g_textChanged.store(true);
        logging::Translation(r.channel.c_str(), r.sender.c_str(),
                             r.original.c_str(), r.translated.c_str());

        TtsMode ttsMode = config::GetTtsMode();
        if (ttsMode != TtsMode::Off) {
            const char* ttsText = (ttsMode == TtsMode::Translated)
                ? r.translated.c_str()
                : r.original.c_str();
            tts::Speak(ttsText, r.sender.empty() ? nullptr : r.sender.c_str());
        }
    });

    if (config::Get().demoMode) {
        TranslationMode mode = config::GetTranslationMode();
        if (mode != TranslationMode::Off) {
            translate::Queue("", "", g_demoMessages[0]);
        } else {
            std::lock_guard<std::mutex> lock(g_textMutex);
            g_originalText = g_demoMessages[0];
        }
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

    // バックバッファ変化検出 (ResizeBuffers 後はポインタが変わる)
    // GetBuffer は安価な COM 参照カウント操作。CreateRTV は変化時のみ実行
    ID3D11Texture2D* backBuffer = nullptr;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (backBuffer) {
        if (backBuffer != g_cachedBackBuffer) {
            if (g_rtv)             { g_rtv->Release();             g_rtv             = nullptr; }
            if (g_cachedBackBuffer) { g_cachedBackBuffer->Release(); g_cachedBackBuffer = nullptr; }
            g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
            g_cachedBackBuffer = backBuffer; // 参照カウントを保持してポインタ比較に使用
        } else {
            backBuffer->Release(); // 変化なし: 余分な参照を解放
        }
    }

    RenderFrame();
}

LRESULT overlay::OnWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!g_initialized) return 0;
    LRESULT result = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
    if (ImGui::GetIO().WantCaptureMouse) return result;
    return 0;
}

bool overlay::IsRadioOn() {
    return config::GetTranslationMode() != TranslationMode::Off
        && ollama::GetRadioState() == RadioState::ON;
}

void overlay::SetDisplayText(const char* original, const char* translated) {
    std::lock_guard<std::mutex> lock(g_textMutex);
    g_originalText   = original   ? original   : "";
    g_translatedText = translated ? translated : "";
}

void overlay::OnChatMessage(const std::string& sender, const std::string& message) {
    if (config::Get().demoMode) return;
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

    // 原文は常に保存
    {
        std::lock_guard<std::mutex> lock(g_textMutex);
        g_originalText = message;
    }

    TranslationMode mode = config::GetTranslationMode();
    if (mode != TranslationMode::Off) {
        translate::Queue("", sender, message);
    } else {
        std::lock_guard<std::mutex> lock(g_textMutex);
        g_translatedText = "";
        g_textChanged.store(true);
    }
}

void overlay::Shutdown() {
    if (!g_initialized) return;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (g_rtv)             { g_rtv->Release();             g_rtv             = nullptr; }
    if (g_cachedBackBuffer) { g_cachedBackBuffer->Release(); g_cachedBackBuffer = nullptr; }
    if (g_context)         { g_context->Release();         g_context         = nullptr; }
    if (g_device)          { g_device->Release();          g_device          = nullptr; }

    g_initialized = false;
    g_hwnd = nullptr;

    logging::Debug("[Overlay] シャットダウン完了");
}
