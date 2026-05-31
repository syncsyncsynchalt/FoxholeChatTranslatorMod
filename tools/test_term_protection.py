#!/usr/bin/env python3
"""
term_protection.txt のパターン検証 + Ollama 翻訳テスト
C++ の InitTermRegexes / ProtectTerms / RestoreTerms を Python で再現
"""

import re
import json
import sys
import io
import urllib.request
import os

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")

OLLAMA_URL = "http://localhost:11434/api/generate"
MODEL      = "phi4-mini:latest"
NUM_CTX    = 512

SYSTEM_PROMPT = "You are a Foxhole war-game chat translator."
TRANSLATE_PROMPT = (
    "Translate the following war-game chat into natural, casual Japanese."
    " Be concise — one short sentence. Paraphrase freely; keep key meaning."
    " Output ONLY the translated text."
)

# --------------------------------------------------------
# term_protection.txt パーサ (C++ の InitTermRegexes 相当)
# --------------------------------------------------------
def load_term_regexes(path):
    regexes = []
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\r\n").rstrip()
            if not line or line.startswith("#"):
                continue
            icase = False
            if line.endswith(" i"):
                line = line[:-2]
                icase = True
            # \b を自動付与 (既に \b がある場合はスキップ)
            if r"\b" not in line:
                pat = r"\b" + line + r"\b"
            else:
                pat = line
            flags = re.IGNORECASE if icase else 0
            try:
                regexes.append((re.compile(pat, flags), icase))
            except re.error as e:
                print(f"  [WARN] regex error '{line}': {e}")
    return regexes

def protect_terms(text, regexes):
    """マッチした用語を {{T0}}, {{T1}}... に置換し、置換マップを返す"""
    spans = []
    for (rx, _) in regexes:
        for m in rx.finditer(text):
            spans.append((m.start(), m.end(), m.group()))
    if not spans:
        return text, []
    # 開始位置でソート、重複除去
    spans.sort(key=lambda x: x[0])
    deduped = []
    last_end = 0
    for s, e, orig in spans:
        if s >= last_end:
            deduped.append((s, e, orig))
            last_end = e
    # 後ろから置換
    replacements = []
    result = text
    for s, e, orig in reversed(deduped):
        ph = "{{T%d}}" % len(replacements)
        replacements.append((ph, orig))
        result = result[:s] + ph + result[e:]
    return result, replacements

def restore_terms(translated, replacements):
    """{{T0}} 等を原文に戻す"""
    for ph, orig in replacements:
        translated = translated.replace(ph, orig)
        # フォールバック: {{}} が除去されて Tn だけ残った場合
        bare = ph[2:-2]  # {{T0}} -> T0
        translated = re.sub(r'\b' + re.escape(bare) + r'\b', orig, translated)
    return translated

# --------------------------------------------------------
# Ollama 翻訳
# --------------------------------------------------------
def translate(text, system_terms):
    system = SYSTEM_PROMPT
    if system_terms:
        system += " Keep these game-specific terms exactly as-is: " + ", ".join(system_terms) + "."
    body = json.dumps({
        "model": MODEL,
        "system": system,
        "prompt": TRANSLATE_PROMPT + "\n\n" + text,
        "stream": False,
        "options": {"num_ctx": NUM_CTX, "num_predict": 120, "temperature": 0.1}
    }).encode()
    req = urllib.request.Request(OLLAMA_URL, data=body,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.loads(r.read())["response"].strip()
    except Exception as e:
        return f"[ERROR] {e}"

# --------------------------------------------------------
# テストケース
# --------------------------------------------------------
TEST_CASES = [
    # (説明, 入力文, 期待される保護語リスト)
    ("MARBAN 地名",
     "NEED MORTARS TO MARBAN",
     ["MARBAN"]),

    ("wardens / collies 派閥名",
     "there's at least 9 wardens up north and 20+ collies",
     ["wardens", "collies"]),

    ("Great March 地名",
     "i have aluminum dropping at north scrap in great march!",
     ["great march"]),

    ("gunboat 艦艇名",
     "10% chance 90% fail, they will have t2 in a couple hours, have to wait for gunboat",
     ["gunboat"]),

    ("KC 略称 (King's Cage)",
     "ur not holding KC",
     ["KC"]),

    ("wardens 長文",
     "one time I drove a bike at the wardens and they just all surrendered immediately out of sheer fear",
     ["wardens"]),

    ("Able wardens (Able はシャード名・非保護, wardens は保護)",
     "I hate to be the barer of bad news but Able wardens are the same",
     ["wardens"]),

    ("halftracks 車両",
     "is it me or is tech going very fast this war. i feel like i blinked once and were almost to having halftracks",
     ["halftracks"]),

    ("Warden + logi 複合",
     "I have a Warden RPG gun boat that needs a owner. Already stocked with 30mm and RPGs",
     ["Warden"]),  # "gun boat" は2語なので gunboats? にはマッチしない

    ("King's Cage フルネーム",
     "King's Cage is contested, send logi NOW",
     ["King's Cage", "logi"]),
]

def main():
    base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    txt_path = os.path.join(base, "term_protection.txt")
    regexes = load_term_regexes(txt_path)
    print(f"読み込み: {len(regexes)} パターン ({txt_path})\n")
    print("=" * 70)

    pass_count = 0
    fail_count = 0

    for desc, src, expected_terms in TEST_CASES:
        masked, replacements = protect_terms(src, regexes)
        protected = [orig for (_, orig) in replacements]

        # 期待保護語が全てマッチしているか確認
        ok = all(
            any(e.lower() == p.lower() for p in protected)
            for e in expected_terms
        )

        status = "PASS" if ok else "FAIL"
        if ok:
            pass_count += 1
        else:
            fail_count += 1

        print(f"[{status}] {desc}")
        print(f"  原文   : {src}")
        print(f"  マスク : {masked}")
        print(f"  保護語 : {protected if protected else '(なし)'}")
        if not ok:
            missing = [e for e in expected_terms
                       if not any(e.lower() == p.lower() for p in protected)]
            print(f"  *** 期待保護語が未マッチ: {missing}")

        # Ollama で翻訳してマスク → 復元の往復テスト
        system_terms = protected if protected else []
        translated_masked = translate(masked, system_terms)
        translated_final  = restore_terms(translated_masked, replacements)

        print(f"  翻訳前 : {masked}")
        print(f"  LLM出力: {translated_masked}")
        print(f"  復元後 : {translated_final}")
        print()

    print("=" * 70)
    print(f"結果: {pass_count} PASS / {fail_count} FAIL")

if __name__ == "__main__":
    main()
