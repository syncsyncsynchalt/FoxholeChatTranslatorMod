#pragma once
// ============================================================
// translate.h - Ollama 翻訳モジュール (ワーカーDLL側)
// WinHTTP で Ollama API を呼び出し、チャットメッセージを翻訳する
// ============================================================

#include <functional>
#include <string>
#include "config.h"

namespace translate {

struct TranslateConfig {
    std::string endpoint          = "http://localhost:11434/api/generate";
    std::string performancePreset = "Medium"; // "Low" / "Medium" / "High"
};

// 直近の Sync() 呼び出しの Ollama 内部統計 (Sync() 直後に呼ぶこと)
struct SyncStats {
    double tokPerSec = 0.0;
    int    evalCount = 0;
};

struct TranslateResult {
    std::string     channel;
    std::string     sender;
    std::string     original;
    std::string     translated;
    bool            ok      = true;              // false = 翻訳失敗 (Ollama 接続不可等)
    TtsMode         ttsMode = TtsMode::Off;      // キュー投入時のスナップショット
};

using ResultCallback = std::function<void(const TranslateResult&)>;

// WinHTTP セッション初期化 + ワーカースレッド起動
bool Init(const TranslateConfig& cfg);

// ワーカー停止 + WinHTTP クリーンアップ
void Shutdown();

// 翻訳完了時に呼ばれるコールバックを登録
void SetResultCallback(ResultCallback cb);

// 非同期翻訳キュー投入 (キュー投入時点のモードをスナップショットとして受け取る)
void Queue(const std::string& channel, const std::string& sender, const std::string& message,
           TranslationMode translationMode, TtsMode ttsMode);

// 同期翻訳 (ブロッキング) - テスト・診断ツール用
std::string Sync(const std::string& text);

// 直近の Sync() の Ollama 内部統計を返す
SyncStats GetLastSyncStats();

// Ollama 死活確認 (GET /api/version)
bool IsHealthy();

// モデルの確認・DL (ollama.cpp から呼ばれる)
bool EnsureModel();

// 翻訳キューが処理中かどうかを返す (overlay_test 送信タイミング制御用)
bool IsBusy();

// ワーカースレッドを detach (DLL_PROCESS_DETACH 緊急用)
void DetachThread();

} // namespace translate
