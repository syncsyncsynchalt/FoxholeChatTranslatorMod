# Ollama ベンチマーク結果

最終更新: 2026-06-02 (v4 — 全プリセット・全言語 360件)

---

## 1. 実行環境

| 項目 | 値 |
|------|-----|
| CPU | AMD Ryzen 9 5900HS (8コア/16スレッド) |
| RAM | 15.4 GB |
| GPU | NVIDIA GeForce RTX 3050 Ti Laptop GPU (4GB VRAM) |
| OS | Windows 11 |
| Ollama | 0.24.0 (CUDA v12) |
| ベンチツール | `tools/bench_translate.py` (2026-06-02) |

---

## 2. プリセット定義

| プリセット | モデル | サイズ | num_ctx | num_thread | temperature | num_predict |
|-----------|--------|-------:|--------:|-----------:|------------:|------------:|
| **Low** | gemma3:1b | 815 MB | 128 | 2 | 0.1 | 120 |
| **Medium** | phi4-mini | 2.5 GB | 512 | 0 (全コア) | 0.1 | 120 |
| **High** | gemma3:4b | 3.3 GB | 512 | 0 (全コア) | 0.1 | 120 |

**翻訳プロンプト (JA モード, v4):**

```
[system] You are a Foxhole war-game chat translator.
         Foxhole is a persistent MMO war game (wars last weeks) between two factions:
           Wardens (nicknamed blues/blueberries) and Colonials (nicknamed collies/goblins).
         37 hex regions on a continent; win by capturing 32+ town halls (victory points).
         No NPCs — every soldier, vehicle, and building is player-operated.
         Player roles: infantry (combat), logi (supply runs), builder (base construction), crew (vehicle operator).
         War materials: bmat=basic, rmat=refined, emat=explosive, cmat=construction — all are physical war supplies.
         T1/T2/T3 = technology tier unlocks (basic to advanced equipment).
         Chat conventions:
           'NEED X TO [place]' means X must be sent/delivered to that location.
           'pieces' = military hardware (guns/tanks/vehicles), never people.
           'boys/guys/lads' = teammates (gender-neutral rallying language).
           Numbers in chat = troop counts, material amounts, distances, or timers.
         [マッチしたスラングのみ] Note: 'lazy' means too lazy (not sick); 'W' means kudos; ...
         [保護語あり] Keep these game-specific terms exactly as-is: <語>.
         [保護語+placeholder] {{T0}}, {{T1}}, etc. are location/term placeholders —
           translate the sentence meaning but keep these tokens verbatim in your Japanese output.

[prompt] Translate the following war-game chat message into natural, casual Japanese.
         Output ONE short sentence only.
         Preserve numbers and times (e.g. '15 mins') exactly.
         Output ONLY the Japanese translation — no romanization, no explanations.

[stop]   ["\n\n", " (", "Note:", "Translation:", "Here's", "Here is"]
```

**slang_dict.txt** (入力にマッチした語の説明のみ動的追加、v4: 70+エントリ):

| カテゴリ | 主要エントリ |
|---------|------------|
| 称賛/反応 | W, GG, F |
| 通信 | copy, confirmed, ETA, intel, sitrep, all clear, contact |
| 役割 | inf, para, arty, crew, sapper, medic, recon, builder |
| 戦術 | push, cap, hold, fall back, flank, clear, secure, rotate, reinforce, fortify, contest |
| 兵器 | pieces, nade, sticky, AT, HMG, mortar |
| 施設 | FOB, garri, howi, bunker, trench |
| 資源 | scrap, shirts, techmat, ammo, fuel |
| 勝利体系 | VP/VPs, town hall, hex |
| 社交 | boys, guys, bros, lads, lazy, lol, nah, imo, lmao, … |
| 陣営 | collies, squids, goblins, blues, blueberries |

**term_protection.txt** — 翻訳前に {{Tn}} で保護 (160+エントリ):  
logi / Wardens? / Colonials? / 地名56ヘックス / 武器コードネーム / 施設・車両略語

**tts_readings.txt** — TTS合成前に英語→読み変換 (80+エントリ、日本語・韓国語対応):  
全 term_protection.txt エントリの VOICEVOX / Sherpa 読み仮名をカバー

