# Ollama ベンチマーク結果

最終更新: 2026-06-01

---

## 1. 実行環境

| 項目 | 値 |
|------|-----|
| CPU | AMD Ryzen 9 5900HS (8コア/16スレッド) |
| RAM | 15.4 GB |
| GPU | NVIDIA GeForce RTX 3050 Ti Laptop GPU (4GB VRAM) |
| OS | Windows 11 |
| Ollama | 0.24.0 (CUDA v12) |
| ベンチツール | `tools/bench_translate.py` (2026-06-01) |

---

## 2. プリセット定義

| プリセット | モデル | サイズ | num_ctx | num_thread | temperature | num_predict |
|-----------|--------|-------:|--------:|-----------:|------------:|------------:|
| **Low** | gemma3:1b | 815 MB | 128 | 2 | 0.1 | 120 |
| **Medium** | phi4-mini | 2.5 GB | 512 | 0 (全コア) | 0.1 | 120 |
| **High** | gemma3:4b | 3.3 GB | 512 | 0 (全コア) | 0.1 | 120 |

**翻訳プロンプト (JA モード):**

```
[system] You are a Foxhole war-game chat translator.
         Keep these game-specific terms exactly as-is: <マッチした保護語>.
[prompt] Translate the following war-game chat into natural, casual Japanese.
         Be concise — one short sentence. Paraphrase freely; keep key meaning.
         Output ONLY the translated text.
```

system フィールドは保護語がゼロでも常時設定する。保護語がない場合は 2 文目を省略。

---

## 3. 速度比較

各言語 × 入力長 10 サンプルの中央値 (s)。ウォームキャッシュ (初回モデルロードを除外)。  
Short: 短文 (〜5 語)、Medium: 通常文 (〜10 語)、Long: 長文 (15 語以上)。

**EN → JA**

| プリセット | Short | Medium | Long |
|-----------|------:|-------:|-----:|
| Low    | 2.67 | 2.71 | 2.76 |
| Medium | 2.70 | 2.86 | 3.08 |
| High   | 2.90 | 3.08 | 3.34 |

**RU → JA**

| プリセット | Short | Medium | Long |
|-----------|------:|-------:|-----:|
| Low    | 2.61 | 2.71 | 2.78 |
| Medium | 2.61 | 2.78 | 3.09 |
| High   | 2.83 | 3.04 | 3.28 |

**ZH → JA**

| プリセット | Short | Medium | Long |
|-----------|------:|-------:|-----:|
| Low    | 2.63 | 2.73 | 2.77 |
| Medium | 2.63 | 2.88 | 3.10 |
| High   | 2.87 | 3.07 | 3.30 |

**KO → JA**

| プリセット | Short | Medium | Long |
|-----------|------:|-------:|-----:|
| Low    | 2.63 | 2.72 | 2.76 |
| Medium | 2.66 | 2.82 | 3.01 |
| High   | 2.84 | 3.03 | 3.28 |

初回リクエスト (モデルロード込み): Low ~4.2 s、Medium ~9.7 s、High ~8.3 s。

---

## 4. 翻訳品質比較

◎=自然な日本語、○=意味は通じる、△=不自然・一部誤訳、✗=誤訳・英語出力

例文は実際の translation_log.csv (2026-06-01) から採取したゲーム内チャットログ。

### EN→JA — Short (〜5語)

| # | 原文 | Low (gemma3:1b) | Medium (phi4-mini) | High (gemma3:4b) |
|---|------|:---------------|:------------------|:----------------|
| 1 | ah fixed now | Fix it, please. ✗ | 今、修理しました。 ○ | 問題ないよ。 ○ |
| 2 | NEED MORTARS TO MARBAN | Let's go, Mortars! ✗ | 必要な迫撃砲を持っているか確認する。 △ | ミサイルを投げて、敵陣を制圧したい。 ✗ |
| 3 | GUN IS DEADDDDDDD | Gun's gone, man! ✗ | ガンは死んだよ！ ◎ | 砲撃が切れた！ ◎ |
| 4 | 4 PIECES DEAD | やあ、みんな、4人の仲間が死んだ。 ○ | 死体が4つ出てきた。 △ | 四体死んだ。 ◎ |
| 5 | 15 mins til planes | 飛行機、15分だよ。 ◎ | 飛行機が出発準備完了です。 △ | 飛行機が15分で来るよ。 ◎ |
| 6 | ur not holding KC | You're not holding any crazy stuff, okay? ✗ | KCを保持していないよ。 ◎ | KCをキープしてないな。 ◎ |
| 7 | GO BOYS GO | Let's go, boys, let's go! ✗ | ガンマンになれ！ ✗ | レッツ行こう！ △ |
| 8 | nah game not dead | Game still running, no, not dead. ✗ | ゲームはまだ終わっていないよ。 ◎ | まだゲームは動いてるよ。 ◎ |
| 9 | peko loves charlie players | ペコはチャリープレイヤーが好きだよ。 ◎ | ペコがチャーリーのプレイヤーを好きだよ。 ◎ | ピーコはチャーリープレイヤーが好きだね。 ○ |
| 10 | even just a tiny bit | Please provide the chat you'd like me to translate! ✗ | 「ほんの少しでも」 ◎ | ちょっとでもいいよ。 ◎ |

