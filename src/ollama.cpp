// ============================================================
// ollama.cpp - Ollama プロセス管理・ヘルス監視
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "ollama.h"
#include "translate.h"
#include "log.h"

// ============================================================
// 内部状態
// ============================================================

static bool              g_managed         = false; // エンドポイントが localhost 系なら自動管理
static std::string       g_ollamaDir;               // 同梱 ollama.exe のディレクトリ
static HANDLE            g_ollamaProcess   = nullptr;
static HANDLE            g_ollamaJobObject = nullptr;

static std::atomic<RadioState> g_radioState{RadioState::ON};
static std::atomic<bool>       g_userEnabled{true};
static std::atomic<bool>       g_running{false};
static std::atomic<bool>       g_restartRequested{false};

static std::thread             g_healthThread;
static std::mutex              g_healthMutex;
static std::condition_variable g_healthCv;

// ============================================================
// ユーティリティ
// ============================================================

static bool IsLocalEndpoint(const std::string& endpoint) {
    return endpoint.find("localhost") != std::string::npos
        || endpoint.find("127.0.0.1") != std::string::npos
        || endpoint.find("::1")       != std::string::npos;
}

// DLL のベースディレクトリを取得
static std::string GetDllBaseDir() {
    char dllPath[MAX_PATH];
    HMODULE hSelf;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&GetDllBaseDir), &hSelf);
    GetModuleFileNameA(hSelf, dllPath, MAX_PATH);
    std::string dir(dllPath);
    size_t pos = dir.rfind('\\');
    if (pos != std::string::npos) dir = dir.substr(0, pos + 1);
    return dir;
}

// 同梱 ollama.exe のパスを探す
static std::string FindBundledOllama() {
    if (!g_ollamaDir.empty()) {
        std::string exe = g_ollamaDir + "\\ollama.exe";
        if (GetFileAttributesA(exe.c_str()) != INVALID_FILE_ATTRIBUTES) return exe;
    }
    std::string baseDir = GetDllBaseDir();
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);
    std::string cwdStr = cwd;
    if (!cwdStr.empty() && cwdStr.back() != '\\') cwdStr += '\\';

    std::string candidates[] = {
        baseDir + "tools\\ollama\\ollama.exe",
        baseDir + "..\\..\\Mods\\ChatTranslator\\tools\\ollama\\ollama.exe",
        cwdStr  + "tools\\ollama\\ollama.exe",
    };
    for (auto& path : candidates) {
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }
    return "";
}

// ollama serve を起動し、Job Object に割り当てる
static bool StartProcess(const std::string& exePath) {
    std::string dir = exePath.substr(0, exePath.rfind('\\'));
    std::string modelsDir = dir + "\\models";
    CreateDirectoryA(modelsDir.c_str(), nullptr);
    SetEnvironmentVariableA("OLLAMA_MODELS", modelsDir.c_str());

    // Job Object を作成 (初回のみ): KILL_ON_JOB_CLOSE でゲーム終了時に自動終了
    if (!g_ollamaJobObject) {
        g_ollamaJobObject = CreateJobObject(nullptr, nullptr);
        if (g_ollamaJobObject) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(g_ollamaJobObject,
                JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
    }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::string cmdLine = "\"" + exePath + "\" serve";

    // CREATE_BREAKAWAY_FROM_JOB: Steam の Job Object から切り離し、Steam フリーズを防ぐ
    // Job Object が breakaway を禁止している場合 (ERROR_ACCESS_DENIED) はフラグなしで再試行
    BOOL ok = CreateProcessA(nullptr, &cmdLine[0], nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB,
                             nullptr, dir.c_str(), &si, &pi);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
        ok = CreateProcessA(nullptr, &cmdLine[0], nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi);
    }
    if (!ok) {
        logging::Debug("[Ollama] ollama serve 起動失敗: %u", GetLastError());
        return false;
    }

    CloseHandle(pi.hThread);

    // Job Object に割り当て: 当 Job Object のハンドルを閉じると OS がプロセスを終了
    if (g_ollamaJobObject) {
        AssignProcessToJobObject(g_ollamaJobObject, pi.hProcess);
    }

    if (g_ollamaProcess) {
        CloseHandle(g_ollamaProcess);
    }
    g_ollamaProcess = pi.hProcess;

    logging::Debug("[Ollama] ollama serve 起動 (PID=%u, exe=%s)", pi.dwProcessId, exePath.c_str());
    logging::Debug("[Ollama] OLLAMA_MODELS=%s", modelsDir.c_str());

    // サーバー起動を待機 (最大30秒)
    for (int i = 0; i < 60; i++) {
        Sleep(500);
        if (translate::IsHealthy()) {
            logging::Debug("[Ollama] ollama serve 応答確認 (%d ms)", (i + 1) * 500);
            return true;
        }
    }
    logging::Debug("[Ollama] ollama serve 応答タイムアウト (30秒)");
    return false;
}

