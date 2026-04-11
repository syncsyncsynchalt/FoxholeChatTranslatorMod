#pragma once
// ============================================================
// ue4.h - Unreal Engine 4 内部型定義
// Foxhole (UE4 4.24.3) x64 Shipping ビルド向け
// Dumper-7 SDK出力に基づく正確なオフセット
// ============================================================

#include <cstdint>
#include <cstring>
#include <windows.h>
#include <string>

// ============================================================
// メモリ安全ヘルパー
// ============================================================

inline bool IsReadableMemory(const void* ptr, size_t size = 8) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_WRITECOPY)) != 0;
}

// ============================================================
// 基本型
// ============================================================

struct FName {
    int32_t ComparisonIndex;
    int32_t Number;
};

template<typename T>
struct TArray {
    T*      Data;
    int32_t Count;
    int32_t Max;
};

struct FString {
    wchar_t* Data;
    int32_t  Count;
    int32_t  Max;

    const wchar_t* c_str() const { return Data ? Data : L""; }
    bool IsValid() const { return Data != nullptr && Count > 0 && Count < 100000; }
};

// ============================================================
// Foxhole SDK: チャット関連 Enum (Dumper-7出力)
// ============================================================

enum class EChatChannel : uint8_t {
    Default    = 0,
    RegionTeam = 1,
    RegionTeamAir = 2,
    WorldTeam  = 3,
    Logistics  = 4,
    Intel      = 5,
    LocalAll   = 6,
    Squad      = 7,
    Regiment   = 8,
    Whisper    = 9,
    Admin      = 10,
    MAX        = 11,
};

enum class EChatLanguage : uint8_t {
    English  = 1,
    Russian  = 2,
    Korean   = 4,
    Chinese  = 8,
};

// ============================================================
// Foxhole SDK: チャットRPCパラメータ構造体 (Dumper-7出力)
// これらはProcessEventのparms引数にそのまま対応する
// ============================================================

// War.SimPlayerController.ClientChatMessage (0x28 bytes)
struct Parms_ClientChatMessage {
    EChatChannel  Channel;                  // 0x00
    uint8_t       _pad1[7];                 // 0x01
    void*         SenderPlayerState;        // 0x08  (APlayerState*)
    FString       MsgString;                // 0x10
    EChatLanguage BroadcastLanguage;        // 0x20
    bool          ReportingWhisperToSelf;   // 0x21
    bool          bIsEnabled;               // 0x22
    uint8_t       _pad2[5];                 // 0x23
};
static_assert(sizeof(Parms_ClientChatMessage) == 0x28);

// War.SimPlayerController.ClientChatMessageWithTag (0x38 bytes)
struct Parms_ClientChatMessageWithTag {
    EChatChannel  Channel;                  // 0x00
    uint8_t       _pad1[7];                 // 0x01
    void*         SenderPlayerState;        // 0x08
    FString       SenderRegimentTag;        // 0x10
    FString       MsgString;                // 0x20
    EChatLanguage BroadcastLanguage;        // 0x30
    bool          ReportingWhisperToSelf;   // 0x31
    bool          bIsEnabled;               // 0x32
    uint8_t       _pad2[5];                 // 0x33
};
static_assert(sizeof(Parms_ClientChatMessageWithTag) == 0x38);

// War.SimPlayerController.ClientWorldChatMessage (0x48 bytes)
struct Parms_ClientWorldChatMessage {
    FString       Message;                  // 0x00
    FString       SenderName;               // 0x10
    FString       SenderRegimentTag;        // 0x20
    FString       SenderOnlineId;           // 0x30
    uint8_t       SenderTeamID;             // 0x40
    EChatChannel  Channel;                  // 0x41
    EChatLanguage BroadcastLanguage;        // 0x42
    uint8_t       MapId;                    // 0x43
    bool          bReportingWhisperToSelf;  // 0x44
    bool          bIsEnabled;               // 0x45
    uint8_t       _pad[2];                  // 0x46
};
static_assert(sizeof(Parms_ClientWorldChatMessage) == 0x48);

// APlayerState::PlayerNamePrivate オフセット (Dumper-7: Engine_classes.hpp)
constexpr int PLAYERSTATE_PLAYERNAME_OFFSET = 0x328;

// ============================================================
// UE4 型変換ユーティリティ
// ============================================================

// FString → UTF-8 変換
inline std::string FStringToUtf8(const FString& fstr) {
    if (!fstr.IsValid()) return "";
    if (!IsReadableMemory(fstr.Data)) return "";

    int len = fstr.Count - 1; // null terminator 除く
    if (len <= 0) return "";

    char buf[2048];
    int written = WideCharToMultiByte(CP_UTF8, 0, fstr.Data, len, buf, sizeof(buf) - 1, nullptr, nullptr);
    if (written <= 0) return "";
    buf[written] = 0;
    return std::string(buf);
}

// EChatChannel → 文字列
inline const char* ChannelName(EChatChannel ch) {
    switch (ch) {
    case EChatChannel::Default:       return "Default";
    case EChatChannel::RegionTeam:    return "Team";
    case EChatChannel::RegionTeamAir: return "TeamAir";
    case EChatChannel::WorldTeam:     return "World";
    case EChatChannel::Logistics:     return "Logistics";
    case EChatChannel::Intel:         return "Intel";
    case EChatChannel::LocalAll:      return "Local";
    case EChatChannel::Squad:         return "Squad";
    case EChatChannel::Regiment:      return "Regiment";
    case EChatChannel::Whisper:       return "Whisper";
    case EChatChannel::Admin:         return "Admin";
    default:                          return "Unknown";
    }
}

// ============================================================
// UObject オフセット (Foxhole UE4 4.24.3 x64)
// ============================================================

namespace ue4 {

constexpr int UOBJECT_NAME_OFFSET  = 0x18;
constexpr int UOBJECT_CLASS_OFFSET = 0x10;
constexpr int UOBJECT_OUTER_OFFSET = 0x20;

} // namespace ue4
