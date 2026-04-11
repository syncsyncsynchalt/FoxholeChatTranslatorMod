// ============================================================
// hooks.cpp - チャットキャプチャロジック (ワーカーDLL側)
// ProcessEventコールバックでチャットメッセージを識別・抽出する
// ============================================================

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "hooks.h"
#include "ue4.h"
#include "gnames.h"
#include "config.h"
#include "log.h"
#include "chat_message.h"
#include "overlay.h"
#include "translate.h"

// ============================================================
// DLLベースディレクトリ取得
// ============================================================

static std::string GetDllBaseDir() {
    char dllPath[MAX_PATH];
    HMODULE hSelf;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&GetDllBaseDir), &hSelf);
    GetModuleFileNameA(hSelf, dllPath, MAX_PATH);
    std::string dir(dllPath);
    size_t lastSlash = dir.rfind('\\');
    if (lastSlash != std::string::npos) dir = dir.substr(0, lastSlash + 1);
    return dir;
}

// ============================================================
// UE4 ヘルパー
// ============================================================

static bool GetPlayerNameSafe(void* playerState, FString* out) {
    if (!playerState || !IsReadableMemory(playerState)) return false;
    __try {
        auto namePtr = reinterpret_cast<FString*>(
            reinterpret_cast<uintptr_t>(playerState) + PLAYERSTATE_PLAYERNAME_OFFSET);
        *out = *namePtr;
        return out->IsValid() && IsReadableMemory(out->Data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static std::string GetPlayerName(void* playerState) {
    FString fs = {};
    if (GetPlayerNameSafe(playerState, &fs)) return FStringToUtf8(fs);
    return "";
}

// ============================================================
// チャットFName CI 検出
// ============================================================

enum ChatFuncId {
    CHAT_FUNC_CLIENT_CHAT_MESSAGE = 0,
    CHAT_FUNC_CLIENT_CHAT_MESSAGE_WITH_TAG,
    CHAT_FUNC_CLIENT_WORLD_CHAT_MESSAGE,
    CHAT_FUNC_COUNT
};

static const char* g_chatFuncNames[CHAT_FUNC_COUNT] = {
    "ClientChatMessage",
    "ClientChatMessageWithTag",
    "ClientWorldChatMessage",
};

static int32_t g_chatFuncCIs[CHAT_FUNC_COUNT] = { -1, -1, -1 };
static bool    g_chatCIsReady = false;

static void SearchChatFNames() {
    logging::Debug("=== チャット関連FName検索 ===");
    int found = 0;
    for (int i = 0; i < CHAT_FUNC_COUNT; i++) {
        g_chatFuncCIs[i] = gnames::FindFNameIndex(g_chatFuncNames[i]);
        if (g_chatFuncCIs[i] >= 0) {
            char verify[128] = {};
            gnames::ResolveFName(g_chatFuncCIs[i], verify, sizeof(verify));
            logging::Debug("  %s -> CI=%d [verify='%s']", g_chatFuncNames[i], g_chatFuncCIs[i], verify);
            found++;
        } else {
            logging::Debug("  %s -> 未検出", g_chatFuncNames[i]);
        }
    }
    logging::Debug("  %d/%d 件検出", found, CHAT_FUNC_COUNT);
    g_chatCIsReady = (found > 0);
}

// ============================================================
// GNames 遅延リトライ
// ============================================================

static int g_gnamesRetryCount = 0;

static void TryLazyGNamesResolve() {
    if (gnames::IsAvailable() || g_gnamesRetryCount >= 20) return;
    static int callCounter = 0;
    if (++callCounter % 30000 != 0) return;
    g_gnamesRetryCount++;
    logging::Debug("GNames: 遅延リトライ #%d", g_gnamesRetryCount);
    if (gnames::Find()) {
        char test[64] = {};
        if (gnames::ResolveFName(0, test, sizeof(test))) {
            logging::Debug("GNames: 検出成功! index 0 = '%s'", test);
            SearchChatFNames();
        }
    }
}

// ============================================================
// チャット関数識別
// ============================================================

static int IdentifyChatFunc(void* function) {
    if (!function || !g_chatCIsReady) return -1;
    __try {
        uintptr_t funcAddr = reinterpret_cast<uintptr_t>(function);
        if (funcAddr < 0x10000) return -1;
        // UE4 ProcessEvent の function ポインタは常に有効なため、
        // VirtualQuery (IsReadableMemory) を省略し SEH で保護する
        int32_t funcCI = *reinterpret_cast<int32_t*>(funcAddr + ue4::UOBJECT_NAME_OFFSET);
        for (int i = 0; i < CHAT_FUNC_COUNT; i++) {
            if (g_chatFuncCIs[i] >= 0 && funcCI == g_chatFuncCIs[i]) {
                uint32_t flags = *reinterpret_cast<uint32_t*>(funcAddr + 0x98);
                if (!(flags & 0x40)) return -1;
                return i;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}

// ============================================================
// パラメータ読み取り
// ============================================================

struct RawChatParams {
    FString      msgStr;
    FString      senderStr;
    FString      regTagStr;
    EChatChannel channel;
    bool         hasSender;
    bool         hasRegTag;
    void*        senderPlayerState;
};

static bool ReadChatParamsSafe(int chatFunc, void* parms, RawChatParams* out) {
    __try {
        memset(out, 0, sizeof(*out));
        out->channel = EChatChannel::Default;

        switch (chatFunc) {
        case CHAT_FUNC_CLIENT_CHAT_MESSAGE: {
            auto p = reinterpret_cast<Parms_ClientChatMessage*>(parms);
            out->msgStr = p->MsgString;
            out->senderPlayerState = p->SenderPlayerState;
            out->channel = p->Channel;
            out->hasSender = true;
            break;
        }
        case CHAT_FUNC_CLIENT_CHAT_MESSAGE_WITH_TAG: {
            auto p = reinterpret_cast<Parms_ClientChatMessageWithTag*>(parms);
            out->msgStr = p->MsgString;
            out->senderPlayerState = p->SenderPlayerState;
            out->regTagStr = p->SenderRegimentTag;
            out->channel = p->Channel;
            out->hasSender = true;
            out->hasRegTag = true;
            break;
        }
        case CHAT_FUNC_CLIENT_WORLD_CHAT_MESSAGE: {
            auto p = reinterpret_cast<Parms_ClientWorldChatMessage*>(parms);
            out->msgStr = p->Message;
            out->senderStr = p->SenderName;
            out->regTagStr = p->SenderRegimentTag;
            out->channel = p->Channel;
            out->hasRegTag = true;
            break;
        }
        default:
            return false;
        }
        return out->msgStr.IsValid() && IsReadableMemory(out->msgStr.Data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// ============================================================
// FNameBlockOffsetBits 自動検出
// ============================================================

static bool g_shiftDetected = false;

static bool TryDetectShift(void* function) {
    __try {
        uintptr_t funcAddr = reinterpret_cast<uintptr_t>(function);
        if (funcAddr < 0x10000 || !IsReadableMemory(function)) return false;

        uintptr_t classPtr = *reinterpret_cast<uintptr_t*>(funcAddr + 0x10);
        if (classPtr < 0x10000 || !IsReadableMemory(reinterpret_cast<void*>(classPtr))) return false;

        int32_t rawCI = *reinterpret_cast<int32_t*>(funcAddr + ue4::UOBJECT_NAME_OFFSET);
        if (rawCI <= 0 || rawCI < 65536) return false;

        char buf[128];
        for (int shift = 16; shift >= 14; shift--) {
            if (gnames::ResolveFNameWithShift(rawCI, shift, buf, sizeof(buf))) {
                int currentBits = gnames::GetBlockOffsetBits();
                if (shift != currentBits) {
                    logging::Debug("FNameBlockOffsetBits 検出: %d (旧=%d, CI=%d -> '%s')",
                        shift, currentBits, rawCI, buf);
                    gnames::SetBlockOffsetBits(shift);

                    logging::Debug("chat CI 再計算 (shift=%d)...", shift);
                    int found = 0;
                    for (int i = 0; i < CHAT_FUNC_COUNT; i++) {
                        g_chatFuncCIs[i] = gnames::FindFNameIndex(g_chatFuncNames[i]);
                        if (g_chatFuncCIs[i] >= 0) {
                            char verify[128] = {};
                            gnames::ResolveFName(g_chatFuncCIs[i], verify, sizeof(verify));
                            logging::Debug("  %s -> CI=%d [verify='%s']", g_chatFuncNames[i], g_chatFuncCIs[i], verify);
                            found++;
                        }
                    }
                    g_chatCIsReady = (found > 0);
                    logging::Debug("  %d/%d 件検出", found, CHAT_FUNC_COUNT);
                } else {
                    logging::Debug("FNameBlockOffsetBits 確認OK: %d (CI=%d -> '%s')", shift, rawCI, buf);
                }
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// ============================================================
// ProcessEvent コールバック
// ============================================================

void hooks::OnProcessEvent(void* thisObj, void* function, void* parms) {
    if (!gnames::IsAvailable()) {
        TryLazyGNamesResolve();
        return;
    }
    if (!g_chatCIsReady) return;

    // FNameBlockOffsetBits 自動検出 (最初の100回)
    if (!g_shiftDetected) {
        static volatile long shiftAttempts = 0;
        long sa = InterlockedIncrement(&shiftAttempts);
        if (sa <= 100) {
            if (TryDetectShift(function)) {
                g_shiftDetected = true;
            }
        } else {
            g_shiftDetected = true;
        }
    }

    // チャット関数か判定
    int chatFunc = IdentifyChatFunc(function);
    if (chatFunc < 0 || !parms) return;

    // SEH安全にパラメータ読み取り
    RawChatParams data;
    if (!ReadChatParamsSafe(chatFunc, parms, &data)) return;

    // メッセージ抽出
    std::string msg = FStringToUtf8(data.msgStr);
    if (msg.empty()) return;

    std::string sender;
    if (data.hasSender) {
        sender = GetPlayerName(data.senderPlayerState);
    } else {
        sender = FStringToUtf8(data.senderStr);
    }

    std::string regTag;
    if (data.hasRegTag) regTag = FStringToUtf8(data.regTagStr);

    std::string displaySender = sender;
    if (!regTag.empty()) displaySender = "[" + regTag + "] " + sender;
    if (displaySender.empty()) displaySender = "???";

    const char* chName = ChannelName(data.channel);

    // 重複排除: 同一メッセージを500ms以内に2回処理しない
    static std::string s_lastDedupKey;
    static DWORD s_lastDedupTime = 0;
    std::string dedupKey = std::string(chName) + "|" + displaySender + "|" + msg;
    DWORD now = GetTickCount();
    if (dedupKey == s_lastDedupKey && (now - s_lastDedupTime) < 500) {
        return;
    }
    s_lastDedupKey = dedupKey;
    s_lastDedupTime = now;

    // ChatMessage を構築
    ChatMessage chatMsg;
    chatMsg.channel     = chName;
    chatMsg.sender      = displaySender;
    chatMsg.message     = msg;
    chatMsg.channelEnum = data.channel;
    chatMsg.timestamp   = now;

    // Stage 1: ログ出力
    logging::Debug("[%s] %s: %s", chName, displaySender.c_str(), msg.c_str());
    logging::Chat(chName, displaySender.c_str(), msg.c_str());

    // Stage 9: 実チャットメッセージをオーバーレイに渡す
    overlay::OnChatMessage(displaySender, msg);
}

// ============================================================
// 公開インターフェース
// ============================================================

void hooks::SetHookedPEAddress(uintptr_t addr) {
    logging::Debug("フック済みPEアドレス設定: 0x%llX", addr);
}

bool hooks::Init() {
    std::string baseDir = GetDllBaseDir();

    config::Load(baseDir.c_str());
    const Config& cfg = config::Get();

    logging::Init(baseDir.c_str(), cfg.enableConsole);
    if (!cfg.logFilePath.empty()) {
        logging::SetChatLogPath(cfg.logFilePath.c_str());
    }

    logging::Debug("===================================");
    logging::Debug("Foxhole Chat Translator");
    logging::Debug("===================================");

    if (gnames::Find()) {
        char testName[64] = {};
        if (gnames::ResolveFName(0, testName, sizeof(testName))) {
            logging::Debug("GNames OK: index 0 = '%s'", testName);
            SearchChatFNames();
        } else {
            logging::Debug("GNames: ResolveFName失敗");
        }
    } else {
        logging::Debug("GNames 未検出 - 遅延リトライで再試行");
    }

    logging::Debug("チャットログ出力先: %s", cfg.logFilePath.c_str());
    return true;
}

void hooks::Shutdown() {
    logging::Debug("ワーカーシャットダウン");
    logging::Shutdown();
}
