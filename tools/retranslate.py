#!/usr/bin/env python3
"""
retranslate.py - chat_log.txt から再翻訳して translation_log.csv を生成する

使い方:
  python retranslate.py [--limit N] [--model MODEL]

オプション:
  --limit N       処理するメッセージ数の上限 (デフォルト: 全件)
  --model MODEL   Ollama モデル名 (デフォルト: gemma3:4b)
  --dry-run       翻訳せずにパース結果だけ表示
"""

import re
import csv
import sys
import json
import time
import argparse
import unicodedata
import urllib.request
import urllib.error
from pathlib import Path

# Windows コンソールの CP932 エンコードエラーを防ぐ
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

# ============================================================
# パス設定
# ============================================================

WIN64_DIR = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Foxhole\War\Binaries\Win64")
CHAT_LOG  = WIN64_DIR / "chat_log.txt"
OUT_CSV   = WIN64_DIR / "translation_log.csv"

OLLAMA_ENDPOINT = "http://localhost:11434/api/generate"
TARGET_LANG     = "Japanese"

# ============================================================
# ログパース
# ============================================================

LOG_RE = re.compile(
    r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\] \[([^\]]+)\] (.+?): (.+)$"
)

# 翻訳対象外チャンネル (システムメッセージ・文字化けが多い)
SKIP_CHANNELS = {"ChatListenLanguages", "Unknown"}

# CJK 文字のコードポイント範囲
_CJK_RANGES = [
    (0x3000, 0x9FFF),   # CJK 統合漢字・ひらがな・カタカナ等
    (0xF900, 0xFAFF),   # CJK 互換漢字
    (0x20000, 0x2A6DF), # CJK 統合漢字拡張 B
]

def is_japanese(text: str) -> bool:
    """ひらがな・カタカナを含む場合は日本語と判定する"""
    for ch in text:
        cp = ord(ch)
        if 0x3040 <= cp <= 0x30FF:  # ひらがな・カタカナ
            return True
    return False


def parse_chat_log(path: Path) -> list[dict]:
    """chat_log.txt を解析し、重複排除したエントリのリストを返す"""
    entries = []
    seen: set[tuple] = set()

    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = LOG_RE.match(line)
            if not m:
                continue

            timestamp, channel, sender, message = m.groups()
            message = message.strip()

            if channel in SKIP_CHANNELS:
                continue
            if not message:
                continue

            # 重複排除 (同一チャンネル・送信者・メッセージ)
            key = (channel, sender, message)
            if key in seen:
                continue
            seen.add(key)

            entries.append({
                "timestamp": timestamp,
                "channel":   channel,
                "sender":    sender,
                "original":  message,
            })

    return entries

# ============================================================
# 固有名詞プレースホルダー保護 (translate.cpp と同一ロジック)
# ============================================================

TERM_DICT_PATH = WIN64_DIR / "term_protection.txt"

# コンパイル済みパターンのリスト
_TERM_PATTERNS: list[tuple[re.Pattern, bool]] = []


def _load_term_patterns(path: Path) -> None:
    """term_protection.txt を読み込んで _TERM_PATTERNS を構築する"""
    if not path.exists():
        print(f"[WARN] {path} が見つかりません。固有名詞保護は無効。", file=sys.stderr)
        return

    count = 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\r\n").rstrip()
            if not line or line.startswith("#"):
                continue

            icase = False
            if line.endswith(" i"):
                line = line[:-2]
                icase = True

            # \b が含まれていなければ自動付与
            pat = line if r"\b" in line else rf"\b{line}\b"
            flags = re.IGNORECASE if icase else 0
            try:
                _TERM_PATTERNS.append((re.compile(pat, flags), icase))
                count += 1
            except re.error as e:
                print(f"[WARN] 正規表現エラー '{line}': {e}", file=sys.stderr)

    print(f"[INFO] term_protection.txt: {count} 件読み込み ({path})")


_load_term_patterns(TERM_DICT_PATH)


def protect_terms(text: str) -> tuple[str, list[tuple[str, str]]]:
    """保護対象語をプレースホルダーに置き換えて (変換後テキスト, マッピング) を返す"""
    spans: list[tuple[int, int, str]] = []  # (start, end, original)
    for pat, _ in _TERM_PATTERNS:
        for m in pat.finditer(text):
            spans.append((m.start(), m.end(), m.group()))

    if not spans:
        return text, []

    # 開始位置でソートし重複を除去 (先着優先)
    spans.sort(key=lambda s: s[0])
    deduped: list[tuple[int, int, str]] = []
    last_end = 0
    for start, end, orig in spans:
        if start >= last_end:
            deduped.append((start, end, orig))
            last_end = end

    # 後ろから置換 ({{T0}} 形式: LLM がテンプレート変数として認識し除去しにくい)
    replacements: list[tuple[str, str]] = []
    result = text
    for start, end, orig in reversed(deduped):
        ph = f"{{{{T{len(replacements)}}}}}"
        replacements.append((ph, orig))
        result = result[:start] + ph + result[end:]

    return result, replacements


def restore_terms(translated: str, replacements: list[tuple[str, str]]) -> str:
    """翻訳結果中のプレースホルダーを原文に戻す。
    LLM が外側の {{ }} を除去して Tn だけ残した場合もフォールバックで復元する。"""
    import re as _re
    for i, (ph, orig) in enumerate(replacements):
        # 1st pass: 完全一致 {{Tn}}
        if ph in translated:
            translated = translated.replace(ph, orig)
            continue

        # 2nd pass: Tn のみ残った場合 (単語境界チェック付き)
        bare = f"T{i}"
        translated = _re.sub(rf"(?<![A-Za-z0-9]){_re.escape(bare)}(?![A-Za-z0-9])",
                             orig, translated)

    return translated


