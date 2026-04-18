#pragma once
// ============================================================
// translate.h - Ollama 翻訳モジュール (ワーカーDLL側)
// WinHTTP で Ollama API を呼び出し、チャットメッセージを翻訳する
// ============================================================

#include <functional>
#include <string>

namespace translate {

struct TranslateConfig {
    std::string endpoint   = "http://localhost:11434/api/generate";
    std::string targetLang = "Japanese";
    std::string ollamaDir;          // 同梱 ollama.exe のディレクトリ (空=自動検出)
    std::string performancePreset = "Medium"; // "Low" / "Medium" / "High"
};

struct TranslateResult {
    std::string channel;
    std::string sender;
    std::string original;
    std::string translated;
};

using ResultCallback = std::function<void(const TranslateResult&)>;

// WinHTTP セッション初期化 + ワーカースレッド起動
bool Init(const TranslateConfig& cfg);

// ワーカー停止 + WinHTTP クリーンアップ
void Shutdown();

// 翻訳完了時に呼ばれるコールバックを登録
void SetResultCallback(ResultCallback cb);

// 非同期翻訳キュー投入
void Queue(const std::string& channel, const std::string& sender, const std::string& message);

// 同期翻訳 (ブロッキング) - テスト・診断ツール用
std::string Sync(const std::string& text);

// Ollama 死活確認 (GET /api/version)
bool IsHealthy();

// Ollama 再起動 (EnsureOllama 再実行)
bool Restart();

} // namespace translate