// Ollama 起動確認 + モデル確認/DL
static bool EnsureRunning() {
    if (translate::IsHealthy()) {
        return translate::EnsureModel();
    }
    std::string exe = FindBundledOllama();
    if (exe.empty()) {
        logging::Debug("[Ollama] バイナリ未検出、自動起動スキップ");
        return false;
    }
    if (!StartProcess(exe)) return false;
    return translate::EnsureModel();
}

// ============================================================
// ヘルスワーカー
// ============================================================

static void HealthWorker() {
    logging::Debug("[Ollama] ヘルスワーカー開始");
    while (g_running.load()) {
        {
            std::unique_lock<std::mutex> lock(g_healthMutex);
            g_healthCv.wait_for(lock, std::chrono::seconds(5), [] {
                return g_restartRequested.load() || !g_running.load();
            });
        }
        if (!g_running.load()) break;

        if (g_restartRequested.exchange(false)) {
            if (g_managed) {
                g_radioState.store(RadioState::RESTARTING);
                bool ok = EnsureRunning();
                g_radioState.store(ok ? RadioState::ON : RadioState::FAULT);
                logging::Debug("[Ollama] 再起動 %s", ok ? "成功" : "失敗");
            }
            continue;
        }

        // 定期ヘルスチェック
        if (!g_userEnabled.load()) continue;
        bool ok = translate::IsHealthy();
        RadioState cur = g_radioState.load();
        if (ok) {
            if (cur == RadioState::FAULT || cur == RadioState::OFF)
                g_radioState.store(RadioState::ON);
        } else if (cur == RadioState::ON) {
            g_radioState.store(g_managed ? RadioState::FAULT : RadioState::OFF);
            logging::Debug("[Ollama] ダウン検出 (%s)", g_managed ? "FAULT" : "OFF");
        }
    }
    logging::Debug("[Ollama] ヘルスワーカー終了");
}

// ============================================================
// 公開 API
// ============================================================

void ollama::Init(const std::string& ollamaDir, const std::string& endpoint) {
    g_ollamaDir = ollamaDir;
    g_managed   = IsLocalEndpoint(endpoint);

    if (g_managed) {
        logging::Debug("[Ollama] ローカルエンドポイント: 自動管理モード");
        if (!EnsureRunning()) {
            logging::Debug("[Ollama] 初期化失敗 - FAULT 状態で続行");
            g_radioState.store(RadioState::FAULT);
        }
    } else {
        logging::Debug("[Ollama] リモートエンドポイント: 接続のみモード");
    }

    g_running.store(true);
    g_healthThread = std::thread(HealthWorker);
}

void ollama::Shutdown() {
    g_running.store(false);
    g_healthCv.notify_all();
    if (g_healthThread.joinable()) g_healthThread.join();

    // Job Object を閉じる → KILL_ON_JOB_CLOSE により Ollama プロセスが終了
    if (g_ollamaJobObject) {
        CloseHandle(g_ollamaJobObject);
        g_ollamaJobObject = nullptr;
    }

    if (g_ollamaProcess) {
        WaitForSingleObject(g_ollamaProcess, 3000);
        CloseHandle(g_ollamaProcess);
        g_ollamaProcess = nullptr;
    }

    logging::Debug("[Ollama] シャットダウン完了");
}

void ollama::DetachThread() {
    // DLL_PROCESS_DETACH (プロセス終了) 専用: スレッドは既に OS に終了済み
    if (g_healthThread.joinable()) g_healthThread.detach();
    // Job Object を閉じて Ollama を終了させる
    if (g_ollamaJobObject) {
        CloseHandle(g_ollamaJobObject);
        g_ollamaJobObject = nullptr;
    }
    if (g_ollamaProcess) {
        CloseHandle(g_ollamaProcess);
        g_ollamaProcess = nullptr;
    }
}

RadioState ollama::GetRadioState() {
    return g_radioState.load();
}

void ollama::SetUserEnabled(bool enabled) {
    g_userEnabled.store(enabled);
    if (!enabled) {
        g_radioState.store(RadioState::OFF);
    } else if (g_radioState.load() == RadioState::OFF) {
        // 再有効化: ヘルスチェックで ON/FAULT に遷移させる
        g_healthCv.notify_one();
    }
}

void ollama::RequestRestart() {
    g_restartRequested.store(true);
    g_healthCv.notify_one();
}
