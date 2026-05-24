#pragma once
#include <string>

namespace tts_install {
    // tools/tts/ 内の不足コンポーネントをバックグラウンドでインストール
    // ttsLanguage: config の TTS.Language 値 ("auto","ja","en",...)
    void StartIfNeeded(const std::string& ttsDir, const std::string& ttsLanguage);

    bool IsRunning();

    // 現在のステップ説明 (オーバーレイ表示用)
    std::string GetStatusText();

    void Shutdown();
}
