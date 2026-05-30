---
name: feedback_overlay_textchanged
description: g_textChanged.exchange(false) の戻り値を捨てない。g_entries 更新後は必ず store(true) を呼ぶ
metadata:
  type: feedback
---

`g_textChanged.exchange(false)` の戻り値は `bool changed = ...` で受け取り、その後の `g_entries` コピー判定に使うこと。

**Why:** 戻り値を捨てると「変化フラグが立っているときのみコピー」というロジックが成立せず、毎フレーム全エントリを deque コピーし続ける（60FPS × std::string コピー = 無駄なアロケーション累積）。

**How to apply:**
1. `bool changed = g_textChanged.exchange(false)` で受け取る
2. `g_entries` を変更する箇所（pending 処理、demo モード更新、翻訳完了コールバック等）はすべて変更後に `g_textChanged.store(true)` を呼ぶ
3. demo モードのタイマー更新ブロックでは store(true) が抜けがちなので注意

overlay.cpp の該当パターン（2026年5月 修正済み）:
```cpp
bool changed = g_textChanged.exchange(false);
// ... g_entries を変更する箇所には store(true) を追加 ...
static std::deque<MessageEntry> s_cachedEntries;
if (changed || s_cachedEntries.empty()) {
    std::lock_guard<std::mutex> lock(g_textMutex);
    s_cachedEntries = g_entries;
}
const std::deque<MessageEntry>& entries = s_cachedEntries;
```
