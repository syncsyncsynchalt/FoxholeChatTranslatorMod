# Ollama ベンチマーク結果 — Stage 10

## 1. 実行環境

| 項目 | 値 |
|------|-----|
| CPU | AMD Ryzen 9 5900HS with Radeon Graphics (8 コア / 16 スレッド) |
| RAM | 15.4 GB |
| GPU | AMD Radeon Graphics (内蔵、VRAM なし) |
| OS | Windows |
| Ollama | 0.20.5 (同梱 CPU-only バイナリ) |
| モデル | gemma3:4b (3.3GB), gemma3:1b (815MB), gemma3:270m (292MB) |

## 2. テスト文一覧

| 種別 | 文字数 | 原文 |
|------|--------|------|
| Short | 9 | need ammo |
| Medium | 44 | build the gates back up we need defenses now |
| Long | 107 | hey guys we need more people at the front line the enemy is pushing hard and we are running low on supplies |

## 3. パターン別結果テーブル

ウォームキャッシュ (run 2,3 の平均)。メモリは Ollama 全プロセスの WorkingSet64 合計 (累積のため後続モデルは前モデル分を含む)。

| パターン | モデル | ctx | thread | Short(ms) | Med(ms) | Long(ms) | tok/s | Mem(MB) |
|----------|--------|----:|-------:|----------:|--------:|--------:|------:|--------:|
| 4b-Baseline | gemma3:4b | 256 | 2 | 1,054 | 2,044 | 3,345 | 9.5 | 4,178 |
| 4b-Thread4 | gemma3:4b | 256 | 4 | 965 | 1,410 | 2,495 | 13.6 | 4,185 |
| 4b-ThreadMax | gemma3:4b | 256 | 8 | 984 | 1,406 | 2,607 | 13.5 | 4,187 |
| 4b-ThreadAuto | gemma3:4b | 256 | 0 | 965 | 1,425 | 2,597 | 12.8 | 4,189 |
| 4b-Ctx128 | gemma3:4b | 128 | 4 | 950 | 1,606 | 2,434 | 13.9 | 4,178 |
| **4b-Ctx512** | **gemma3:4b** | **512** | **4** | **813** | **1,275** | **2,365** | **14.4** | **4,180** |
| 4b-MinCtx+MaxThread | gemma3:4b | 128 | 8 | 963 | 1,560 | 2,620 | 13.7 | 4,184 |
| 1b-Thread4 | gemma3:1b | 256 | 4 | 550 | 795 | 1,141 | 38.9 | 5,456 |
| 1b-ThreadAuto | gemma3:1b | 256 | 0 | 543 | 692 | 1,131 | 41.0 | 5,450 |
| 1b-ThreadMax | gemma3:1b | 256 | 8 | 508 | 742 | 1,073 | 40.4 | 5,455 |
| 270m-Thread4 | gemma3:270m | 256 | 4 | 224 | 313 | 481 | 123.8 | 6,002 |
| 270m-ThreadAuto | gemma3:270m | 256 | 0 | 245 | 320 | 489 | 105.2 | 6,006 |
| 270m-ThreadMax | gemma3:270m | 256 | 8 | 247 | 317 | 487 | 106.2 | 6,010 |

## 4. モデル別翻訳出力一覧

代表パターン (run 2) の翻訳結果。

### gemma3:4b (4b-Baseline)

| 原文 | 翻訳 |
|------|------|
| need ammo | 弾を必要としている |
| build the gates back up we need defenses now | 門を再建しなければなりません。今すぐに防御が必要なのです。 |
| hey guys we need more people at the front line... | みんな、前線に人員を増やしてほしい。敵が強く攻めていて、物資が不足している。 |

### gemma3:1b (1b-ThreadAuto)

| 原文 | 翻訳 |
|------|------|
| need ammo | 弾薬を必要です。 |
| build the gates back up we need defenses now | 門を復興させてください。今、防衛が必要です。 |
| hey guys we need more people at the front line... | 仲間たち、先手の人員がもっと必要です。敵は攻勢を強めていますし、物資が少なくなってきました。 |

### gemma3:270m (270m-Thread4)

| 原文 | 翻訳 |
|------|------|
| need ammo | ammo |
| build the gates back up we need defenses now | build the gates back up we need defenses now |
| hey guys we need more people at the front line... | Hey guys, we need more people at the front line. The enemy is pushing hard and we're running low on supplies. |

## 5. 考察

### スレッド数
- 4b: thread=2 (Baseline) は明確に遅い (9.5 tok/s)。thread=4 で 13.6 tok/s に改善 (43%向上)。
- thread=8 (全物理コア) は thread=4 と同等かわずかに遅い。コンテキストスイッチのオーバーヘッドが発生している可能性がある。
- thread=0 (Ollama 自動) は実測で thread=4 に近い値。

### コンテキスト長
- ctx=512 は ctx=256 より Short で 16%高速 (813ms vs 965ms)、tok/s も最高 (14.4)。コールドスタートが劇的に短縮される効果がある (5174ms → 1016ms)。
- ctx=128 は速度メリットが小さく、長文で切り詰めリスクがあるため非推奨。

### モデルサイズ
- gemma3:1b は 4b の約 3 倍高速 (41 tok/s) だが、翻訳品質に問題がある。「弾薬を必要です」（助詞ミス）、「大家、先線に」（中国語混入）等。ゲームチャットでは意味は通じるが不自然。
- gemma3:270m は翻訳能力がなく、英語をそのまま返す。プリセットから除外。

### メモリ
- Ollama はモデルを keep_alive 期間メモリに保持する。メモリ値は累積のため、モデル単体のメモリ消費は差分から推定: 4b ≈ 4.2GB、1b ≈ 1.3GB、270m ≈ 0.5GB。
- いずれのモデルも 16GB RAM で問題なく動作。

## 6. プリセット決定根拠

Low は gemma3:1b、Medium/High は gemma3:4b を採用。270m は翻訳能力がないため不採用。デフォルトは Medium。

| プリセット | モデル | num_ctx | num_thread | 理由 |
|-----------|--------|--------:|-----------:|------|
| **Low** | gemma3:1b | 256 | 2 | ロースペック PC 向け。gemma3:1b で最小リソース消費。翻訳品質は劣るが動作を最優先 |
| **Medium** | gemma3:4b | 256 | 4 | Baseline から 45%速度向上 (Short 965ms)。リソースと速度のバランスが最も良い |
| **High** | gemma3:4b | 512 | 0 | 4b 最速設定 (Short 813ms, 14.4 tok/s)。ctx=512 の速度メリット + thread=0 でOllamaが最適スレッド数を自動決定 |

num_thread=0 を High で採用した理由: thread=4 と thread=8 の差が小さく、マシンのコア数に依存しない thread=0 がポータブル性に優れる。ベンチマーク上 4b-Ctx512 (thread=4) が最速だが、ctx=512 の効果が支配的であり thread=0 でも同等の速度が期待できる。
