#!/usr/bin/env python3
"""
TTS テストツール - Sherpa-ONNX Piper VITS 版
install.ps1 でインストールした models/ をゲーム起動なしでテストする。

使い方:
  python tools/tts_test.py
"""

import importlib
import importlib.util
import subprocess
import sys

def _ensure_deps():
    pkgs = [("sherpa-onnx", "sherpa_onnx"), ("sounddevice", "sounddevice"), ("numpy", "numpy")]
    missing = [pkg for pkg, mod in pkgs if importlib.util.find_spec(mod) is None]
    if missing:
        print(f"依存パッケージをインストール中: {', '.join(missing)} ...")
        subprocess.check_call([sys.executable, "-m", "pip", "install", *missing, "--quiet"])
        print("インストール完了\n")

_ensure_deps()

import tkinter as tk
from tkinter import ttk, scrolledtext
import configparser
import threading
import os
import re as _re



# ============================================================
# 言語判定 (tts.cpp DetectLanguage と同じロジック)
# ============================================================

def detect_language(text: str) -> str:
    cyrillic = hangul = cjk = hiragana = katakana = 0
    for ch in text[:200]:
        cp = ord(ch)
        if   0x0400 <= cp <= 0x04FF: cyrillic  += 1
        elif 0xAC00 <= cp <= 0xD7AF: hangul    += 1
        elif 0x3040 <= cp <= 0x309F: hiragana  += 1
        elif 0x30A0 <= cp <= 0x30FF: katakana  += 1
        elif 0x4E00 <= cp <= 0x9FFF: cjk       += 1
    if cyrillic:              return "ru"
    if hangul:                return "ko"
    if hiragana or katakana:  return "ja"
    if cjk:                   return "zh"
    return "en"


# ============================================================
# ディレクトリ解決
# ============================================================

def _find_tts_dir(base_dir: str) -> str:
    """
    base_dir = Mods/ChatTranslator/
    ゲームバイナリ = ../../War/Binaries/Win64/
    """
    game_bin = os.path.normpath(
        os.path.join(base_dir, "..", "..", "War", "Binaries", "Win64")
    )
    return os.path.join(game_bin, "tools", "tts")


# ============================================================
# Sherpa-ONNX TTS エンジン
# ============================================================

class TtsEngine:
    LANGS = ["en", "ja", "ru", "zh", "ko"]

    def __init__(self, tts_dir: str):
        self.tts_dir = tts_dir
        self._models: dict = {}
        self._lock = threading.Lock()
        self._sherpa = None
        self._import_error = ""
        self._try_import()

    def _try_import(self):
        try:
            import sherpa_onnx
            self._sherpa = sherpa_onnx
        except Exception as e:
            self._import_error = str(e)

    @property
    def available(self) -> bool:
        return self._sherpa is not None

    @property
    def import_error(self) -> str:
        return self._import_error

    def model_installed(self, lang: str) -> bool:
        base = os.path.join(self.tts_dir, "models", lang)
        # Supertonic (日本語など)
        if os.path.exists(os.path.join(base, "duration_predictor.int8.onnx")):
            return True
        # VITS (Piper / mimic3)
        return (os.path.exists(os.path.join(base, "model.onnx")) and
                os.path.exists(os.path.join(base, "tokens.txt")))

    def _load_model(self, lang: str):
        s = self._sherpa
        model_dir = os.path.join(self.tts_dir, "models", lang)

        # --- Supertonic ---
        dp = os.path.join(model_dir, "duration_predictor.int8.onnx")
        if os.path.exists(dp):
            supertonic = s.OfflineTtsSupertonicModelConfig(
                duration_predictor = dp,
                text_encoder       = os.path.join(model_dir, "text_encoder.int8.onnx"),
                vector_estimator   = os.path.join(model_dir, "vector_estimator.int8.onnx"),
                vocoder            = os.path.join(model_dir, "vocoder.int8.onnx"),
                tts_json           = os.path.join(model_dir, "tts.json"),
                unicode_indexer    = os.path.join(model_dir, "unicode_indexer.bin"),
                voice_style        = os.path.join(model_dir, "voice.bin"),
            )
            cfg = s.OfflineTtsConfig(
                model=s.OfflineTtsModelConfig(supertonic=supertonic, num_threads=2),
                max_num_sentences=1,
            )
            return s.OfflineTts(cfg)

        # --- VITS (Piper / mimic3) ---
        model_path   = os.path.join(model_dir, "model.onnx")
        tokens_path  = os.path.join(model_dir, "tokens.txt")
        lexicon_path = os.path.join(model_dir, "lexicon.txt")
        espeak_dir   = os.path.join(self.tts_dir, "espeak-ng-data")
        dict_dir     = os.path.join(model_dir, "dict") if lang == "zh" else ""

        if not (os.path.exists(model_path) and os.path.exists(tokens_path)):
            return None

        vits = s.OfflineTtsVitsModelConfig(
            model         = model_path,
            tokens        = tokens_path,
            lexicon       = lexicon_path if os.path.exists(lexicon_path) else "",
            data_dir      = espeak_dir if lang != "zh" and os.path.exists(espeak_dir) else "",
            dict_dir      = dict_dir if os.path.exists(dict_dir) else "",
            noise_scale   = 0.667,
            noise_scale_w = 0.8,
            length_scale  = 1.0,
        )
        cfg = s.OfflineTtsConfig(
            model=s.OfflineTtsModelConfig(vits=vits, num_threads=2),
            max_num_sentences=1,
        )
        return s.OfflineTts(cfg)

    def get_model(self, lang: str):
        with self._lock:
            if lang not in self._models:
                self._models[lang] = self._load_model(lang)
            return self._models[lang]

    def generate(self, text: str, lang: str, speed: float = 1.0):
        """(samples, sample_rate, actual_lang) | None を返す。C++ 同様 EN フォールバックあり。"""
        model = self.get_model(lang)
        actual = lang
        if model is None and lang != "en":
            model  = self.get_model("en")
            actual = "en"
        if model is None:
            return None
        audio = model.generate(text, sid=0, speed=speed)
        return audio.samples, audio.sample_rate, actual


