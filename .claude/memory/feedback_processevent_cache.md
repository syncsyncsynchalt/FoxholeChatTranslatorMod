---
name: feedback_processevent_cache
description: ProcessEvent ホットパスでチャット関数を識別する際はアドレスキャッシュを使うこと
metadata:
  type: feedback
---

IdentifyChatFunc のような「全 ProcessEvent 呼び出しで毎回実行される関数」では、UFunction オブジェクトのメモリ読み出し（`*(funcAddr + UOBJECT_NAME_OFFSET)`）を無条件に行ってはいけない。

**Why:** ProcessEvent はフレームあたり数百〜数千回呼ばれる。毎回ランダムな UFunction アドレスを参照するとキャッシュミスが累積し、走行中など PE 頻度が高い場面でフレームラグになる。

**How to apply:** 「初回にチャット関数ポインタを学習 → `s_chatFuncPtrs[]` に保存 → 以降はポインタ等値比較（L1 キャッシュ内、1ns 以下）で即返却」パターンを必ず使う。CI 照合（メモリリード）はポインタキャッシュヒット後はスキップされる。

実装例（hooks.cpp の IdentifyChatFunc）:
```cpp
static void* s_chatFuncPtrs[CHAT_FUNC_COUNT] = {};

for (int i = 0; i < CHAT_FUNC_COUNT; i++) {
    if (s_chatFuncPtrs[i] && s_chatFuncPtrs[i] == function) return i;
}
// キャッシュミスのみ CI 読み出し
__try { ... s_chatFuncPtrs[i] = function; return i; } ...
```
