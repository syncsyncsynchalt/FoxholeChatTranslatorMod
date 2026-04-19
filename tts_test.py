#!/usr/bin/env python3
"""
TTS テストツール - ゲーム起動不要
config.ini の設定を表示しながら WinRT OneCore TTS をテストする。
C++ tts.cpp と同じ SSML 生成ロジック(言語自動判定・senderハッシュ)を再現。
"""

import tkinter as tk
from tkinter import ttk, scrolledtext
import configparser
import subprocess
import threading
import os
import sys
import tempfile

# ============================================================
# C++ tts.cpp と同じロジックを Python で再実装
# ============================================================

# BuildSsml の kPitches / kRates テーブル (tts.cpp と同一)
PITCHES = ["-20%", "-15%", "-10%", "-5%", "0%", "+5%", "+10%", "+15%", "+20%"]
RATES   = ["95%",  "100%", "105%", "110%", "115%"]

LANG_TO_TAG = {
    "en": "en-US", "ru": "ru-RU", "ko": "ko-KR",
    "zh": "zh-CN", "ja": "ja-JP",
}


def detect_language(text: str) -> str:
    """DetectLanguage() の Python 版 (tts.cpp と同じ判定順序)"""
    cyrillic = hangul = cjk = hiragana = katakana = 0
    for ch in text[:200]:
        cp = ord(ch)
        if 0x0400 <= cp <= 0x04FF: cyrillic += 1
        elif 0xAC00 <= cp <= 0xD7AF: hangul += 1
        elif 0x3040 <= cp <= 0x309F: hiragana += 1
        elif 0x30A0 <= cp <= 0x30FF: katakana += 1
        elif 0x4E00 <= cp <= 0x9FFF: cjk += 1
    if cyrillic:            return "ru"
    if hangul:              return "ko"
    if hiragana or katakana: return "ja"
    if cjk:                 return "zh"
    return "en"


def sender_prosody(sender: str):
    """
    BuildSsml() の sender ハッシュ処理を再現。
    MSVC std::hash<std::string> は FNV-1a (64bit) を使うため同じアルゴリズムを採用。
    """
    if not sender:
        return "0%", "100%"
    # FNV-1a 64bit (MSVC の std::hash<std::string> に相当)
    h = 14695981039346656037
    for b in sender.encode("utf-8"):
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    pitch = PITCHES[h % 9]
    rate  = RATES[(h >> 8) % 5]
    return pitch, rate


def escape_xml(text: str) -> str:
    return (text.replace("&", "&amp;").replace("<", "&lt;")
                .replace(">", "&gt;").replace('"', "&quot;")
                .replace("'", "&apos;"))


def build_ssml(text: str, lang_tag: str, pitch: str, rate: str) -> str:
    """C++ BuildSsml() と同一の SSML を生成"""
    return (
        f"<speak version='1.0' "
        f"xmlns='http://www.w3.org/2001/10/synthesis' "
        f"xml:lang='{lang_tag}'>"
        f"<prosody pitch='{pitch}' rate='{rate}'>"
        f"{escape_xml(text)}"
        f"</prosody></speak>"
    )


def apply_speaking_rate(ssml: str, speaking_rate: float) -> str:
    """
    SSML の prosody rate に speaking_rate を乗算して返す。
    WinRT の SynthesizeSsmlToStreamAsync は <prosody rate> を優先し
    SpeakingRate プロパティを無視するため、SSML 側に直接反映させる。
    """
    def replace_rate(m):
        pct = int(m.group(1))
        return f"rate='{round(pct * speaking_rate)}%'"
    return _re.sub(r"rate='(\d+)%'", replace_rate, ssml)


# ============================================================
# 設定読み込み
# ============================================================

