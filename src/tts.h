#pragma once
// ============================================================
// tts.h - 多言語TTS読み上げ (Windows OneCore / WinRT)
// ============================================================

namespace tts {

// 初期化 (ワーカースレッド起動)
void Init();

// テキストを非同期で読み上げ (言語は自動判定)
// sender が非NULLの場合、送信者名から決定論的に声色を選択する
void Speak(const char* textUtf8, const char* senderUtf8 = nullptr);

// 現在の読み上げを中断
void Stop();

// シャットダウン (スレッド終了)
void Shutdown();

} // namespace tts
