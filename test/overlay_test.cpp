// ============================================================
// overlay_test.cpp - オーバーレイ UI テストホスト
// ゲームなしで chat_translator.dll のオーバーレイを確認する
//
// 操作:
//   F1     : デモメッセージを手動送信
//   L      : ログ再生 一時停止/再開
//   R      : ログ再生 先頭から再開
//   ESC    : 終了
//
// 起動オプション:
//   --log <path>      : 読み込む chat_log.txt のパス
//   --interval <ms>   : メッセージ間隔 (デフォルト 3000ms)
//   --no-auto         : 起動後の自動再生を無効化
//
// 注意: version.dll と同ディレクトリに置くと D3D11 初期化がクラッシュする。
//       CMake が build/overlay_test/ に出力するのはこのため。
// ============================================================

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>

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
typedef int     (*PFN_WorkerIsBusy)();

// ============================================================
// グローバル状態
// ============================================================

static HMODULE            g_hDll        = nullptr;
static PFN_OnPresent      g_fnPresent   = nullptr;
static PFN_OnWndProc      g_fnWndProc   = nullptr;
static PFN_OnChatMessage  g_fnChatMsg   = nullptr;
static PFN_WorkerShutdown g_fnShutdown  = nullptr;
static PFN_WorkerIsBusy   g_fnIsBusy    = nullptr;

static ID3D11Device*           g_device    = nullptr;
static ID3D11DeviceContext*    g_context   = nullptr;
static IDXGISwapChain*         g_swapChain = nullptr;
static ID3D11RenderTargetView* g_rtv       = nullptr;
static HWND                    g_hwnd      = nullptr;

// ============================================================
// デモメッセージ (F1 キーで順番に送信)
// ============================================================