Low は 10 件中 7 件で英語出力。文脈のない短文 ("even just a tiny bit") ではプロンプトそのものを返すケースも。Medium・High は概ね安定するが、"GO BOYS GO" は全プリセットで誤訳傾向 (Medium: 意味不明、High: カタカナ英語混在)。"NEED MORTARS TO MARBAN" は地名 MARBAN を全プリセットが固有名詞と認識できず誤訳 → `term_protection.txt` での保護を推奨。

### EN→JA — Medium (〜10語)

| # | 原文 | Low (gemma3:1b) | Medium (phi4-mini) | High (gemma3:4b) |
|---|------|:---------------|:------------------|:----------------|
| 1 | be sure to build defenses as well. | しっかり防衛線を築きなさい。 ◎ | 防御も確実に構築してください。 ◎ | 陣地をしっかり築いてね。 ◎ |
| 2 | i know thats why im annoyed lol | マジで、 annoyed なのよ。 △ | わかってるんだけど、それでイライラしているよ。 ◎ | マジで、それ知ってるからイライラしてるんだよ。 ◎ |
| 3 | how would you even make water wet? | どうやって水に水をつけられるの？ ○ | 水を濡らすのはどうやってするんだろう？ ◎ | マジで、水はどうやって濡らすの？ ◎ |
| 4 | why arent we taking the scurry gold? | どうして金稼ぎのゴミを置いてないの？ ✗ | なぜスクリューロイヤルゴールドを取っていないの？ △ | 金は取れないの？ ○ |
| 5 | there's at least 9 wardens up north and 20+ collies | There are a lot of guards up north, and a bunch of dogs too. ✗ | 北には少なくとも９人のウォードナーがいて、２０以上のコリーもいる。 ◎ | 北には少なくとも9人の警備兵と、20人以上いるっぽい。 ○ |
| 6 | i have aluminum dropping at north scrap in great march! | Aluminum's falling in the north scrap in the march. ✗ | 北部のグレートマーチでアルミニウムが落ちているよ！ ◎ | アルミが北スクラップで大行進中！ △ |
| 7 | W devs for fixing the small train bug | Train bug fix is coming soon. ✗ | 小さな列車のバグを修正するために開発者たちがいるよ。 △ | バグの小さな電車を直してくれ、開発さん。 △ |
| 8 | so how's the Saltbrussy front? Ya'll grabbed it yet? | Saltbrussy is still a mess, everyone's got it. ✗ | サルトブリッジの前線はどう？まだ奪取した？ ◎ | ソルトブリッシー前線はどう？まだ取れてる？ ◎ |
| 9 | losing one slot for a 7,92 is not worth it imo | ちょっと、7,92の失ったスロットは、あんまり価値がないと思うんだ。 ◎ | スロットを1つ失っても、7.92mmは価値がないと思う。 ◎ | 7,92で1スロットを落とすのは、価値がないと思う。 ◎ |
| 10 | any active fronts im lazy and dont wanna use live maps | やたらと活動しないから、マップを使わないよ。 ✗ | アクティブな戦線があるけど、気分が悪くてリアルタイムの地図を使わないんだ。 △ | 戦闘ラインに疲れた、マップも使いたくない。 ◎ |

Low は 10 件中 4 件で英語出力。特にゲーム固有名詞 (wardens, collies, Saltbrussy) を含む文で失敗。Medium は全件日本語出力、地名・固有名詞の保持も良好。High は "i have aluminum dropping at north scrap in **great march**!" を "大行進中" と誤訳するなど地名の動詞化ミスが発生。"W devs" (= いい開発者) の意味は全プリセット未捕捉。

