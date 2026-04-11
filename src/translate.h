#pragma once
// ============================================================
// translate.h - Ollama 翻訳モジュール (ワーカーDLL側)
// WinHTTP で Ollama API を呼び出し、チャットメッセージを翻訳する
// ============================================================

#include <string>

namespace translate {

struct TranslateConfig {
    std::string endpoint   = "http://localhost:11434/api/generate";
    std::string model      = "gemma3:4b";
    std::string targetLang = "Japanese";
};

// WinHTTP セッション初期化 + ワーカースレッド起動
bool Init(const TranslateConfig& cfg);

// ワーカー停止 + WinHTTP クリーンアップ
void Shutdown();

// 同期翻訳 (ブロッキング) - テストアプリ・対話テスト用
std::string Sync(const std::string& text);

// 非同期翻訳キュー投入 - DLL 側 ProcessEvent コールバックから使用
void Queue(const std::string& channel, const std::string& sender, const std::string& message);

} // namespace translate
