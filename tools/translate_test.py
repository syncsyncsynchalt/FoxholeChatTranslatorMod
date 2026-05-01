#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
翻訳テストツール - ゲーム起動不要
Ollamaサーバー管理・リソースモニター・翻訳テスト統合ツール
C++ translate.cpp と同じプロンプト・パラメータ・プリセットを再現。
"""

import tkinter as tk
from tkinter import ttk, scrolledtext
import configparser
import threading
import os
import re as _re
import json
import time
import urllib.request
import urllib.error
import subprocess
import ctypes
import ctypes.wintypes as _wt
import collections

# ============================================================
# 定数
# ============================================================

PRESETS = {
    "High":   {"model": "gemma3:4b", "num_ctx": 512, "num_thread": 0, "temperature": 0.1},
    "Medium": {"model": "gemma3:1b", "num_ctx": 256, "num_thread": 0, "temperature": 0.1},
    "Low":    {"model": "gemma3:1b",   "num_ctx": 128, "num_thread": 2, "temperature": 0.1},
}

_NUM_CPUS = os.cpu_count() or 1

# ============================================================
# 設定読み込み
# ============================================================

def load_config(base_dir: str) -> dict:
    defaults = {
        "translation_enabled": True,
        "target_language":     "Japanese",
        "ollama_endpoint":     "http://localhost:11434/api/generate",
        "performance_preset":  "Medium",
        "enable_console":      True,
        "init_delay_ms":       10000,
        "demo_mode":           False,
        "tts_language":        "auto",
        "log_file_path":       "",
    }
    ini_path = os.path.join(base_dir, "config.ini")
    if not os.path.exists(ini_path):
        return defaults
    p = configparser.RawConfigParser()
    p.read(ini_path, encoding="utf-8")

    def gs(sec, key, fb): return p.get(sec, key) if p.has_option(sec, key) else fb
    def gi(sec, key, fb): return p.getint(sec, key) if p.has_option(sec, key) else fb

    return {
        "translation_enabled": gi("Translation", "Enabled",           1) != 0,
        "target_language":     gs("Translation", "TargetLanguage",    defaults["target_language"]),
        "ollama_endpoint":     gs("Translation", "OllamaEndpoint",    defaults["ollama_endpoint"]),
        "performance_preset":  gs("Translation", "PerformancePreset", defaults["performance_preset"]),
        "enable_console":      gi("General",     "EnableConsole",     1) != 0,
        "init_delay_ms":       gi("General",     "InitDelayMs",       10000),
        "demo_mode":           gi("Overlay",     "DemoMode",          0) != 0,
        "tts_language":        gs("TTS",         "Language",          defaults["tts_language"]),
        "log_file_path":       gs("General",     "LogFilePath",       ""),
    }

# ============================================================
# chat_log.txt パーサ
# ============================================================

_CHAT_LOG_RE = _re.compile(r'^\[[\d\- :]+\]\s+\[([^\]]+)\]\s+([^:]+):\s+(.+)$')

def parse_chat_log(path: str) -> list:
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
# Ollama API
# ============================================================

def ollama_version(endpoint: str, timeout: float = 3.0):
    try:
        url = endpoint.rstrip("/").rsplit("/api/", 1)[0] + "/api/version"
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return json.loads(r.read()).get("version", "unknown")
    except Exception:
        return None


def do_translate(endpoint: str, model: str, target_lang: str,
                 num_ctx: int, num_thread: int, text: str,
                 num_predict: int = 120, temperature: float = 0.8) -> tuple:
    prompt = (
        "You are a translator. The user sends a chat message in any language."
        f" Translate it to {target_lang}."
        " Output ONLY the translated text, nothing else. No explanations."
        f" If the message is already in {target_lang}, output it unchanged."
        f"\n\n{text}"
    )
    options = {"num_ctx": num_ctx, "num_predict": num_predict,
               "temperature": temperature}
    if num_thread != 0:
        options["num_thread"] = num_thread

    body = json.dumps({
        "model": model, "prompt": prompt, "stream": False, "options": options,
    }).encode("utf-8")
    req = urllib.request.Request(
        endpoint, data=body,
        headers={"Content-Type": "application/json"}, method="POST"
    )
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            data = json.loads(r.read())
    except urllib.error.URLError as e:
        return f"(接続失敗: {e.reason})", int((time.time() - t0) * 1000), 0
    except Exception as e:
        return f"(エラー: {e})", int((time.time() - t0) * 1000), 0

    elapsed = int((time.time() - t0) * 1000)
    if "error" in data:
        err = data["error"]
        if "memory" in err.lower():
            return "(メモリ不足: PerformancePreset を Low に変更してください)", elapsed, 0
        if "not found" in err.lower():
            return f"(モデル未インストール: ollama pull {model})", elapsed, 0
        return f"(Ollamaエラー: {err})", elapsed, 0

    return data.get("response", "").rstrip(), elapsed, data.get("eval_count", 0)

# ============================================================
# Windows プロセス情報 (ctypes)
# ============================================================

_k32   = ctypes.windll.kernel32
_psapi = ctypes.windll.psapi

_PROCESS_QUERY_LIMITED = 0x1000
_PROCESS_VM_READ       = 0x0010


class _FILETIME(ctypes.Structure):
    _fields_ = [("lo", _wt.DWORD), ("hi", _wt.DWORD)]


class _PMC_EX(ctypes.Structure):
    _fields_ = [
        ("cb",                         _wt.DWORD),
        ("PageFaultCount",             _wt.DWORD),
        ("PeakWorkingSetSize",         ctypes.c_size_t),
        ("WorkingSetSize",             ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage",    ctypes.c_size_t),
        ("QuotaPagedPoolUsage",        ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage",     ctypes.c_size_t),
        ("PagefileUsage",              ctypes.c_size_t),
        ("PeakPagefileUsage",          ctypes.c_size_t),
        ("PrivateUsage",               ctypes.c_size_t),
    ]


class _MEMSTATEX(ctypes.Structure):
    _fields_ = [
        ("dwLength",                _wt.DWORD),
        ("dwMemoryLoad",            _wt.DWORD),
        ("ullTotalPhys",            ctypes.c_uint64),
        ("ullAvailPhys",            ctypes.c_uint64),
        ("ullTotalPageFile",        ctypes.c_uint64),
        ("ullAvailPageFile",        ctypes.c_uint64),
        ("ullTotalVirtual",         ctypes.c_uint64),
        ("ullAvailVirtual",         ctypes.c_uint64),
        ("ullAvailExtendedVirtual", ctypes.c_uint64),
    ]


def _total_ram_mb() -> int:
    try:
        ms = _MEMSTATEX()
        ms.dwLength = ctypes.sizeof(ms)
        _k32.GlobalMemoryStatusEx(ctypes.byref(ms))
        return int(ms.ullTotalPhys // (1024 * 1024))
    except Exception:
        return 16384


def _proc_stats(pid: int):
    """(cpu_100ns, mem_bytes) または None"""
    h = _k32.OpenProcess(_PROCESS_QUERY_LIMITED | _PROCESS_VM_READ, False, pid)
    if not h:
        return None
    try:
        ct = _FILETIME(); et = _FILETIME(); kt = _FILETIME(); ut = _FILETIME()
        _k32.GetProcessTimes(h, ctypes.byref(ct), ctypes.byref(et),
                              ctypes.byref(kt), ctypes.byref(ut))
        cpu = ((kt.hi << 32) | kt.lo) + ((ut.hi << 32) | ut.lo)
        mc = _PMC_EX(); mc.cb = ctypes.sizeof(mc)
        _psapi.GetProcessMemoryInfo(h, ctypes.byref(mc), mc.cb)
        return cpu, mc.WorkingSetSize
    finally:
        _k32.CloseHandle(h)

# ============================================================
# GPU モニター (nvidia-smi)
# ============================================================

_nvidia_ok = None  # None=未確認, True/False


def _query_gpu():
    """(gpu_pct, mem_used_mb, mem_total_mb) または None"""
    global _nvidia_ok
    if _nvidia_ok is False:
        return None
    try:
        r = subprocess.run(
            ["nvidia-smi",
             "--query-gpu=utilization.gpu,memory.used,memory.total",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=5,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        if r.returncode == 0:
            parts = [x.strip() for x in r.stdout.strip().split(",")]
            if len(parts) >= 3:
                _nvidia_ok = True
                return int(parts[0]), int(parts[1]), int(parts[2])
        _nvidia_ok = False
    except Exception:
        _nvidia_ok = False
    return None

# ============================================================
# リソースモニタースレッド
# ============================================================

_TOTAL_RAM_MB = _total_ram_mb()


class _Monitor(threading.Thread):
    INTERVAL = 2.0

    def __init__(self):
        super().__init__(daemon=True, name="ResourceMonitor")
        self._stop_evt  = threading.Event()
        self._lock      = threading.Lock()
        self._pid       = None
        self._prev_cpu  = 0
        self._prev_wall = time.monotonic()
        self._data      = dict(
            alive=False, cpu_pct=0.0, mem_mb=0,
            gpu_pct=-1, gpu_mem_mb=-1, gpu_mem_total=-1,
        )

    def set_pid(self, pid):
        with self._lock:
            if self._pid != pid:
                self._pid       = pid
                self._prev_cpu  = 0
                self._prev_wall = time.monotonic()

    def get_data(self) -> dict:
        with self._lock:
            return dict(self._data)

    def stop(self):
        self._stop_evt.set()

    def run(self):
        while not self._stop_evt.is_set():
            self._sample()
            self._stop_evt.wait(self.INTERVAL)

    def _sample(self):
        with self._lock:
            pid = self._pid

        cpu_pct = 0.0
        mem_mb  = 0
        alive   = False

        if pid:
            stats = _proc_stats(pid)
            if stats:
                cpu_100ns, mem_bytes = stats
                now = time.monotonic()
                dt  = now - self._prev_wall
                if dt > 0.1 and self._prev_cpu > 0:
                    delta   = cpu_100ns - self._prev_cpu
                    cpu_pct = delta / (dt * _NUM_CPUS * 10_000_000) * 100
                    cpu_pct = max(0.0, min(100.0, cpu_pct))
                self._prev_cpu  = cpu_100ns
                self._prev_wall = now
                mem_mb = mem_bytes // (1024 * 1024)
                alive  = True

        gpu = _query_gpu()

        with self._lock:
            self._data.update(
                alive=alive, cpu_pct=cpu_pct, mem_mb=mem_mb,
                gpu_pct=gpu[0] if gpu else -1,
                gpu_mem_mb=gpu[1] if gpu else -1,
                gpu_mem_total=gpu[2] if gpu else -1,
            )

# ============================================================
# サーバー管理ユーティリティ
# ============================================================

def _find_bundled_ollama(base_dir: str):
    path = os.path.join(base_dir, "tools", "ollama", "ollama.exe")
    return path if os.path.isfile(path) else None


def _find_running_pid():
    try:
        r = subprocess.run(
            ["tasklist", "/fi", "IMAGENAME eq ollama.exe", "/fo", "csv", "/nh"],
            capture_output=True, text=True, timeout=5,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        for line in r.stdout.strip().splitlines():
            parts = [p.strip('"') for p in line.split('","')]
            if len(parts) >= 2 and parts[0].lower() == "ollama.exe":
                return int(parts[1])
    except Exception:
        pass
    return None


def _launch_ollama(exe_path: str):
    models_dir = os.path.join(os.path.dirname(exe_path), "models")
    os.makedirs(models_dir, exist_ok=True)
    env = os.environ.copy()
    env["OLLAMA_MODELS"] = models_dir
    try:
        return subprocess.Popen(
            [exe_path, "serve"],
            env=env,
            cwd=os.path.dirname(exe_path),
            creationflags=subprocess.CREATE_NO_WINDOW,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except Exception:
        return None

# ============================================================
# リソースバー ウィジェット
# ============================================================

def _dim_color(hex_color: str, factor: float = 0.25) -> str:
    h = hex_color.lstrip("#")
    r = int(int(h[0:2], 16) * factor)
    g = int(int(h[2:4], 16) * factor)
    b = int(int(h[4:6], 16) * factor)
    return f"#{r:02x}{g:02x}{b:02x}"


class _Graph(tk.Frame):
    _H       = 50
    _SAMPLES = 60   # 60サンプル × 2秒 = 約2分

    def __init__(self, parent, label: str, color: str = "#4a9eff",
                 traffic_light: bool = True):
        super().__init__(parent)
        self._color = color
        self._tl    = traffic_light
        self._fill  = _dim_color(color)
        self._vals  = collections.deque([None] * self._SAMPLES, maxlen=self._SAMPLES)

        row = tk.Frame(self)
        row.pack(fill=tk.X)
        ttk.Label(row, text=label, font=("", 8), width=9,
                  anchor=tk.W).pack(side=tk.LEFT)
        self._lbl = ttk.Label(row, text="—", font=("", 8), foreground="#555")
        self._lbl.pack(side=tk.RIGHT)

        self._cv = tk.Canvas(self, height=self._H, bg="#1e1e2e",
                              highlightthickness=1, highlightbackground="#555")
        self._cv.pack(fill=tk.X, pady=(1, 4))
        self._cv.bind("<Configure>", lambda e: self._redraw())

    def set(self, pct: float, text: str = ""):
        pct = max(0.0, min(100.0, float(pct)))
        self._vals.append(pct)
        self._lbl.config(text=text or f"{pct:.0f}%")
        self._redraw()

    def set_na(self, text: str = "N/A"):
        self._vals.append(None)
        self._lbl.config(text=text)
        self._redraw()

    def _line_color(self, pct: float) -> str:
        if not self._tl:
            return self._color
        if pct <= 50:
            return "#3ab858"
        if pct <= 80:
            return "#e09020"
        return "#e03030"

    def _redraw(self):
        w = self._cv.winfo_width()
        h = self._cv.winfo_height()
        if w <= 1:
            return
        self._cv.delete("all")

        for g in (25, 50, 75):
            y = int(h * (1 - g / 100))
            self._cv.create_line(0, y, w, y, fill="#2a2a3e", dash=(2, 4))

        vals = list(self._vals)
        n    = len(vals)
        last = next((v for v in reversed(vals) if v is not None), None)
        lc   = self._line_color(last) if last is not None else self._color

        seg = []
        for i, v in enumerate(vals):
            x = int(w * i / (n - 1)) if n > 1 else w - 1
            if v is not None:
                y = max(1, min(h - 1, int(h * (1 - v / 100))))
                seg.append((x, y))
            else:
                if len(seg) >= 2:
                    self._draw_seg(seg, lc, h)
                seg = []
        if len(seg) >= 2:
            self._draw_seg(seg, lc, h)

    def _draw_seg(self, pts, lc, h):
        poly = [(pts[0][0], h), *pts, (pts[-1][0], h)]
        self._cv.create_polygon([c for p in poly for c in p],
                                fill=self._fill, outline="")
        self._cv.create_line([c for p in pts for c in p],
                             fill=lc, width=1)

# ============================================================
# メイン App
# ============================================================

class App:
    def __init__(self, root: tk.Tk, base_dir: str):
        self.root         = root
        self.base_dir     = base_dir
        self.cfg          = load_config(base_dir)
        self.log_messages: list = []
        self._our_proc       = None   # 自前起動した Popen
        self._server_pid     = None
        self._monitor        = _Monitor()
        self._monitor.start()
        self._res_timer      = None
        self._stopped_poll_id = None

        root.title("翻訳テストツール — ChatTranslator")
        root.geometry("1060x760")
        root.minsize(820, 600)
        root.protocol("WM_DELETE_WINDOW", self._on_close)
        try:
            from ctypes import windll
            windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass

        self._build_ui()
        threading.Thread(target=self._bg_init_check, daemon=True).start()

    # ----------------------------------------------------------
    # 起動時チェック
    # ----------------------------------------------------------

    def _bg_init_check(self):
        ver = ollama_version(self.cfg["ollama_endpoint"], timeout=3.0)
        pid = _find_running_pid() if ver else None
        self.root.after(0, lambda v=ver, p=pid: self._on_init_checked(v, p))

    def _on_init_checked(self, ver, pid):
        if ver:
            self._server_pid = pid
            self._monitor.set_pid(pid)
            self._set_server_ui("external", ver, pid)
            self._start_res_loop()
            self._update_status_bar(ver)
            self._start_warmup()
        else:
            if self._our_proc is not None:
                return  # すでに起動処理中
            exe = _find_bundled_ollama(self.base_dir)
            if exe:
                self.status_var.set("Ollama が停止しています — 自動起動します...")
                proc = _launch_ollama(exe)
                if proc:
                    self._our_proc = proc
                    self._set_server_ui("starting")
                    threading.Thread(target=self._wait_for_start, daemon=True).start()
                else:
                    self._set_server_ui("stopped")
                    self._update_status_bar(None)
                    self._start_stopped_poll()
            else:
                self._set_server_ui("stopped")
                self._update_status_bar(None)
                self._start_stopped_poll()

    # ----------------------------------------------------------
    # UI 構築
    # ----------------------------------------------------------

    def _build_ui(self):
        paned = ttk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        paned.pack(fill=tk.BOTH, expand=True, padx=6, pady=(6, 2))
        left = ttk.Frame(paned, width=295)
        paned.add(left, weight=1)
        right = ttk.Frame(paned)
        paned.add(right, weight=3)
        self._build_left(left)
        self._build_right(right)

        sb = ttk.Frame(self.root, relief=tk.SUNKEN, padding=(4, 1))
        sb.pack(fill=tk.X, side=tk.BOTTOM)
        self.status_var = tk.StringVar(value="起動確認中...")
        ttk.Label(sb, textvariable=self.status_var, anchor=tk.W).pack(
            side=tk.LEFT, fill=tk.X, expand=True)
        self._sb_var = tk.StringVar(value="Ollama: 確認中")
        self._sb_lbl = ttk.Label(sb, textvariable=self._sb_var,
                                  anchor=tk.E, foreground="#888")
        self._sb_lbl.pack(side=tk.RIGHT)

    def _build_left(self, parent):
        # config.ini
        cf = ttk.LabelFrame(parent, text=" config.ini ", padding=6)
        cf.pack(fill=tk.X, padx=4, pady=(4, 2))
        c = self.cfg
        for label, val in [
            ("翻訳",       "有効" if c["translation_enabled"] else "無効"),
            ("翻訳先言語", c["target_language"]),
            ("Ollama EP",  c["ollama_endpoint"]),
            ("プリセット", c["performance_preset"]),
        ]:
            row = tk.Frame(cf)
            row.pack(fill=tk.X, pady=1)
            ttk.Label(row, text=label + ":", width=12, anchor=tk.E,
                      font=("", 8)).pack(side=tk.LEFT)
            ttk.Label(row, text=val, foreground="#005599",
                      font=("", 8, "bold")).pack(side=tk.LEFT, padx=(3, 0))

        # サーバー制御
        sf = ttk.LabelFrame(parent, text=" Ollamaサーバー ", padding=8)
        sf.pack(fill=tk.X, padx=4, pady=2)

        dot_row = tk.Frame(sf)
        dot_row.pack(fill=tk.X)
        self._dot_lbl = ttk.Label(dot_row, text="●", foreground="#aaa", font=("", 12))
        self._dot_lbl.pack(side=tk.LEFT)
        self._srv_lbl = ttk.Label(dot_row, text="確認中...", font=("", 9, "bold"))
        self._srv_lbl.pack(side=tk.LEFT, padx=(4, 0))

        self._pid_lbl = ttk.Label(sf, text="", foreground="#666", font=("", 8))
        self._pid_lbl.pack(anchor=tk.W)

        btn_row = tk.Frame(sf)
        btn_row.pack(fill=tk.X, pady=(4, 0))
        self._start_btn = ttk.Button(btn_row, text="起動", width=7,
                                     command=self._start_server)
        self._start_btn.pack(side=tk.LEFT)
        self._stop_btn = ttk.Button(btn_row, text="停止", width=7,
                                    command=self._stop_server, state=tk.DISABLED)
        self._stop_btn.pack(side=tk.LEFT, padx=(4, 0))

        # リソースモニター
        rf = ttk.LabelFrame(parent, text=" リソースモニター ", padding=8)
        rf.pack(fill=tk.X, padx=4, pady=2)

        self._bar_cpu  = _Graph(rf, "CPU",  traffic_light=True)
        self._bar_cpu.pack(fill=tk.X)
        self._bar_ram  = _Graph(rf, "RAM",  color="#9040e0", traffic_light=True)
        self._bar_ram.pack(fill=tk.X)
        self._bar_gpu  = _Graph(rf, "GPU",  color="#20a080", traffic_light=True)
        self._bar_gpu.pack(fill=tk.X)
        self._bar_vram = _Graph(rf, "VRAM", color="#20a080", traffic_light=False)
        self._bar_vram.pack(fill=tk.X)

        self._res_note = ttk.Label(rf, text="Ollamaが起動すると表示されます",
                                   foreground="#999", font=("", 7))
        self._res_note.pack(anchor=tk.W)

        # 翻訳履歴
        hf = ttk.LabelFrame(parent, text=" 翻訳履歴 ", padding=6)
        hf.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)
        self.history_box = scrolledtext.ScrolledText(
            hf, state=tk.DISABLED, wrap=tk.WORD,
            font=("", 8), foreground="#333", background="#f5f5f5")
        self.history_box.pack(fill=tk.BOTH, expand=True)
        ttk.Button(hf, text="クリア", width=6,
                   command=self._clear_history).pack(anchor=tk.E, pady=(4, 0))

    def _build_right(self, parent):
        # 翻訳パラメータ
        pf = ttk.LabelFrame(parent, text=" 翻訳パラメータ ", padding=8)
        pf.pack(fill=tk.X, padx=4, pady=4)

        r1 = ttk.Frame(pf)
        r1.pack(fill=tk.X, pady=3)
        ttk.Label(r1, text="プリセット:").grid(row=0, column=0, sticky=tk.E, padx=(0, 4))
        self.preset_var = tk.StringVar(value=self.cfg["performance_preset"])
        pcb = ttk.Combobox(r1, textvariable=self.preset_var,
                           values=list(PRESETS.keys()), width=8, state="readonly")
        pcb.grid(row=0, column=1, sticky=tk.W)
        pcb.bind("<<ComboboxSelected>>", lambda _: self._on_preset_change())

        ttk.Label(r1, text="モデル:").grid(row=0, column=2, sticky=tk.E, padx=(16, 4))
        self.model_var = tk.StringVar()
        ttk.Entry(r1, textvariable=self.model_var, width=16).grid(
            row=0, column=3, sticky=tk.W)

        ttk.Label(r1, text="翻訳先:").grid(row=0, column=4, sticky=tk.E, padx=(16, 4))
        self.target_var = tk.StringVar(value=self.cfg["target_language"])
        ttk.Entry(r1, textvariable=self.target_var, width=12).grid(
            row=0, column=5, sticky=tk.W)

        r2 = ttk.Frame(pf)
        r2.pack(fill=tk.X, pady=3)
        ttk.Label(r2, text="num_ctx:").grid(row=0, column=0, sticky=tk.E, padx=(0, 4))
        self.ctx_var = tk.IntVar()
        ttk.Spinbox(r2, textvariable=self.ctx_var,
                    from_=64, to=2048, increment=64, width=6).grid(
            row=0, column=1, sticky=tk.W)

        ttk.Label(r2, text="num_thread:").grid(row=0, column=2, sticky=tk.E, padx=(16, 4))
        self.thread_var = tk.IntVar()
        ttk.Spinbox(r2, textvariable=self.thread_var,
                    from_=0, to=32, increment=1, width=5).grid(
            row=0, column=3, sticky=tk.W)
        ttk.Label(r2, text="(0=自動)", foreground="#888",
                  font=("", 7)).grid(row=0, column=4, padx=(4, 0))

        ttk.Label(r2, text="num_predict:").grid(row=0, column=5, sticky=tk.E, padx=(16, 4))
        self.predict_var = tk.IntVar(value=120)
        ttk.Spinbox(r2, textvariable=self.predict_var,
                    from_=20, to=512, increment=10, width=5).grid(
            row=0, column=6, sticky=tk.W)

        ttk.Label(r2, text="temperature:").grid(row=0, column=7, sticky=tk.E, padx=(16, 4))
        self.temp_var = tk.DoubleVar(value=0.1)
        ttk.Spinbox(r2, textvariable=self.temp_var,
                    from_=0.0, to=2.0, increment=0.05, width=5,
                    format="%.2f").grid(row=0, column=8, sticky=tk.W)
        ttk.Label(r2, text="(低=精度重視)", foreground="#888",
                  font=("", 7)).grid(row=0, column=9, padx=(4, 0))

        self._on_preset_change()

        # 入力テキスト
        tf = ttk.LabelFrame(parent, text=" 入力テキスト ", padding=8)
        tf.pack(fill=tk.BOTH, expand=False, padx=4, pady=4)

        self.input_box = scrolledtext.ScrolledText(tf, height=5, wrap=tk.WORD,
                                                    font=("", 10))
        self.input_box.pack(fill=tk.BOTH, expand=True)

        pr = ttk.Frame(tf)
        pr.pack(fill=tk.X, pady=(6, 0))
        ttk.Label(pr, text="プリセット文:", font=("", 8)).pack(side=tk.LEFT)
        for label, txt in [
            ("English", "Enemy tanks spotted at grid F5, need artillery support now!"),
            ("Русский", "Противник прорвал линию обороны, отступаем на запасные позиции."),
            ("한국어",  "보급품이 부족합니다. 탄약과 의료품을 요청합니다."),
            ("中文",    "我们需要更多的工程师来修复这座桥。"),
            ("日本語",  "グリッドF5に敵戦車を確認、砲撃支援を要請する！"),
        ]:
            ttk.Button(pr, text=label, width=7,
                       command=lambda t=txt: self._set_input(t)).pack(side=tk.LEFT, padx=2)

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
        for label, cat in [("短い ≤20", "short"), ("普通 21-60", "normal"), ("長い 61+", "long")]:
            ttk.Button(lr, text=label, width=9,
                       command=lambda c=cat: self._pick_log_sample(c)).pack(
                side=tk.LEFT, padx=2)

        # 翻訳結果
        rf2 = ttk.LabelFrame(parent, text=" 翻訳結果 ", padding=8)
        rf2.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)
        self.result_box = scrolledtext.ScrolledText(
            rf2, height=5, wrap=tk.WORD, font=("", 10),
            state=tk.DISABLED, foreground="#003388", background="#f0f4ff")
        self.result_box.pack(fill=tk.BOTH, expand=True)
        self.perf_lbl = ttk.Label(rf2, text="", foreground="#666", font=("", 8))
        self.perf_lbl.pack(anchor=tk.E, pady=(2, 0))

        # コントロール
        cf2 = ttk.Frame(parent)
        cf2.pack(fill=tk.X, padx=4, pady=4)
        self.translate_btn = ttk.Button(cf2, text="▶  翻訳", width=10,
                                        command=self._start_translate)
        self.translate_btn.pack(side=tk.LEFT, padx=(0, 6))
        ttk.Button(cf2, text="クリア", width=6,
                   command=self._clear_all).pack(side=tk.LEFT, padx=8)

        sr = ttk.Frame(cf2)
        sr.pack(side=tk.RIGHT)
        ttk.Label(sr, text="Sender:").pack(side=tk.LEFT)
        self.sender_var = tk.StringVar()
        ttk.Entry(sr, textvariable=self.sender_var, width=18).pack(
            side=tk.LEFT, padx=(4, 0))

    # ----------------------------------------------------------
    # サーバー UI 状態
    # ----------------------------------------------------------

    def _set_server_ui(self, state: str, version: str = "", pid=None):
        if state == "running":      # 自前起動
            self._dot_lbl.config(foreground="#00bb00")
            self._srv_lbl.config(text=f"起動中 (自動) v{version}")
            self._pid_lbl.config(text=f"PID: {pid}" if pid else "PID: 不明")
            self._start_btn.config(state=tk.DISABLED)
            self._stop_btn.config(state=tk.NORMAL)
        elif state == "external":   # 外部で起動済み
            self._dot_lbl.config(foreground="#00bb00")
            self._srv_lbl.config(text=f"起動中 v{version}")
            self._pid_lbl.config(text=f"PID: {pid}" if pid else "PID: 不明")
            self._start_btn.config(state=tk.NORMAL)
            self._stop_btn.config(state=tk.DISABLED)
        elif state == "starting":
            self._dot_lbl.config(foreground="#e0a000")
            self._srv_lbl.config(text="起動中...")
            self._pid_lbl.config(text="")
            self._start_btn.config(state=tk.DISABLED)
            self._stop_btn.config(state=tk.DISABLED)
        else:                       # stopped
            self._dot_lbl.config(foreground="#aaaaaa")
            self._srv_lbl.config(text="停止")
            self._pid_lbl.config(text="")
            self._start_btn.config(state=tk.NORMAL)
            self._stop_btn.config(state=tk.DISABLED)
            self._clear_bars()

    def _update_status_bar(self, ver):
        if ver:
            self._sb_var.set(f"Ollama v{ver}")
            self._sb_lbl.config(foreground="#008800")
            self.status_var.set("準備完了 — テキストを入力して「翻訳」を押してください")
        else:
            self._sb_var.set("Ollama: 未接続")
            self._sb_lbl.config(foreground="#cc0000")
            self.status_var.set(
                "Ollama が停止しています — 「起動」ボタンで同梱版を起動できます")

    # ----------------------------------------------------------
    # サーバー起動 / 停止
    # ----------------------------------------------------------

    def _start_server(self):
        exe = _find_bundled_ollama(self.base_dir)
        if not exe:
            self.status_var.set(
                f"同梱 ollama.exe が見つかりません: {self.base_dir}\\tools\\ollama\\")
            return
        proc = _launch_ollama(exe)
        if not proc:
            self.status_var.set("ollama serve の起動に失敗しました")
            return
        self._our_proc = proc
        self._set_server_ui("starting")
        self.status_var.set("Ollama を起動中... (最大30秒)")
        threading.Thread(target=self._wait_for_start, daemon=True).start()

    def _wait_for_start(self):
        endpoint   = self.cfg["ollama_endpoint"]
        t0         = time.time()
        dot_colors = ["#e0a000", "#b07800", "#e0c000"]
        for i in range(60):
            if self._our_proc and self._our_proc.poll() is not None:
                self.root.after(0, self._on_start_failed)
                return
            ver = ollama_version(endpoint, timeout=1.0)
            if ver:
                pid = _find_running_pid()
                self.root.after(0, lambda v=ver, p=pid: self._on_start_ok(v, p))
                return
            elapsed  = int(time.time() - t0)
            dc       = dot_colors[i % len(dot_colors)]
            self.root.after(0, lambda e=elapsed, c=dc: self._on_start_progress(e, c))
            time.sleep(0.5)
        self.root.after(0, self._on_start_failed)

    def _on_start_progress(self, elapsed: int, dot_color: str):
        self._dot_lbl.config(foreground=dot_color)
        self.status_var.set(f"Ollama を起動中... ({elapsed} 秒経過)")

    def _on_start_ok(self, ver, pid):
        self._server_pid = pid
        self._monitor.set_pid(pid)
        self._set_server_ui("running", ver, pid)
        self._update_status_bar(ver)
        self._start_res_loop()
        self._start_warmup()

    def _on_start_failed(self):
        self._our_proc = None
        self._set_server_ui("stopped")
        self.status_var.set("Ollama の起動に失敗しました")
        self._start_stopped_poll()

    def _stop_server(self):
        if self._res_timer:
            self.root.after_cancel(self._res_timer)
            self._res_timer = None
        if self._our_proc:
            try:
                self._our_proc.terminate()
                self._our_proc.wait(timeout=5)
            except Exception:
                try:
                    self._our_proc.kill()
                except Exception:
                    pass
            self._our_proc = None
        self._server_pid = None
        self._monitor.set_pid(None)
        self._set_server_ui("stopped")
        self._update_status_bar(None)

    # ----------------------------------------------------------
    # 停止中ポーリング（外部起動の自動検知）
    # ----------------------------------------------------------

    def _start_stopped_poll(self):
        if self._stopped_poll_id:
            self.root.after_cancel(self._stopped_poll_id)
        self._stopped_poll_id = self.root.after(5000, self._auto_poll_tick)

    def _auto_poll_tick(self):
        self._stopped_poll_id = None
        if self._server_pid or self._our_proc:
            return  # 既に起動中または起動処理中
        threading.Thread(target=self._bg_poll_check, daemon=True).start()

    def _bg_poll_check(self):
        ver = ollama_version(self.cfg["ollama_endpoint"], timeout=3.0)
        pid = _find_running_pid() if ver else None
        self.root.after(0, lambda v=ver, p=pid: self._on_poll_result(v, p))

    def _on_poll_result(self, ver, pid):
        if ver:
            self._server_pid = pid
            self._monitor.set_pid(pid)
            self._set_server_ui("external", ver, pid)
            self._start_res_loop()
            self._update_status_bar(ver)
            self._start_warmup()
        else:
            self._start_stopped_poll()  # 見つからなければ再スケジュール

    # ----------------------------------------------------------
    # ウォームアップ
    # ----------------------------------------------------------

    def _start_warmup(self):
        preset = PRESETS.get(self.preset_var.get(), PRESETS["Medium"])
        model  = self.model_var.get().strip() or preset["model"]
        self.status_var.set(f"モデル ({model}) をウォームアップ中...")
        self.translate_btn.config(state=tk.DISABLED)

        endpoint   = self.cfg["ollama_endpoint"]
        num_ctx    = preset["num_ctx"]
        num_thread = preset["num_thread"]

        def run():
            do_translate(endpoint, model, "Japanese",
                         num_ctx, num_thread, "hello",
                         num_predict=1, temperature=0.1)
            self.root.after(0, self._on_warmup_done)

        threading.Thread(target=run, daemon=True).start()

    def _on_warmup_done(self):
        self.translate_btn.config(state=tk.NORMAL)
        self.status_var.set("準備完了 — テキストを入力して「翻訳」を押してください")

    # ----------------------------------------------------------
    # リソースモニター更新 (2秒ごと)
    # ----------------------------------------------------------

    def _start_res_loop(self):
        if self._res_timer:
            self.root.after_cancel(self._res_timer)
        self._res_update()

    def _res_update(self):
        self._res_timer = self.root.after(2000, self._res_update)
        d = self._monitor.get_data()

        if not d["alive"] and self._server_pid:
            # プロセスが死んだ
            self.root.after_cancel(self._res_timer)
            self._res_timer  = None
            self._server_pid = None
            self._our_proc   = None
            self._set_server_ui("stopped")
            self._update_status_bar(None)
            self._start_stopped_poll()
            return

        if not d["alive"]:
            self._clear_bars()
            return

        self._res_note.config(
            text=f"PID {self._server_pid}  |  システムRAM {_TOTAL_RAM_MB:,} MB  |  2秒更新")

        self._bar_cpu.set(d["cpu_pct"],
                          f"{d['cpu_pct']:.1f}%  ({_NUM_CPUS} コア)")

        ram_pct = d["mem_mb"] / _TOTAL_RAM_MB * 100 if _TOTAL_RAM_MB else 0
        self._bar_ram.set(ram_pct, f"{d['mem_mb']:,} MB")

        if d["gpu_pct"] >= 0:
            self._bar_gpu.set(d["gpu_pct"], f"{d['gpu_pct']}%")
        else:
            self._bar_gpu.set_na("N/A (nvidia-smi未検出)")

        if d["gpu_mem_mb"] >= 0 and d["gpu_mem_total"] > 0:
            vram_pct = d["gpu_mem_mb"] / d["gpu_mem_total"] * 100
            self._bar_vram.set(vram_pct,
                               f"{d['gpu_mem_mb']:,} / {d['gpu_mem_total']:,} MB")
        else:
            self._bar_vram.set_na()

    def _clear_bars(self):
        self._res_note.config(text="Ollamaが起動すると表示されます")
        for bar in (self._bar_cpu, self._bar_ram, self._bar_gpu, self._bar_vram):
            bar.set_na()

    # ----------------------------------------------------------
    # プリセット変更
    # ----------------------------------------------------------

    def _on_preset_change(self):
        p = PRESETS.get(self.preset_var.get(), PRESETS["Medium"])
        self.model_var.set(p["model"])
        self.ctx_var.set(p["num_ctx"])
        self.thread_var.set(p["num_thread"])
        self.temp_var.set(p["temperature"])

    # ----------------------------------------------------------
    # 翻訳
    # ----------------------------------------------------------

    def _start_translate(self):
        text = self.input_box.get("1.0", tk.END).strip()
        if not text:
            self.status_var.set("テキストを入力してください")
            return
        self.translate_btn.config(state=tk.DISABLED)
        self._set_result("翻訳中...")
        self.perf_lbl.config(text="")

        endpoint    = self.cfg["ollama_endpoint"]
        model       = self.model_var.get().strip() or "gemma3:1b"
        target_lang = self.target_var.get().strip() or "Japanese"
        num_ctx     = self.ctx_var.get()
        num_thread  = self.thread_var.get()
        num_predict = self.predict_var.get()
        temperature = self.temp_var.get()

        def run():
            result, elapsed, cnt = do_translate(
                endpoint, model, target_lang, num_ctx, num_thread, text, num_predict,
                temperature)
            self.root.after(0, lambda r=result, e=elapsed, c=cnt:
                            self._on_done(r, e, c, text, model))

        threading.Thread(target=run, daemon=True).start()

    def _on_done(self, result, elapsed, cnt, src, model):
        self.translate_btn.config(state=tk.NORMAL)
        self._set_result(result)
        speed = f"  {cnt / (elapsed / 1000):.1f} tok/s" if cnt and elapsed else ""
        perf  = f"{elapsed} ms  |  {cnt} tokens{speed}"
        self.perf_lbl.config(text=perf)
        self.status_var.set(f"完了 — {perf}  [{model}]")
        self._append_history(src, result, model, elapsed)

    # ----------------------------------------------------------
    # 結果 / 履歴
    # ----------------------------------------------------------

    def _set_result(self, text: str):
        self.result_box.config(state=tk.NORMAL)
        self.result_box.delete("1.0", tk.END)
        self.result_box.insert(tk.END, text)
        self.result_box.config(state=tk.DISABLED)

    def _append_history(self, src, result, model, ms):
        self.history_box.config(state=tk.NORMAL)
        sp = src[:55] + ("…" if len(src) > 55 else "")
        rp = result[:55] + ("…" if len(result) > 55 else "")
        self.history_box.insert(tk.END, f"[{ms}ms / {model}]\n>{sp}\n={rp}\n\n")
        self.history_box.see(tk.END)
        self.history_box.config(state=tk.DISABLED)

    def _clear_history(self):
        self.history_box.config(state=tk.NORMAL)
        self.history_box.delete("1.0", tk.END)
        self.history_box.config(state=tk.DISABLED)

    # ----------------------------------------------------------
    # 入力
    # ----------------------------------------------------------

    def _set_input(self, text: str):
        self.input_box.delete("1.0", tk.END)
        self.input_box.insert(tk.END, text)

    def _clear_all(self):
        self.input_box.delete("1.0", tk.END)
        self._set_result("")
        self.perf_lbl.config(text="")

    # ----------------------------------------------------------
    # ログサンプル
    # ----------------------------------------------------------

    def _resolve_log_path(self) -> str:
        configured = self.cfg.get("log_file_path", "")
        if configured:
            return configured
        game_root  = os.path.normpath(os.path.join(self.base_dir, "..", ".."))
        candidates = [
            os.path.join(game_root, "War", "Binaries", "Win64", "chat_log.txt"),
            os.path.join(self.base_dir, "chat_log.txt"),
        ]
        for path in candidates:
            if os.path.exists(path):
                return path
        return candidates[0]

    def _load_log(self):
        path = self._resolve_log_path()
        if not os.path.exists(path):
            self.log_status_lbl.config(
                text=f"見つかりません: {os.path.basename(path)}", foreground="#cc0000")
            return
        self.log_messages = parse_chat_log(path)
        n  = len(self.log_messages)
        s  = sum(1 for m in self.log_messages if len(m["message"]) <= 20)
        nm = sum(1 for m in self.log_messages if 20 < len(m["message"]) <= 60)
        lo = sum(1 for m in self.log_messages if len(m["message"]) > 60)
        self.log_status_lbl.config(
            text=f"{n}件 (短{s}/普{nm}/長{lo})", foreground="#005599")
        self.status_var.set(f"ログ読み込み完了: {n}件")

    def _pick_log_sample(self, category: str):
        import random
        if not self.log_messages:
            self._load_log()
            if not self.log_messages:
                self.status_var.set("ログが空です")
                return
        SHORT, LONG = 20, 60
        if category == "short":
            pool = [m for m in self.log_messages if len(m["message"]) <= SHORT]
        elif category == "normal":
            pool = [m for m in self.log_messages if SHORT < len(m["message"]) <= LONG]
        else:
            pool = [m for m in self.log_messages if len(m["message"]) > LONG]
        if not pool:
            self.status_var.set("該当するメッセージがありません")
            return
        entry = random.choice(pool)
        self._set_input(entry["message"])
        self.sender_var.set(entry["sender"])
        self.status_var.set(
            f"[{entry['channel']}] {entry['sender']}: "
            f"{entry['message'][:50]}…  ({len(entry['message'])}文字)")

    # ----------------------------------------------------------
    # 終了処理
    # ----------------------------------------------------------

    def _on_close(self):
        if self._res_timer:
            self.root.after_cancel(self._res_timer)
        if self._stopped_poll_id:
            self.root.after_cancel(self._stopped_poll_id)
        self._monitor.stop()
        if self._our_proc:
            try:
                self._our_proc.terminate()
                self._our_proc.wait(timeout=3)
            except Exception:
                try:
                    self._our_proc.kill()
                except Exception:
                    pass
        self.root.destroy()

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
