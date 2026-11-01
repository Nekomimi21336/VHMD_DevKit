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

from . import bindings, headset_info, mode, offsets, tracking
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
            # performance settings (each trades some quality or safety for less latency)
            "auto_latency": True, "predict": True, "amf": True, "eye_scale": 1.0, "udp_video": False, "fec": 30, "foveation": 0,
            "bindings": {},                    # changes from bindings.DEFAULTS
            "body_tracking": True, "hand_tracking": False, "wear_detect": True, "mic": False, "mic_device": "CABLE Input",
            "offsets": offsets.DEFAULTS,       # by device id, see offsets.py
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
        "env": "Use SteamVR with", "env_vhmd": "VHMD Link", "env_pico": "PICO Connect",
        "env_off": "As before VHMD",
        "env_hint_vhmd": "This headset through VHMD (VHMD's HMD and controllers).",
        "env_hint_pico": "The physical headset through its own app; VHMD only adds trackers.",
        "env_hint_off": "VHMD's driver is off; SteamVR's settings are back to how they were before VHMD.",
        "vr_off_mode": "Running (VHMD off)",
        "match": "Match the headset's display",
        "match_hint": "VHMD renders with a different field of view or resolution than this headset.",
        "auto_vr": "Start SteamVR when the headset connects",
        "mic": "Use the headset's microphone",
        "mic_hint": "Needs VB-CABLE on this PC. In VRChat and other programs, choose \"CABLE Output (VB-Audio Virtual Cable)\" as the microphone.",
        "mic_monitor": "Listen to yourself (loopback)",
        "mic_monitor_hint": "Plays it on this PC's default output, which also reaches the headset a moment later. Speakers may howl. Off again when VHMD Link starts.",
        "mic_monitor_same": "The default output is \"{device}\": it cannot be played there as well.",
        "mic_permission": "Allow the microphone on the headset.",
        "mic_no_device": "\"{device}\" was not found. Install VB-CABLE.",
        "mic_busy": "Another program is using \"{device}\" on its own. Close that program or change its output.",
        "mic_waiting": "Waiting for sound from the headset.",
        "mic_on": "Sound is reaching the PC.",
        "wear": "Put SteamVR on standby when the headset is taken off",
        "wear_hint": "Off: video goes on while the headset is put down.",
        "quality": "Quality", "codec": "Codec", "smooth": "Motion",
        "q_low": "Low (20 Mbit/s)", "q_standard": "Standard (40 Mbit/s)", "q_high": "High (60 Mbit/s)", "q_max": "Highest (80 Mbit/s)",
        "c_hevc": "HEVC (recommended)", "c_h264": "H.264", "c_av1": "AV1 (newer GPUs)",
        "b0": "Lowest latency", "b1": "Smoother (+1 frame)",
        "track": "Sent to SteamVR",
        "track_hint": "Pause holds a device where it is. Disconnect takes it off SteamVR until you reconnect it. "
                      "Both end with SteamVR.",
        "track_none": "Nothing is being sent to SteamVR.", "track_failed": "Could not change {name}: {error}",
        "track_tracking": "Tracking", "track_silent": "No data", "track_paused": "Paused",
        "track_disconnected": "Disconnected", "track_rate": "{rate}/s", "track_pause": "Pause", "track_resume": "Resume",
        "track_disconnect": "Disconnect", "track_reconnect": "Reconnect", "d_hmd": "Headset",
        "off": "Position and angle correction",
        "off_hint": "Controllers move along their own axes, trackers along the room's. Changes apply at once.",
        "off_device": "Device", "off_x": "Left-right X (cm)", "off_y": "Up-down Y (cm)", "off_z": "Back-front Z (cm)",
        "off_yaw": "Yaw (degrees)", "off_pitch": "Pitch (degrees)", "off_roll": "Roll (degrees)",
        "off_reset": "Back to this device's default", "d_left_hand": "Left controller",
        "d_right_hand": "Right controller", "d_waist": "Waist", "d_left_foot": "Left foot", "d_right_foot": "Right foot",
        "body": "Body tracking", "perf_body_tracking": "Use PICO Motion Trackers (waist and feet in SteamVR)",
        "perf_body_tracking_hint": "Calibrate them on the headset first. The SteamVR trackers appear once they are tracked.",
        "perf_hand_tracking": "Use tracked hands when the controllers are put down",
        "perf_hand_tracking_hint": "Turn on hand tracking in the headset's settings too. A pinch is the trigger.",
        "bind": "Controller buttons", "bind_hint": "What each button presses in SteamVR. Changes apply at once.",
        "bind_reset": "Back to defaults", "b_left_menu": "Left menu button (three lines)", "b_left_x": "Left X",
        "b_left_y": "Left Y", "b_left_joystick_click": "Left stick press", "b_right_a": "Right A", "b_right_b": "Right B",
        "b_right_joystick_click": "Right stick press", "t_system": "SteamVR menu", "t_menu": "App menu", "t_x": "X",
        "t_y": "Y", "t_a": "A", "t_b": "B", "t_joystick_click": "Stick press", "t_trigger_click": "Trigger",
        "t_grip_click": "Grip", "t_none": "Nothing",
        "perf": "Performance",
        "perf_auto_latency": "Trim the wait before display to what the timing needs",
        "perf_auto_latency_hint": "Up to about 3 ms less delay. Off: a fixed margin, safest against stutter on poor Wi-Fi.",
        "perf_predict": "Predict motion for when the image is shown",
        "perf_predict_hint": "Hands and head line up better while moving. Off if fast motion overshoots or wobbles.",
        "perf_amf": "AMD ultra-low-latency encoder (AMF)",
        "perf_amf_hint": "About 2 ms less per frame on AMD GPUs. Off: Windows' standard encoder (Media Foundation).",
        "perf_scale": "Resolution", "s100": "100% (sharpest)", "s85": "85%", "s70": "70% (fastest)",
        "perf_udp_video": "Send the picture over UDP with error correction (experimental)",
        "perf_udp_video_hint": "A lost Wi-Fi packet no longer holds up the picture; lost parts are rebuilt. Uses more bandwidth. Needs the native data path.",
        "perf_fec": "Error correction", "f20": "20% (light)", "f30": "30%", "f50": "50% (strong; bursty Wi-Fi)",
        "perf_fov": "Foveated encoding", "v0": "Off", "v1": "Light (49% of the pixels)", "v2": "Strong (30% of the pixels)",
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
        "env": "SteamVR の使い方", "env_vhmd": "VHMD Link", "env_pico": "PICO Connect",
        "env_off": "元の環境",
        "env_hint_vhmd": "このヘッドセットを VHMD でつなぐ(VHMD の HMD とコントローラー)。",
        "env_hint_pico": "実機をそのメーカーのアプリでつなぐ。VHMD はトラッカーだけを足す。",
        "env_hint_off": "VHMD のドライバを止め、SteamVR の設定を VHMD を入れる前に戻す。",
        "vr_off_mode": "起動中(VHMD は停止)",
        "match": "ヘッドセットの画面に合わせる",
        "match_hint": "VHMD の視野角・解像度がこのヘッドセットと違います。",
        "auto_vr": "ヘッドセットがつながったら SteamVR を起動する",
        "mic": "ヘッドセットのマイクを使う",
        "mic_hint": "この PC に VB-CABLE が必要です。VRChat などのアプリのマイクを「CABLE Output (VB-Audio Virtual Cable)」にしてください。",
        "mic_monitor": "自分の声を試し聞きする(ループバック)",
        "mic_monitor_hint": "既定の再生デバイスで鳴らします。ヘッドセットにも少し遅れて届きます。スピーカーではハウリングすることがあります。VHMD Link を起動し直すと切れます。",
        "mic_monitor_same": "既定の出力が「{device}」なので、試し聞きはできません。",
        "mic_permission": "ヘッドセットでマイクを許可してください。",
        "mic_no_device": "「{device}」が見つかりません。VB-CABLE を入れてください。",
        "mic_busy": "別のアプリが「{device}」を占有しています。そのアプリを閉じるか、出力先を変えてください。",
        "mic_waiting": "ヘッドセットの音を待っています。",
        "mic_on": "PC に音が届いています。",
        "wear": "ヘッドセットを外したら SteamVR を待機させる",
        "wear_hint": "切ると、ヘッドセットを置いているあいだも映像を送り続けます。",
        "quality": "画質", "codec": "圧縮方式", "smooth": "動き",
        "q_low": "低(20 Mbit/s)", "q_standard": "標準(40 Mbit/s)", "q_high": "高(60 Mbit/s)", "q_max": "最高(80 Mbit/s)",
        "c_hevc": "HEVC(推奨)", "c_h264": "H.264", "c_av1": "AV1(新しい GPU)",
        "b0": "遅れを最小に", "b1": "なめらか(+1 フレーム)",
        "track": "SteamVR に送っている機器",
        "track_hint": "停止はその場で固定します。切断は再接続するまで SteamVR から外します。どちらも SteamVR を終了すると元に戻ります。",
        "track_none": "SteamVR に送っている機器はありません。", "track_failed": "{name} を変更できませんでした: {error}",
        "track_tracking": "追跡中", "track_silent": "受信なし", "track_paused": "停止中",
        "track_disconnected": "切断中", "track_rate": "{rate}/秒", "track_pause": "停止", "track_resume": "再開",
        "track_disconnect": "切断", "track_reconnect": "再接続", "d_hmd": "ヘッドセット",
        "off": "位置と角度の補正",
        "off_hint": "コントローラーは本体の向き、トラッカーは部屋の向きに沿って動きます。変えるとすぐ反映されます。",
        "off_device": "機器", "off_x": "左右 X(cm)", "off_y": "上下 Y(cm)", "off_z": "前後 Z(cm)",
        "off_yaw": "ヨー(度)", "off_pitch": "ピッチ(度)", "off_roll": "ロール(度)",
        "off_reset": "この機器を初期値に戻す", "d_left_hand": "左コントローラー", "d_right_hand": "右コントローラー",
        "d_waist": "腰", "d_left_foot": "左足", "d_right_foot": "右足",
        "body": "体のトラッキング", "perf_body_tracking": "PICO Motion Tracker を使う(SteamVR に腰と足のトラッカーを出す)",
        "perf_body_tracking_hint": "先にヘッドセットで較正してください。トラッキングが始まると SteamVR にトラッカーが現れます。",
        "perf_hand_tracking": "コントローラーを置いたら手のトラッキングに切り替える",
        "perf_hand_tracking_hint": "ヘッドセットの設定でもハンドトラッキングをオンにしてください。親指と人差し指でつまむとトリガーになります。",
        "bind": "コントローラーのボタン", "bind_hint": "ボタンを押したときに SteamVR へ送る入力。変えるとすぐ反映されます。",
        "bind_reset": "既定に戻す", "b_left_menu": "左の三本線ボタン", "b_left_x": "左 X", "b_left_y": "左 Y",
        "b_left_joystick_click": "左スティックの押し込み", "b_right_a": "右 A", "b_right_b": "右 B",
        "b_right_joystick_click": "右スティックの押し込み", "t_system": "SteamVR のメニュー", "t_menu": "アプリのメニュー",
        "t_x": "X", "t_y": "Y", "t_a": "A", "t_b": "B", "t_joystick_click": "スティックの押し込み", "t_trigger_click": "トリガー",
        "t_grip_click": "グリップ", "t_none": "何もしない",
        "perf": "パフォーマンス",
        "perf_auto_latency": "表示までの余裕をタイミングに合わせて詰める",
        "perf_auto_latency_hint": "遅れが最大 3 ms ほど減る。切ると余裕を固定(電波が悪いときのカクつきに一番強い)。",
        "perf_predict": "表示される時刻に合わせて動きを先読みする",
        "perf_predict_hint": "動いているときの手・頭のずれが減る。速い動きで行き過ぎ・揺れが気になるときは切る。",
        "perf_amf": "AMD の超低遅延エンコーダー(AMF)",
        "perf_amf_hint": "AMD の GPU で 1 フレーム約 2 ms 速い。切ると Windows 標準のエンコーダー(Media Foundation)。",
        "perf_scale": "解像度", "s100": "100%(一番きれい)", "s85": "85%", "s70": "70%(一番速い)",
        "perf_udp_video": "映像を UDP と誤り訂正で送る(実験的)",
        "perf_udp_video_hint": "Wi-Fi でパケットが落ちても映像が待たされず、落ちた分は組み立て直す。通信量が増える。ネイティブのデータの通り道が必要。",
        "perf_fec": "誤り訂正", "f20": "20%(軽い)", "f30": "30%", "f50": "50%(強い・電波が途切れがち)",
        "perf_fov": "中心をくっきり・周辺を粗く", "v0": "しない", "v1": "弱(画素 49%)", "v2": "強(画素 30%)",
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


