#pragma once
// ============================================================
// ue4.h - Unreal Engine 4 内部型定義
// Foxhole (UE4 4.24.3) x64 Shipping ビルド向け
// Dumper-7 SDK出力に基づく正確なオフセット
// ============================================================

#include <cstdint>
#include <cstring>

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

// War.SimPlayerController.ServerChat (0x18 bytes)
struct Parms_ServerChat {
    FString       Message;                  // 0x00
    EChatChannel  ChatChannel;              // 0x10
    EChatLanguage BroadcastLanguage;        // 0x11
    uint8_t       _pad[6];                  // 0x12
};
static_assert(sizeof(Parms_ServerChat) == 0x18);

// APlayerState::PlayerNamePrivate オフセット (Dumper-7: Engine_classes.hpp)
constexpr int PLAYERSTATE_PLAYERNAME_OFFSET = 0x328;

// ============================================================
// FNameEntry - GNames テーブル内のエントリ
// ============================================================

struct FNameEntryHeader {
    uint16_t Data;
    bool IsWide() const { return Data & 1; }
    int  GetLength() const { return Data >> 1; }
};

struct FNameEntry {
    FNameEntryHeader Header;
    union {
        char    AnsiName[1];
        wchar_t WideName[1];
    };

    bool GetName(char* buf, int bufSize) const {
        if (Header.IsWide()) {
            int len = Header.GetLength();
            if (len >= bufSize) len = bufSize - 1;
            for (int i = 0; i < len; i++) buf[i] = static_cast<char>(WideName[i]);
            buf[len] = 0;
        } else {
            int len = Header.GetLength();
            if (len >= bufSize) len = bufSize - 1;
            memcpy(buf, AnsiName, len);
            buf[len] = 0;
        }
        return true;
    }
};

// ============================================================
// GNames アクセス
// ============================================================

inline const FNameEntry* ResolveFNameChunked(
    uintptr_t gnamesBase, int32_t comparisonIndex,
    int blockArrayOffset = 0x40, int stride = 2)
{
    int block  = comparisonIndex >> 16;
    int offset = comparisonIndex & 0xFFFF;
    uintptr_t* blocks = reinterpret_cast<uintptr_t*>(gnamesBase + blockArrayOffset);
    if (!blocks[block]) return nullptr;
    uintptr_t entryAddr = blocks[block] + static_cast<uintptr_t>(offset) * stride;
    return reinterpret_cast<const FNameEntry*>(entryAddr);
}

inline const FNameEntry* ResolveFNameFlat(
    uintptr_t gnamesBase, int32_t comparisonIndex)
{
    struct FNameArray { uintptr_t* Data; int32_t Count; int32_t Max; };
    auto arr = reinterpret_cast<FNameArray*>(gnamesBase);
    if (comparisonIndex < 0 || comparisonIndex >= arr->Count) return nullptr;
    return reinterpret_cast<const FNameEntry*>(arr->Data[comparisonIndex]);
}

// ============================================================
// UObject オフセット (Foxhole UE4 4.24.3 x64)
// ============================================================

namespace ue4 {

constexpr int UOBJECT_NAME_OFFSET  = 0x18;
constexpr int UOBJECT_CLASS_OFFSET = 0x10;
constexpr int UOBJECT_OUTER_OFFSET = 0x20;

inline FName GetObjectFName(void* obj) {
    return *reinterpret_cast<FName*>(reinterpret_cast<uintptr_t>(obj) + UOBJECT_NAME_OFFSET);
}

using ProcessEventFn = void(__thiscall*)(void* thisObj, void* function, void* parms);

} // namespace ue4
