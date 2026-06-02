#!/usr/bin/env python3
"""
Foxhole翻訳品質ベンチマーク
言語 (EN/RU/ZH/KO) × 長さ (Short/Medium/Long) × 10サンプル × プリセット3種
"""

import json
import re
import time
import statistics
import sys
import io
import urllib.request

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")

OLLAMA_URL = "http://localhost:11434/api/generate"
SYSTEM_PROMPT_BASE = (
    "You are a Foxhole war-game chat translator."
    " Foxhole is a massively multiplayer persistent war game"
    " with two factions: Wardens and Colonials."
)
TRANSLATE_PROMPT = (
    "Translate the following war-game chat message into natural, casual Japanese."
    " Output ONE short sentence only."
    " Preserve numbers and times (e.g. '15 mins') exactly."
    " Output ONLY the Japanese translation — no romanization, no explanations."
)
STOP_SEQUENCES = ["\n\n", " (", "Note:", "Translation:", "Here's", "Here is"]

# slang_dict.txt と同等の辞書 (per-message マッチング用)
SLANG_DICT = {
    "W":          "expression of praise before a noun (e.g. 'W devs' = kudos to the devs; translate as praising whoever follows)",
    "GG":         "good game/well played",
    "F":          "paying respects (informal acknowledgment of a loss)",
    "logi":       "補給 (Foxhole supply delivery; always translate logi as 補給 in Japanese, never as リソース or ログイン)",
    "logi run":   "補給任務 (Foxhole supply delivery mission; translate as 補給任務 in Japanese)",
    "inf":        "infantry",
    "para":       "paratrooper",
    "arty":       "artillery",
    "lazy":       "too lazy (not sick or ill)",
    "nah":        "no",
    "gonna":      "going to",
    "wanna":      "want to",
    "gotta":      "got to/have to",
    "imo":        "in my opinion",
    "tbh":        "to be honest",
    "rn":         "right now",
    "atm":        "at the moment",
    "lmao":       "laughing hard",
    "lol":        "laughing",
    "ngl":        "not going to lie",
    "push":       "attack/advance toward the enemy",
    "cap":        "capture (seize an objective)",
    "hold":       "defend and not retreat",
    "rush":       "attack quickly without preparation",
    "pieces":     "military equipment or vehicles (artillery, tanks; NOT people — '4 pieces dead' = 4 guns/vehicles destroyed)",
    "boys":       "teammates (gender-neutral; 'GO BOYS GO' = rallying cry to the team)",
    "guys":       "teammates (gender-neutral)",
    "bros":       "teammates",
    "collies":    "Colonials (faction)",
    "squids":     "Colonials (faction)",
    "goblins":    "Wardens (faction)",
    "blues":      "Wardens (faction)",
    "blueberries":"Wardens (faction)",
}

def build_system_prompt(text):
    matched = []
    for term, meaning in SLANG_DICT.items():
        if re.search(r'\b' + re.escape(term) + r'\b', text, re.IGNORECASE):
            matched.append(f"'{term}' means {meaning}")
    s = SYSTEM_PROMPT_BASE
    if matched:
        s += " Note: " + "; ".join(matched) + "."
    return s

PRESETS = [
    ("Low",    "gemma3:1b",        128, 2),
    ("Medium", "phi4-mini:latest", 512, 0),
    ("High",   "gemma3:4b",        512, 0),
]