def wheel_scrolls_page(box: ttk.Combobox, scroll) -> None:
    """The wheel over a drop-down scrolls the page: it must not change the setting."""
    def wheel(e):
        scroll(e)
        return "break"

    box.bind("<MouseWheel>", wheel)


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
        sh = root.winfo_screenheight()
        root.geometry(f"660x{min(980, sh - 120)}")
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
                                           buffer=int(self.s["buffer"]), refresh=float(self.s["refresh"]),
                                           auto_latency=bool(self.s["auto_latency"]), predict=bool(self.s["predict"]),
                                           encoder_backend="auto" if self.s["amf"] else "mf",
                                           eye_scale=float(self.s["eye_scale"]),
                                           udp_video=bool(self.s["udp_video"]), fec=int(self.s["fec"]),
                                           foveation=int(self.s["foveation"]), bindings=dict(self.s["bindings"]),
                                           body_tracking=bool(self.s["body_tracking"]),
                                           hand_tracking=bool(self.s["hand_tracking"]),
                                           wear_detect=bool(self.s["wear_detect"]), mic=bool(self.s["mic"]),
                                           mic_device=str(self.s["mic_device"]),
                                           offsets=dict(self.s["offsets"])),
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

        head = tk.Frame(self.root, bg=pal["bg"])
        head.pack(fill="x", padx=14, pady=(12, 10))
        tk.Label(head, text="VHMD Link", bg=pal["bg"], fg=pal["fg"], font=f["title"]).pack(side="left")
        self.dev_pill = tk.Label(head, text=t["edition_dev"], bg=pal["warn"], fg=pal["warn_fg"], font=f["small_bold"],
                                 padx=8, pady=2)

        # The cards scroll when the window is shorter than they are (header and
        # footer stay).
        area = tk.Frame(self.root, bg=pal["bg"])
        area.pack(fill="both", expand=True)
        canvas = tk.Canvas(area, bg=pal["bg"], highlightthickness=0)
        bar = ttk.Scrollbar(area, orient="vertical", command=canvas.yview)
        self.body = tk.Frame(canvas, bg=pal["bg"])
        win = canvas.create_window((0, 0), window=self.body, anchor="nw")
        self.body.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.bind("<Configure>", lambda e: canvas.itemconfigure(win, width=e.width))
        canvas.configure(yscrollcommand=bar.set)
        canvas.pack(side="left", fill="both", expand=True)
        bar.pack(side="right", fill="y")
        self.scroll = lambda e: canvas.yview_scroll(int(-e.delta / 120), "units")
        self.root.bind_all("<MouseWheel>", self.scroll)

        # Headset
        c = self.card(self.body, t["headset"])
        row = ttk.Frame(c, style="Card.TFrame")
        row.pack(fill="x", pady=(4, 0))
        self.hs_dot = tk.Label(row, text="●", bg=pal["card"], fg=pal["muted"], font=f["big"])
        self.hs_dot.pack(side="left")
        self.hs_text = tk.Label(row, text="", bg=pal["card"], fg=pal["fg"], font=f["big"])
        self.hs_text.pack(side="left", padx=(6, 0))
        self.hs_sub = ttk.Label(c, text="", style="CardMuted.TLabel", wraplength=470, justify="left")
        self.hs_sub.pack(anchor="w", pady=(2, 0))

        # SteamVR
        c = self.card(self.body, t["steamvr"])
        row = ttk.Frame(c, style="Card.TFrame")
        row.pack(fill="x", pady=(4, 0))
        self.vr_dot = tk.Label(row, text="●", bg=pal["card"], fg=pal["muted"], font=f["big"])
        self.vr_dot.pack(side="left")
        self.vr_text = tk.Label(row, text="", bg=pal["card"], fg=pal["fg"], font=f["big"])
        self.vr_text.pack(side="left", padx=(6, 0))
        self.vr_btn = ttk.Button(row, text=t["start_vr"], style="Accent.TButton", command=self.start_vr)
        # Which way SteamVR runs: switching restarts it (asked first). "元の環境"
        # restores the settings recorded before VHMD changed anything.
        env = ttk.Frame(c, style="Card.TFrame")
        env.pack(fill="x", pady=(8, 0))
        ttk.Label(env, text=t["env"], style="CardMuted.TLabel").pack(side="left", padx=(0, 8))
        self.env_btns = {}
        for m in ("vhmd", "pico", "off"):
            b = ttk.Button(env, text=t["env_" + m], command=lambda m=m: self.switch(target=m))
            b.pack(side="left", padx=(0, 6))
            self.env_btns[m] = b
        self.env_hint = ttk.Label(c, text="", style="CardMuted.TLabel", wraplength=470, justify="left")
        self.env_hint.pack(anchor="w", pady=(2, 0))
        self.match_row = ttk.Frame(c, style="Card.TFrame")
        ttk.Label(self.match_row, text=t["match_hint"], style="CardMuted.TLabel", wraplength=300,
                  justify="left").pack(side="left")
        ttk.Button(self.match_row, text=t["match"], command=lambda: self.switch(target="vhmd", headset=True)).pack(side="right")
        self.busy_lbl = ttk.Label(c, text="", style="CardMuted.TLabel")
        self.busy_lbl.pack(anchor="w")
        self.auto_var = tk.BooleanVar(value=bool(self.s["auto_steamvr"]))
        ttk.Checkbutton(c, text=t["auto_vr"], style="Card.TCheckbutton", variable=self.auto_var,
                        command=lambda: self.set("auto_steamvr", self.auto_var.get())).pack(anchor="w", pady=(4, 0))
        wear_var = tk.BooleanVar(value=bool(self.s["wear_detect"]))
        ttk.Checkbutton(c, text=t["wear"], style="Card.TCheckbutton", variable=wear_var,
                        command=lambda: self.set_perf("wear_detect", wear_var.get())).pack(anchor="w", pady=(4, 0))
        ttk.Label(c, text=t["wear_hint"], style="CardMuted.TLabel", wraplength=470,
                  justify="left").pack(anchor="w", padx=(22, 0))

        # Picture and sound
        c = self.card(self.body, t["video"])
        grid = ttk.Frame(c, style="Card.TFrame")
        grid.pack(fill="x", pady=(4, 0))
        self.q_var = self.combo(grid, 0, t["quality"], [(k, t["q_" + k]) for k, _ in QUALITY], "quality")
        self.c_var = self.combo(grid, 1, t["codec"], [("hevc", t["c_hevc"]), ("av1", t["c_av1"]), ("h264", t["c_h264"])], "codec")
        self.r_var = self.combo(grid, 2, t["refresh"], [(72, t["r72"]), (90, t["r90"])], "refresh")
        mic_var = tk.BooleanVar(value=bool(self.s["mic"]))
        ttk.Checkbutton(c, text=t["mic"], style="Card.TCheckbutton", variable=mic_var,
                        command=lambda: self.set_perf("mic", mic_var.get())).pack(anchor="w", pady=(8, 0))
        ttk.Label(c, text=t["mic_hint"], style="CardMuted.TLabel", wraplength=470,
                  justify="left").pack(anchor="w", padx=(22, 0))
        monitor_var = tk.BooleanVar(value=False)   # not saved: it can howl
        self.monitor_btn = ttk.Checkbutton(c, text=t["mic_monitor"], style="Card.TCheckbutton", variable=monitor_var,
                                           command=lambda: self.set_monitor(monitor_var.get()))
        self.monitor_btn.pack(anchor="w", padx=(22, 0), pady=(6, 0))
        ttk.Label(c, text=t["mic_monitor_hint"], style="CardMuted.TLabel", wraplength=450,
                  justify="left").pack(anchor="w", padx=(44, 0))
        self.mic_lbl = ttk.Label(c, text="", style="Card.TLabel")
        self.mic_lbl.pack(anchor="w", padx=(22, 0))

        # Performance: what trades quality or safety for less delay, each on its own.
        c = self.card(self.body, t["perf"])
        grid = ttk.Frame(c, style="Card.TFrame")
        grid.pack(fill="x", pady=(4, 0))
        self.b_var = self.combo(grid, 0, t["smooth"], [(0, t["b0"]), (1, t["b1"])], "buffer")
        self.s_var = self.combo(grid, 1, t["perf_scale"], [(1.0, t["s100"]), (0.85, t["s85"]), (0.7, t["s70"])],
                                "eye_scale")
        self.v_var = self.combo(grid, 2, t["perf_fov"], [(0, t["v0"]), (1, t["v1"]), (2, t["v2"])], "foveation")
        self.f_var = self.combo(grid, 3, t["perf_fec"], [(20, t["f20"]), (30, t["f30"]), (50, t["f50"])], "fec")
        self.perf_vars = {}
        for key in ("auto_latency", "predict", "amf", "udp_video"):
            var = tk.BooleanVar(value=bool(self.s[key]))
            ttk.Checkbutton(c, text=t["perf_" + key], style="Card.TCheckbutton", variable=var,
                            command=lambda k=key, v=var: self.set_perf(k, v.get())).pack(anchor="w", pady=(6, 0))
            ttk.Label(c, text=t["perf_" + key + "_hint"], style="CardMuted.TLabel", wraplength=470,
                      justify="left").pack(anchor="w", padx=(22, 0))
            self.perf_vars[key] = var

        # Controller buttons
        c = self.card(self.body, t["bind"])
        ttk.Label(c, text=t["bind_hint"], style="CardMuted.TLabel", wraplength=470, justify="left").pack(anchor="w")
        grid = ttk.Frame(c, style="Card.TFrame")
        grid.pack(fill="x", pady=(4, 0))
        self.bind_vars = {}
        for hand, buttons in bindings.BUTTONS.items():
            for button in buttons:
                self.bind_combo(grid, len(self.bind_vars), hand, button)
        ttk.Button(c, text=t["bind_reset"], command=self.reset_bindings).pack(anchor="w", pady=(6, 0))

        # What SteamVR gets
        c = self.card(self.body, t["track"])
        ttk.Label(c, text=t["track_hint"], style="CardMuted.TLabel", wraplength=470, justify="left").pack(anchor="w")
        self.track_grid = ttk.Frame(c, style="Card.TFrame")
        self.track_grid.pack(fill="x", pady=(4, 0))
        self.track_grid.columnconfigure(0, weight=1)
        self.track_rows: dict = {}
        self.track_none = ttk.Label(self.track_grid, text=t["track_none"], style="CardMuted.TLabel")
        self.track_msg = ttk.Label(c, style="CardMuted.TLabel", wraplength=470, justify="left")
        self.update_tracking()

        # Position and angle corrections
        c = self.card(self.body, t["off"])
        ttk.Label(c, text=t["off_hint"], style="CardMuted.TLabel", wraplength=470, justify="left").pack(anchor="w")
        grid = ttk.Frame(c, style="Card.TFrame")
        grid.pack(fill="x", pady=(4, 0))
        ttk.Label(grid, text=t["off_device"], style="Card.TLabel").grid(row=0, column=0, sticky="w", pady=3, padx=(0, 12))
        self.off_device = tk.StringVar()
        self.off_box = ttk.Combobox(grid, textvariable=self.off_device, state="readonly", width=26)
        self.off_box.grid(row=0, column=1, sticky="w", pady=3)
        wheel_scrolls_page(self.off_box, self.scroll)
        self.off_box.bind("<<ComboboxSelected>>", lambda _e: self.show_offset())
        self.off_choices: list = []
        self.off_vars = []
        cm, degrees = offsets.MAX_METERS * 100, offsets.MAX_DEGREES
        fields = [("off_x", cm, 0.5), ("off_y", cm, 0.5), ("off_z", cm, 0.5),
                  ("off_yaw", degrees, 1), ("off_pitch", degrees, 1), ("off_roll", degrees, 1)]
        for row, (key, limit, step) in enumerate(fields, start=1):
            ttk.Label(grid, text=t[key], style="Card.TLabel").grid(row=row, column=0, sticky="w", pady=3, padx=(0, 12))
            var = tk.StringVar(value="0")
            box = ttk.Spinbox(grid, from_=-limit, to=limit, increment=step, width=9, textvariable=var,
                              command=self.offset_changed)
            box.grid(row=row, column=1, sticky="w", pady=3)
            box.bind("<Return>", lambda _e: self.offset_changed())
            box.bind("<FocusOut>", lambda _e: self.offset_changed())
            wheel_scrolls_page(box, self.scroll)
            self.off_vars.append(var)
        ttk.Button(c, text=t["off_reset"], command=self.reset_offset).pack(anchor="w", pady=(6, 0))
        self.update_offset_devices()

        # Body tracking
        c = self.card(self.body, t["body"])
        var = tk.BooleanVar(value=bool(self.s["body_tracking"]))
        ttk.Checkbutton(c, text=t["perf_body_tracking"], style="Card.TCheckbutton", variable=var,
                        command=lambda: self.set_perf("body_tracking", var.get())).pack(anchor="w", pady=(6, 0))
        hand_var = tk.BooleanVar(value=bool(self.s["hand_tracking"]))
        ttk.Checkbutton(c, text=t["perf_hand_tracking"], style="Card.TCheckbutton", variable=hand_var,
                        command=lambda: self.set_perf("hand_tracking", hand_var.get())).pack(anchor="w", pady=(6, 0))
        ttk.Label(c, text=t["perf_hand_tracking_hint"], style="CardMuted.TLabel", wraplength=470,
                  justify="left").pack(anchor="w", padx=(22, 0))
        ttk.Label(c, text=t["perf_body_tracking_hint"], style="CardMuted.TLabel", wraplength=470,
                  justify="left").pack(anchor="w", padx=(22, 0))

        # Developer
        self.dev_card_outer = tk.Frame(self.body, bg=pal["border"], padx=1, pady=1)
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


    def combo(self, parent, row: int, label: str, choices, key: str) -> tk.StringVar:
        ttk.Label(parent, text=label, style="Card.TLabel").grid(row=row, column=0, sticky="w", pady=3, padx=(0, 12))
        labels = [lab for _, lab in choices]
        var = tk.StringVar(value=dict(choices).get(self.s[key], labels[0]))
        box = ttk.Combobox(parent, values=labels, textvariable=var, state="readonly", width=26)
        box.grid(row=row, column=1, sticky="w", pady=3)
        wheel_scrolls_page(box, self.scroll)

        def changed(_e=None) -> None:
            value = next(v for v, lab in choices if lab == var.get())
            if value == self.s[key]:
                return
            self.set(key, value)
            if key == "refresh":   # the headset switches, VHMD follows it (no reconnect)
                self.bridge.o.refresh = float(value)
                self.log(f"refresh rate: {value} Hz")
                return
            if key == "foveation":
                self.bridge.set_video(foveation=int(value))
                self.log(f"foveated encoding: {value}")
                return
            if key == "fec":
                self.bridge.set_video(fec=int(value))
                self.log(f"error correction: {value}%")
                return
            if key == "eye_scale":
                self.bridge.set_video(eye_scale=float(value))
                self.log(f"resolution: {float(value) * 100:.0f}%")
                return
            if key == "quality":
                self.bridge.set_video(bitrate=self.bitrate())
            else:
                self.bridge.set_video(bitrate=self.bitrate(), codec=str(self.s["codec"]), buffer=int(self.s["buffer"]))
            self.log(f"video settings: {self.bitrate():g} Mbit/s, {self.s['codec']}, buffer {self.s['buffer']}")

        box.bind("<<ComboboxSelected>>", changed)
        return var

    def bind_label(self, name: str) -> str:
        return self.t["t_" + bindings.table(self.s["bindings"]).get(name, name.partition(".")[2])]

    def bind_combo(self, parent, row: int, hand: str, button: str) -> None:
        name = f"{hand}.{button}"
        ttk.Label(parent, text=self.t[f"b_{hand}_{button}"], style="Card.TLabel").grid(row=row, column=0, sticky="w",
                                                                                      pady=3, padx=(0, 12))
        choices = [(d, self.t["t_" + d]) for d in bindings.TARGETS[hand]]
        var = tk.StringVar(value=self.bind_label(name))
        box = ttk.Combobox(parent, values=[lab for _, lab in choices], textvariable=var, state="readonly", width=26)
        box.grid(row=row, column=1, sticky="w", pady=3)
        wheel_scrolls_page(box, self.scroll)

        def changed(_e=None) -> None:
            self.set_bindings({**self.s["bindings"], name: next(v for v, lab in choices if lab == var.get())})
            self.log(f"button binding: {name} presses {self.s['bindings'][name]}")

        box.bind("<<ComboboxSelected>>", changed)
        self.bind_vars[name] = var

    def set_bindings(self, overrides: Dict[str, str]) -> None:
        self.set("bindings", overrides)
        self.bridge.set_bindings(overrides)

    def reset_bindings(self) -> None:
        self.set_bindings({})
        for name, var in self.bind_vars.items():
            var.set(self.bind_label(name))
        self.log("button bindings: defaults")

    def track_row(self, row: int, name: str) -> dict:
        t, pal, grid = self.t, self.pal, self.track_grid
        w = {"name": ttk.Label(grid, text=t.get("d_" + name, name), style="Card.TLabel"),
             "dot": tk.Label(grid, text="●", bg=pal["card"], fg=pal["muted"], font=self.fonts["small_bold"]),
             "state": ttk.Label(grid, style="Card.TLabel", width=9),
             "rate": ttk.Label(grid, style="CardMuted.TLabel", width=7, anchor="e"),
             "buttons": [ttk.Button(grid, width=7) for _ in range(2)], "actions": None}
        w["name"].grid(row=row, column=0, sticky="w", pady=2)
        w["dot"].grid(row=row, column=1, padx=(8, 3))
        w["state"].grid(row=row, column=2, sticky="w")
        w["rate"].grid(row=row, column=3, padx=(0, 8))
        for i, button in enumerate(w["buttons"]):
            button.grid(row=row, column=4 + i, padx=(4, 0), pady=1)
        return w

    def update_tracking(self) -> None:
        """The list of what SteamVR gets, as the driver last reported it."""
        t, pal = self.t, self.pal
        rows = tracking.rows(self.bridge.devices)
        if [r["id"] for r in rows] != list(self.track_rows):
            for w in self.track_rows.values():
                for widget in (w["name"], w["dot"], w["state"], w["rate"], *w["buttons"]):
                    widget.destroy()
            self.track_rows = {r["id"]: self.track_row(i, r["name"]) for i, r in enumerate(rows)}
        if rows:
            self.track_none.grid_forget()
        else:
            self.track_none.grid(row=0, column=0, sticky="w", pady=2)
        colors = {"tracking": "#22c55e", "paused": "#f59e0b"}
        for r in rows:
            w = self.track_rows[r["id"]]
            w["dot"].config(fg=colors.get(r["state"], pal["muted"]))
            w["state"].config(text=t["track_" + r["state"]])
            w["rate"].config(text=t["track_rate"].format(rate=r["rate"]) if r["rate"] else "")
            if r["actions"] != w["actions"]:
                w["actions"] = r["actions"]
                for i, button in enumerate(w["buttons"]):
                    if i < len(r["actions"]):
                        key, set_mode = r["actions"][i]
                        button.config(text=t["track_" + key],
                                      command=lambda d=r["id"], n=r["name"], m=set_mode: self.set_tracking(d, n, m))
                        button.grid()
                    else:
                        button.grid_remove()

    def set_tracking(self, device: str, name: str, set_mode: str) -> None:
        error = self.bridge.set_tracking(device, set_mode)
        if error:
            text = self.t["track_failed"].format(name=self.t.get("d_" + name, name), error=error)
            self.track_msg.config(text=text)
            self.track_msg.pack(anchor="w", pady=(4, 0))
            self.log(f"tracking of {device}: {error}")
        else:
            self.track_msg.pack_forget()
            self.log(f"tracking of {device}: {set_mode}")
        self.update_tracking()

    def offset_id(self) -> str:
        name = self.off_device.get()
        return next((i for i, label in self.off_choices if label == name), offsets.HANDS[0])

    def update_offset_devices(self) -> None:
        """The devices to correct, as SteamVR gets them."""
        found = [(i, self.t.get("d_" + name, name)) for i, name in offsets.choices(self.bridge.devices)]
        if found == self.off_choices:
            return
        self.off_choices = found
        self.off_box.configure(values=[label for _, label in found])
        if self.off_device.get() not in [label for _, label in found]:
            self.off_device.set(found[0][1])
            self.show_offset()

    def show_offset(self) -> None:
        value = self.s["offsets"].get(self.offset_id(), offsets.ZERO)
        shown = [v * 100 for v in value["pos"]] + list(value["ypr"])
        for var, v in zip(self.off_vars, shown):
            var.set(f"{v:g}")

    def offset_changed(self) -> None:
        try:
            v = [float(var.get() or 0) for var in self.off_vars]
        except ValueError:
            self.show_offset()   # not a number: back to what is in effect
            return
        device = self.offset_id()
        value = offsets.clean({"pos": [x / 100 for x in v[:3]], "ypr": v[3:]})
        old = self.s["offsets"].get(device, offsets.ZERO)
        if all(abs(a - b) < 1e-6 for a, b in zip(value["pos"] + value["ypr"], old["pos"] + old["ypr"])):
            return
        self.bridge.set_offset(device, value)
        self.set("offsets", self.bridge.o.offsets)
        self.show_offset()
        self.log(f"correction of {device}: {value['pos']} m, {value['ypr']} degrees")

    def reset_offset(self) -> None:
        value = offsets.default(self.offset_id())
        self.bridge.set_offset(self.offset_id(), value)
        self.set("offsets", self.bridge.o.offsets)
        self.show_offset()
        self.log(f"correction of {self.offset_id()}: {value['pos']} m, {value['ypr']} degrees")

    def set(self, key: str, value) -> None:
        self.s[key] = value
        save_settings(self.s)

    def set_monitor(self, on: bool) -> None:
        self.bridge.o.mic_monitor = on           # with the next driver request
        self.log(f"microphone monitor: {'on' if on else 'off'}")

    def set_perf(self, key: str, on: bool) -> None:
        """A performance switch: applied right away (the encoder after a short reconnect)."""
        self.set(key, on)
        if key == "auto_latency":
            self.bridge.o.auto_latency = on
        elif key == "predict":
            self.bridge.o.predict = on           # told to the headset with the next status
        elif key == "body_tracking":
            self.bridge.o.body_tracking = on     # the same
        elif key == "hand_tracking":
            self.bridge.o.hand_tracking = on     # the same
        elif key == "wear_detect":
            self.bridge.o.wear_detect = on       # the same
        elif key == "mic":
            self.bridge.o.mic = on               # the same
        elif key == "amf":
            self.bridge.set_video(encoder_backend="auto" if on else "mf")
        elif key == "udp_video":
            self.bridge.set_video(udp=on)
        self.log(f"performance: {key} {'on' if on else 'off'}")

    # -- actions -----------------------------------------------------------------
    def start_vr(self) -> None:
        self.log("starting SteamVR")
        try:
            mode.start_steamvr()
            self.auto_started_at = time.monotonic()
        except OSError as e:
            self.log(f"could not start SteamVR: {e}")

    def switch(self, target: str, headset: Optional[bool] = None) -> None:
        if self.busy or (target == self.mode and headset is None):
            return
        if mode.steamvr_running() and not messagebox.askyesno(self.t["title"], self.t["confirm_restart"]):
            return

        def work() -> None:
            try:
                # Only the VHMD mode starts SteamVR; the others leave that to the user.
                mode.switch(target, progress=lambda m: (setattr(self, "busy", m), self.log(m)), headset=headset,
                            restart=target == "vhmd")
                self.mode = mode.current_mode()
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
        self.bridge.close()
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
        self.update_offset_devices()
        self.update_tracking()
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
        mic = self.bridge.mic_state()
        self.monitor_btn.state(["disabled" if mic == "off" else "!disabled"])
        if self.bridge.o.mic_monitor and self.bridge.mic_driver.get("monitor") == "same device":
            mic = "monitor_same"
        self.mic_lbl.config(text="" if mic == "off" else t["mic_" + mic].format(device=self.bridge.o.mic_device))

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
        elif running and self.mode == "off":
            text, color = t["vr_off_mode"], pal["muted"]
        elif running:
            text, color = t["vr_other"], "#22c55e"
        elif starting:
            text, color = t["vr_starting"], "#f59e0b"
        else:
            text, color = t["vr_off"], pal["muted"]
        self.vr_dot.config(fg=color)
        self.vr_text.config(text=text)
        self.vr_btn.pack_forget()
        if not running and not starting and not self.busy:
            self.vr_btn.pack(side="right")
        for m, b in self.env_btns.items():
            b.configure(style="Accent.TButton" if m == self.mode else "TButton",
                        state="disabled" if self.busy else "normal")
        self.env_hint.config(text=t["env_hint_" + self.mode] if self.mode in self.env_btns else "")
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
            sync_text = (f"sync {sync.get('shift_ms', 0):+.2f} ms (total {st['sync_total_ms']:+.1f}, "
                         f"wait target {st['sync_target_ms'] or 0.4 * 1000 / max(1.0, h.get('display', 72) or 72):.1f})"
                         if sync.get("ok")
                         else f"sync: {sync.get('error', 'waiting')}" if sync else "sync: -")
            lines = [
                f"PC     video {st['video_in']}/{st['video_out']} fps  {st['mbps']:5.1f} Mbit/s "
                f"(now {st['bitrate_now']:.0f})   poses {st['msgs']}/s   {sync_text}",
                f"Head   received {h.get('received', 0):.0f}  decoded {h.get('decoded', 0):.0f}  "
                f"shown {h.get('shown', 0):.0f}/s at {h.get('display', 0):.0f} Hz  repeats {h.get('repeats', 0):.0f}",
                f"       wait {h.get('wait_ms', 0):.1f} ms (p95 {h.get('wait_p95_ms', 0):.1f})  "
                f"reprojected {h.get('reprojected', 0):.0f}%  audio {h.get('audio_packets', 0):.0f}/s "
                f"underruns {h.get('audio_underruns', 0)}  decode {h.get('decode_ms', 0):.2f} ms "
                f"(max {h.get('decode_max_ms', 0):.1f})",
                f"{t['control']}: {t['anyone'] if control == 'any' else control}   {st['summary']}",
                f"Relay  [{st['data_path']}]  video +{st['relay_ms'][0]:.2f} ms (p99 {st['relay_ms'][1]:.2f}, max {st['relay_ms'][2]:.2f})   "
                f"pose +{st['forward_us'][0]:.0f} us (max {st['forward_us'][1]:.0f})",
                f"Ahead  {h.get('predict_ms', 0):.1f} ms   shown {h.get('late_ms', 0):+.1f} ms after the predicted time",
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
