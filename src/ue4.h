#pragma once
// ============================================================
// ue4.h - Unreal Engine 4 内部型定義
// UE4 4.24-4.27 x64 Shipping ビルド向け
// ============================================================

#include <cstdint>
#include <cstring>

// ============================================================
// 基本型
// ============================================================

// FName: UE4のインターン化された文字列識別子
struct FName {
    int32_t ComparisonIndex; // GNames配列へのインデックス
    int32_t Number;          // インスタンス番号 (通常0)
};

// TArray: UE4の動的配列
template<typename T>
struct TArray {
    T*      Data;
    int32_t Count;
    int32_t Max;
};

// FString: UE4のワイド文字列
struct FString {
    wchar_t* Data;
    int32_t  Count;
    int32_t  Max;

    const wchar_t* c_str() const { return Data ? Data : L""; }
    bool IsValid() const { return Data != nullptr && Count > 0; }
};

// ============================================================
// FNameEntry - GNames テーブル内のエントリ
// UE4 バージョンによってレイアウトが異なる
// ============================================================

// UE4 4.23+ (chunked name pool)
// FNameEntryHeader: 2 bytes
// Bit 0: bIsWide (0=ANSI, 1=Wide)
// Bits 1-15: Length (最大16383文字)
struct FNameEntryHeader {
    uint16_t Data;

    bool IsWide() const { return Data & 1; }
    int  GetLength() const { return Data >> 1; }
};

// FNameEntry (4.23+ layout)
struct FNameEntry {
    FNameEntryHeader Header;
    union {
        char    AnsiName[1]; // 実際のサイズは Header.GetLength()
        wchar_t WideName[1];
    };

    // ANSI名を取得 (IsWide=false の場合)
    const char* GetAnsiName() const { return AnsiName; }

    // 決め打ちで名前を文字列にコピー (最大 bufSize-1 文字)
    bool GetName(char* buf, int bufSize) const {
        if (Header.IsWide()) {
            // ワイド文字からASCIIに変換 (簡易)
            int len = Header.GetLength();
            if (len >= bufSize) len = bufSize - 1;
            for (int i = 0; i < len; i++) {
                buf[i] = static_cast<char>(WideName[i]);
            }
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
// UE4 4.23+: FNamePool (チャンク化されたプール)
// ============================================================

// FNamePool の内部構造 (簡略版)
// 実際のレイアウトはエンジンバージョンに依存
//
// struct FNameEntryAllocator {
//     mutable FRWLock Lock;
//     int32 CurrentBlock;
//     int32 CurrentByteCursor;
//     uint8* Blocks[FNameMaxBlockBits]; // ブロックポインタ配列
// };
//
// ComparisonIndex の解釈 (4.23+):
//   Block  = ComparisonIndex >> 16
//   Offset = ComparisonIndex & 0xFFFF

// ブロックベースの FName 解決
// gnames: FNamePool のベースアドレス
// blockArrayOffset: ブロック配列ポインタへのオフセット (バージョン依存)
inline const FNameEntry* ResolveFNameChunked(
    uintptr_t gnamesBase,
    int32_t   comparisonIndex,
    int       blockArrayOffset = 0x40, // デフォルトオフセット (要調整)
    int       stride = 2               // エントリのアライメント
) {
    int block  = comparisonIndex >> 16;
    int offset = comparisonIndex & 0xFFFF;

    // ブロック配列: gnamesBase + blockArrayOffset
    uintptr_t* blocks = reinterpret_cast<uintptr_t*>(gnamesBase + blockArrayOffset);
    if (!blocks[block]) return nullptr;

    uintptr_t entryAddr = blocks[block] + static_cast<uintptr_t>(offset) * stride;
    return reinterpret_cast<const FNameEntry*>(entryAddr);
}

// フラット配列ベースの FName 解決 (UE4 4.0-4.22)
// gnames: TArray<FNameEntry*>* のアドレス
inline const FNameEntry* ResolveFNameFlat(
    uintptr_t gnamesBase,
    int32_t   comparisonIndex
) {
    struct FNameArray {
        uintptr_t* Data;
        int32_t    Count;
        int32_t    Max;
    };
    auto arr = reinterpret_cast<FNameArray*>(gnamesBase);
    if (comparisonIndex < 0 || comparisonIndex >= arr->Count) return nullptr;
    return reinterpret_cast<const FNameEntry*>(arr->Data[comparisonIndex]);
}

// ============================================================
// UObject (部分的なレイアウト)
// UE4 4.24-4.27 x64
// ============================================================

// UObjectBase のメモリレイアウト:
// 0x00: VTable*            (8 bytes)
// 0x08: EObjectFlags       (4 bytes)
// 0x0C: InternalIndex      (4 bytes)
// 0x10: ClassPrivate*      (8 bytes)
// 0x18: NamePrivate        (8 bytes = FName)
// 0x20: OuterPrivate*      (8 bytes)
// Total: 0x28 (40 bytes)
//
// 注意: これらのオフセットはエンジンバージョンや
// カスタム修正により異なる場合があります

namespace ue4 {

constexpr int UOBJECT_NAME_OFFSET  = 0x18; // FName NamePrivate
constexpr int UOBJECT_CLASS_OFFSET = 0x10; // UClass* ClassPrivate
constexpr int UOBJECT_OUTER_OFFSET = 0x20; // UObject* OuterPrivate

// UObject から FName を取得
inline FName GetObjectFName(void* obj) {
    return *reinterpret_cast<FName*>(reinterpret_cast<uintptr_t>(obj) + UOBJECT_NAME_OFFSET);
}

// ProcessEvent の関数シグネチャ
// void UObject::ProcessEvent(UFunction* Function, void* Parms)
// x64: rcx=this, rdx=Function, r8=Parms
using ProcessEventFn = void(__thiscall*)(void* thisObj, void* function, void* parms);

} // namespace ue4
