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
#include <deque>

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

struct MessageEntry {
    std::string sender;
    std::string original;
    std::string translated;
    bool        translating = false; // true = 翻訳待ち
};

static std::mutex               g_textMutex;
static std::deque<MessageEntry> g_entries;
static const size_t             kMaxEntries = 5;

static float g_scrollOrig  = 0.0f; // 最新エントリの原文スクロール
static float g_scrollTrans = 0.0f; // 最新エントリの訳文スクロール

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

// 静的クリップ表示 (過去エントリ用: スクロールなし)
static void RenderClippedText(const char* id, const char* text, float areaWidth) {
    float childWidth = (areaWidth < 10.0f) ? 10.0f : areaWidth;
    ImGui::BeginChild(id, ImVec2(childWidth, ImGui::GetTextLineHeightWithSpacing()),
                      false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextUnformatted(text);
    ImGui::EndChild();
}

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
    case TranslationMode::Off: return "--";
    case TranslationMode::JA:  return "JA";
    case TranslationMode::JAZ: return "JAZ";
    case TranslationMode::EN:  return "EN";
    case TranslationMode::RU:  return "RU";
    case TranslationMode::ZH:  return "ZH";
    case TranslationMode::KO:  return "KO";
    default:                   return "??";
    }
}

static const char* TtsModeLabel(TtsMode m) {
    switch (m) {
    case TtsMode::Off:        return "--";
    case TtsMode::Original:   return "Src";
    case TtsMode::Translated: return "Tr";
    default:                  return "??";
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
    static float       s_textWidth = 270.0f; // リサイズハンドルで変更可能
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
            "--","JA","JAZ","EN","RU","ZH","KO","Src","Tr", nullptr
        };
        float maxLabelW = 0.0f;
        for (int i = 0; kAllLabels[i]; ++i) {
            float w = ImGui::CalcTextSize(kAllLabels[i]).x;
            if (w > maxLabelW) maxLabelW = w;
        }
        s_cachedBtnW = maxLabelW + 2.0f * kFPX + 4.0f;
    }
    float kBtnWidth = s_cachedBtnW;

    static const float kToggleW  = 14.0f; // 展開トグルボタン幅 (常に確保)
    float toggleColW = kToggleW;
    float totalW = kPadX + toggleColW + kBtnWidth + kGap + s_textWidth + kPadX;
    float totalH = kPadY + lineHWS + lineHWS + kPadY;

    g_textChanged.exchange(false);

    // デモモード: タイマー駆動でメッセージを切り替え
    if (config::Get().demoMode) {
        g_demoTimer += io.DeltaTime;
        if (g_demoTimer >= g_demoInterval) {
            g_demoTimer = 0.0f;
            g_demoIndex = (g_demoIndex + 1) % g_demoCount;
            TranslationMode mode = config::GetTranslationMode();
            {
                std::lock_guard<std::mutex> lock(g_textMutex);
                if (g_entries.size() >= kMaxEntries) g_entries.pop_front();
                g_entries.push_back({"", g_demoMessages[g_demoIndex], "", mode != TranslationMode::Off});
                g_scrollOrig  = 0.0f;
                g_scrollTrans = 0.0f;
            }
            if (mode != TranslationMode::Off) {
                translate::Queue("", "", g_demoMessages[g_demoIndex]);
            }
        }
    }

    // エントリのコピーを取得
    std::deque<MessageEntry> entries;
    {
        std::lock_guard<std::mutex> lock(g_textMutex);
        entries = g_entries;
    }
    if (entries.empty()) entries.push_back({"", "", "", false});

    // 最新エントリの表示テキストを決定
    const MessageEntry& latest   = entries.back();
    std::string latestOrig  = latest.sender.empty() ? latest.original
                                                     : latest.sender + u8": " + latest.original;
    RadioState radioState = ollama::GetRadioState();
    std::string latestTrans;
    if (config::GetTranslationMode() == TranslationMode::Off) {
        latestTrans = u8"[Off]";
    } else if (radioState == RadioState::RESTARTING) {
        int attempt = 0, maxAttempts = 0;
        ollama::GetRestartProgress(attempt, maxAttempts);
        char buf[64];
        snprintf(buf, sizeof(buf), u8"[Restarting %d/%d...]", attempt, maxAttempts);
        latestTrans = buf;
    } else if (radioState == RadioState::FAULT) {
        latestTrans = u8"[Error]";
    } else if (latest.translating) {
        latestTrans = u8"[...]";
    } else {
        latestTrans = latest.translated;
    }

    // 過去エントリ展開/折りたたみ
    size_t numPast = entries.size() - 1;
    static bool s_historyExpanded = false;
    if (numPast == 0) s_historyExpanded = false;

    float historyH = (s_historyExpanded && numPast > 0) ? numPast * 2.0f * lineHWS : 0.0f;
    totalH = kPadY + historyH + lineHWS + lineHWS + kPadY;

    // 初回のみデフォルト位置 (右下) に配置。以降はドラッグで移動した位置を維持
    static ImVec2 s_winPos = ImVec2(-1.f, -1.f);
    // エントリ数変化で winY が変わるので常に下端を固定
    float baseWinX = io.DisplaySize.x - totalW - kMargin;
    float baseWinY = io.DisplaySize.y - totalH - kMargin;
    if (s_winPos.x < 0.f) s_winPos = ImVec2(baseWinX, baseWinY);
    // エントリ増減で下端がずれないよう Y を補正 (ドラッグ中は補正しない)
    static float s_lastTotalH = totalH;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && s_lastTotalH != totalH) {
        s_winPos.y += s_lastTotalH - totalH; // 下端固定
        s_lastTotalH = totalH;
    }

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

    // --- リサイズハンドル (右下三角グリップ、横幅変更) ---
    static bool s_resizing = false;
    {
        static const float kHandleSize = 12.0f;
        ImVec2 wp   = ImGui::GetWindowPos();
        ImVec2 ws   = ImGui::GetWindowSize();
        ImVec2 hMin = ImVec2(wp.x + ws.x - kHandleSize, wp.y + ws.y - kHandleSize);
        ImVec2 hMax = ImVec2(wp.x + ws.x,               wp.y + ws.y);
        bool overHandle = io.MousePos.x >= hMin.x && io.MousePos.x <= hMax.x &&
                          io.MousePos.y >= hMin.y && io.MousePos.y <= hMax.y;
        if (!s_resizing && overHandle && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            s_resizing = true;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            s_resizing = false;
        }
        if (s_resizing) {
            s_textWidth += io.MouseDelta.x;
            if (s_textWidth < 80.0f) s_textWidth = 80.0f;
        }
        if (overHandle || s_resizing) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        ImU32 col = (overHandle || s_resizing)
            ? IM_COL32(200, 200, 200, 200) : IM_COL32(140, 140, 140, 120);
        ImGui::GetWindowDrawList()->AddTriangleFilled(
            ImVec2(hMax.x, hMin.y), ImVec2(hMax.x, hMax.y), ImVec2(hMin.x, hMax.y), col);
    }

    // タイトルバーなしでもドラッグで移動できるようにする
    // IsWindowHovered() はchild windowで誤動作するため、生座標でヒットテスト
    static bool s_dragging = false;
    if (!s_dragging && !s_resizing && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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

    // --- 過去エントリ (ボタンなし、薄い色で表示) ---
    float pastTextX = toggleColW + kBtnWidth + kGap;
    float pastTextW = s_textWidth;
    if (s_historyExpanded) for (size_t i = 0; i + 1 < entries.size(); ++i) {
        const auto& e = entries[i];
        float rowY = ImGui::GetCursorPosY();

        // 原文行 (sender: original)
        std::string line1 = e.sender.empty() ? e.original : e.sender + u8": " + e.original;
        ImGui::SetCursorPos(ImVec2(pastTextX, rowY));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.75f, 0.75f, 0.85f));
        char origId[32]; snprintf(origId, sizeof(origId), "##pq_o%zu", i);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        RenderClippedText(origId, line1.c_str(), pastTextW);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        // 訳文行
        std::string line2 = e.translating ? u8"[...]" : e.translated;
        ImGui::SetCursorPos(ImVec2(pastTextX, rowY + lineHWS));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.85f, 0.65f, 0.75f));
        char transId[32]; snprintf(transId, sizeof(transId), "##pq_t%zu", i);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        RenderClippedText(transId, line2.c_str(), pastTextW);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        ImGui::SetCursorPos(ImVec2(0.0f, rowY + 2.0f * lineHWS));
    }

    // --- 展開/折りたたみトグルボタン (最新2行の左端に2行スパン、常に表示) ---
    float mainRowsTop = ImGui::GetCursorPosY();
    {
        // 上端 = row1ボタン上端、下端 = row2ボタン下端 に揃える
        float toggleBtnOffY = (lineHWS - kBtnH) * 0.5f;
        ImGui::SetCursorPos(ImVec2(0.0f, mainRowsTop + toggleBtnOffY));
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.2f, 0.2f, 0.2f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.4f, 0.4f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        if (ImGui::Button(s_historyExpanded ? "-" : "+", ImVec2(kToggleW, lineHWS + kBtnH)) && numPast > 0) {
            s_historyExpanded = !s_historyExpanded;
        }
        ImGui::PopStyleColor(3);
        ImGui::SetCursorPos(ImVec2(0.0f, mainRowsTop));
    }

    auto RenderRow = [&](const char* marqueeId, const char* text, float& scroll,
                         const char* btnLabel, auto onBtnClick) {
        float rowTopY = ImGui::GetCursorPosY();

        // ボタンをトグル列の右・行の縦中央に配置
        float btnOffY = (lineHWS - kBtnH) * 0.5f;
        ImGui::SetCursorPos(ImVec2(toggleColW, rowTopY + btnOffY));

        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.2f, 0.2f, 0.2f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.4f, 0.4f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        if (ImGui::Button(btnLabel, ImVec2(kBtnWidth, kBtnH))) {
            onBtnClick();
        }
        ImGui::PopStyleColor(3);

        // テキスト領域をボタン右に配置
        ImGui::SetCursorPos(ImVec2(toggleColW + kBtnWidth + kGap, rowTopY));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        RenderMarqueeText(marqueeId, text, s_textWidth, scroll, io.DeltaTime);
        ImGui::PopStyleVar();

        // 次行の先頭へ
        ImGui::SetCursorPos(ImVec2(0.0f, rowTopY + lineHWS));
    };

    // --- 最新エントリ: TTS ボタン + 原文 ---
    RenderRow("##mq_orig", latestOrig.c_str(), g_scrollOrig,
              TtsModeLabel(config::GetTtsMode()), [&]() {
        config::CycleTtsMode();
        logging::Debug("[Overlay] TtsMode -> %s", TtsModeLabel(config::GetTtsMode()));
    });

    // --- 最新エントリ: TL ボタン + 訳文/ステータス ---
    RenderRow("##mq_tran", latestTrans.c_str(), g_scrollTrans,
              TranslationModeLabel(config::GetTranslationMode()), [&]() {
        config::CycleTranslationMode();
        if (config::GetTranslationMode() == TranslationMode::Off) {
            std::lock_guard<std::mutex> lock(g_textMutex);
            for (auto& e : g_entries) e.translating = false;
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
            // sender + original で対応エントリを後ろから検索して更新
            for (auto it = g_entries.rbegin(); it != g_entries.rend(); ++it) {
                if (it->translating && it->sender == r.sender && it->original == r.original) {
                    it->translated  = r.ok ? r.translated : r.original;
                    it->translating = false;
                    // 最新エントリが更新されたらスクロールをリセット
                    if (it == g_entries.rbegin()) {
                        g_scrollOrig  = 0.0f;
                        g_scrollTrans = 0.0f;
                    }
                    break;
                }
            }
        }
        g_textChanged.store(true);
        logging::Translation(r.channel.c_str(), r.sender.c_str(),
                             r.original.c_str(), r.translated.c_str());

        TtsMode ttsMode = config::GetTtsMode();
        if (ttsMode != TtsMode::Off) {
            // 翻訳失敗 (Ollama 接続不可) のエラー文字列は読み上げない
            bool useTranslated = (ttsMode == TtsMode::Translated) && r.ok;
            const char* ttsText = useTranslated ? r.translated.c_str() : r.original.c_str();
            tts::Speak(ttsText, r.sender.empty() ? nullptr : r.sender.c_str());
        }
    });

    if (config::Get().demoMode) {
        std::lock_guard<std::mutex> lock(g_textMutex);
        TranslationMode mode = config::GetTranslationMode();
        g_entries.push_back({"", g_demoMessages[0], "", mode != TranslationMode::Off});
        if (mode != TranslationMode::Off) {
            translate::Queue("", "", g_demoMessages[0]);
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
    if (g_entries.size() >= kMaxEntries) g_entries.pop_front();
    g_entries.push_back({"", original ? original : "", translated ? translated : "", false});
    g_scrollOrig  = 0.0f;
    g_scrollTrans = 0.0f;
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

    TranslationMode mode = config::GetTranslationMode();

    {
        std::lock_guard<std::mutex> lock(g_textMutex);
        if (g_entries.size() >= kMaxEntries) g_entries.pop_front();
        g_entries.push_back({sender, message, "", mode != TranslationMode::Off});
        g_scrollOrig  = 0.0f;
        g_scrollTrans = 0.0f;
    }
    g_textChanged.store(true);

    if (mode != TranslationMode::Off) {
        translate::Queue("", sender, message);
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