static const struct { const char* sender; const char* message; } kTestMessages[] = {
    // ---- EN Short ----
    { "ColonelMuster",   "at foxcatcher NOW AT mines" },
    { "TankDriver42",    "latch logi!! smalls n bmat gone" },
    { "WardenHQ",        "qrf wightwalk base is down!!" },
    // ---- EN Medium ----
    { "LtBonecrusher",   "colonial BTs crows nest heading bridge AT ASAP!! rmat for atg if anyone has some" },
    { "LogiRunner99",    "SC buckler up but 300mm empty... logi ammo backline?? need 3-4 crates min pls" },
    { "FrontlineAce",    "QRF thru latch rn!! overlook bb line cracking need conc n bmat NOW" },
    // ---- EN Long ----
    { "MortarMike",      "WEATHERED PUSH 6+ BTs + APC thru crows nest, AT QRF heavy logi all hands foxcatcher def line NOW repair stations empty BRING RMAT" },
    { "CommanderJAX",    "fob necropolis fell lost 3 BTs and shirts, rally huntsfort rebuild line, logi bmat conc 20mm, SC shift fire frostmarch before they flank rear!!" },
    { "LightfootLogi",   "war: wardens solas gorge -> white chapel, BB crumbling post cracking, cord SC still up but AT mines gone, need 3 logi trucks 7.62 + rmat fob 0200" },
    // ---- RU Short ----
    { "IvanKorolev",     "AT foxcatcher срочно!!" },
    { "DmitriZhukov",    "лоджи лэтч!! патроны bmat нет" },
    { "SergeiPetrov",    "qrf wightwalk базу роняют!!" },
    // ---- RU Medium ----
    { "AlexNovik",       "BT колонисты crows nest к мосту AT нужен!! rmat для atg привезите плиз" },
    { "NikolaiVolkov",   "SC buckler работает но 300мм нет... кто лоджи амму бэклайн?? 3-4 ящика хотя бы" },
    { "YuriMelnikov",    "QRF через latch рн!! bunker line overlook надо conc bmat быстро" },
    // ---- RU Long ----
    { "AntonBorisov",    "WEATHERED PUSH рн!! 6+ BT + APC через crows nest AT QRF тяжёлый лоджи НУЖНЫ все на линию foxcatcher ремонт пуст ВЕЗИТЕ RMAT" },
    { "PavelSidorov",    "фоб necropolis пал 3 BT и рубашки потеряли, сбор huntsfort, лоджи bmat conc 20мм, SC перенести огонь frostmarch пока не зашли с тыла!!" },
    { "VasiliZotov",     "варданы solas gorge -> white chapel, BB crumbling post трещит, cord SC жив но AT мины кончились, 3 лоджи 7.62 rmat фоб до 0200" },
    // ---- KO Short ----
    { "KimJunho",        "foxcatcher AT 지금 바로!!" },
    { "ParkMinSeo",      "latch 로지!! 탄약 bmat 없음" },
    { "ChoiSungHo",      "qrf wightwalk 기지 털리고 있어!!" },
    // ---- KO Medium ----
    { "LeeJaeWon",       "식민지 BT crows nest 다리로 오고있어 AT 빨리!! rmat 있으면 atg에 좀" },
    { "YunKyungHo",      "SC buckler 살아있는데 300mm 없어... 백라인 탄약 로지 가는사람?? 3-4 크레이트만" },
    { "JungDaeHyun",     "latch로 QRF 오고있어 rn!! overlook bb 라인 conc bmat 지금 당장" },
    // ---- KO Long ----
    { "OhSeungWoo",      "WEATHERED 푸쉬 rn!! BT 6대+ APC crows nest 통과중 AT QRF 중보급 다 필요 foxcatcher 방어선 전원 집합!! 수리소 비어있음 RMAT 들고와" },
    { "ShinHanSol",      "necropolis fob 뚫렸어 BT 3대 셔츠 다 날림, huntsfort 집결, 로지 bmat conc 20mm, SC frostmarch로 사격 전환 후방 안 뚫리게!!" },
    { "BaekJiHoon",      "전황: 워든 solas gorge 뚫어서 white chapel 쌓는중, BB crumbling post 무너지고있어, cord SC 살아있는데 AT 지뢰 다 소진, 로지 3대 7.62mm rmat fob 02시까지" },
    // ---- ZH Short ----
    { "WangFang",        "foxcatcher 反坦 现在!!" },
    { "LiuYang",         "latch 补给!! 弹药bmat没了" },
    { "ChenWei",         "qrf wightwalk 基地要丢了!!" },
    // ---- ZH Medium ----
    { "ZhangMing",       "殖民军BT crows nest冲桥了 要AT快!! 有rmat给atg带一点" },
    { "XuLi",            "SC buckler开着但300mm没了... 有人去后方跑弹药吗?? 3-4箱就够" },
    { "HeZiyang",        "殖民军QRF过latch了!! overlook bb线要conc bmat快" },
    // ---- ZH Long ----
    { "WuJun",           "WEATHERED全面推了!! 6+BT+APC过crows nest 要AT QRF重补给 foxcatcher防线全员来!! 修理站空的 带RMAT来" },
    { "LinHao",          "necropolis fob丢了 损失3辆BT和补给, huntsfort集结, 补给带bmat conc 20mm, SC转移炮火到frostmarch别让他们抄后路!!" },
    { "SunRui",          "战况: 瓦登solas gorge -> white chapel堆防线, BB crumbling post快崩了, cord SC还在但AT地雷耗完, 3辆补给车7.62 rmat fob 02:00前" },
    // ---- JA Short ----
    { "TanakaHiroshi",   "foxcatcher AT今すぐ!!" },
    { "SuzukiRyo",       "latchまでロジ!! 弾薬bmat切れ" },
    { "YamadaKen",       "qrf wightwalk 拠点やばい!!" },
    // ---- JA Medium ----
    { "NakamuraYuta",    "コロニアルBT crows nest 橋に向かってる AT急いで!! rmatあったらatgに持ってきて" },
    { "SatoKenji",       "SC buckler動いてるけど300mm切れた... 弾薬バックラインロジ行ける人?? 3〜4クレートでいい" },
    { "ItoMasaki",       "latchからQRF来てる rn!! overlook bbラインconc bmat今すぐ" },
    // ---- JA Long ----
    { "KobayashiHaru",   "WEATHERED全面プッシュ!! BT6両+APCがcrows nest通過中 AT QRF重ロジ全部必要 foxcatcher防衛ライン全員集合!! 修理ステ空なのでRMAT持ってきて" },
    { "WatanabeKo",      "necropolis fob落とされた BT3両とシャツ全部ロスト huntsfort集合 ロジbmat conc 20mm SC frostmarchに砲撃転換 後ろ取られる前に!!" },
    { "OtaNoboru",       "戦況: wardenがsolas gorgeから突入しwhite chapel強化中, BBラインcrumbling postがやばい, cord SC生きてるけどATマイン枯渇, ロジ3台7.62 rmat fob 02:00まで" },
};
static const int kTestMessageCount = sizeof(kTestMessages) / sizeof(kTestMessages[0]);
static int g_msgIndex = 0;

