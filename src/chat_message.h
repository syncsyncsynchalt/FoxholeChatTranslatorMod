#pragma once
// ============================================================
// chat_message.h - チャットメッセージ構造体
// ProcessEvent から抽出されたチャットデータの共通型
// Stage 2 (翻訳) 以降のパイプラインが受け取るインターフェース
// ============================================================

#include <string>
#include <cstdint>
#include "ue4.h"

struct ChatMessage {
    std::string  channel;       // チャンネル名 ("Team", "World", "Local" 等)
    std::string  sender;        // 送信者表示名 ("[TAG] PlayerName" 形式)
    std::string  message;       // メッセージ本文 (UTF-8)
    EChatChannel channelEnum;   // チャンネル列挙型 (フィルタリング用)
    DWORD        timestamp;     // GetTickCount() でのタイムスタンプ
};
