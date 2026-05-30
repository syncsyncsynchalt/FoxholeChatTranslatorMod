---
name: feedback_tts_num_threads
description: Sherpa-ONNX / VOICEVOX の num_threads はゲームフレームレート優先なら 1 に設定する
metadata:
  type: feedback
---

TTS エンジン（Sherpa-ONNX Supertonic/VITS、VOICEVOX）の `num_threads` / `cpu_num_threads` は `1` に設定する。

**Why:** デフォルト `2` で ONNX 推論が走ると、走行中など CPU 負荷が高い場面で 2 スレッド分のスパイクが乗る。`THREAD_PRIORITY_BELOW_NORMAL` でも CPU が混雑している場合は干渉を避けられない。合成時間がやや増えるが（300-500ms 程度の増加）、フレームレートの安定性を優先するなら 1 が適切。

**How to apply:** tts.cpp の 3 箇所に設定値がある（2026年5月時点）:
- `CreateSupertonicModel`: `cfg.model.num_threads = 1`
- `CreateVitsModel` 相当: `cfg.model.num_threads = 1`
- VOICEVOX: `initOpts.cpu_num_threads = 1`

関連: [[feedback_worker_thread_priority]]