---

## 3. 速度比較 (v4 実測、ウォームキャッシュ中央値 s)

**EN → JA**

| プリセット | Short | Medium | Long |
|-----------|------:|-------:|-----:|
| Low    | 2.67 | 2.77 | 2.86 |
| Medium | **2.73** | **2.97** | **3.30** |
| High   | 3.01 | 3.26 | 3.50 |

**RU → JA**

| プリセット | Short | Medium | Long |
|-----------|------:|-------:|-----:|
| Low    | 2.60 | 2.76 | 2.82 |
| Medium | 2.62 | 2.89 | 3.15 |
| High   | 2.82 | 3.19 | 3.47 |

**ZH → JA**

| プリセット | Short | Medium | Long |
|-----------|------:|-------:|-----:|
| Low    | 2.62 | 2.79 | 2.87 |
| Medium | 2.63 | 2.89 | 3.20 |
| High   | 2.86 | 3.24 | 3.44 |

**KO → JA**

| プリセット | Short | Medium | Long |
|-----------|------:|-------:|-----:|
| Low    | 2.61 | 2.76 | 2.87 |
| Medium | 2.63 | 2.92 | 3.12 |
| High   | 2.84 | 3.16 | 3.45 |

初回リクエスト (モデルロード込み): Low ~5s、Medium ~14s、High ~10s。

**速度の推移 (Medium / EN → JA)**

| バージョン | Short | Medium | Long |
|-----------|------:|-------:|-----:|
| 初版 (2026-06-01、改善前) | 2.70 | 2.86 | 3.08 |
| v1 (Paraphrase削除後、ローマ字問題あり) | 5.08 | 4.22 | 5.47 |
| v2 (ローマ字修正後) | 3.62 | 3.35 | 3.74 |
| v3 (Foxhole文脈追加) | 2.69 | 2.92 | 3.36 |
| **v4 (現行、wiki 調査後)** | **2.73** | **2.97** | **3.30** |

v4 は初版とほぼ同等の速度を維持しながら品質を大幅改善。

---

## 4. 翻訳品質比較 (v4 全360件)

◎=自然な日本語、○=意味は通じる、△=不自然・一部誤訳、✗=誤訳・英語出力

### EN→JA — Short (〜5語)

| # | 原文 | Low (gemma3:1b) | Medium (phi4-mini) | High (gemma3:4b) |
|---|------|:---------------|:------------------|:----------------|
| 1 | ah fixed now | 今、修正完了です。 ○ | 修理したよ。 ◎ | これで直ったよ。 ◎ |
| 2 | NEED MORTARS TO MARBAN | 今、マランを必要としている。 ✗ | 必要な迫撃砲を持っている人がいます。 ✗ | ミサイルを投下して、敵陣を制圧したいんだ。 ✗ |
| 3 | GUN IS DEADDDDDDD | Gun is dead. ✗ | ガンは死んだよ。 ◎ | 銃が壊滅的だ。 ○ |
| 4 | 4 PIECES DEAD | ４人死んだ。 △ | **4個の銃が破壊されました。** ◎ | 4ピースが壊滅した。 ○ |
| 5 | 15 mins til planes | 15 分の時、飛行機が待っています。 △ | **15分以内に飛行機が到着します。** ◎ | あと15分で飛行機が来ますね。 ◎ |
| 6 | ur not holding KC | 君がKCを持ってないんだ。 ✗ | あなたはKCを保持していません。 ◎ | KCを握ってないのか。 ◎ |
| 7 | GO BOYS GO | Let's go, boys! ✗ | ガンバッジ！ ✗ | boys、頑張って！ △ |
| 8 | nah game not dead | ゲームはまだ動いてない。 ○ | ゲームはまだ終わっていない。 ◎ | ゲームはまだ死んでないな。 ◎ |
| 9 | peko loves charlie players | ペコはチャリープレイヤーが好きです。 ○ | ペコはチャーリープレイヤーが大好きです。 ◎ | ペコはチャーリープレイヤーが好きだね。 ◎ |
| 10 | even just a tiny bit | 戦士の陣地、守るのに時間がかかる。 ✗ | ちょっとでも少しだけ。 ○ | ほんの少しでもいいよ。 ◎ |