// ============================================================
// ログ再生
// ============================================================

struct LogEntry {
    std::string sender;
    std::string message;
};

static std::vector<LogEntry> g_logEntries;
static std::atomic<bool>     g_replayRunning{false};
static std::atomic<bool>     g_replayPaused{false};
static std::atomic<int>      g_replayIndex{0};
static int                   g_replayIntervalMs = 3000;
static bool                  g_replayLoop       = false;
static std::thread           g_replayThread;

// chat_log.txt の1行をパース: "[timestamp] [channel] sender: message"
static bool ParseLogLine(const char* line, LogEntry* out) {
    if (!line || line[0] != '[') return false;

    // タイムスタンプブロック "]" を探す
    const char* p = strchr(line, ']');
    if (!p || p[1] != ' ') return false;
    p += 2;

    // チャンネルブロック "[channel]"
    if (*p != '[') return false;
    p = strchr(p, ']');
    if (!p || p[1] != ' ') return false;
    p += 2;

    // "sender: message" を分割 (最初の ": " で区切る)
    const char* colon = strstr(p, ": ");
    if (!colon) return false;

    out->sender  = std::string(p, static_cast<size_t>(colon - p));
    out->message = std::string(colon + 2);

    // 末尾の改行を除去
    while (!out->message.empty() &&
           (out->message.back() == '\n' || out->message.back() == '\r')) {
        out->message.pop_back();
    }

    return !out->sender.empty() && !out->message.empty();
}

static int LoadChatLog(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;

    char line[1024];
    int  count = 0;
    while (fgets(line, sizeof(line), f)) {
        LogEntry e;
        if (ParseLogLine(line, &e)) {
            g_logEntries.push_back(std::move(e));
            count++;
        }
    }
    fclose(f);
    return count;
}

