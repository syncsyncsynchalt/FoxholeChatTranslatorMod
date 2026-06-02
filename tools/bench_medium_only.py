#!/usr/bin/env python3
"""Medium プリセット (phi4-mini) のみ EN->JA ベンチマーク"""
import json, re, time, statistics, sys, io, urllib.request

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")

OLLAMA_URL = "http://localhost:11434/api/generate"
SYSTEM_BASE = (
    "You are a Foxhole war-game chat translator."
    " Foxhole is a massively multiplayer persistent war game"
    " with two factions: Wardens and Colonials."
)
PROMPT = (
    "Translate the following war-game chat message into natural, casual Japanese."
    " Output ONE short sentence only."
    " Preserve numbers and times (e.g. '15 mins') exactly."
    " Output ONLY the Japanese translation — no romanization, no explanations."
)
STOP = ["\n\n", " (", "Note:", "Translation:", "Here's", "Here is"]
SLANG = {
    "W":       "kudos/well done",
    "GG":      "good game/well played",
    "logi":    "logistics/supply (not login)",
    "inf":     "infantry",
    "lazy":    "too lazy (not sick or ill)",
    "nah":     "no",
    "gonna":   "going to",
    "wanna":   "want to",
    "gotta":   "got to/have to",
    "imo":     "in my opinion",
    "lol":     "laughing",
    "push":    "attack/advance",
    "cap":     "capture",
    "collies": "Colonials (faction)",
    "squids":  "Colonials (faction)",
    "goblins": "Wardens (faction)",
}

TESTS = {
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
}

def build_sys(text):
    matched = [f"'{t}' means {m}" for t, m in SLANG.items()
               if re.search(r'\b' + re.escape(t) + r'\b', text, re.IGNORECASE)]
    s = SYSTEM_BASE
    if matched:
        s += " Note: " + "; ".join(matched) + "."
    return s

def is_jp(text):
    return any(0x3040 <= ord(c) <= 0x30FF or 0x4E00 <= ord(c) <= 0x9FFF for c in text)

def post(text):
    payload = {
        "model": "phi4-mini:latest",
        "prompt": PROMPT + "\n\n" + text,
        "system": build_sys(text),
        "stream": False,
        "stop": STOP,
        "options": {"num_ctx": 512, "num_predict": 120, "temperature": 0.1},
    }
    data = json.dumps(payload).encode()
    req = urllib.request.Request(OLLAMA_URL, data, {"Content-Type": "application/json"})
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=90) as r:
            body = json.loads(r.read())
    except Exception as e:
        print(f"  ERROR: {e}", flush=True)
        return None, 0.0
    return body.get("response", "").strip(), time.perf_counter() - t0

LENGTHS = ["Short", "Medium", "Long"]
results = {l: [] for l in LENGTHS}
total = sum(len(TESTS[l]) for l in LENGTHS)
done = 0

for length in LENGTHS:
    for src in TESTS[length]:
        done += 1
        cold = " [cold]" if done == 1 else ""
        dst, elapsed = post(src)
        if dst is None:
            dst = "(ERROR)"
            elapsed = 0.0
        results[length].append((src, dst, elapsed))
        jp = "✓" if is_jp(dst) else "✗"
        print(f"[{done:2d}/{total}] {length} {elapsed:.2f}s {jp}{cold}", flush=True)
        print(f"  src: {src[:70]}", flush=True)
        print(f"  dst: {dst[:70]}", flush=True)

print("\n" + "=" * 60, flush=True)
print("Medium (phi4-mini) EN→JA 結果", flush=True)
print("=" * 60, flush=True)

for length in LENGTHS:
    print(f"\n### {length}\n", flush=True)
    print("| # | 原文 | 訳文 | JP? |", flush=True)
    print("|---|------|------|-----|", flush=True)
    for i, (src, dst, _) in enumerate(results[length]):
        mark = "◎" if is_jp(dst) else "✗"
        print(f"| {i+1} | {src[:45]} | {dst[:45]} | {mark} |", flush=True)

print("\n### 速度 (中央値)\n", flush=True)
print("| 長さ | 中央値 (s) |", flush=True)
print("|------|----------:|", flush=True)
for length in LENGTHS:
    times = [e for _, _, e in results[length]]
    print(f"| {length} | {statistics.median(times):.2f} |", flush=True)