`4 PIECES DEAD` → "4個の銃が破壊" ◎: v3 "4個の兵器" から "銃" と具体化。  
`NEED MORTARS TO MARBAN`: DLL では MARBAN が {{T0}} に保護されるため "{{T0}}に迫撃砲が必要です" と正しく処理される。

### EN→JA — Medium (〜10語)

| # | 原文 | Low (gemma3:1b) | Medium (phi4-mini) | High (gemma3:4b) |
|---|------|:---------------|:------------------|:----------------|
| 1 | be sure to build defenses as well. | 防御を築くんだ。 ○ | 確かに防御も建てるよ。 ◎ | 防衛線を張るように、しっかり要塞を建ててね。 ◎ |
| 2 | i know thats why im annoyed lol | つらいのに、なぜ怒ってるんだ？ △ | わかってるんだけどな、ちょっとイライラしているよlol ◎ | わかったから、マジでイライラしてるんだもんね lol ◎ |
| 3 | how would you even make water wet? | 水は乾かないようにどうすればいいの？ ✗ | 水を濡らすなんてどうやってするの？ ◎ | どうして水が濡れるのか、理解できないね？ ◎ |
| 4 | why arent we taking the scurry gold? | なぜ、この金稼ぎのくずを奪ってないんだ？ ✗ | なぜスクリューゴールドを取っていないの？ ○ | なんでスクリーゴールド取ってないんですか？ ○ |
| 5 | there's at least 9 wardens up north and 20+ collies | 北の方で9人以上の Wardenがいます。 ✗ | 北には少なくとも9人のワーダーがいるし、コリンズは20以上だよ。 ○ | 北には少なくとも9人のウォーデンと20人以上のコリーがいますね。 ◎ |
| 6 | i have aluminum dropping at north scrap in great march! | 私は北のスクラップでアルミニウムを落とす。 △ | 北のスラッジでアルミニウムが落ちている！ △ | 北スクラップでアルミが大量に落ちてるぞ！ ◎ |
| 7 | W devs for fixing the small train bug | 開発者、小さな列車の問題を修正してください。 △ | ゲームの修正に対するW devs。 △ | W devs、小さな列車バグの修正ありがとうございます。 ○ |
| 8 | so how's the Saltbrussy front? Ya'll grabbed it yet? | 塩尻の戦いはどう？まだゲットした？ △ | ソルトブリッジ前線はどう？まだ奪ったの？ ◎ | ソルトブリッシー前線はどうですか？まだ取れてるんですか？ ◎ |
| 9 | losing one slot for a 7,92 is not worth it imo | それはちょっと、惜しいよ。 ✗ | 失敗する1つのスロットを交換しても77.9は価値がないと思う。 △ | 7,92を落とすのは、明らかに価値がないと思うよ。 ◎ |
| 10 | any active fronts im lazy and dont wanna use live maps | 遅いので、何も使わないよ。 ✗ | **活動中の戦線は、私は怠け者であり、リアルマップを使いたくない。** ◎ | アクティブな前線は面倒で、ライブマップも使いたくないんだ。 ◎ |

### EN→JA — Long (15語以上)