def load_config(base_dir: str) -> dict:
    defaults = {
        "tts_language": "auto",
        "translation_enabled": True,
        "target_language": "Japanese",
        "ollama_endpoint": "http://localhost:11434/api/generate",
        "performance_preset": "Medium",
        "enable_console": True,
        "init_delay_ms": 10000,
        "demo_mode": False,
        "prefix": "★",
    }
    ini_path = os.path.join(base_dir, "config.ini")
    if not os.path.exists(ini_path):
        return defaults
    p = configparser.RawConfigParser()
    p.read(ini_path, encoding="utf-8")

    def gs(sec, key, fallback):
        return p.get(sec, key) if p.has_option(sec, key) else fallback
    def gi(sec, key, fallback):
        return p.getint(sec, key) if p.has_option(sec, key) else fallback

    return {
        "tts_language":         gs("TTS",         "Language",              defaults["tts_language"]),
        "translation_enabled":  gi("Translation",  "Enabled",               1) != 0,
        "target_language":      gs("Translation",  "TargetLanguage",        defaults["target_language"]),
        "ollama_endpoint":      gs("Translation",  "OllamaEndpoint",        defaults["ollama_endpoint"]),
        "performance_preset":   gs("Translation",  "PerformancePreset",     defaults["performance_preset"]),
        "enable_console":       gi("General",      "EnableConsole",         1) != 0,
        "init_delay_ms":        gi("General",      "InitDelayMs",           10000),
        "demo_mode":            gi("Overlay",      "DemoMode",              0) != 0,
        "log_file_path":        gs("General",      "LogFilePath",           ""),
    }


# ============================================================
# chat_log.txt パーサ
# ============================================================

import re as _re
_CHAT_LOG_RE = _re.compile(r'^\[[\d\- :]+\]\s+\[([^\]]+)\]\s+([^:]+):\s+(.+)$')

def parse_chat_log(path: str) -> list[dict]:
    """
    chat_log.txt を解析して {channel, sender, message} のリストを返す。
    書式: [YYYY-MM-DD HH:MM:SS] [channel] sender: message
    """
    messages = []
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = _CHAT_LOG_RE.match(line.strip())
                if m:
                    messages.append({
                        "channel": m.group(1),
                        "sender":  m.group(2).strip(),
                        "message": m.group(3).strip(),
                    })
    except Exception:
        pass
    return messages


# ============================================================
# PowerShell 経由で WinRT 音声を列挙・再生
# ============================================================

# WinRT AllVoices を列挙し "名前|言語|Natural/Standard" 形式で出力する。
# WinRT が失敗した場合は SAPI5 にフォールバック。
PS_LIST_VOICES = """
$OutputEncoding = [System.Text.Encoding]::UTF8
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
foreach ($iv in $s.GetInstalledVoices()) {
    $i = $iv.VoiceInfo
    $nat = if ($i.Name -like '*Natural*') { 'Natural' } else { 'Standard' }
    Write-Output ('SAPI5|' + $i.Name + '|' + $i.Culture + '|' + $nat)
}
"""


def list_voices() -> tuple[list[dict], str]:
    """(voice_list, engine_name) を返す。voice_list の各要素は dict(name, language, type)"""
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", PS_LIST_VOICES],
            capture_output=True, text=True, timeout=20, encoding="utf-8", errors="replace"
        )
        voices = []
        engine = "不明"
        for line in r.stdout.strip().splitlines():
            parts = line.split("|")
            if len(parts) < 4:
                continue
            eng_tag, name, lang, typ = parts[0], parts[1], parts[2], parts[3]
            engine = "WinRT OneCore" if eng_tag == "WINRT" else "SAPI5"
            # 表示名: "Microsoft Haruka Desktop" → "Microsoft Haruka"
            display = name.replace(" Desktop", "").replace(" Mobile", "")
            voices.append({"name": name, "display": display, "language": lang, "type": typ})
        return voices, engine
    except Exception as e:
        return [{"name": f"エラー: {e}", "language": "", "type": ""}], "エラー"


def make_speak_ps(ssml: str, voice_name: str, speaking_rate: float, engine: str = "") -> str:
    """SSML を再生する PowerShell スクリプトを生成"""
    ssml_esc     = ssml.replace("'", "''")
    voice_esc    = (voice_name or "").replace("'", "''")
    rate_str     = f"{speaking_rate:.4f}"

    # WinRT の SynthesizeSsmlToStreamAsync は PowerShell 5.1 サブプロセスでは
    # GetResults() が System.__ComObject として投影され呼び出せないため、
    # SAPI5 (System.Speech) で統一する。音声品質は同等。
    # 音声名マッチ: WinRT名 "Microsoft Haruka" → SAPI5名 "Microsoft Haruka Desktop"
    # のように部分一致で対応する。
    return f"""
$OutputEncoding = [System.Text.Encoding]::UTF8
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
if ('{voice_esc}' -ne '') {{
    $installed = $s.GetInstalledVoices() | ForEach-Object {{ $_.VoiceInfo.Name }}
    $match = $installed | Where-Object {{ $_ -eq '{voice_esc}' }} | Select-Object -First 1
    if (-not $match) {{
        $match = $installed | Where-Object {{ $_ -like '*{voice_esc}*' }} | Select-Object -First 1
    }}
    if ($match) {{ $s.SelectVoice($match) }}
}}
$s.SpeakSsml('{ssml_esc}')
Write-Output ('OK_SAPI5:' + $s.Voice.Name)
"""


