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
# Ollama 翻訳
# ============================================================

def translate(text: str, model: str) -> str:
    """translate.cpp:BuildRequestBody と同一プロンプトで翻訳する"""
    prompt = (
        f"You are a translator. The user sends a chat message in any language."
        f" Translate it to {TARGET_LANG}."
        f" Output ONLY the translated text, nothing else. No explanations."
        f" If the message is already in {TARGET_LANG}, output it unchanged."
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
            data = json.loads(resp.read())
            return data.get("response", "").strip()
    except urllib.error.URLError as e:
        return f"[ERROR: {e}]"
    except Exception as e:
        return f"[ERROR: {e}]"


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
