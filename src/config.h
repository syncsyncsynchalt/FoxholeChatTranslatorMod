#pragma once
// ============================================================
// config.h - 共有設定管理
// config.ini から読み込んだ設定値を全モジュールに提供
// ============================================================

#include <string>

// 翻訳先言語 + TTS エンジン選択を兼ねるランタイム切り替えモード
enum class TranslationMode {
    Off = 0,  // 翻訳無効
    JA,       // 日本語 (Sherpa-ONNX)
    JAZ,      // 日本語ずんだもん口調 (VOICEVOX)
    EN,       // 英語
    RU,       // ロシア語
    ZH,       // 中国語
    KO,       // 韓国語
};

// TTS 読み上げ対象のランタイム切り替えモード
enum class TtsMode {
    Off = 0,   // 読み上げ無効
    Original,  // 原文読み上げ
    Translated // 翻訳後読み上げ
};

struct Config {
    // [General]
    bool        enableConsole    = true;

    // [Translation]
    std::string ollamaEndpoint   = "http://localhost:11434/api/generate";
    std::string performancePreset = "Medium"; // "Low" / "Medium" / "High"

    // [Overlay]
    bool        demoMode         = true;

    // [TTS]
    uint32_t    ttsVoicevoxStyleId   = 3;     // 3=ずんだもんノーマル (JAZ モード)
    uint32_t    ttsVoicevoxJaStyleId = 13;   // 13=青山龍星ノーマル (JA モード)
    float       ttsVolume          = 0.8f;  // 0.0〜1.0
    bool        ttsVerboseLog      = false; // 合成フェーズの詳細ログ
};

namespace config {

// config.ini を読み込んで Config とランタイム状態を構築
// baseDir: DLLのあるディレクトリ (末尾 '\\' 付き)
void Load(const char* baseDir);

// 現在のグローバル設定を取得
const Config& Get();

// --- ランタイム切り替え (スレッドセーフ) ---

TranslationMode GetTranslationMode();
// Off -> JA -> JAZ -> EN -> RU -> ZH -> KO -> Off と循環
void CycleTranslationMode();

TtsMode GetTtsMode();
// Off -> Original -> Translated -> Off と循環
void CycleTtsMode();

} // namespace config