# ============================================================
# メイン UI
# ============================================================

class App:
    def __init__(self, root: tk.Tk, base_dir: str):
        self.root       = root
        self.base_dir   = base_dir
        self.cfg        = load_config(base_dir)
        self.all_voices: list[dict] = []
        self.engine     = "読み込み中"
        self.speak_proc: subprocess.Popen | None = None
        self.selected_voice = ""  # リストで選択した音声名 (空=自動)
        self.log_messages: list[dict] = []

        root.title("TTS テストツール — ChatTranslator")
        root.geometry("980x720")
        root.minsize(780, 560)
        try:
            from ctypes import windll
            windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass

        self._build_ui()
        threading.Thread(target=self._bg_load_voices, daemon=True).start()

    # ----------------------------------------------------------
    # UI 構築
    # ----------------------------------------------------------

    def _build_ui(self):
        # ── 上段ペイン (設定 | 操作パネル) ──
        paned = ttk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        paned.pack(fill=tk.BOTH, expand=True, padx=6, pady=(6, 2))

        left = ttk.Frame(paned, width=280)
        paned.add(left, weight=1)

        right = ttk.Frame(paned)
        paned.add(right, weight=3)

        self._build_left(left)
        self._build_right(right)

        # ── ステータスバー ──
        sb = ttk.Frame(self.root, relief=tk.SUNKEN, padding=(4, 1))
        sb.pack(fill=tk.X, side=tk.BOTTOM)
        self.status_var = tk.StringVar(value="音声を読み込み中...")
        ttk.Label(sb, textvariable=self.status_var, anchor=tk.W).pack(side=tk.LEFT, fill=tk.X, expand=True)
        self.engine_var = tk.StringVar(value="エンジン: —")
        ttk.Label(sb, textvariable=self.engine_var, anchor=tk.E, foreground="#666").pack(side=tk.RIGHT)

    def _build_left(self, parent):
        # config.ini 表示
        cf = ttk.LabelFrame(parent, text=" config.ini ", padding=8)
        cf.pack(fill=tk.X, padx=4, pady=4)

        c = self.cfg
        rows = [
            ("TTS 言語モード",    c["tts_language"],
             "auto の場合はテキスト内容から自動判定"),
            ("翻訳",             "有効" if c["translation_enabled"] else "無効",   ""),
            ("翻訳先言語",       c["target_language"],                             ""),
            ("Ollama EP",        c["ollama_endpoint"],                             ""),
            ("パフォーマンス",   c["performance_preset"],                          ""),
            ("デモモード",       "ON" if c["demo_mode"] else "OFF",               ""),
            ("初期化待機",       f'{c["init_delay_ms"]} ms',                      ""),
        ]
        for label, val, tip in rows:
            row = ttk.Frame(cf)
            row.pack(fill=tk.X, pady=1)
            ttk.Label(row, text=label + ":", width=14, anchor=tk.E,
                      font=("", 8)).pack(side=tk.LEFT)
            lbl = ttk.Label(row, text=val, foreground="#005599",
                            font=("", 8, "bold"))
            lbl.pack(side=tk.LEFT, padx=(3, 0))
            if tip:
                lbl.configure(cursor="question_arrow")

        # 音声一覧
        vf = ttk.LabelFrame(parent, text=" 利用可能な音声 ", padding=6)
        vf.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        frow = ttk.Frame(vf)
        frow.pack(fill=tk.X, pady=(0, 4))
        ttk.Label(frow, text="言語:").pack(side=tk.LEFT)
        self.voice_filter_var = tk.StringVar(value="全て")
        fcb = ttk.Combobox(frow, textvariable=self.voice_filter_var,
                           values=["全て", "ja", "en", "ru", "zh", "ko"],
                           width=5, state="readonly")
        fcb.pack(side=tk.LEFT, padx=4)
        fcb.bind("<<ComboboxSelected>>", lambda _: self._refresh_voice_list())

        ttk.Button(frow, text="自動解除", width=7,
                   command=self._clear_voice_selection).pack(side=tk.RIGHT)

        frame = ttk.Frame(vf)
        frame.pack(fill=tk.BOTH, expand=True)
        sb = ttk.Scrollbar(frame)
        sb.pack(side=tk.RIGHT, fill=tk.Y)
        self.voice_lb = tk.Listbox(frame, yscrollcommand=sb.set,
                                   selectmode=tk.SINGLE, activestyle="dotbox",
                                   font=("", 8))
        self.voice_lb.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        sb.config(command=self.voice_lb.yview)
        self.voice_lb.bind("<<ListboxSelect>>", self._on_voice_select)

        self.voice_lb.insert(tk.END, "  読み込み中...")

    def _build_right(self, parent):
        # ── TTSパラメータ ──
        pf = ttk.LabelFrame(parent, text=" TTSパラメータ ", padding=8)
        pf.pack(fill=tk.X, padx=4, pady=4)

        # 行1: 言語モード + Speaking Rate
        r1 = ttk.Frame(pf)
        r1.pack(fill=tk.X, pady=3)
        ttk.Label(r1, text="言語モード:").grid(row=0, column=0, sticky=tk.E, padx=(0, 4))
        self.lang_var = tk.StringVar(value=self.cfg["tts_language"])
        lcb = ttk.Combobox(r1, textvariable=self.lang_var,
                           values=["auto", "ja", "en", "ru", "zh", "ko"],
                           width=6, state="readonly")
        lcb.grid(row=0, column=1, sticky=tk.W)
        lcb.bind("<<ComboboxSelected>>", lambda _: self._update_preview())

        ttk.Label(r1, text="Speaking Rate:").grid(row=0, column=2, sticky=tk.E,
                                                   padx=(16, 4))
        self.rate_var = tk.DoubleVar(value=1.1)
        scl = ttk.Scale(r1, from_=0.5, to=3.0, variable=self.rate_var,
                        orient=tk.HORIZONTAL, length=140,
                        command=lambda _: self._on_rate_change())
        scl.grid(row=0, column=3)
        self.rate_lbl = ttk.Label(r1, text="1.10", width=4)
        self.rate_lbl.grid(row=0, column=4, padx=(4, 0))
        ttk.Label(r1, text="(C++デフォルト=1.1)", foreground="#888",
                  font=("", 7)).grid(row=0, column=5, padx=(4, 0))

        # 行2: Sender
        r2 = ttk.Frame(pf)
        r2.pack(fill=tk.X, pady=3)
        ttk.Label(r2, text="Sender名:").grid(row=0, column=0, sticky=tk.E, padx=(0, 4))
        self.sender_var = tk.StringVar()
        se = ttk.Entry(r2, textvariable=self.sender_var, width=22)
        se.grid(row=0, column=1, sticky=tk.W)
        se.bind("<KeyRelease>", lambda _: self._update_preview())
        self.prosody_lbl = ttk.Label(r2, text="→ pitch: 0%, rate: 100%",
                                     foreground="#555", font=("", 8))
        self.prosody_lbl.grid(row=0, column=2, padx=(8, 0))

        # 行3: 選択音声
        r3 = ttk.Frame(pf)
        r3.pack(fill=tk.X, pady=3)
        ttk.Label(r3, text="使用音声:").grid(row=0, column=0, sticky=tk.E, padx=(0, 4))
        self.voice_lbl = ttk.Label(r3, text="(リストから選択 or 自動)",
                                   foreground="#888")
        self.voice_lbl.grid(row=0, column=1, sticky=tk.W)

        # ── テキスト入力 ──
        tf = ttk.LabelFrame(parent, text=" 発話テキスト ", padding=8)
        tf.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        self.text_box = scrolledtext.ScrolledText(tf, height=5, wrap=tk.WORD,
                                                  font=("", 10))
        self.text_box.pack(fill=tk.BOTH, expand=True)
        self.text_box.bind("<KeyRelease>", lambda _: self._update_preview())

        preset_row = ttk.Frame(tf)
        preset_row.pack(fill=tk.X, pady=(6, 0))
        ttk.Label(preset_row, text="プリセット:", font=("", 8)).pack(side=tk.LEFT)
        PRESETS = [
            ("日本語", "こんにちは！今日の戦況はいかがですか？ 装甲部隊が前進しています。"),
            ("English", "Hello! How is the front line today? Armoured units are advancing."),
            ("Русский", "Привет! Как обстановка на фронте? Бронеколонна продвигается вперёд."),
            ("中文",  "你好！前线情况如何？装甲部队正在推进中。"),
            ("한국어", "안녕하세요! 오늘 전선 상황은 어떤가요? 기갑 부대가 전진하고 있습니다."),
        ]
        for label, txt in PRESETS:
            ttk.Button(preset_row, text=label, width=7,
                       command=lambda t=txt: self._set_preset(t)).pack(side=tk.LEFT, padx=2)

        # ── ログサンプル ──
        log_row = ttk.Frame(tf)
        log_row.pack(fill=tk.X, pady=(4, 0))
        ttk.Label(log_row, text="ログ:", font=("", 8)).pack(side=tk.LEFT)
        self.log_status_lbl = ttk.Label(log_row, text="未読み込み",
                                        foreground="#888", font=("", 8))
        self.log_status_lbl.pack(side=tk.LEFT, padx=(4, 8))
        ttk.Button(log_row, text="読み込む", width=8,
                   command=self._load_log).pack(side=tk.LEFT)
        ttk.Separator(log_row, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y,
                                                         padx=6, pady=2)
        for label, cat in [("短い ≤20", "short"), ("普通 21-60", "normal"), ("長い 61+", "long")]:
            ttk.Button(log_row, text=label, width=9,
                       command=lambda c=cat: self._pick_log_sample(c)).pack(side=tk.LEFT, padx=2)

        # ── SSML プレビュー ──
        sf = ttk.LabelFrame(parent,
                            text=" SSML プレビュー (C++ tts.cpp と同一 / 再生時は Speaking Rate を rate に乗算) ",
                            padding=6)
        sf.pack(fill=tk.X, padx=4, pady=4)
        self.ssml_txt = tk.Text(sf, height=3, state=tk.DISABLED, wrap=tk.WORD,
                                foreground="#333", background="#f8f8f8",
                                font=("Consolas", 8))
        self.ssml_txt.pack(fill=tk.X)

        # ── コントロール ──
        cf2 = ttk.Frame(parent)
        cf2.pack(fill=tk.X, padx=4, pady=4)
        self.speak_btn = ttk.Button(cf2, text="▶  発話", width=10,
                                    command=self._speak)
        self.speak_btn.pack(side=tk.LEFT, padx=(0, 6))
        self.stop_btn = ttk.Button(cf2, text="■  停止", width=8,
                                   command=self._stop, state=tk.DISABLED)
        self.stop_btn.pack(side=tk.LEFT)
        ttk.Button(cf2, text="テキストをクリア",
                   command=self._clear_text).pack(side=tk.LEFT, padx=8)

    # ----------------------------------------------------------
    # バックグラウンド処理
    # ----------------------------------------------------------

    def _bg_load_voices(self):
        voices, engine = list_voices()
        self.all_voices = voices
        self.engine     = engine
        self.root.after(0, self._on_voices_loaded)

    def _on_voices_loaded(self):
        self._refresh_voice_list()
        self.engine_var.set(f"エンジン: {self.engine}")
        n = len(self.all_voices)
        self.status_var.set(f"音声 {n} 件 ({self.engine})  — 準備完了")

    # ----------------------------------------------------------
    # 音声リスト操作
    # ----------------------------------------------------------

    def _visible_voices(self) -> list[dict]:
        filt = self.voice_filter_var.get()
        if filt == "全て":
            return self.all_voices
        return [v for v in self.all_voices
                if v["language"].lower().startswith(filt.lower())]

    def _refresh_voice_list(self):
        self.voice_lb.delete(0, tk.END)
        for v in self._visible_voices():
            tag = "★" if v["type"] == "Natural" else "  "
            label = v.get("display", v["name"])
            self.voice_lb.insert(tk.END, f"{tag} {label}  ({v['language']})")
        if not self._visible_voices():
            self.voice_lb.insert(tk.END, "  (一致する音声なし)")

    def _on_voice_select(self, _event=None):
        sel = self.voice_lb.curselection()
        if not sel:
            return
        visible = self._visible_voices()
        idx = sel[0]
        if idx < len(visible):
            self.selected_voice = visible[idx]["name"]
            self.voice_lbl.config(text=self.selected_voice, foreground="#005599")

    def _clear_voice_selection(self):
        self.selected_voice = ""
        self.voice_lbl.config(text="(リストから選択 or 自動)", foreground="#888")
        self.voice_lb.selection_clear(0, tk.END)

    # ----------------------------------------------------------
    # パラメータ更新 & プレビュー
    # ----------------------------------------------------------

    def _on_rate_change(self):
        self.rate_lbl.config(text=f"{self.rate_var.get():.2f}")
        self._update_preview()

    def _update_preview(self):
        text   = self.text_box.get("1.0", tk.END).strip()
        sender = self.sender_var.get().strip()
        pitch, rate = sender_prosody(sender)
        self.prosody_lbl.config(text=f"→ pitch: {pitch}, rate: {rate}")

        if not text:
            self._set_ssml_preview("(テキストを入力するとSSMLが表示されます)")
            return

        lang_mode = self.lang_var.get()
        detected  = detect_language(text) if lang_mode == "auto" else lang_mode
        lang_tag  = LANG_TO_TAG.get(detected, "en-US")

        # 自動モード時は音声フィルタも追従
        if lang_mode == "auto":
            self.voice_filter_var.set(detected)
            self._refresh_voice_list()

        ssml = build_ssml(text, lang_tag, pitch, rate)
        self._set_ssml_preview(ssml)

    def _set_ssml_preview(self, text: str):
        self.ssml_txt.config(state=tk.NORMAL)
        self.ssml_txt.delete("1.0", tk.END)
        self.ssml_txt.insert(tk.END, text)
        self.ssml_txt.config(state=tk.DISABLED)

    def _set_preset(self, text: str):
        self.text_box.delete("1.0", tk.END)
        self.text_box.insert(tk.END, text)
        self._update_preview()

    def _clear_text(self):
        self.text_box.delete("1.0", tk.END)
        self._set_ssml_preview("")

    # ----------------------------------------------------------
    # ログサンプル
    # ----------------------------------------------------------

    def _resolve_log_path(self) -> str:
        # config.ini で明示指定されている場合はそちらを優先
        configured = self.cfg.get("log_file_path", "")
        if configured:
            return configured
        # DLL と同じディレクトリ = ゲームの War\Binaries\Win64\
        # base_dir は Mods\ChatTranslator なので2階層上がってゲームルートを求める
        game_root = os.path.normpath(os.path.join(self.base_dir, "..", ".."))
        candidates = [
            os.path.join(game_root, "War", "Binaries", "Win64", "chat_log.txt"),
            os.path.join(self.base_dir, "chat_log.txt"),
        ]
        for path in candidates:
            if os.path.exists(path):
                return path
        # 見つからなければ最初の候補を返す(エラーメッセージに表示)
        return candidates[0]

    def _load_log(self):
        path = self._resolve_log_path()
        if not os.path.exists(path):
            self.log_status_lbl.config(
                text=f"見つかりません: {os.path.basename(path)}", foreground="#cc0000")
            self.status_var.set(f"ログファイルが見つかりません: {path}")
            return
        self.log_messages = parse_chat_log(path)
        n = len(self.log_messages)
        shorts  = sum(1 for m in self.log_messages if len(m["message"]) <= 20)
        normals = sum(1 for m in self.log_messages if 20 < len(m["message"]) <= 60)
        longs   = sum(1 for m in self.log_messages if len(m["message"]) > 60)
        self.log_status_lbl.config(
            text=f"{n}件 (短{shorts}/普{normals}/長{longs})", foreground="#005599")
        self.status_var.set(f"ログ読み込み完了: {path}  ({n}件)")

    def _pick_log_sample(self, category: str):
        import random
        if not self.log_messages:
            self._load_log()
            if not self.log_messages:
                self.status_var.set("ログが空です。ゲームプレイ後に chat_log.txt が生成されます。")
                return
        SHORT, LONG = 20, 60
        if category == "short":
            pool = [m for m in self.log_messages if len(m["message"]) <= SHORT]
            label = f"短い (≤{SHORT}文字)"
        elif category == "normal":
            pool = [m for m in self.log_messages if SHORT < len(m["message"]) <= LONG]
            label = f"普通 ({SHORT+1}–{LONG}文字)"
        else:
            pool = [m for m in self.log_messages if len(m["message"]) > LONG]
            label = f"長い (>{LONG}文字)"
        if not pool:
            self.status_var.set(f"「{label}」に該当するメッセージがありません")
            return
        entry = random.choice(pool)
        self._set_preset(entry["message"])
        self.sender_var.set(entry["sender"])
        self._update_preview()
        preview = entry["message"][:50] + ("…" if len(entry["message"]) > 50 else "")
        self.status_var.set(
            f"[{entry['channel']}] {entry['sender']}: {preview}  ({len(entry['message'])}文字)")

    # ----------------------------------------------------------
    # 発話 / 停止
    # ----------------------------------------------------------

    def _speak(self):
        text = self.text_box.get("1.0", tk.END).strip()
        if not text:
            self.status_var.set("テキストを入力してください")
            return

        sender    = self.sender_var.get().strip()
        lang_mode = self.lang_var.get()
        detected  = detect_language(text) if lang_mode == "auto" else lang_mode
        lang_tag  = LANG_TO_TAG.get(detected, "en-US")
        pitch, rate = sender_prosody(sender)

        # 音声決定: リスト選択 > senderハッシュ > リスト先頭
        voice_name = self.selected_voice
        if not voice_name:
            candidates = [v for v in self.all_voices
                          if v["language"].lower().startswith(detected)]
            naturals   = [v for v in candidates if v["type"] == "Natural"]
            pool = naturals if naturals else candidates
            if pool:
                if sender:
                    h = 14695981039346656037
                    for b in sender.encode("utf-8"):
                        h ^= b; h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
                    voice_name = pool[h % len(pool)]["name"]
                else:
                    voice_name = pool[0]["name"]

        ssml          = build_ssml(text, lang_tag, pitch, rate)
        speaking_rate = self.rate_var.get()
        # Speaking Rate を <prosody rate> に乗算 (WinRT は SSML rate を優先するため)
        playback_ssml = apply_speaking_rate(ssml, speaking_rate)
        ps_script     = make_speak_ps(playback_ssml, voice_name, speaking_rate, self.engine)

        self.speak_btn.config(state=tk.DISABLED)
        self.stop_btn.config(state=tk.NORMAL)
        vname_short = voice_name[:30] + "..." if len(voice_name) > 30 else voice_name
        self.status_var.set(
            f"発話中… lang={detected}  pitch={pitch}  rate={rate}  "
            f"spkRate={speaking_rate:.2f}  voice={vname_short or '(auto)'}"
        )

        def run():
            try:
                self.speak_proc = subprocess.Popen(
                    ["powershell", "-NoProfile", "-NonInteractive", "-Command", ps_script],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                    encoding="utf-8", errors="replace"
                )
                stdout, stderr = self.speak_proc.communicate(timeout=90)
                if "OK_SAPI5:" in stdout:
                    used_voice = stdout.strip().split("OK_SAPI5:")[-1].strip()
                    msg = f"完了 (SAPI5: {used_voice})"
                elif "OK" in stdout:
                    msg = "完了"
                else:
                    msg = f"エラー: {stderr.strip()[:100] or 'unknown'}"
            except subprocess.TimeoutExpired:
                self.speak_proc.kill()
                msg = "タイムアウト"
            except Exception as e:
                msg = f"例外: {e}"
            finally:
                self.speak_proc = None
                self.root.after(0, lambda m=msg: self._on_speak_done(m))

        threading.Thread(target=run, daemon=True).start()

    def _on_speak_done(self, msg: str):
        self.speak_btn.config(state=tk.NORMAL)
        self.stop_btn.config(state=tk.DISABLED)
        self.status_var.set(f"状態: {msg}")

    def _stop(self):
        if self.speak_proc and self.speak_proc.poll() is None:
            self.speak_proc.kill()
        self.stop_btn.config(state=tk.DISABLED)
        self.speak_btn.config(state=tk.NORMAL)
        self.status_var.set("停止しました")


# ============================================================
# エントリポイント
# ============================================================

def main():
    base_dir = os.path.dirname(os.path.abspath(__file__))
    root = tk.Tk()
    App(root, base_dir)
    root.mainloop()


if __name__ == "__main__":
    main()
