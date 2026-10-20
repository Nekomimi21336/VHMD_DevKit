"""VHMD Link for the PC: the window (and tray icon) that connects a standalone
headset running the VHMD Link app to SteamVR.

    python -m vhmd_client.link [--minimized] [--dev] [--lang ja|en]
    pythonw clients/python/vhmd_link.pyw        (no console window)
    "VHMD Link.exe"                              (built by tools/build_link_exe.py; no Python needed)

It runs the relay (headset_bridge.Bridge), shows whether the headset and
SteamVR are connected, starts SteamVR when the headset connects (VHMD mode
only), switches SteamVR to VHMD mode or to the headset's display on request,
and keeps the video settings. Closing the window leaves it in the tray.
The developer view (always on with the "VHMD Link Dev" headset app, or --dev)
adds the link statistics, the log and tools.

Settings: %LOCALAPPDATA%/VHMD/link_settings.json.
"""

from __future__ import annotations

import argparse
import collections
import ctypes
import json
import os
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk
from typing import Dict, Optional

from . import headset_info, mode
from .fps import PALETTES, setup_style, ui_language, windows_theme
from .headset_bridge import Bridge, BridgeOptions
from .tray import TrayIcon

DATA = Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "VHMD"
FROZEN = bool(getattr(sys, "frozen", False))   # running as "VHMD Link.exe" (PyInstaller)
SETTINGS = DATA / "link_settings.json"
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
RUN_NAME = "VHMD Link"
QUALITY = (("low", 20.0), ("standard", 40.0), ("high", 60.0), ("max", 80.0))
DEFAULTS = {"quality": "standard", "codec": "hevc", "buffer": 0, "refresh": 72, "auto_steamvr": True,
            "dev_view": False}