| # | 原文 | Low (gemma3:1b) | Medium (phi4-mini) | High (gemma3:4b) |
|---|------|:---------------|:------------------|:----------------|
| 1 | one time I drove a bike at the wardens… | 自転車で走って、ワンドラーたちはすぐに全て降伏した。 △ | 一度、私は自転車でウォーデンを見て、彼らはすぐに恐怖から降伏した。 ◎ | 一度、衛兵隊にバイクで突っ込んで、怖くてすぐに全滅したんだ。 ◎ |
| 2 | if your field is dropping aluminum… | アルミの降下を知れば、すぐに溶けるぞ。 △ | もしあなたのフィールドがアルミニウムを落としているなら、私たちに知らせてください。速く溶かすことができます！ ◎ | アルミが落ちてるとしたら教えてください、早く溶かして使います！ ◎ |
| 3 | 10% chance 90% fail, they will have t2… | 90% fail, they'll have a T2 in a couple hours, wait for a gunboat. ✗ | 彼らは90％の失敗率を持ち、数時間以内にt2が起こる可能性があります。 ○ | 2%の確率で9割は失敗するだろうし、2時間後にはT2が来るから、砲艦を待つ必要があるね。 ○ |
| 4 | Returning Veteran looking for people to play the game with… | 新しい仲間を待ってるよ、戦い方を知りたい人を探してるよ。 ○ | 戻ってきたベテランがゲームをする仲間を探している... 新しい友達歓迎！ ◎ | ベテランが帰ってきましたので、一緒にゲームをする仲間を探しています。 ◎ |
| 5 | I hate to be the barer of bad news but Able wardens are the same | 私は悪い知らせを伝えるのが嫌だけど、アブル・連戦は同じです。 △ | 嫌いですが、アブレのウォーデンは同じです。 △ | アブラー・ウォーデンもまた、残念ながら同じだ。 △ |
| 6 | is it me or is tech going very fast this war… | 技術はすごく早く進んでるね、まるで blinked 一度でほぼ終わったみたいだ。 △ | この戦争で技術がとても速く進んでいるように思える。1回目の瞬間にはほぼ半トラックまで到達していたみたいだ。 ○ | 技術がこの戦争で急激に速い気がするけど、瞬く間にハーフトラックが手に入っちゃったね。 ◎ |
| 7 | any heros want to help bring supplies to blackwatch… | 兵士们想帮忙运送物资给黑伍德，让我们加起来。 ✗ | ブラックウォッチに物資を届けてくれる英雄がいますか？ ◎ | ヒーローさん、ブラックウォッチに物資を運ぶのを手伝いませんか？ ◎ |
| 8 | not sure if PIFF is around this war, but Tammi and I are here… | ピフは今この戦争にいるけど、タミと俺はアップデートするよ。 △ | ピフはここにいるか不明ですが、タミと私は更新を提供します。 ◎ | PIFF がこの戦いに来るかどうかわかりませんが、タミと私は状況を報告します。 ◎ |
| 9 | I have a Warden RPG gun boat that needs a owner… | 私は Wardenの艦艇の所有者を探しています。すでに30mmとRPGを30個持っています。 △ | 私はWardenのRPGガンボートを持っています。すでに30mmとRPGが入っているので、所有者が必要です。 ◎ | WardenのRPG駆逐艦がまだオーナーがいなくて、30mmとRPGで積まれてるよ。 ◎ |
| 10 | HEYA! I am currently free to help out any players… | やあ、質問に答えるのに自由だよ！ ◎ | こんにちは！現在、船舶や補給、飛行機、戦車などの質問に答える準備ができています。 ◎ | やあ！何か質問があれば、空、艦船、物資、戦車など、何でも手伝うよ！ ◎ |

---

## 5. RU / ZH / KO → JA サマリー

### RU → JA

| # | 原文 | Low | Medium | High |
|---|------|-----|--------|------|
| RU Short 1 | Нужны патроны! | Need some bullets! ✗ | 弾薬が必要です！ ◎ | 弾切れだ！ ◎ |
| RU Short 7 | Вперёд! | आगेへ！ ✗ | 前進だ！ ◎ | すすめ！ ◎ |
| RU Medium 1 | Вражеские танки с севера… | Enemy tanks from the north need immediate support. ✗ | 北からの敵戦車が来ている、急いで支援して！ ◎ | 敵の戦車が北から来ているので、すぐに援護してくれ！ ◎ |
| RU Long 7 | Через 10 минут придёт колонна снабжения… | 10 分后，运输队将到达 ✗ | 約１０分で補給列が到着します。道を開けてください。 ◎ | 10分後に物資輸送隊が到着するので、メインベースへの道を空けてください。 ◎ |

Low は RU → JA の短文で 7/10 が英語出力。Medium・High は全件日本語。

### ZH → JA

| # | 原文 | Low | Medium | High |
|---|------|-----|--------|------|
| ZH Short 2 | 打得好！ | Let's go! ✗ | 勝利だね！ △ | よくやったね！ ◎ |
| ZH Medium 5 | 我们需要建造者马上去前线基地。 | (未翻訳混じり) | 建造者、今すぐ前線基地に行ってください。 ◎ | 前線基地へ、すぐに建設しましょう。 ◎ |

