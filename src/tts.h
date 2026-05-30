#pragma once
// ============================================================
// tts.h - 多言語TTS読み上げ (Windows OneCore / WinRT)
// ============================================================

#include <cstdint>
#include <functional>

namespace tts {

// 初期化 (ワーカースレッド起動)
// language: "auto"=テキストから自動判定 / "ja" "en" "ru" "zh" "ko"=強制指定
void Init(const char* language = "auto", uint32_t voicevoxStyleId = 3, uint32_t voicevoxJaStyleId = 13);

// テキストを非同期で読み上げ (言語は自動判定)
// sender が非NULLの場合、送信者名から決定論的に声色を選択する
// onSynthesisReady: 合成完了・再生開始直前に呼ばれるコールバック (nullptr 可)
//   合成失敗時もデストラクタ経由で必ず1回呼ばれる (表示が止まったまま残らないよう保証)
void Speak(const char* textUtf8, const char* senderUtf8 = nullptr,
           std::function<void()> onSynthesisReady = nullptr);

// TTS キューが処理中または再生中かどうかを返す (overlay_test 送信タイミング制御用)
bool IsBusy();

// 現在の読み上げを中断
void Stop();

// シャットダウン (スレッド終了)
void Shutdown();

// ワーカースレッドを detach (DLL_PROCESS_DETACH 緊急用 - ~std::thread が std::terminate を呼ばないよう)
void DetachThread();

} // namespace tts