# ============================================================
# VOICEVOX Core TTS エンジン (ctypes)
# ============================================================

class VoicevoxEngine:
    """VOICEVOX Core DLL を ctypes でロードして日本語音声合成"""

    def __init__(self, tts_dir: str, style_id: int = 3):
        self.tts_dir  = tts_dir
        self.style_id = style_id
        self._lib     = None
        self._synth   = None
        self._jtalk   = None
        self._TtsOpts = None
        self._ready   = False
        self._error   = ""
        self._try_init()

    def _vv_dir(self):
        return os.path.join(self.tts_dir, "voicevox")

    def installed(self) -> bool:
        return os.path.exists(
            os.path.join(self._vv_dir(), "c_api", "lib", "voicevox_core.dll"))

    @property
    def ready(self) -> bool:
        return self._ready

    @property
    def error(self) -> str:
        return self._error

    def _try_init(self):
        import ctypes
        import glob as _glob
        try:
            vv = self._vv_dir()
            dll_path = os.path.join(vv, "c_api", "lib", "voicevox_core.dll")
            if not os.path.exists(dll_path):
                self._error = "voicevox_core.dll なし (install.ps1 を実行してください)"
                return

            ort_dir  = os.path.join(vv, "onnxruntime", "lib")
            ort_dlls = _glob.glob(os.path.join(ort_dir, "*.dll"))
            if not ort_dlls:
                self._error = "voicevox_onnxruntime.dll なし"
                return
            ort_path = ort_dlls[0]

            os.add_dll_directory(os.path.abspath(ort_dir))
            os.add_dll_directory(os.path.abspath(os.path.join(vv, "c_api", "lib")))

            lib = ctypes.CDLL(dll_path)

            class _LoadOrtOpts(ctypes.Structure):
                _fields_ = [("filename", ctypes.c_char_p)]
            class _InitOpts(ctypes.Structure):
                _fields_ = [("acceleration_mode", ctypes.c_int32),
                             ("cpu_num_threads",   ctypes.c_uint16)]
            class _TtsOpts(ctypes.Structure):
                _fields_ = [("enable_interrogative_upspeak", ctypes.c_bool)]

            lib.voicevox_onnxruntime_load_once.argtypes = [
                _LoadOrtOpts, ctypes.POINTER(ctypes.c_void_p)]
            lib.voicevox_onnxruntime_load_once.restype  = ctypes.c_int32
            lib.voicevox_open_jtalk_rc_new.argtypes = [
                ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
            lib.voicevox_open_jtalk_rc_new.restype  = ctypes.c_int32
            lib.voicevox_make_default_initialize_options.argtypes = []
            lib.voicevox_make_default_initialize_options.restype  = _InitOpts
            lib.voicevox_synthesizer_new.argtypes = [
                ctypes.c_void_p, ctypes.c_void_p, _InitOpts,
                ctypes.POINTER(ctypes.c_void_p)]
            lib.voicevox_synthesizer_new.restype  = ctypes.c_int32
            lib.voicevox_voice_model_file_open.argtypes = [
                ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
            lib.voicevox_voice_model_file_open.restype  = ctypes.c_int32
            lib.voicevox_synthesizer_load_voice_model.argtypes = [
                ctypes.c_void_p, ctypes.c_void_p]
            lib.voicevox_synthesizer_load_voice_model.restype  = ctypes.c_int32
            lib.voicevox_voice_model_file_delete.argtypes = [ctypes.c_void_p]
            lib.voicevox_voice_model_file_delete.restype  = None
            lib.voicevox_make_default_tts_options.argtypes = []
            lib.voicevox_make_default_tts_options.restype  = _TtsOpts
            lib.voicevox_synthesizer_tts.argtypes = [
                ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32,
                _TtsOpts,
                ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_void_p)]
            lib.voicevox_synthesizer_tts.restype  = ctypes.c_int32
            lib.voicevox_wav_free.argtypes = [ctypes.c_void_p]
            lib.voicevox_wav_free.restype  = None
            lib.voicevox_synthesizer_delete.argtypes = [ctypes.c_void_p]
            lib.voicevox_synthesizer_delete.restype  = None
            lib.voicevox_open_jtalk_rc_delete.argtypes = [ctypes.c_void_p]
            lib.voicevox_open_jtalk_rc_delete.restype  = None

            ort_ptr = ctypes.c_void_p()
            rc = lib.voicevox_onnxruntime_load_once(
                _LoadOrtOpts(filename=ort_path.encode()), ctypes.byref(ort_ptr))
            if rc != 0:
                self._error = f"ONNX Runtime ロード失敗 rc={rc}"
                return

            dict_dir = os.path.join(vv, "dict")
            if os.path.isdir(dict_dir):
                for name in os.listdir(dict_dir):
                    sub = os.path.join(dict_dir, name)
                    if os.path.isdir(sub):
                        dict_dir = sub
                        break
            jtalk_ptr = ctypes.c_void_p()
            rc = lib.voicevox_open_jtalk_rc_new(
                dict_dir.encode(), ctypes.byref(jtalk_ptr))
            if rc != 0:
                self._error = f"OpenJTalk 初期化失敗 rc={rc}"
                return

            init_opts = lib.voicevox_make_default_initialize_options()
            init_opts.acceleration_mode = 1
            init_opts.cpu_num_threads   = 2
            synth_ptr = ctypes.c_void_p()
            rc = lib.voicevox_synthesizer_new(
                ort_ptr, jtalk_ptr, init_opts, ctypes.byref(synth_ptr))
            if rc != 0:
                lib.voicevox_open_jtalk_rc_delete(jtalk_ptr)
                self._error = f"Synthesizer 作成失敗 rc={rc}"
                return

            vvms_dir = os.path.join(vv, "models", "vvms")
            loaded = 0
            for vvm in _glob.glob(os.path.join(vvms_dir, "*.vvm")):
                m_ptr = ctypes.c_void_p()
                if lib.voicevox_voice_model_file_open(
                        vvm.encode(), ctypes.byref(m_ptr)) == 0:
                    lib.voicevox_synthesizer_load_voice_model(synth_ptr, m_ptr)
                    lib.voicevox_voice_model_file_delete(m_ptr)
                    loaded += 1
            if loaded == 0:
                lib.voicevox_synthesizer_delete(synth_ptr)
                lib.voicevox_open_jtalk_rc_delete(jtalk_ptr)
                self._error = f"VVM モデルなし: {vvms_dir}"
                return

            self._lib     = lib
            self._synth   = synth_ptr
            self._jtalk   = jtalk_ptr
            self._TtsOpts = _TtsOpts
            self._ready   = True
        except Exception as e:
            self._error = str(e)

    def synthesize(self, text: str):
        """(samples_float32_ndarray, sample_rate) または None を返す"""
        if not self._ready:
            return None
        import ctypes
        import numpy as np
        opts = self._lib.voicevox_make_default_tts_options()
        opts.enable_interrogative_upspeak = False
        wav_len = ctypes.c_size_t(0)
        wav_ptr = ctypes.c_void_p(0)
        rc = self._lib.voicevox_synthesizer_tts(
            self._synth, text.encode("utf-8"),
            ctypes.c_uint32(self.style_id), opts,
            ctypes.byref(wav_len), ctypes.byref(wav_ptr))
        if rc != 0 or not wav_ptr:
            return None
        data = bytes(ctypes.string_at(wav_ptr.value, wav_len.value))
        self._lib.voicevox_wav_free(wav_ptr)
        pcm, sr = self._parse_wav(data)
        if pcm is None:
            return None
        return pcm.astype(np.float32) / 32768.0, sr

    @staticmethod
    def _parse_wav(data: bytes):
        import struct
        import numpy as np
        if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
            return None, None
        pos, sr, bits, pcm = 12, None, None, None
        while pos + 8 <= len(data):
            tag  = data[pos:pos+4]
            size = struct.unpack_from("<I", data, pos+4)[0]
            if tag == b"fmt " and size >= 16:
                sr   = struct.unpack_from("<I", data, pos+12)[0]
                bits = struct.unpack_from("<H", data, pos+22)[0]
            elif tag == b"data":
                pcm  = data[pos+8:pos+8+size]
            pos += 8 + size + (size % 2)
        if pcm is None or sr is None or bits != 16:
            return None, None
        return np.frombuffer(pcm, dtype=np.int16).copy(), sr


# ============================================================
# config.ini 読み込み
# ============================================================

def load_config(base_dir: str) -> dict:
    defaults = {
        "tts_language": "auto",
        "tts_speaking_rate": 1.0,
        "translation_enabled": True,
        "target_language": "Japanese",
        "performance_preset": "Medium",
        "demo_mode": False,
        "init_delay_ms": 10000,
    }
    ini = os.path.join(base_dir, "config.ini")
    if not os.path.exists(ini):
        return defaults
    p = configparser.RawConfigParser()
    p.read(ini, encoding="utf-8")

    def gs(s, k, fb): return p.get(s, k)        if p.has_option(s, k) else fb
    def gi(s, k, fb): return p.getint(s, k)     if p.has_option(s, k) else fb
    def gf(s, k, fb): return p.getfloat(s, k)   if p.has_option(s, k) else fb

    return {
        "tts_language":          gs("TTS",         "Language",           "auto"),
        "tts_speaking_rate":     gf("TTS",         "SpeakingRate",       1.0),
        "tts_voicevox_style_id": gi("TTS",         "VoicevoxStyleId",    3),
        "translation_enabled":gi("Translation", "Enabled",            1) != 0,
        "target_language":    gs("Translation", "TargetLanguage",     "Japanese"),
        "performance_preset": gs("Translation", "PerformancePreset",  "Medium"),
        "demo_mode":          gi("Overlay",     "DemoMode",           0) != 0,
        "init_delay_ms":      gi("General",     "InitDelayMs",        10000),
        "log_file_path":      gs("General",     "LogFilePath",        ""),
    }


# ============================================================
# chat_log.txt パーサ
# ============================================================

_CHAT_RE = _re.compile(r'^\[[\d\- :]+\]\s+\[([^\]]+)\]\s+([^:]+):\s+(.+)$')

def parse_chat_log(path: str) -> list:
    msgs = []
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = _CHAT_RE.match(line.strip())
                if m:
                    msgs.append({"channel": m.group(1),
                                 "sender":  m.group(2).strip(),
                                 "message": m.group(3).strip()})
    except Exception:
        pass
    return msgs


# ============================================================
# メイン UI
# ============================================================

PRESETS = [
    ("日本語",  "こんにちは！今日の戦況はいかがですか？装甲部隊が前進しています。"),
    ("English", "Hello! How is the front line today? Armoured units are advancing."),
    ("Русский", "Привет! Как обстановка на фронте? Бронеколонна продвигается вперёд."),
    ("中文",    "你好！前线情况如何？装甲部队正在推进中。"),
    ("한국어",  "안녕하세요! 오늘 전선 상황은 어떤가요? 기갑 부대가 전진하고 있습니다."),
]

LANG_NAMES = {"en": "英語", "ja": "日本語", "ru": "ロシア語", "zh": "中国語", "ko": "韓国語"}


class App:
    def __init__(self, root: tk.Tk, base_dir: str):
        self.root      = root
        self.base_dir  = base_dir
        self.tts_dir   = _find_tts_dir(base_dir)
        self.cfg       = load_config(base_dir)
        self.engine    = TtsEngine(self.tts_dir)
        self.vv_engine = VoicevoxEngine(self.tts_dir,
                                        self.cfg.get("tts_voicevox_style_id", 3))
        self.log_msgs: list = []
        self._play_thread: threading.Thread | None = None
        self._stop_flag = threading.Event()

        root.title("TTS テストツール (Sherpa-ONNX) — ChatTranslator")
        root.geometry("900x660")
        root.minsize(740, 520)
        try:
            from ctypes import windll
            windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass

        self._build_ui()
        self._check_deps()

    # ----------------------------------------------------------
    # 依存チェック
    # ----------------------------------------------------------

    def _check_deps(self):
        import sys
        missing = []
        errors  = []
        if not self.engine.available:
            missing.append("sherpa-onnx")
            if self.engine.import_error:
                errors.append(f"sherpa-onnx: {self.engine.import_error}")
        try:
            import sounddevice  # noqa: F401
        except Exception as e:
            missing.append("sounddevice")
            errors.append(f"sounddevice: {e}")

        if missing:
            deps = " ".join(missing)
            py   = sys.executable
            err_detail = "\n".join(errors) if errors else ""
            self.status_var.set(
                f"[未インストール] {deps} — 下の情報欄を確認してください")
            self._set_info(
                f"パッケージが不足しています: {deps}\n\n"
                f"以下のコマンドをコピーして実行してから、このウィンドウを再起動してください:\n\n"
                f'"{py}" -m pip install {deps}\n\n'
                f"--- 使用中の Python ---\n{py}\n\n"
                + (f"--- エラー詳細 ---\n{err_detail}\n\n" if err_detail else "")
                + "モデルが未セットアップの場合は先に install.ps1 を実行してください。")
        else:
            self._refresh_model_status()

    # ----------------------------------------------------------
    # UI 構築
    # ----------------------------------------------------------

    def _build_ui(self):
        paned = ttk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        paned.pack(fill=tk.BOTH, expand=True, padx=6, pady=(6, 2))

        left  = ttk.Frame(paned, width=240)
        right = ttk.Frame(paned)
        paned.add(left,  weight=1)
        paned.add(right, weight=3)

        self._build_left(left)
        self._build_right(right)

        sb = ttk.Frame(self.root, relief=tk.SUNKEN, padding=(4, 1))
        sb.pack(fill=tk.X, side=tk.BOTTOM)
        self.status_var = tk.StringVar(value="起動中...")
        ttk.Label(sb, textvariable=self.status_var, anchor=tk.W).pack(
            side=tk.LEFT, fill=tk.X, expand=True)

    def _build_left(self, parent):
        # config.ini
        cf = ttk.LabelFrame(parent, text=" config.ini ", padding=8)
        cf.pack(fill=tk.X, padx=4, pady=4)
        c = self.cfg
        rows = [
            ("TTS言語",     c["tts_language"]),
            ("読み上げ速度", f'{c["tts_speaking_rate"]:.2f}'),
            ("翻訳先",      c["target_language"]),
            ("プリセット",  c["performance_preset"]),
        ]
        for label, val in rows:
            row = ttk.Frame(cf)
            row.pack(fill=tk.X, pady=1)
            ttk.Label(row, text=label + ":", width=11, anchor=tk.E,
                      font=("", 8)).pack(side=tk.LEFT)
            ttk.Label(row, text=val, foreground="#005599",
                      font=("", 8, "bold")).pack(side=tk.LEFT, padx=(3, 0))

        # モデルステータス
        mf = ttk.LabelFrame(parent, text=" インストール済みモデル ", padding=8)
        mf.pack(fill=tk.X, padx=4, pady=4)
        self._model_labels: dict[str, tk.Label] = {}
        for lang in TtsEngine.LANGS:
            row = ttk.Frame(mf)
            row.pack(fill=tk.X, pady=2)
            ttk.Label(row, text=f"{lang.upper()} {LANG_NAMES[lang]}:",
                      width=14, anchor=tk.W, font=("", 8)).pack(side=tk.LEFT)
            lbl = ttk.Label(row, text="確認中…", font=("", 8))
            lbl.pack(side=tk.LEFT)
            self._model_labels[lang] = lbl

        ttk.Button(mf, text="再確認", width=8,
                   command=self._refresh_model_status).pack(pady=(6, 0))

        # 情報テキスト
        inf = ttk.LabelFrame(parent, text=" 情報 ", padding=6)
        inf.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)
        self.info_text = tk.Text(inf, wrap=tk.WORD, font=("", 8),
                                 state=tk.DISABLED, background="#f8f8f8",
                                 foreground="#333", relief=tk.FLAT)
        self.info_text.pack(fill=tk.BOTH, expand=True)

    def _build_right(self, parent):
        # パラメータ
        pf = ttk.LabelFrame(parent, text=" TTSパラメータ ", padding=8)
        pf.pack(fill=tk.X, padx=4, pady=4)

        r1 = ttk.Frame(pf)
        r1.pack(fill=tk.X, pady=3)
        ttk.Label(r1, text="言語モード:").grid(row=0, column=0, sticky=tk.E, padx=(0, 4))
        self.lang_var = tk.StringVar(value=self.cfg["tts_language"])
        lcb = ttk.Combobox(r1, textvariable=self.lang_var,
                           values=["auto", "ja", "en", "ru", "zh", "ko"],
                           width=6, state="readonly")
        lcb.grid(row=0, column=1, sticky=tk.W)
        lcb.bind("<<ComboboxSelected>>", lambda _: self._update_detected())

        ttk.Label(r1, text="読み上げ速度:").grid(row=0, column=2, sticky=tk.E, padx=(16, 4))
        self.rate_var = tk.DoubleVar(value=self.cfg["tts_speaking_rate"])
        scl = ttk.Scale(r1, from_=0.5, to=2.0, variable=self.rate_var,
                        orient=tk.HORIZONTAL, length=140,
                        command=lambda _: self._on_rate_change())
        scl.grid(row=0, column=3)
        self.rate_lbl = ttk.Label(r1, text=f"{self.cfg['tts_speaking_rate']:.2f}", width=4)
        self.rate_lbl.grid(row=0, column=4, padx=(4, 0))

        r2 = ttk.Frame(pf)
        r2.pack(fill=tk.X, pady=3)
        ttk.Label(r2, text="検出言語:").grid(row=0, column=0, sticky=tk.E, padx=(0, 4))
        self.detected_lbl = ttk.Label(r2, text="—", foreground="#555")
        self.detected_lbl.grid(row=0, column=1, sticky=tk.W)

        # テキスト入力
        tf = ttk.LabelFrame(parent, text=" 発話テキスト ", padding=8)
        tf.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        self.text_box = scrolledtext.ScrolledText(
            tf, height=5, wrap=tk.WORD, font=("", 10))
        self.text_box.pack(fill=tk.BOTH, expand=True)
        self.text_box.bind("<KeyRelease>", lambda _: self._update_detected())

        pr = ttk.Frame(tf)
        pr.pack(fill=tk.X, pady=(6, 0))
        ttk.Label(pr, text="プリセット:", font=("", 8)).pack(side=tk.LEFT)
        for label, txt in PRESETS:
            ttk.Button(pr, text=label, width=7,
                       command=lambda t=txt: self._set_preset(t)
                       ).pack(side=tk.LEFT, padx=2)

        lr = ttk.Frame(tf)
        lr.pack(fill=tk.X, pady=(4, 0))
        ttk.Label(lr, text="ログ:", font=("", 8)).pack(side=tk.LEFT)
        self.log_status_lbl = ttk.Label(lr, text="未読み込み",
                                        foreground="#888", font=("", 8))
        self.log_status_lbl.pack(side=tk.LEFT, padx=(4, 8))
        ttk.Button(lr, text="読み込む", width=8,
                   command=self._load_log).pack(side=tk.LEFT)
        ttk.Separator(lr, orient=tk.VERTICAL).pack(
            side=tk.LEFT, fill=tk.Y, padx=6, pady=2)
        for label, cat in [("短≤20", "short"), ("普21-60", "normal"), ("長61+", "long")]:
            ttk.Button(lr, text=label, width=7,
                       command=lambda c=cat: self._pick_log_sample(c)
                       ).pack(side=tk.LEFT, padx=2)

        # コントロール
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
        self.gen_lbl = ttk.Label(cf2, text="", foreground="#888", font=("", 8))
        self.gen_lbl.pack(side=tk.LEFT, padx=8)

    # ----------------------------------------------------------
    # モデル状態確認
    # ----------------------------------------------------------

    def _refresh_model_status(self):
        installed = []
        for lang, lbl in self._model_labels.items():
            if lang == "ja" and self.vv_engine.ready:
                lbl.config(text="OK (VOICEVOX)", foreground="#007700")
                installed.append(lang)
            elif self.engine.model_installed(lang):
                if lang == "ja" and self.vv_engine.installed():
                    lbl.config(text="OK (Sherpa) ※VV初期化失敗", foreground="#888800")
                else:
                    lbl.config(text="OK", foreground="#007700")
                installed.append(lang)
            elif lang == "ja" and self.vv_engine.installed():
                lbl.config(text="初期化失敗", foreground="#cc0000")
            else:
                lbl.config(text="未インストール", foreground="#999999")

        if installed:
            self.status_var.set(
                f"利用可能: {', '.join(installed)}  |  "
                f"tts_dir: {self.tts_dir}")
            self._set_info(
                f"インストール済み言語: {', '.join(installed)}\n\n"
                f"モデルディレクトリ:\n{self.tts_dir}\n\n"
                f"他の言語を追加する場合:\n"
                f"  .\\install.ps1 -LangsOnly <lang>\n"
                f"  例: -LangsOnly ko")
        else:
            self.status_var.set("モデルが見つかりません — install.ps1 を実行してください")
            self._set_info(
                f"モデルが見つかりません。\n\n"
                f"以下を実行してモデルをダウンロードしてください:\n"
                f"  .\\install.ps1\n\n"
                f"インストール先:\n{self.tts_dir}")

    # ----------------------------------------------------------
    # テキスト・パラメータ更新
    # ----------------------------------------------------------

    def _on_rate_change(self):
        self.rate_lbl.config(text=f"{self.rate_var.get():.2f}")

    def _update_detected(self):
        text = self.text_box.get("1.0", tk.END).strip()
        if not text:
            self.detected_lbl.config(text="—")
            return
        mode = self.lang_var.get()
        lang = detect_language(text) if mode == "auto" else mode
        name = LANG_NAMES.get(lang, lang)
        if lang == "ja" and (self.vv_engine.ready or self.vv_engine.installed()):
            ok = True
        else:
            ok = self.engine.model_installed(lang)
        icon  = "✓" if ok else "✗ モデル未インストール"
        color = "#007700" if ok else "#cc0000"
        self.detected_lbl.config(text=f"{lang} ({name})  {icon}", foreground=color)

    def _set_preset(self, text: str):
        self.text_box.delete("1.0", tk.END)
        self.text_box.insert(tk.END, text)
        self._update_detected()

    def _clear_text(self):
        self.text_box.delete("1.0", tk.END)
        self._update_detected()

    def _set_info(self, text: str):
        self.info_text.config(state=tk.NORMAL)
        self.info_text.delete("1.0", tk.END)
        self.info_text.insert(tk.END, text)
        self.info_text.config(state=tk.DISABLED)

    # ----------------------------------------------------------
    # ログサンプル
    # ----------------------------------------------------------

    def _resolve_log_path(self) -> str:
        game_root = os.path.normpath(os.path.join(self.base_dir, "..", ".."))
        candidates = [
            os.path.join(game_root, "War", "Binaries", "Win64", "chat_log.txt"),
            os.path.join(self.base_dir, "chat_log.txt"),
        ]
        for p in candidates:
            if os.path.exists(p):
                return p
        return candidates[0]

    def _load_log(self):
        path = self._resolve_log_path()
        if not os.path.exists(path):
            self.log_status_lbl.config(
                text=f"見つかりません: {os.path.basename(path)}", foreground="#cc0000")
            return
        self.log_msgs = parse_chat_log(path)
        n      = len(self.log_msgs)
        shorts = sum(1 for m in self.log_msgs if len(m["message"]) <= 20)
        norms  = sum(1 for m in self.log_msgs if 20 < len(m["message"]) <= 60)
        longs  = sum(1 for m in self.log_msgs if len(m["message"]) > 60)
        self.log_status_lbl.config(
            text=f"{n}件 (短{shorts}/普{norms}/長{longs})", foreground="#005599")

    def _pick_log_sample(self, category: str):
        import random
        if not self.log_msgs:
            self._load_log()
            if not self.log_msgs:
                self.status_var.set("ログが空です。ゲームプレイ後に chat_log.txt が生成されます。")
                return
        if   category == "short":  pool = [m for m in self.log_msgs if len(m["message"]) <= 20]
        elif category == "normal": pool = [m for m in self.log_msgs if 20 < len(m["message"]) <= 60]
        else:                      pool = [m for m in self.log_msgs if len(m["message"]) > 60]
        if not pool:
            self.status_var.set("該当するメッセージがありません")
            return
        entry = random.choice(pool)
        self._set_preset(entry["message"])
        self.status_var.set(
            f"[{entry['channel']}] {entry['sender']}: "
            f"{entry['message'][:50]}{'…' if len(entry['message'])>50 else ''}")

    # ----------------------------------------------------------
    # 発話
    # ----------------------------------------------------------

    def _speak(self):
        text = self.text_box.get("1.0", tk.END).strip()
        if not text:
            self.status_var.set("テキストを入力してください")
            return

        mode  = self.lang_var.get()
        lang  = detect_language(text) if mode == "auto" else mode
        speed = float(self.rate_var.get())

        # VOICEVOX (日本語) — C++ 同様に Sherpa より優先
        # sherpa-onnx 未インストールでも VOICEVOX があれば動作する
        if lang == "ja" and self.vv_engine.ready:
            self._stop_flag.clear()
            self.speak_btn.config(state=tk.DISABLED)
            self.stop_btn.config(state=tk.NORMAL)
            self.gen_lbl.config(text="合成中 (VOICEVOX)…")
            self.status_var.set(
                f"合成中 (VOICEVOX ずんだもん)  styleId={self.vv_engine.style_id}")

            def run_vv():
                try:
                    import sounddevice as sd
                    import numpy as np
                    result = self.vv_engine.synthesize(text)
                    if result is None:
                        self.root.after(0, lambda: self._on_done("VOICEVOX 合成失敗"))
                        return
                    samples, sr = result
                    self.root.after(0, lambda: self.gen_lbl.config(text="再生中 (VOICEVOX)…"))
                    arr = np.array(samples, dtype=np.float32)
                    sd.play(arr, sr, blocking=False)
                    import time
                    while True:
                        if self._stop_flag.is_set():
                            sd.stop()
                            break
                        try:
                            if not sd.get_stream().active:
                                break
                        except Exception:
                            break
                        time.sleep(0.05)
                    msg = f"完了 (VOICEVOX)  {len(samples)/sr:.1f}秒  sr={sr}Hz"
                except Exception as e:
                    msg = f"エラー: {e}"
                self.root.after(0, lambda m=msg: self._on_done(m))

            self._play_thread = threading.Thread(target=run_vv, daemon=True)
            self._play_thread.start()
            return

        # Sherpa-ONNX パス (C++ 同様: モデルなし → EN フォールバック)
        if not self.engine.available:
            import sys
            self.status_var.set(
                f"sherpa-onnx 未インストール — "
                f'"{sys.executable}" -m pip install sherpa-onnx')
            return

        if not self.engine.model_installed(lang):
            if lang != "en" and self.engine.model_installed("en"):
                self.status_var.set(f"[{lang}] モデルなし → EN フォールバック")
                lang = "en"
            else:
                self.status_var.set(
                    f"[{lang}] モデル未インストール  →  .\\install.ps1 -LangsOnly {lang}")
                return

        self._stop_flag.clear()
        self.speak_btn.config(state=tk.DISABLED)
        self.stop_btn.config(state=tk.NORMAL)
        self.gen_lbl.config(text="合成中…")
        self.status_var.set(f"合成中  lang={lang}  speed={speed:.2f}")

        def run():
            try:
                import sounddevice as sd
                import numpy as np

                result = self.engine.generate(text, lang, speed)
                if result is None:
                    self.root.after(0, lambda: self._on_done("モデル読み込み失敗"))
                    return

                samples, sr, actual_lang = result
                self.root.after(0, lambda: self.gen_lbl.config(text="再生中…"))

                arr = np.array(samples, dtype=np.float32)
                sd.play(arr, sr, blocking=False)
                import time
                while True:
                    if self._stop_flag.is_set():
                        sd.stop()
                        break
                    try:
                        if not sd.get_stream().active:
                            break
                    except Exception:
                        break
                    time.sleep(0.05)

                fb_tag = f" (EN fallback)" if actual_lang != lang else ""
                msg = f"完了  lang={actual_lang}{fb_tag}  {len(samples)/sr:.1f}秒  sr={sr}Hz"
            except Exception as e:
                import sys
                py = sys.executable
                msg = f"エラー: {e} | Python: {py}"
            self.root.after(0, lambda m=msg: self._on_done(m))

        self._play_thread = threading.Thread(target=run, daemon=True)
        self._play_thread.start()

    def _on_done(self, msg: str):
        self.speak_btn.config(state=tk.NORMAL)
        self.stop_btn.config(state=tk.DISABLED)
        self.gen_lbl.config(text="")
        self.status_var.set(msg)

    def _stop(self):
        self._stop_flag.set()
        try:
            import sounddevice as sd
            sd.stop()
        except Exception:
            pass
        self.stop_btn.config(state=tk.DISABLED)
        self.speak_btn.config(state=tk.NORMAL)
        self.gen_lbl.config(text="")
        self.status_var.set("停止しました")


# ============================================================
# エントリポイント
# ============================================================

def main():
    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = tk.Tk()
    App(root, base_dir)
    root.mainloop()


if __name__ == "__main__":
    main()
