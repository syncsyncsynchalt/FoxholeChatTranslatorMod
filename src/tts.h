#pragma once
// ============================================================
// tts.h - 多言語TTS読み上げ (Windows OneCore / WinRT)
// ============================================================

#include <cstdint>
#include <string>

namespace tts {

// 初期化 (ワーカースレッド起動)
// language: "auto"=テキストから自動判定 / "ja" "en" "ru" "zh" "ko"=強制指定
void Init(const char* language = "auto", uint32_t voicevoxStyleId = 3, uint32_t voicevoxJaStyleId = 13);

// 次の発話テキストを設定する (現在の発話が終わった後に読み上げる。上書き可)
void SetLatest(const char* textUtf8);

// 現在発話中のテキストを返す (overlay ハイライト用。発話中でなければ空文字)
std::string GetSpeakingText();

// 現在の読み上げを中断
void Stop();

// シャットダウン (スレッド終了)
void Shutdown();

// ワーカースレッドを detach (DLL_PROCESS_DETACH 緊急用 - ~std::thread が std::terminate を呼ばないよう)
void DetachThread();

} // namespace tts