### EN→JA — Long (15語以上)

| # | 原文 | Low (gemma3:1b) | Medium (phi4-mini) | High (gemma3:4b) |
|---|------|:---------------|:------------------|:----------------|
| 1 | one time I drove a bike at the wardens and they just all surrendered immediately out of sheer fear | えー、Wardenって、Bikesでぶっ壊して、すぐに降りました。 △ | 一度、警備員に自転車で突っ込んだら全員がただ怖さから降伏した。 ◎ | 一度、警備兵に自転車で突っ込んで、怖くてすぐに降参させたよ。 ◎ |
| 2 | if your field is dropping aluminum let us know so we can melt it down quick! | アルミ片落とし、早く溶けたら教えてね！ △ | もしフィールドにアルミニウムが落ちているなら教えてください。急いで溶かしましょう！ ◎ | アルミが落ちてたら教えて！早く溶かしてね！ ◎ |
| 3 | 10% chance 90% fail, they will have t2 in a couple hours, have to wait for gunboat | "Gotta wait, they're gonna have T2 in a couple hours..." ✗ | 約1割のチャンスで、残りは失敗する。数時間以内にt2が来るので、待つしかない。 ○ | ほぼ確実に失敗するけど、2時間ほどでT2が来るから、ガントリー待ちになるね。 ◎ |
| 4 | Returning Veteran looking for people to play the game with .. new bros welcome to hang out too | Let's play some Foxhole, new friends, welcome! ✗ | 復帰兵士がゲームをする仲間を探している。新しい友達も歓迎！ ◎ | ベテランのやつが、一緒に遊んでくれる人を探してるよ、新規さんも大歓迎！ ◎ |
| 5 | I hate to be the barer of bad news but Able wardens are the same | "I'm really bummed about the bad news, but the Wardens are awful." ✗ | いいなで、残念ながらアブレのウォーデンは同じだよ。 △ | アビリティの警備隊も同じく、残念です。 △ |
| 6 | is it me or is tech going very fast this war. i feel like i blinked once and were almost to having halftracks | 技術がめっちゃくちゃになったね、まるで速攻で終わるみたいだ。 △ | 技術が本当に速く進んでいるように思える。ほぼ半トラックまで到達したかのようだった。 ◎ | 技術が急に進んでる気がするけど、気づけば半トラックも出てきちゃってる。 ◎ |
| 7 | any heros want to help bring supplies to blackwatch so we can build it up? | みんな、Blackwatchに物資を届けて、勢力を増やすのに協力してくれ。 ○ | ヘロイズ、ブラックウォッチに物資を届けてくれる人がいますか？建設のために。 ◎ | 誰かヒーローがブラックウォッチに物資を運ぶのを手伝ってくれない？ ◎ |
| 8 | not sure if PIFF is around this war, but Tammi and I are here to give updates | Tammi and I are here to tell you about the game. ✗ | PIFF ここにいるか不明ですが、タミと私は状況の更新をします。 ◎ | PIFF がいるかどうかわからないけど、タミと私は状況を報告します。 ◎ |
| 9 | I have a Warden RPG gun boat that needs a owner. Already stocked with 30mm and RPGs | 俺の Warden 船にオーナーを探してるんだ。30mmとRPGが揃ってるよ。 ◎ | 私のウォーデンRPGガンボートは所有者が必要です。既に30mmとロケット推進弾を装備しています。 ◎ | 要人待ちのウォーデンRPGのボートで、30mmとRPGが積まれてるよ。 ○ |
| 10 | HEYA! I am currently free to help out any players with questions regarding anything from, naval, logi, planes, tanks, and so on! | やあ、今、どんな質問でも聞いてるよ！ ◎ | こんにちは！現在、艦隊、物流、飛行機、戦車などの質問に対応する準備ができています。どうぞお気軽に聞いてくださいね！ ◎ | 何か質問があれば、いつでも手伝うよ！ ◎ |

Low は 10 件中 4 件で英語出力。Medium・High は全件日本語で品質も良好。"Able wardens" (固有チーム名) は全プリセット共通で誤解釈 → 保護語登録推奨。"gunboat" の訳 (Medium: 省略、High: "ガントリー") はいずれも不正確だが文全体の意味は伝わる。

---

生データ: `tools/bench_result_new.txt`
