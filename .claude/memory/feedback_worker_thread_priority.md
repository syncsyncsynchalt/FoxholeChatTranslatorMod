---
name: feedback_worker_thread_priority
description: 全バックグラウンドワーカースレッドは先頭で BELOW_NORMAL に設定すること
metadata:
  type: feedback
---

新しいワーカースレッド関数を作るときは、必ず先頭行に `SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL)` を書く。

**Why:** TTS スレッドは設定済みだったが翻訳スレッド（translate.cpp WorkerThread）が NORMAL のまま放置されていた。ゲームが CPU を多く必要とする走行中などにスタッターを引き起こす。I/O ブロッキング待ち中はスケジューリングされないが、正規表現マッチや文字列処理の瞬間には NORMAL 優先でゲームスレッドと競合しうる。

**How to apply:** `WorkerThread` / `TtsWorker` 等を新規に追加する際はコード最初の行として設定する。既存スレッドを修正するときも漏れがないかチェックする。

関連: [[feedback_tts_num_threads]]