static void StartReplayThread() {
    g_replayRunning = true;
    g_replayPaused  = false;

    g_replayThread = std::thread([]() {
        int total = static_cast<int>(g_logEntries.size());
        while (g_replayRunning) {
            if (g_replayPaused) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            int idx = g_replayIndex.fetch_add(1);
            if (idx >= total) {
                if (g_replayLoop) {
                    g_replayIndex = 0;
                    printf("[Replay] ループ再開\n");
                    // 先頭に戻る前に短い休止 (次メッセージ投入前の余白)
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
                printf("\n[Replay] 全メッセージ送信完了 (%d件)\n", total);
                g_replayRunning = false;
                break;
            }

            const auto& e = g_logEntries[idx];
            if (g_fnChatMsg) {
                g_fnChatMsg(e.sender.c_str(), e.message.c_str());
            }
            printf("[Replay %d/%d] %s: %s\n",
                   idx + 1, total, e.sender.c_str(), e.message.c_str());
            fflush(stdout);

            // TTS 再生完了待機: 翻訳パイプライン開始を待つ最低 500ms + IsBusy ポーリング
            // IsBusy 未取得時はフォールバックとして固定インターバルを使う
            if (g_fnIsBusy) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                for (int waited = 0; waited < 600 && g_replayRunning && !g_replayPaused; waited++) {
                    if (!g_fnIsBusy()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            } else {
                for (int i = 0; i < g_replayIntervalMs / 100 && g_replayRunning; i++) {
                    if (g_replayPaused) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
    });
}

static void StopReplayThread() {
    g_replayRunning = false;
    if (g_replayThread.joinable()) g_replayThread.join();
}

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
            const auto& tm = kTestMessages[g_msgIndex % kTestMessageCount];
            g_fnChatMsg(tm.sender, tm.message);
            printf("[F1] メッセージ送信 (%s): %s\n", tm.sender, tm.message);
            g_msgIndex++;

        } else if (wp == 'L') {
            if (g_logEntries.empty()) {
                printf("[Replay] ログが読み込まれていません\n");
            } else if (g_replayRunning && !g_replayPaused) {
                g_replayPaused = true;
                printf("[Replay] 一時停止 (%d/%d)\n",
                       g_replayIndex.load(), (int)g_logEntries.size());
            } else if (g_replayRunning && g_replayPaused) {
                g_replayPaused = false;
                printf("[Replay] 再開 (%d/%d)\n",
                       g_replayIndex.load(), (int)g_logEntries.size());
            } else {
                // 再生完了後: 続きから再開
                StartReplayThread();
                printf("[Replay] 再生開始 (%d/%d)\n",
                       g_replayIndex.load(), (int)g_logEntries.size());
            }

        } else if (wp == 'R') {
            if (g_logEntries.empty()) {
                printf("[Replay] ログが読み込まれていません\n");
            } else {
                StopReplayThread();
                g_replayIndex = 0;
                StartReplayThread();
                printf("[Replay] 先頭から再開 (%d件)\n", (int)g_logEntries.size());
            }
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

int main(int argc, char* argv[]) {
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    SetConsoleOutputCP(CP_UTF8);
    printf("=== Overlay Test Host ===\n");

    // コマンドライン引数解析
    std::string logPath;
    bool        autoPlay = true;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            logPath = argv[++i];
        } else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            g_replayIntervalMs = atoi(argv[++i]);
            if (g_replayIntervalMs < 100) g_replayIntervalMs = 100;
        } else if (strcmp(argv[i], "--no-auto") == 0) {
            autoPlay = false;
        } else if (strcmp(argv[i], "--loop") == 0) {
            g_replayLoop = true;
        }
    }

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
        "Overlay Test Host  [F1: test msg]  [L: pause/resume]  [R: restart]  [ESC: exit]",
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
    g_fnIsBusy       = (PFN_WorkerIsBusy)            GetProcAddress(g_hDll, "WorkerIsBusy");

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

    // ログファイルをロード
    // パス未指定の場合は baseDir の chat_log.txt を探す
    if (logPath.empty()) {
        logPath = std::string(baseDir) + "chat_log.txt";
    }

    int loaded = LoadChatLog(logPath.c_str());
    if (loaded > 0) {
        printf("OK: ログ読み込み完了: %s (%d件, 間隔=%dms)\n",
               logPath.c_str(), loaded, g_replayIntervalMs);
    } else {
        // ログなし → デモメッセージをデフォルトエントリとして使用
        for (int i = 0; i < kTestMessageCount; i++) {
            LogEntry e;
            e.sender  = kTestMessages[i].sender;
            e.message = kTestMessages[i].message;
            g_logEntries.push_back(e);
        }
        g_replayLoop = true;
        printf("INFO: ログファイルなし (%s)\n", logPath.c_str());
        printf("INFO: デモメッセージ %d件をループ再生します\n", kTestMessageCount);
        printf("      --log <path> でログファイルを指定できます\n");
    }
    printf("操作: L=一時停止/再開  R=先頭から再開  F1=手動送信\n");

    // 自動再生: 翻訳・TTS 初期化完了を待ってから開始
    if (autoPlay && !g_logEntries.empty()) {
        printf("[Replay] 3秒後に自動再生を開始します...\n");
        // メインループ開始後に別スレッドから開始するため遅延スレッドを使う
        std::thread([&]() {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            if (!g_replayRunning) {
                printf("[Replay] 自動再生開始 (%d件)\n", (int)g_logEntries.size());
                StartReplayThread();
            }
        }).detach();
    }

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
    StopReplayThread();

    if (g_fnShutdown) g_fnShutdown();
    if (g_rtv)        { g_rtv->Release();       g_rtv       = nullptr; }
    if (g_swapChain)  { g_swapChain->Release();  g_swapChain = nullptr; }
    if (g_context)    { g_context->Release();    g_context   = nullptr; }
    if (g_device)     { g_device->Release();     g_device    = nullptr; }
    if (g_hDll)       { FreeLibrary(g_hDll);     g_hDll      = nullptr; }

    printf("終了\n");
    return 0;
}