TEXT = {
    "en": {
        "title": "VHMD Link", "edition_dev": "Developer view", "headset": "Headset", "steamvr": "SteamVR",
        "video": "Picture and sound", "dev": "Developer",
        "hs_wait": "Waiting for the headset", "hs_wait_sub": "Start VHMD Link on the headset (same Wi-Fi, or a USB cable)",
        "hs_on": "Connected", "hs_sub": "{model} · {link} · app {edition} {version}",
        "wifi": "Wi-Fi {addr}", "usb": "USB cable",
        "vr_off": "Not running", "vr_starting": "Starting...", "vr_on": "Running (VHMD)", "vr_pico": "Running (physical HMD mode)",
        "vr_other": "Running", "start_vr": "Start SteamVR", "to_vhmd": "Use VHMD mode",
        "match": "Match the headset's display",
        "match_hint": "VHMD renders with a different field of view or resolution than this headset.",
        "auto_vr": "Start SteamVR when the headset connects",
        "quality": "Quality", "codec": "Codec", "smooth": "Motion",
        "q_low": "Low (20 Mbit/s)", "q_standard": "Standard (40 Mbit/s)", "q_high": "High (60 Mbit/s)", "q_max": "Highest (80 Mbit/s)",
        "c_hevc": "HEVC (recommended)", "c_h264": "H.264", "c_av1": "AV1 (newer GPUs)",
        "b0": "Lowest latency", "b1": "Smoother (+1 frame)",
        "refresh": "Refresh rate", "r72": "72 Hz", "r90": "90 Hz (more GPU load)",
        "dev_view": "Developer view", "startup": "Open when Windows starts",
        "take_back": "Give control to the headset", "fps_tool": "Keyboard/mouse control", "log": "Log",
        "quit": "Quit", "open": "Open", "tray_hint": "Closing the window keeps VHMD Link in the notification area.",
        "confirm_restart": "SteamVR restarts for this; running VR apps close. Continue?",
        "busy": "Working: {}", "failed": "Failed: {}",
        "already": "VHMD Link is already running (its network ports are in use).",
        "control": "Control", "anyone": "anyone",
    },
    "ja": {
        "title": "VHMD Link", "edition_dev": "開発者向け", "headset": "ヘッドセット", "steamvr": "SteamVR",
        "video": "映像と音", "dev": "開発者向け",
        "hs_wait": "ヘッドセットを待っています", "hs_wait_sub": "ヘッドセットで VHMD Link を起動してください(同じ Wi-Fi か USB ケーブル)",
        "hs_on": "接続しました", "hs_sub": "{model} · {link} · アプリ {edition} {version}",
        "wifi": "Wi-Fi {addr}", "usb": "USB ケーブル",
        "vr_off": "停止中", "vr_starting": "起動しています…", "vr_on": "起動中(VHMD)", "vr_pico": "起動中(実機 HMD モード)",
        "vr_other": "起動中", "start_vr": "SteamVR を起動", "to_vhmd": "VHMD モードにする",
        "match": "ヘッドセットの画面に合わせる",
        "match_hint": "VHMD の視野角・解像度がこのヘッドセットと違います。",
        "auto_vr": "ヘッドセットがつながったら SteamVR を起動する",
        "quality": "画質", "codec": "圧縮方式", "smooth": "動き",
        "q_low": "低(20 Mbit/s)", "q_standard": "標準(40 Mbit/s)", "q_high": "高(60 Mbit/s)", "q_max": "最高(80 Mbit/s)",
        "c_hevc": "HEVC(推奨)", "c_h264": "H.264", "c_av1": "AV1(新しい GPU)",
        "b0": "遅れを最小に", "b1": "なめらか(+1 フレーム)",
        "refresh": "リフレッシュレート", "r72": "72 Hz", "r90": "90 Hz(GPU の負荷が増える)",
        "dev_view": "開発者向けの表示", "startup": "Windows の起動時に開く",
        "take_back": "操作をヘッドセットに戻す", "fps_tool": "キーボード・マウス操作", "log": "ログ",
        "quit": "終了", "open": "開く", "tray_hint": "ウィンドウを閉じても VHMD Link は通知領域で動き続けます。",
        "confirm_restart": "SteamVR を再起動します。実行中の VR アプリは終了します。よろしいですか?",
        "busy": "処理中: {}", "failed": "失敗しました: {}",
        "already": "VHMD Link はすでに起動しています(ネットワークのポートが使用中です)。",
        "control": "操作", "anyone": "制限なし",
    },
}


def load_settings() -> Dict[str, object]:
    try:
        s = json.loads(SETTINGS.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        s = {}
    return {**DEFAULTS, **{k: v for k, v in s.items() if k in DEFAULTS}}


def save_settings(s: Dict[str, object]) -> None:
    DATA.mkdir(parents=True, exist_ok=True)
    SETTINGS.write_text(json.dumps(s, indent=1), encoding="utf-8")


def draw_icon(folder: Path) -> Optional[Path]:
    """Draws the app icon (a blue rounded square with "V") into folder as
    vhmd_link.ico and vhmd_link.png (needs Pillow)."""
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        return None
    img = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((8, 8, 248, 248), radius=56, fill=(47, 111, 237, 255))
    try:
        font = ImageFont.truetype("segoeuib.ttf", 170)
    except OSError:
        font = ImageFont.load_default()
    d.text((128, 132), "V", fill=(255, 255, 255, 255), font=font, anchor="mm")
    folder.mkdir(parents=True, exist_ok=True)
    ico = folder / "vhmd_link.ico"
    img.save(ico, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (256, 256)])
    img.resize((64, 64)).save(folder / "vhmd_link.png")
    return ico


def make_icon() -> Optional[Path]:
    """The app icon in %LOCALAPPDATA%/VHMD (the tray needs a file): copied from
    the exe's bundle, else drawn once."""
    ico = DATA / "vhmd_link.ico"
    if ico.exists():
        return ico
    bundle = Path(getattr(sys, "_MEIPASS", ""))
    if FROZEN and (bundle / "vhmd_link.ico").exists():
        DATA.mkdir(parents=True, exist_ok=True)
        for name in ("vhmd_link.ico", "vhmd_link.png"):
            shutil.copyfile(bundle / name, DATA / name)
        return ico
    return draw_icon(DATA)


