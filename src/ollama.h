#pragma once
// ============================================================
// ollama.h - Ollama プロセス管理・ヘルス監視
// ============================================================
#include <string>

enum class RadioState { ON, FAULT, RESTARTING };

namespace ollama {

// 初期化: Ollama 起動確認 + ヘルスワーカー開始
// ollamaDir: 同梱 ollama.exe のディレクトリ (空=自動検出)
// endpoint:  http://host:port/... (localhost 系なら自動管理、それ以外は接続のみ)
void Init(const std::string& ollamaDir, const std::string& endpoint);

// シャットダウン: ヘルスワーカー停止 + Ollama プロセス終了
void Shutdown();

// ワーカースレッドを detach (DLL_PROCESS_DETACH 緊急用)
void DetachThread();

// 現在のラジオ状態を取得 (スレッドセーフ)
RadioState GetRadioState();

// 自動リトライの進捗を取得 (RESTARTING 状態のとき意味を持つ)
void GetRestartProgress(int& attempt, int& maxAttempts);

} // namespace ollama