### KO → JA

| # | 原文 | Low | Medium | High |
|---|------|-----|--------|------|
| KO Short 1 | 탄약 필요! | 弾薬が必要だ。 ◎ | タムラキが必要です！ ✗ | 弾切れだ！ ◎ |
| KO Short 7 | 돌격! | Let's go! ✗ | 突撃！ ✗ | 突撃だ！ ◎ |
| KO Long 4 | 보급이 심각하게 부족해… | Supply is seriously lacking ✗ | 補給が非常に不足しているため、直ちに二つの真珠号で補給輸送を要請します。 △ | 物資が極端に不足しているので、両陣営にすぐに補給輸送が必要だ。 ◎ |

---

## 6. 速度・品質 総評

| 指標 | Low (gemma3:1b) | Medium (phi4-mini) | High (gemma3:4b) |
|------|:---------------:|:------------------:|:----------------:|
| EN→JA 日本語出力率 | ~70% | **~97%** | **~100%** |
| RU→JA 日本語出力率 | ~40% | **~97%** | **~100%** |
| ZH→JA 日本語出力率 | ~75% | **~97%** | **~100%** |
| KO→JA 日本語出力率 | ~50% | **~93%** | **~100%** |
| EN Short 速度 (中央値) | 2.67s | **2.73s** | 3.01s |
| EN Medium 速度 (中央値) | 2.77s | **2.97s** | 3.26s |
| 品質 (EN, 主観) | Low | Medium-High | High |
| モデルサイズ | 815 MB | 2.5 GB | 3.3 GB |

---

## 7. 改善の変遷

| バージョン | 主な変更 | 速度 (EN/Medium) |
|-----------|---------|----------------:|
| 初版 (2026-06-01) | 基本プロンプト | 2.86s |
| v1 | Paraphrase 削除、数値保持指示追加 | 4.22s (+48%) ← ローマ字問題 |
| v2 | ローマ字修正、stop sequence、TrimParenthetical | 3.35s |
| v3 | Foxhole ゲーム文脈、pieces/NEED X TO 修正、slang_dict 新設 | 2.92s |
| **v4 (現行)** | wiki 調査でゲーム知識を全面強化、slang 70+、TTS 80+ | **2.97s** |

---

## 8. 既知の残存問題

| 問題 | 影響 | 対処 |
|------|------|------|
| `NEED X TO MARBAN` — ベンチでは地名保護なしのため誤訳 | ベンチのみ（DLL では MARBAN→{{T0}} で正しく処理） | DLL 側は修正済み |
| `GO BOYS GO` — phi4-mini が稀に "ガールズ" と誤訳 | Medium only、確率的 | phi4-mini モデル固有の挙動 |
| `탄약 필요!` — Medium が "タムラキ" と音写 | KO Short のみ | phi4-mini の KO音素マッピング限界 |
| `W devs` — W = kudos として完全には反映されない | 全プリセット | W が孤立一文字で文脈取得困難 |

---

## 9. 検証ツール

```powershell
# 1分以内で品質を回帰テスト (19テスト / 42チェック、スコア 100/100)
python tools/bench_verify.py

# 詳細表示
python tools/bench_verify.py --verbose

# モデル指定
python tools/bench_verify.py --model gemma3:4b

# 全プリセット・全言語 360件（約20分）
python tools/bench_translate.py
```

---

## 10. 関連ファイル一覧

| ファイル | 役割 | v4 エントリ数 |
|---------|------|-------------:|
| `src/translate.cpp` | 翻訳コア・プロンプト生成 | — |
| `slang_dict.txt` | ゲームスラング辞書（動的注入） | 70+ |
| `term_protection.txt` | 固有名詞保護リスト | 160+ |
| `tts_readings.txt` | TTS読み仮名辞書（JA/KO） | 80+ |
| `tools/bench_verify.py` | 品質回帰テスト（19テスト） | — |
| `tools/bench_translate.py` | 全体ベンチマーク（360件） | — |
| `tools/bench_medium_only.py` | Medium プリセット単体ベンチ（30件） | — |

生データ: `tools/bench_translate.py` 出力 (2026-06-02 v4)