def launcher_command() -> str:
    """How Windows should start this app at sign-in (no console window)."""
    if FROZEN:
        return f'"{sys.executable}" --minimized'
    exe = Path(sys.executable)
    pyw = exe.with_name("pythonw.exe")
    script = Path(__file__).resolve().parent.parent / "vhmd_link.pyw"
    return f'"{pyw if pyw.exists() else exe}" "{script}" --minimized'


def startup_enabled() -> bool:
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
            winreg.QueryValueEx(k, RUN_NAME)
            return True
    except OSError:
        return False


def set_startup(on: bool) -> None:
    import winreg
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
        if on:
            winreg.SetValueEx(k, RUN_NAME, 0, winreg.REG_SZ, launcher_command())
        else:
            try:
                winreg.DeleteValue(k, RUN_NAME)
            except OSError:
                pass


class LinkApp:
    def __init__(self, root: tk.Tk, lang: str, dev: bool, minimized: bool) -> None:
        self.root, self.t, self.lang = root, TEXT[lang], lang
        self.s = load_settings()
        if dev:
            self.s["dev_view"] = True
        self.pal = pal = PALETTES[windows_theme()]
        family = "Yu Gothic UI" if lang == "ja" else "Segoe UI"
        self.fonts = f = {"ui": (family, 10), "small": (family, 9), "small_bold": (family, 9, "bold"),
                          "head": (family, 11, "bold"), "title": (family, 16, "bold"), "big": (family, 13, "bold"),
                          "mono": ("Consolas", 9), "mono_small": ("Consolas", 9)}
        setup_style(root, pal, f)
        root.title(self.t["title"])
        root.minsize(520, 420)
        self.log_lines: "collections.deque[str]" = collections.deque(maxlen=300)
        self.log_dirty = False
        self.busy = ""                       # a mode switch is running
        self.was_connected = False
        self.auto_started_at = 0.0
        self.mode = None                      # SteamVR mode from the settings file, refreshed in the background
        self.differs = False

        icon = make_icon()
        if icon and (DATA / "vhmd_link.png").exists():
            try:
                self._icon_img = tk.PhotoImage(file=str(DATA / "vhmd_link.png"))
                root.iconphoto(True, self._icon_img)
            except tk.TclError:
                pass
        self.bridge = Bridge(BridgeOptions(bitrate=self.bitrate(), codec=str(self.s["codec"]),
                                           buffer=int(self.s["buffer"]), refresh=float(self.s["refresh"])),
                             log=self.log)
        try:
            self.bridge.start()
        except OSError:
            messagebox.showerror(self.t["title"], self.t["already"])
            root.destroy()
            raise SystemExit(1)

        self.build()
        self.tray = TrayIcon(self.t["title"], str(icon) if icon else None,
                             [("open", self.t["open"]), ("quit", self.t["quit"])])
        self.tray.start()
        if not self.tray.ok:
            self.log("notification-area icon not available: closing the window quits")
        root.protocol("WM_DELETE_WINDOW", self.hide if self.tray.ok else self.quit)
        if minimized and self.tray.ok:
            root.withdraw()
        threading.Thread(target=self.background, daemon=True).start()
        self.refresh()

    # -- helpers ---------------------------------------------------------------
    def log(self, msg: str) -> None:
        self.log_lines.append(time.strftime("%H:%M:%S ") + (msg if len(msg) <= 160 else msg[:157] + "..."))
        self.log_dirty = True

    def bitrate(self) -> float:
        return dict(QUALITY).get(str(self.s["quality"]), 40.0)

    def card(self, parent, title: str) -> ttk.Frame:
        outer = tk.Frame(parent, bg=self.pal["border"], padx=1, pady=1)
        outer.pack(fill="x", padx=14, pady=(0, 10))
        inner = ttk.Frame(outer, style="Card.TFrame", padding=(14, 10))
        inner.pack(fill="both", expand=True)
        ttk.Label(inner, text=title, style="CardGroup.TLabel").pack(anchor="w")
        return inner

    # -- layout ----------------------------------------------------------------
    def build(self) -> None:
        t, pal, f = self.t, self.pal, self.fonts
        head = tk.Frame(self.root, bg=pal["bg"])
        head.pack(fill="x", padx=14, pady=(12, 10))
        tk.Label(head, text="VHMD Link", bg=pal["bg"], fg=pal["fg"], font=f["title"]).pack(side="left")
        self.dev_pill = tk.Label(head, text=t["edition_dev"], bg=pal["warn"], fg=pal["warn_fg"], font=f["small_bold"],
                                 padx=8, pady=2)

        # Headset
        c = self.card(self.root, t["headset"])
        row = ttk.Frame(c, style="Card.TFrame")
        row.pack(fill="x", pady=(4, 0))
        self.hs_dot = tk.Label(row, text="●", bg=pal["card"], fg=pal["muted"], font=f["big"])
        self.hs_dot.pack(side="left")
        self.hs_text = tk.Label(row, text="", bg=pal["card"], fg=pal["fg"], font=f["big"])
        self.hs_text.pack(side="left", padx=(6, 0))
        self.hs_sub = ttk.Label(c, text="", style="CardMuted.TLabel", wraplength=470, justify="left")
        self.hs_sub.pack(anchor="w", pady=(2, 0))

        # SteamVR
        c = self.card(self.root, t["steamvr"])
        row = ttk.Frame(c, style="Card.TFrame")
        row.pack(fill="x", pady=(4, 0))
        self.vr_dot = tk.Label(row, text="●", bg=pal["card"], fg=pal["muted"], font=f["big"])
        self.vr_dot.pack(side="left")
        self.vr_text = tk.Label(row, text="", bg=pal["card"], fg=pal["fg"], font=f["big"])
        self.vr_text.pack(side="left", padx=(6, 0))
        self.vr_btn = ttk.Button(row, text=t["start_vr"], style="Accent.TButton", command=self.start_vr)
        self.mode_btn = ttk.Button(row, text=t["to_vhmd"], command=lambda: self.switch(headset=None))
        self.match_row = ttk.Frame(c, style="Card.TFrame")
        ttk.Label(self.match_row, text=t["match_hint"], style="CardMuted.TLabel", wraplength=300,
                  justify="left").pack(side="left")
        ttk.Button(self.match_row, text=t["match"], command=lambda: self.switch(headset=True)).pack(side="right")
        self.busy_lbl = ttk.Label(c, text="", style="CardMuted.TLabel")
        self.busy_lbl.pack(anchor="w")
        self.auto_var = tk.BooleanVar(value=bool(self.s["auto_steamvr"]))
        ttk.Checkbutton(c, text=t["auto_vr"], style="Card.TCheckbutton", variable=self.auto_var,
                        command=lambda: self.set("auto_steamvr", self.auto_var.get())).pack(anchor="w", pady=(4, 0))

        # Picture and sound
        c = self.card(self.root, t["video"])
        grid = ttk.Frame(c, style="Card.TFrame")
        grid.pack(fill="x", pady=(4, 0))
        self.q_var = self.combo(grid, 0, t["quality"], [(k, t["q_" + k]) for k, _ in QUALITY], "quality")
        self.c_var = self.combo(grid, 1, t["codec"], [("hevc", t["c_hevc"]), ("av1", t["c_av1"]), ("h264", t["c_h264"])], "codec")
        self.b_var = self.combo(grid, 2, t["smooth"], [(0, t["b0"]), (1, t["b1"])], "buffer")
        self.r_var = self.combo(grid, 3, t["refresh"], [(72, t["r72"]), (90, t["r90"])], "refresh")

        # Developer
        self.dev_card_outer = tk.Frame(self.root, bg=pal["border"], padx=1, pady=1)
        dc = ttk.Frame(self.dev_card_outer, style="Card.TFrame", padding=(14, 10))
        dc.pack(fill="both", expand=True)
        ttk.Label(dc, text=t["dev"], style="CardGroup.TLabel").pack(anchor="w")
        self.stats_lbl = tk.Label(dc, text="", bg=pal["card"], fg=pal["fg"], font=f["mono"], justify="left", anchor="w")
        self.stats_lbl.pack(fill="x", pady=(4, 6))
        btns = ttk.Frame(dc, style="Card.TFrame")
        btns.pack(fill="x")
        ttk.Button(btns, text=t["take_back"], command=self.take_back).pack(side="left")
        ttk.Button(btns, text=t["fps_tool"], command=self.fps_tool).pack(side="left", padx=(8, 0))
        self.log_box = tk.Text(dc, height=8, bg=pal["entry"], fg=pal["muted"], font=f["mono_small"], relief="flat",
                               highlightthickness=0, wrap="word")
        self.log_box.pack(fill="both", expand=True, pady=(8, 0))

        # Footer
        foot = tk.Frame(self.root, bg=pal["bg"])
        foot.pack(side="bottom", fill="x", padx=14, pady=(0, 12))
        self.dev_var = tk.BooleanVar(value=bool(self.s["dev_view"]))
        ttk.Checkbutton(foot, text=t["dev_view"], variable=self.dev_var,
                        command=lambda: self.set("dev_view", self.dev_var.get())).pack(side="left")
        self.startup_var = tk.BooleanVar(value=startup_enabled())
        ttk.Checkbutton(foot, text=t["startup"], variable=self.startup_var, command=self.toggle_startup
                        ).pack(side="left", padx=(12, 0))
        ttk.Button(foot, text=t["quit"], command=self.quit).pack(side="right")
        self.hint = ttk.Label(self.root, text=t["tray_hint"], style="Muted.TLabel")
        self.hint.pack(side="bottom", anchor="w", padx=14, pady=(0, 6))

    def combo(self, parent, row: int, label: str, choices, key: str) -> tk.StringVar:
        ttk.Label(parent, text=label, style="Card.TLabel").grid(row=row, column=0, sticky="w", pady=3, padx=(0, 12))
        labels = [lab for _, lab in choices]
        var = tk.StringVar(value=dict(choices).get(self.s[key], labels[0]))
        box = ttk.Combobox(parent, values=labels, textvariable=var, state="readonly", width=26)
        box.grid(row=row, column=1, sticky="w", pady=3)

        def changed(_e=None) -> None:
            value = next(v for v, lab in choices if lab == var.get())
            if value == self.s[key]:
                return
            self.set(key, value)
            if key == "refresh":   # the headset switches, VHMD follows it (no reconnect)
                self.bridge.o.refresh = float(value)
                self.log(f"refresh rate: {value} Hz")
                return
            if key == "quality":
                self.bridge.set_video(bitrate=self.bitrate())
            else:
                self.bridge.set_video(bitrate=self.bitrate(), codec=str(self.s["codec"]), buffer=int(self.s["buffer"]))
            self.log(f"video settings: {self.bitrate():g} Mbit/s, {self.s['codec']}, buffer {self.s['buffer']}")

        box.bind("<<ComboboxSelected>>", changed)
        return var

    def set(self, key: str, value) -> None:
        self.s[key] = value
        save_settings(self.s)

    # -- actions -----------------------------------------------------------------
    def start_vr(self) -> None:
        self.log("starting SteamVR")
        try:
            mode.start_steamvr()
            self.auto_started_at = time.monotonic()
        except OSError as e:
            self.log(f"could not start SteamVR: {e}")

    def switch(self, headset: Optional[bool]) -> None:
        if self.busy:
            return
        if mode.steamvr_running() and not messagebox.askyesno(self.t["title"], self.t["confirm_restart"]):
            return

        def work() -> None:
            try:
                mode.switch("vhmd", progress=lambda m: (setattr(self, "busy", m), self.log(m)), headset=headset)
                self.busy = ""
            except (Exception, SystemExit) as e:   # noqa: BLE001 - shown to the user
                self.busy = ""
                self.log(self.t["failed"].format(e))
        self.busy = "..."
        threading.Thread(target=work, daemon=True).start()

    def take_back(self) -> None:
        try:
            self.bridge.request("control", source="manual")
            self.log("control: manual (the headset)")
        except (OSError, TimeoutError) as e:
            self.log(self.t["failed"].format(e))

    def fps_tool(self) -> None:
        if FROZEN:   # the same exe runs the tool
            subprocess.Popen([sys.executable, "--fps"])
            return
        cwd = Path(__file__).resolve().parent.parent
        subprocess.Popen([sys.executable, "-m", "vhmd_client.fps"], cwd=str(cwd))

    def toggle_startup(self) -> None:
        try:
            set_startup(self.startup_var.get())
        except OSError as e:
            self.log(self.t["failed"].format(e))
        self.startup_var.set(startup_enabled())

    def hide(self) -> None:
        self.root.withdraw()

    def show(self) -> None:
        self.root.deiconify()
        self.root.lift()
        self.root.focus_force()

    def quit(self) -> None:
        self.tray.stop()
        self.root.destroy()

    # -- updates -----------------------------------------------------------------
    def background(self) -> None:
        """Slow checks off the UI thread: the SteamVR mode and whether VHMD
        renders for this headset; starts SteamVR when the headset connects."""
        while True:
            try:
                self.mode = mode.current_mode()
                info = headset_info.load()
                self.differs = bool(info) and not headset_info.APPLIED.exists()
            except Exception:
                pass
            st = self.bridge.status()
            connected = st["headset"]
            if (connected and not self.was_connected and self.s["auto_steamvr"] and not st["steamvr"]
                    and self.mode == "vhmd" and not self.busy and time.monotonic() - self.auto_started_at > 60
                    and not mode.steamvr_running()):   # asked directly: the bridge's view may not be there yet
                self.log("headset connected: starting SteamVR")
                self.start_vr()
            self.was_connected = connected
            time.sleep(2.0)

    def refresh(self) -> None:
        t, pal = self.t, self.pal
        while True:
            try:
                ev = self.tray.events.get_nowait()
            except Exception:
                break
            if ev == "open":
                self.show()
            elif ev == "quit":
                self.quit()
                return
        st = self.bridge.status()

        if st["headset"]:
            link = t["usb"] if st["link"] == "usb" else t["wifi"].format(addr=st["address"])
            self.hs_dot.config(fg="#22c55e")
            self.hs_text.config(text=t["hs_on"])
            self.hs_sub.config(text=t["hs_sub"].format(model=st["model"] or "?", link=link,
                                                        edition="Dev" if st["edition"] == "dev" else "",
                                                        version=st["version"]).replace("  ", " "))
        else:
            self.hs_dot.config(fg=pal["muted"])
            self.hs_text.config(text=t["hs_wait"])
            self.hs_sub.config(text=t["hs_wait_sub"])

        running = st["steamvr"] or st["driver"]
        starting = not running and time.monotonic() - self.auto_started_at < 40
        if st["driver"] and self.mode == "vhmd":
            text, color = t["vr_on"], "#22c55e"
        elif running and self.mode == "pico":
            text, color = t["vr_pico"], "#f59e0b"
        elif running:
            text, color = t["vr_other"], "#22c55e"
        elif starting:
            text, color = t["vr_starting"], "#f59e0b"
        else:
            text, color = t["vr_off"], pal["muted"]
        self.vr_dot.config(fg=color)
        self.vr_text.config(text=text)
        self.vr_btn.pack_forget()
        self.mode_btn.pack_forget()
        if not running and not starting and not self.busy:
            self.vr_btn.pack(side="right")
        if self.mode == "pico" and not self.busy:
            self.mode_btn.pack(side="right")
        if self.differs and st["headset"] and self.mode == "vhmd" and not self.busy:
            self.match_row.pack(fill="x", pady=(6, 0), before=self.busy_lbl)
        else:
            self.match_row.pack_forget()
        self.busy_lbl.config(text=t["busy"].format(self.busy) if self.busy else "")

        dev = bool(self.s["dev_view"]) or st["edition"] == "dev"
        if dev:
            self.dev_pill.pack(side="left", padx=(10, 0))
            if not self.dev_card_outer.winfo_ismapped():
                self.dev_card_outer.pack(fill="both", expand=True, padx=14, pady=(0, 10))
            h = st["headset_stats"]
            control = st["control"] or "-"
            sync = st["sync"]
            sync_text = (f"sync {sync.get('shift_ms', 0):+.2f} ms (total {st['sync_total_ms']:+.1f})" if sync.get("ok")
                         else f"sync: {sync.get('error', 'waiting')}" if sync else "sync: -")
            lines = [
                f"PC     video {st['video_in']}/{st['video_out']} fps  {st['mbps']:5.1f} Mbit/s "
                f"(now {st['bitrate_now']:.0f})   poses {st['msgs']}/s   {sync_text}",
                f"Head   received {h.get('received', 0):.0f}  decoded {h.get('decoded', 0):.0f}  "
                f"shown {h.get('shown', 0):.0f}/s at {h.get('display', 0):.0f} Hz  repeats {h.get('repeats', 0):.0f}",
                f"       wait {h.get('wait_ms', 0):.1f} ms (p95 {h.get('wait_p95_ms', 0):.1f})  "
                f"reprojected {h.get('reprojected', 0):.0f}%  audio {h.get('audio_packets', 0):.0f}/s "
                f"underruns {h.get('audio_underruns', 0)}",
                f"{t['control']}: {t['anyone'] if control == 'any' else control}   {st['summary']}",
            ]
            self.stats_lbl.config(text="\n".join(lines))
            if self.log_dirty:
                self.log_dirty = False
                self.log_box.config(state="normal")
                self.log_box.delete("1.0", "end")
                self.log_box.insert("end", "\n".join(self.log_lines))
                self.log_box.see("end")
                self.log_box.config(state="disabled")
        else:
            self.dev_pill.pack_forget()
            self.dev_card_outer.pack_forget()

        tip = f"VHMD Link: {self.hs_text.cget('text')} / SteamVR: {self.vr_text.cget('text')}"
        if tip != self.tray.tooltip:
            self.tray.set_tooltip(tip)
        self.root.after(500, self.refresh)


def main() -> None:
    if len(sys.argv) > 1 and sys.argv[1] == "--fps":   # the exe also carries the keyboard/mouse tool
        from . import fps
        sys.argv = [sys.argv[0]] + sys.argv[2:]
        fps.main()
        return
    ap = argparse.ArgumentParser(description="VHMD Link for the PC (window and tray icon)")
    ap.add_argument("--minimized", action="store_true", help="start in the notification area")
    ap.add_argument("--dev", action="store_true", help="show the developer view")
    ap.add_argument("--lang", choices=("en", "ja"), default=None)
    args = ap.parse_args()
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass
    root = tk.Tk()
    LinkApp(root, args.lang or ui_language(), args.dev, args.minimized)
    root.mainloop()


if __name__ == "__main__":
    main()