TESTS = {
    "EN": {
        # 実際の translation_log.csv から採取したゲーム内チャット (2026-06-01)
        "Short": [
            "ah fixed now",
            "NEED MORTARS TO MARBAN",
            "GUN IS DEADDDDDDD",
            "4 PIECES DEAD",
            "15 mins til planes",
            "ur not holding KC",
            "GO BOYS GO",
            "nah game not dead",
            "peko loves charlie players",
            "even just a tiny bit",
        ],
        "Medium": [
            "be sure to build defenses as well.",
            "i know thats why im annoyed lol",
            "how would you even make water wet?",
            "why arent we taking the scurry gold?",
            "there's at least 9 wardens up north and 20+ collies",
            "i have aluminum dropping at north scrap in great march!",
            "W devs for fixing the small train bug",
            "so how's the Saltbrussy front? Ya'll grabbed it yet?",
            "losing one slot for a 7,92 is not worth it imo",
            "any active fronts im lazy and dont wanna use live maps",
        ],
        "Long": [
            "one time I drove a bike at the wardens and they just all surrendered immediately out of sheer fear",
            "if your field is dropping aluminum let us know so we can melt it down quick!",
            "10% chance 90% fail, they will have t2 in a couple hours, have to wait for gunboat",
            "Returning Veteran looking for people to play the game with .. new bros welcome to hang out too",
            "I hate to be the barer of bad news but Able wardens are the same",
            "is it me or is tech going very fast this war. i feel like i blinked once and were almost to having halftracks",
            "any heros want to help bring supplies to blackwatch so we can build it up?",
            "not sure if PIFF is around this war, but Tammi and I are here to give updates",
            "I have a Warden RPG gun boat that needs a owner. Already stocked with 30mm and RPGs",
            "HEYA! I am currently free to help out any players with questions regarding anything from, naval, logi, planes, tanks, and so on!",
        ],
    },
    "RU": {
        "Short": [
            "Нужны патроны!",
            "Хорошая игра!",
            "Враг у ворот!",
            "Отступаем!",
            "Смотри фланги!",
            "Лоджи здесь.",
            "Вперёд!",
            "Нужен медик!",
            "Держи позицию!",
            "Молодцы, всем.",
        ],
        "Medium": [
            "Вражеские танки с севера, нужна немедленная поддержка!",
            "Запасы оружия и материалов на исходе, нужно пополнение.",
            "Держите позицию, не давайте им перейти мост!",
            "Все водители лоджи срочно на основную базу.",
            "Нам нужны строители на форпосте прямо сейчас.",
            "В лесу замечена вражеская пехота, около 10 человек.",
            "Кто может привезти грузовик на базу альфа?",
            "Ратуша под сильным ударом, нужна помощь!",
            "Давим на восток, прикройте огнём.",
            "Есть лишние стройматериалы? Строим бункер здесь.",
        ],
        "Long": [
            "Нам нужны три противотанковых орудия и два полевых орудия на передовой срочно.",
            "Осторожно, на холме снайпер, он уже двадцать минут сидит на одном месте.",
            "Кто-нибудь может меня подобрать? Застрял на пограничной базе без машины.",
            "Запасы критически низкие, нам срочно нужны рейсы лоджи к обоим лисьим норам.",
            "Враг сильно давит на правый фланг, нужна вся доступная пехота для подкрепления.",
            "Гарнизон вот-вот падёт, запрашиваю немедленное подкрепление и снаряжение.",
            "Через 10 минут придёт колонна снабжения, расчистите дорогу к основной базе.",
            "Строю большой бункер на северном гексе, приходите помогать, строители.",
            "Все варденцы давите на север, пока колониальные отвлечены на южном фронте.",
            "Потеряли ратушу, отступаем ко вторичной линии обороны у моста.",
        ],
    },
    "ZH": {
        "Short": [
            "需要弹药！",
            "打得好！",
            "敌人到了！",
            "撤退！",
            "注意侧翼！",
            "补给到了。",
            "冲！",
            "需要医疗！",
            "守住阵地！",
            "大家辛苦了。",
        ],
        "Medium": [
            "敌方坦克从北面推进，需要立即支援！",
            "步枪和建材库存告急，请求补给。",
            "守住阵地，不要让他们过桥！",
            "所有补给车司机立即返回主基地。",
            "我们需要建造者马上去前线基地。",
            "在附近森林发现敌方步兵，约10人。",
            "有人能把卡车开到阿尔法基地吗？",
            "市政厅正在遭受猛烈攻击，需要支援！",
            "我们在向东推进，请提供火力掩护。",
            "有多余的建材吗？我们在这里建地堡。",
        ],
        "Long": [
            "我们需要在前线立即部署三门反坦克炮和两门野战炮。",
            "注意山上的狙击手，他已经在那个位置潜伏了二十分钟。",
            "有人能来接我吗？我被困在边境基地，没有卡车。",
            "库存严重不足，我们需要立即向两个狐穴运送补给。",
            "敌人正在猛烈攻击我们的右翼，需要所有可用步兵增援。",
            "驻军即将失守，请求立即增援和补给。",
            "十分钟后补给车队到来，请清理通往主基地的道路。",
            "我正在北部六边形建造大型地堡基地，建造者们来帮忙。",
            "所有督卫军向北推进，趁殖民军在南线被牵制时行动。",
            "我们失去了市政厅，正在撤退到桥边的次级防线。",
        ],
    },
    "KO": {
        "Short": [
            "탄약 필요!",
            "잘했어요!",
            "적이 왔어!",
            "후퇴!",
            "측면 주시!",
            "보급 왔어.",
            "돌격!",
            "의무병 필요!",
            "진지 사수!",
            "수고했어요.",
        ],
        "Medium": [
            "북쪽에서 적 탱크가 밀려오고 있어, 즉각 지원이 필요해!",
            "소총과 자재가 부족해, 보급이 필요해.",
            "진지를 지켜, 다리를 건너게 하면 안 돼!",
            "모든 보급차 운전수는 즉시 본진으로 돌아와.",
            "전방 기지에 건설자들이 지금 당장 필요해.",
            "근처 숲에서 적 보병 발견, 약 10명.",
            "누가 알파 기지로 트럭 가져다 줄 수 있어?",
            "시청이 맹공격을 받고 있어, 도움이 필요해!",
            "동쪽으로 밀고 있어, 화력 지원해줘.",
            "여분 건설재 있어? 여기서 벙커 짓고 있어.",
        ],
        "Long": [
            "전선에 AT포 3문과 야전포 2문이 즉시 필요합니다.",
            "언덕 위의 저격수를 조심해, 그는 20분째 거기서 잠복하고 있어.",
            "누가 나 좀 데려다 줄 수 있어? 트럭 없이 국경 기지에 갇혀 있어.",
            "보급이 심각하게 부족해, 두 참호로 즉시 보급 운송이 필요해.",
            "적이 오른쪽 측면을 강하게 밀고 있어, 가용한 모든 보병이 증원해야 해.",
            "수비대가 곧 함락될 것 같아, 즉각적인 지원과 보급을 요청해.",
            "10분 후 보급 차량이 오니, 본진 가는 길 정리해 줘.",
            "북쪽 헥스에 대형 벙커 기지 짓고 있어, 건설자들 와서 도와줘.",
            "모든 수호자들은 식민지군이 남쪽 전선에서 분산된 동안 북쪽을 밀어.",
            "시청을 잃었어, 다리 근처 2차 방어선으로 후퇴하고 있어.",
        ],
    },
}