# ============================================================
# Ollama 翻訳
# ============================================================

def _is_single_placeholder(s: str) -> bool:
    """空白除去後が {{Tn}} 1個だけか判定する"""
    stripped = s.strip()
    import re as _re
    return bool(_re.fullmatch(r"\{\{T\d+\}\}", stripped))


def _raw_translate(text: str, model: str) -> str:
    """text をそのまま Ollama に送り生の翻訳結果を返す (失敗時は空文字)"""
    prompt = (
        f"You are a translator. The user sends a chat message in any language."
        f" Translate it to {TARGET_LANG}."
        f" Output ONLY the translated text, nothing else. No explanations."
        f" If the message is already in {TARGET_LANG}, output it unchanged."
        f" IMPORTANT: Keep all {{{{T0}}}}, {{{{T1}}}}, {{{{T2}}}} etc. tokens exactly as-is in your output."
        f"\n\n{text}"
    )
    body = json.dumps({
        "model":  model,
        "prompt": prompt,
        "stream": False,
        "options": {"num_ctx": 256, "temperature": 0.1},
    }).encode()
    req = urllib.request.Request(
        OLLAMA_ENDPOINT, data=body,
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            return json.loads(resp.read()).get("response", "").strip()
    except Exception:
        return ""


def _count_found_terms(result: str, replacements: list[tuple[str, str]]) -> int:
    lower = result.lower()
    return sum(1 for _, orig in replacements if orig.lower() in lower)


def translate(text: str, model: str) -> str:
    """translate.cpp:DoTranslate と同一ロジックで翻訳する"""
    protected, replacements = protect_terms(text)

    # メッセージ全体が1つの保護語だけなら翻訳不要
    if replacements and _is_single_placeholder(protected):
        return replacements[0][1]

    # 1st try: プレースホルダー保護あり
    raw = _raw_translate(protected, model)
    if not raw:
        return "[ERROR: Ollama not responding]"

    result = restore_terms(raw, replacements)

    # 保護語の半数以上が訳文から消えていたら保護なしで再翻訳
    if replacements:
        found    = _count_found_terms(result, replacements)
        expected = len(replacements)
        if found * 2 < expected:
            fallback = _raw_translate(text, model)
            if fallback:
                result = fallback

    return result


def check_ollama() -> bool:
    try:
        with urllib.request.urlopen("http://localhost:11434/api/version", timeout=5):
            return True
    except Exception:
        return False

# ============================================================
# メイン
# ============================================================

def main():
    parser = argparse.ArgumentParser(description="chat_log.txt を再翻訳して CSV を生成")
    parser.add_argument("--limit",        type=int, default=None,        help="処理件数の上限")
    parser.add_argument("--model",        type=str, default="gemma3:4b", help="Ollama モデル名")
    parser.add_argument("--dry-run",      action="store_true",            help="翻訳せず件数だけ表示")
    parser.add_argument("--all-langs",    action="store_true",            help="日本語メッセージも翻訳対象にする (デフォルト: 除外)")
    args = parser.parse_args()

    if not CHAT_LOG.exists():
        print(f"[ERROR] ファイルが見つかりません: {CHAT_LOG}", file=sys.stderr)
        sys.exit(1)

    print(f"[INFO] ログ解析中: {CHAT_LOG}")
    entries = parse_chat_log(CHAT_LOG)
    print(f"[INFO] 重複排除後: {len(entries)} 件")

    if not args.all_langs:
        before = len(entries)
        entries = [e for e in entries if not is_japanese(e["original"])]
        print(f"[INFO] 非日本語のみ: {len(entries)} 件 ({before - len(entries)} 件の日本語を除外)")

    if args.limit:
        entries = entries[: args.limit]
        print(f"[INFO] --limit により {len(entries)} 件に制限")

    if args.dry_run:
        for e in entries[:20]:
            print(f"  [{e['channel']}] {e['sender']}: {e['original']}")
        print("(--dry-run: 翻訳はスキップ)")
        return

    if not check_ollama():
        print("[ERROR] Ollama が起動していません (http://localhost:11434)", file=sys.stderr)
        sys.exit(1)

    print(f"[INFO] モデル: {args.model}  出力先: {OUT_CSV}")
    print()

    start = time.time()
    with open(OUT_CSV, "w", newline="", encoding="utf-8-sig") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=["timestamp", "channel", "sender", "original", "translated"],
        )
        writer.writeheader()

        for i, entry in enumerate(entries, 1):
            t0 = time.time()
            translated = translate(entry["original"], args.model)
            elapsed = time.time() - t0

            entry["translated"] = translated
            writer.writerow(entry)
            f.flush()

            orig_disp  = entry["original"][:45].replace("\n", " ")
            trans_disp = translated[:45].replace("\n", " ")
            print(f"[{i:4d}/{len(entries)}] ({elapsed:.1f}s) {entry['sender']}")
            print(f"         原文: {orig_disp}")
            print(f"         訳文: {trans_disp}")
            print()

    total = time.time() - start
    print(f"[完了] {len(entries)} 件  合計 {total:.0f}s  出力: {OUT_CSV}")


if __name__ == "__main__":
    main()