LANGS   = ["EN", "RU", "ZH", "KO"]
LENGTHS = ["Short", "Medium", "Long"]


def is_japanese(text):
    for c in text:
        cp = ord(c)
        if 0x3040 <= cp <= 0x309F or 0x30A0 <= cp <= 0x30FF:
            return True
    return False


def post(model, num_ctx, num_thread, text):
    payload = {
        "model":  model,
        "prompt": TRANSLATE_PROMPT + "\n\n" + text,
        "system": build_system_prompt(text),
        "stream": False,
        "stop":   STOP_SEQUENCES,
        "options": {
            "num_ctx":     num_ctx,
            "num_predict": 120,
            "temperature": 0.1,
        },
    }
    if num_thread:
        payload["options"]["num_thread"] = num_thread
    data = json.dumps(payload).encode()
    req  = urllib.request.Request(OLLAMA_URL, data=data,
                                  headers={"Content-Type": "application/json"})
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=90) as resp:
            body = json.loads(resp.read())
    except Exception as e:
        return None, 0.0
    return body.get("response", "").strip(), time.perf_counter() - t0


def run():
    # results[preset][lang][length] = [(src, dst, elapsed), ...]
    results = {
        p[0]: {lang: {ln: [] for ln in LENGTHS} for lang in LANGS}
        for p in PRESETS
    }

    total = len(PRESETS) * len(LANGS) * len(LENGTHS) * 10
    done  = 0
    for pi, (preset_label, model, num_ctx, num_thread) in enumerate(PRESETS):
        first = True
        for lang in LANGS:
            for length in LENGTHS:
                for i, src in enumerate(TESTS[lang][length]):
                    dst, elapsed = post(model, num_ctx, num_thread, src)
                    done += 1
                    cold_tag = " [cold]" if first else ""
                    first = False
                    if dst is None:
                        dst = "(ERROR)"
                        elapsed = 0.0
                    results[preset_label][lang][length].append((src, dst, elapsed))
                    jp = "✓" if is_japanese(dst) else "✗"
                    print(f"[{done:3d}/{total}] {preset_label}/{lang}/{length} {elapsed:.2f}s {jp}{cold_tag}")
                    print(f"  src: {src[:60]}")
                    print(f"  dst: {dst[:60]}")

    # ── Markdown 出力 ──────────────────────────────────────────
    print("\n\n" + "="*60)
    print("MARKDOWN OUTPUT")
    print("="*60)

    for lang in LANGS:
        print(f"\n### {lang} → 日本語\n")
        for length in LENGTHS:
            print(f"#### {length}\n")
            # ヘッダー
            print("| # | 原文 | Low | Medium | High |")
            print("|---|------|-----|--------|------|")
            for i in range(10):
                src = TESTS[lang][length][i]
                row = f"| {i+1} | {src} |"
                for preset_label, *_ in PRESETS:
                    _, dst, _ = results[preset_label][lang][length][i]
                    mark = "" if is_japanese(dst) else " ✗"
                    row += f" {dst}{mark} |"
                print(row)
            print()

        # 速度サマリー (Cold除外)
        print(f"#### 速度 (中央値 s、ウォームキャッシュ)\n")
        print("| プリセット | Short | Medium | Long |")
        print("|-----------|------:|-------:|-----:|")
        for preset_label, *_ in PRESETS:
            row = f"| {preset_label} |"
            for length in LENGTHS:
                times = [e for _, _, e in results[preset_label][lang][length]]
                # 最初の言語ENの最初のサンプルはコールドなので除外
                if lang == "EN" and PRESETS[0][0] == preset_label:
                    times = times[1:]  # rough
                med = statistics.median(times) if times else 0
                row += f" {med:.2f} |"
            print(row)
        print()


if __name__ == "__main__":
    run()
