"""First-person (FPS-style) keyboard and mouse control of the VHMD devices, with
a panel that shows and edits every device's position and angles and a top-down
map. Windows only.

    python -m vhmd_client.fps [--host 127.0.0.1] [--port 39570] [--rate 90]

F1 (or the button) captures the mouse: the mouse turns the head, WASD moves it,
the mouse buttons and number keys drive the controllers. Esc releases the mouse;
then the numbers in the table can be edited (Enter applies, Up/Down or the mouse
wheel nudges the value) and devices can be dragged on the map.

Every non-head device is placed relative to a frame: "head" (follows the head's
position and rotation, like the hands in an FPS), "body" (the head's floor
position and yaw, for trackers) or "fixed" (the play space). The table always
shows world coordinates (SteamVR standing space: meters, +Y up, -Z forward;
degrees: yaw left-positive, pitch up-positive).

Pause stops sending so another program (automation) can drive the devices;
"Take over" adopts the poses SteamVR currently reports (needs pyopenvr) so
manual control continues from where the other program left off.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes
import math
import sys
import time
import tkinter as tk
from tkinter import ttk
from typing import Dict, List, Optional, Sequence, Tuple

from . import math3d
from .client import DEFAULT_HOST, DEFAULT_PORT, VHMDClient

if sys.platform != "win32":
    raise SystemExit("vhmd_client.fps is Windows only")

user32 = ctypes.windll.user32
VK = {
    "W": 0x57, "A": 0x41, "S": 0x53, "D": 0x44, "C": 0x43, "Q": 0x51, "E": 0x45, "R": 0x52, "G": 0x47,
    "M": 0x4D, "1": 0x31, "2": 0x32, "3": 0x33, "4": 0x34, "SPACE": 0x20, "SHIFT": 0x10, "ALT": 0x12,
    "LEFT": 0x25, "RIGHT": 0x27, "UP": 0x26, "DOWN": 0x28, "LBUTTON": 0x01, "RBUTTON": 0x02, "MBUTTON": 0x04,
    "Z": 0x5A, "X": 0x58,
}

TEXT = {
    "en": {
        "title": "VHMD FPS control",
        "capture": "Start FPS control (F1)",
        "captured": "FPS control: mouse = look, Esc = release",
        "send_on": "Sending", "send_off": "Paused (not sending)",
        "pause": "Pause", "resume": "Resume",
        "takeover": "Take over from SteamVR",
        "reset": "Reset", "copy": "Copy as Python", "add": "Add",
        "device": "Device", "frame": "Frame", "steamvr": "SteamVR reports",
        "frames": {"head": "head", "body": "body", "fixed": "fixed"},
        "target": "Mouse moves", "speed": "Speed", "sens": "Sensitivity",
        "loco": "WASD", "loco_move": "moves the head", "loco_stick": "left joystick",
        "map": "Top view (drag devices, wheel = zoom)", "forward": "-Z (forward)",
        "no_openvr": "pyopenvr not installed: SteamVR column and take over disabled",
        "no_driver": "driver not answering on {}:{}",
        "copied": "copied {} lines", "absent": "not in SteamVR", "invalid": "not tracked",
        "help": (
            "Mouse: look  |  WASD: move  Space/C: up/down  Shift: fast  Alt: slow  Q/E: roll  R: level\n"
            "F1 head  F2 left hand  F3 right hand  (mouse moves the selected one, wheel = depth)  L: WASD as joystick\n"
            "LMB: right trigger  RMB: left trigger  MMB: right grip  G: both grips  1/2: A/B  3/4: X/Y  M: menu\n"
            "Arrows: right joystick  Z/X: left/right joystick click  P: pause  Esc: release mouse"
        ),
    },
    "ja": {
        "title": "VHMD FPS 操作",
        "capture": "FPS 操作を開始 (F1)",
        "captured": "FPS 操作中: マウスで視点、Esc で解除",
        "send_on": "送信中", "send_off": "一時停止中(送信していません)",
        "pause": "一時停止", "resume": "再開",
        "takeover": "SteamVR の姿勢を引き継ぐ",
        "reset": "初期位置", "copy": "Python としてコピー", "add": "追加",
        "device": "デバイス", "frame": "基準", "steamvr": "SteamVR 上の値",
        "frames": {"head": "頭", "body": "体", "fixed": "固定"},
        "target": "マウスで動かす", "speed": "速度", "sens": "感度",
        "loco": "WASD", "loco_move": "頭を移動", "loco_stick": "左スティック",
        "map": "上から見た図(ドラッグで移動、ホイールで拡大縮小)", "forward": "-Z(前)",
        "no_openvr": "pyopenvr が無いため、SteamVR 上の値と引き継ぎは使えません",
        "no_driver": "{}:{} のドライバが応答しません",
        "copied": "{} 行をコピーしました", "absent": "SteamVR に無い", "invalid": "トラッキングなし",
        "help": (
            "マウス: 視点  |  WASD: 移動  Space/C: 上下  Shift: 速く  Alt: ゆっくり  Q/E: 首をかしげる  R: 水平に戻す\n"
            "F1 頭  F2 左手  F3 右手(選んだものをマウスで動かす、ホイールで前後)  L: WASD をスティックとして送る\n"
            "左クリック: 右トリガー  右クリック: 左トリガー  中クリック: 右グリップ  G: 両グリップ  1/2: A/B  3/4: X/Y  M: メニュー\n"
            "矢印: 右スティック  Z/X: 左/右スティック押し込み  P: 一時停止  Esc: マウス解除"
        ),
    },
}

FRAMES = ("head", "body", "fixed")
DEFAULT_LOCAL = {
    "left_hand": ("head", (-0.18, -0.25, -0.40)),
    "right_hand": ("head", (0.18, -0.25, -0.40)),
}
# Trackers start where a standing person's body parts would be (body frame).
TRACKER_SPOT = {
    "waist": (0.0, 0.95, 0.05), "chest": (0.0, 1.30, 0.05),
    "left_foot": (-0.12, 0.08, 0.0), "right_foot": (0.12, 0.08, 0.0),
    "left_knee": (-0.12, 0.50, -0.05), "right_knee": (0.12, 0.50, -0.05),
    "left_elbow": (-0.30, 1.10, 0.05), "right_elbow": (0.30, 1.10, 0.05),
    "left_shoulder": (-0.18, 1.42, 0.05), "right_shoulder": (0.18, 1.42, 0.05),
}
COLORS = {"hmd": "#2b6cb0", "left_hand": "#c05621", "right_hand": "#2f855a"}
TRACKER_COLOR = "#6b46c1"
OTHER_COLOR = "#888888"


def ui_language() -> str:
    try:
        return "ja" if ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3FF == 0x11 else "en"
    except Exception:
        return "en"


def key(name: str) -> bool:
    return bool(user32.GetAsyncKeyState(VK[name]) & 0x8000)


def fmt(v: float, column: int) -> str:
    """Meters with mm resolution, degrees with 0.1; never "-0.0"."""
    text = f"{v:.3f}" if column < 3 else f"{v:.1f}"
    return text[1:] if text.strip("-0.") == "" and text.startswith("-") else text


def compose(fpos: Sequence[float], frot: Sequence[float], lpos: Sequence[float], lrot: Sequence[float]):
    p = math3d.rotate(frot, lpos)
    return (fpos[0] + p[0], fpos[1] + p[1], fpos[2] + p[2]), math3d.normalize(math3d.multiply(frot, lrot))


def decompose(fpos: Sequence[float], frot: Sequence[float], wpos: Sequence[float], wrot: Sequence[float]):
    inv = math3d.conjugate(frot)
    lpos = math3d.rotate(inv, (wpos[0] - fpos[0], wpos[1] - fpos[1], wpos[2] - fpos[2]))
    return lpos, math3d.normalize(math3d.multiply(inv, wrot))


class Head:
    def __init__(self) -> None:
        self.reset()

    def reset(self) -> None:
        self.pos = [0.0, 1.6, 0.0]
        self.yaw = self.pitch = self.roll = 0.0

    @property
    def rot(self):
        return math3d.from_euler(self.yaw, self.pitch, self.roll)

    def set_rot(self, q) -> None:
        self.yaw, self.pitch, self.roll = math3d.to_euler(q)


class Device:
    """A non-head device: a pose relative to one of the frames."""

    def __init__(self, dev_id: str, kind: str, role: str = "") -> None:
        self.id, self.kind, self.role = dev_id, kind, role
        self.frame = "fixed"
        self.lpos: Tuple[float, float, float] = (0.0, 1.0, 0.0)
        self.lrot = math3d.IDENTITY


class App:
    def __init__(self, root: tk.Tk, client: VHMDClient, rate: float, lang: str) -> None:
        self.root, self.c, self.rate, self.t = root, client, rate, TEXT[lang]
        self.head = Head()
        self.devices: Dict[str, Device] = {}
        self.status_devices: Dict[str, dict] = {}
        self.hmd_enabled = True
        self.sending = True
        self.captured = False
        self.target = "hmd"          # what mouse movement drives
        self.loco_stick = False      # WASD -> left joystick instead of moving the head
        self.speed = tk.DoubleVar(value=1.5)
        self.sens = tk.DoubleVar(value=0.15)
        self.scale = 80.0            # map pixels per meter
        self.drag: Optional[str] = None
        self.reported: Dict[str, Tuple[Tuple[float, float, float], tuple, bool]] = {}
        self.others: List[Tuple[str, Tuple[float, float, float], tuple]] = []
        self.entries: Dict[str, List[ttk.Entry]] = {}
        self.frame_boxes: Dict[str, ttk.Combobox] = {}
        self.report_labels: Dict[str, ttk.Label] = {}
        self.last = self.next_t = time.perf_counter()
        ctypes.windll.winmm.timeBeginPeriod(1)   # Tk timers are otherwise ~15.6 ms apart
        self.vr = None
        self.vr_index: Dict[str, int] = {}
        self.vr_next_scan = 0.0
        self.vr_names: Dict[int, str] = {}
        self.real_hmd = None         # another driver's HMD (e.g. PICO) when ours is disabled
        self.message = tk.StringVar()

        self.load_devices()
        self.init_openvr()
        self.build_ui()
        self.reset_all()
        self.tick()

    # -- setup ----------------------------------------------------------------
    def load_devices(self) -> None:
        try:
            st = self.c.status()
        except Exception:
            self.message.set(self.t["no_driver"].format(*self.c.addr))
            st = {"devices": [{"id": "hmd", "kind": "hmd", "enabled": True},
                              {"id": "left_hand", "kind": "controller", "enabled": True},
                              {"id": "right_hand", "kind": "controller", "enabled": True}]}
        for d in st.get("devices", []):
            self.status_devices[d["id"]] = d
            if d["id"] == "hmd":
                self.hmd_enabled = d.get("enabled", True)
            elif d.get("kind") == "controller" and d.get("enabled", True):
                self.devices[d["id"]] = Device(d["id"], "controller", d.get("role", ""))

    def init_openvr(self) -> None:
        try:
            import openvr
            self.openvr = openvr
            self.vr = openvr.init(openvr.VRApplication_Background)
        except ImportError:
            self.vr = None
            self.message.set(self.t["no_openvr"])
        except Exception as e:  # SteamVR not running
            self.vr = None
            self.message.set(f"SteamVR: {e}")

    # -- UI -------------------------------------------------------------------
    def build_ui(self) -> None:
        t = self.t
        r = self.root
        r.title(t["title"])
        top = ttk.Frame(r, padding=6)
        top.pack(fill="x")
        self.capture_btn = ttk.Button(top, text=t["capture"], command=self.start_capture)
        self.capture_btn.pack(side="left")
        self.pause_btn = ttk.Button(top, text=t["pause"], command=self.toggle_send)
        self.pause_btn.pack(side="left", padx=4)
        self.take_btn = ttk.Button(top, text=t["takeover"], command=self.take_over,
                                   state="normal" if self.vr else "disabled")
        self.take_btn.pack(side="left")
        ttk.Button(top, text=t["reset"], command=self.reset_all).pack(side="left", padx=4)
        ttk.Button(top, text=t["copy"], command=self.copy_python).pack(side="left")

        self.banner = tk.Label(r, text=t["send_on"], anchor="w", padx=8, pady=4, bg="#e6fffa")
        self.banner.pack(fill="x")

        opts = ttk.Frame(r, padding=(6, 2))
        opts.pack(fill="x")
        ttk.Label(opts, text=t["target"]).pack(side="left")
        self.target_lbl = ttk.Label(opts, width=12)
        self.target_lbl.pack(side="left")
        ttk.Label(opts, text=t["loco"]).pack(side="left", padx=(10, 2))
        self.loco_lbl = ttk.Label(opts, width=12)
        self.loco_lbl.pack(side="left")
        ttk.Label(opts, text=t["speed"] + " (m/s)").pack(side="left", padx=(10, 2))
        ttk.Spinbox(opts, from_=0.1, to=10, increment=0.1, width=5, textvariable=self.speed).pack(side="left")
        ttk.Label(opts, text=t["sens"] + " (deg/px)").pack(side="left", padx=(10, 2))
        ttk.Spinbox(opts, from_=0.02, to=1, increment=0.01, width=5, textvariable=self.sens).pack(side="left")

        body = ttk.Frame(r, padding=6)
        body.pack(fill="both", expand=True)
        self.table = ttk.Frame(body)
        self.table.pack(side="left", anchor="n", fill="y")
        right = ttk.Frame(body)
        right.pack(side="left", fill="both", expand=True, padx=(10, 0))
        ttk.Label(right, text=t["map"]).pack(anchor="w")
        self.canvas = tk.Canvas(right, width=440, height=440, bg="white", highlightthickness=1,
                                highlightbackground="#ccc")
        self.canvas.pack(fill="both", expand=True)
        self.canvas.bind("<ButtonPress-1>", self.map_press)
        self.canvas.bind("<B1-Motion>", self.map_drag)
        self.canvas.bind("<ButtonRelease-1>", lambda e: setattr(self, "drag", None))

        add = ttk.Frame(r, padding=(6, 0))
        add.pack(fill="x")
        self.add_box = ttk.Combobox(add, width=24, state="readonly", values=self.tracker_choices())
        self.add_box.pack(side="left")
        if self.add_box["values"]:
            self.add_box.current(0)
        ttk.Button(add, text=t["add"], command=self.add_tracker).pack(side="left", padx=4)
        ttk.Label(add, textvariable=self.message, foreground="#a33").pack(side="left", padx=8)

        ttk.Label(r, text=t["help"], padding=6, justify="left", foreground="#444").pack(fill="x")

        r.bind_all("<F1>", lambda e: self.select_target("hmd", capture=True))
        r.bind_all("<F2>", lambda e: self.select_target("left_hand", capture=True))
        r.bind_all("<F3>", lambda e: self.select_target("right_hand", capture=True))
        r.bind_all("<Escape>", lambda e: self.release_capture())
        r.bind_all("<MouseWheel>", self.wheel)
        r.bind_all("<KeyPress-l>", lambda e: self.captured and self.toggle_loco())
        r.bind_all("<KeyPress-p>", lambda e: self.captured and self.toggle_send())
        r.protocol("WM_DELETE_WINDOW", self.close)
        self.rebuild_table()

    def tracker_choices(self) -> List[str]:
        out = []
        for d in self.status_devices.values():
            if d.get("kind") == "tracker" and d.get("enabled", True) and d["id"] not in self.devices:
                out.append(f"{d['id']} ({d.get('role', '')})")
        return out

    def label_of(self, dev_id: str) -> str:
        d = self.devices.get(dev_id)
        return f"{dev_id} ({d.role})" if d and d.kind == "tracker" and d.role else dev_id

    def rebuild_table(self) -> None:
        for w in self.table.winfo_children():
            w.destroy()
        self.entries.clear()
        self.frame_boxes.clear()
        self.report_labels.clear()
        heads = [self.t["device"], self.t["frame"], "X", "Y", "Z", "Yaw", "Pitch", "Roll", self.t["steamvr"]]
        for col, h in enumerate(heads):
            ttk.Label(self.table, text=h, font=("", 9, "bold")).grid(row=0, column=col, padx=2, sticky="w")
        ids = (["hmd"] if self.hmd_enabled else []) + list(self.devices)
        for row, dev_id in enumerate(ids, start=1):
            color = COLORS.get(dev_id, TRACKER_COLOR)
            tk.Label(self.table, text=self.label_of(dev_id), fg=color, anchor="w").grid(row=row, column=0, sticky="w")
            if dev_id != "hmd":
                names = [self.t["frames"][f] for f in FRAMES]
                box = ttk.Combobox(self.table, width=5, state="readonly", values=names)
                box.current(FRAMES.index(self.devices[dev_id].frame))
                box.bind("<<ComboboxSelected>>", lambda e, i=dev_id: self.change_frame(i))
                box.grid(row=row, column=1, padx=2)
                self.frame_boxes[dev_id] = box
            ents = []
            for k in range(6):
                e = ttk.Entry(self.table, width=7, justify="right")
                e.grid(row=row, column=2 + k, padx=1, pady=1)
                e.bind("<Return>", lambda ev, i=dev_id: self.apply_row(i))
                e.bind("<KP_Enter>", lambda ev, i=dev_id: self.apply_row(i))
                e.bind("<Up>", lambda ev, i=dev_id, k=k: self.nudge(i, k, +1, ev))
                e.bind("<Down>", lambda ev, i=dev_id, k=k: self.nudge(i, k, -1, ev))
                e.bind("<MouseWheel>", lambda ev, i=dev_id, k=k: self.nudge(i, k, 1 if ev.delta > 0 else -1, ev))
                e.bind("<FocusOut>", lambda ev: self.refresh_table(force=True))
                ents.append(e)
            self.entries[dev_id] = ents
            lbl = ttk.Label(self.table, text="-", foreground="#666", width=34)
            lbl.grid(row=row, column=8, padx=4, sticky="w")
            self.report_labels[dev_id] = lbl
        self.refresh_table(force=True)

    # -- poses ----------------------------------------------------------------
    def frame_of(self, name: str):
        if name == "head":
            return tuple(self.head.pos), self.head.rot
        if name == "body":
            return (self.head.pos[0], 0.0, self.head.pos[2]), math3d.from_euler(self.head.yaw)
        return (0.0, 0.0, 0.0), math3d.IDENTITY

    def world(self, dev_id: str):
        if dev_id == "hmd":
            return tuple(self.head.pos), self.head.rot
        d = self.devices[dev_id]
        return compose(*self.frame_of(d.frame), d.lpos, d.lrot)

    def set_world(self, dev_id: str, pos, rot) -> None:
        if dev_id == "hmd":
            self.head.pos = list(pos)
            self.head.set_rot(rot)
            return
        d = self.devices[dev_id]
        d.lpos, d.lrot = decompose(*self.frame_of(d.frame), pos, rot)

    def reset_device(self, d: Device) -> None:
        if d.kind == "controller":
            d.frame, d.lpos = DEFAULT_LOCAL.get(d.id, ("head", (0.0, -0.25, -0.4)))
        else:
            d.frame, d.lpos = "body", TRACKER_SPOT.get(d.role, (0.0, 1.0, 0.3))
        d.lrot = math3d.IDENTITY

    def reset_all(self) -> None:
        self.head.reset()
        for d in self.devices.values():
            self.reset_device(d)
        for dev_id, box in self.frame_boxes.items():
            box.current(FRAMES.index(self.devices[dev_id].frame))
        self.refresh_table(force=True)

    def change_frame(self, dev_id: str) -> None:
        d = self.devices[dev_id]
        pos, rot = self.world(dev_id)        # keep the device where it is
        d.frame = FRAMES[self.frame_boxes[dev_id].current()]
        self.set_world(dev_id, pos, rot)

    def add_tracker(self) -> None:
        sel = self.add_box.get()
        if not sel:
            return
        dev_id = sel.split(" ")[0]
        info = self.status_devices.get(dev_id, {})
        d = Device(dev_id, "tracker", info.get("role", ""))
        self.reset_device(d)
        self.devices[dev_id] = d
        self.add_box["values"] = self.tracker_choices()
        self.add_box.set(self.add_box["values"][0] if self.add_box["values"] else "")
        self.rebuild_table()

    # -- table ----------------------------------------------------------------
    def row_values(self, dev_id: str) -> List[float]:
        pos, rot = self.world(dev_id)
        if dev_id == "hmd":
            ang = (self.head.yaw, self.head.pitch, self.head.roll)
        else:
            ang = math3d.to_euler(rot)
        return [*pos, *ang]

    def refresh_table(self, force: bool = False) -> None:
        focus = self.root.focus_get()
        for dev_id, ents in self.entries.items():
            vals = self.row_values(dev_id)
            for k, e in enumerate(ents):
                if e is focus and not force:
                    continue
                text = fmt(vals[k], k)
                if e.get() != text:
                    e.delete(0, "end")
                    e.insert(0, text)

    def read_row(self, dev_id: str) -> Optional[List[float]]:
        try:
            return [float(e.get()) for e in self.entries[dev_id]]
        except ValueError:
            return None

    def apply_row(self, dev_id: str) -> None:
        v = self.read_row(dev_id)
        if v is None:
            self.refresh_table(force=True)
            return
        if dev_id == "hmd":
            self.head.pos = v[:3]
            self.head.yaw, self.head.pitch, self.head.roll = v[3], max(-89.9, min(89.9, v[4])), v[5]
        else:
            self.set_world(dev_id, v[:3], math3d.from_euler(*v[3:]))
        self.refresh_table(force=True)

    def nudge(self, dev_id: str, k: int, sign: int, ev) -> str:
        v = self.read_row(dev_id)
        if v is None:
            return "break"
        step = (0.01 if k < 3 else 1.0) * (10 if ev.state & 0x1 else 1)  # Shift = x10
        v[k] += sign * step
        e = self.entries[dev_id][k]
        e.delete(0, "end")
        e.insert(0, fmt(v[k], k))
        self.apply_row(dev_id)
        e.focus_set()
        return "break"

    # -- capture --------------------------------------------------------------
    def hwnd(self) -> int:
        return int(self.root.wm_frame(), 16)

    def warp_point(self) -> Tuple[int, int]:
        b = self.banner
        return b.winfo_rootx() + b.winfo_width() // 2, b.winfo_rooty() + b.winfo_height() // 2

    def start_capture(self) -> None:
        if self.captured:
            return
        self.captured = True
        self.banner.focus_set()           # keys must not go into an entry
        self.root.config(cursor="none")
        self.banner.config(cursor="none")
        user32.SetCursorPos(*self.warp_point())
        self.update_banner()

    def release_capture(self) -> None:
        if not self.captured:
            return
        self.captured = False
        self.root.config(cursor="")
        self.banner.config(cursor="")
        self.update_banner()

    def select_target(self, target: str, capture: bool = False) -> None:
        if target != "hmd" and target not in self.devices:
            return
        self.target = target
        if capture:
            self.start_capture()
        self.update_banner()

    def toggle_loco(self) -> None:
        self.loco_stick = not self.loco_stick
        self.update_banner()

    def toggle_send(self) -> None:
        self.sending = not self.sending
        self.pause_btn.config(text=self.t["pause"] if self.sending else self.t["resume"])
        self.update_banner()

    def update_banner(self) -> None:
        t = self.t
        text = t["send_on"] if self.sending else t["send_off"]
        if self.captured:
            text = t["captured"] + "   |   " + text
        bg = "#fefcbf" if self.captured else ("#e6fffa" if self.sending else "#fed7d7")
        self.banner.config(text=text, bg=bg)
        self.target_lbl.config(text=self.target)
        self.loco_lbl.config(text=t["loco_stick"] if self.loco_stick else t["loco_move"])

    def foreground(self) -> bool:
        fg = user32.GetForegroundWindow()
        return fg == self.hwnd() or user32.GetAncestor(fg, 2) == self.hwnd()  # GA_ROOT

    def wheel(self, ev) -> None:
        step = 1 if ev.delta > 0 else -1
        if self.captured:
            if self.target == "hmd":
                self.speed.set(round(max(0.1, min(10.0, self.speed.get() * (1.25 ** step))), 2))
            else:
                d = self.devices[self.target]
                self.move_local(d, (0.0, 0.0, -0.02 * step))
        elif ev.widget is self.canvas:
            self.scale = max(15.0, min(600.0, self.scale * (1.15 ** step)))

    def move_local(self, d: Device, delta) -> None:
        """Moves a device by delta given in the head's frame."""
        pos, rot = self.world(d.id)
        dw = math3d.rotate(self.head.rot, delta)
        self.set_world(d.id, (pos[0] + dw[0], pos[1] + dw[1], pos[2] + dw[2]), rot)

    # -- per frame ------------------------------------------------------------
    def tick(self) -> None:
        now = time.perf_counter()
        dt = min(0.1, now - self.last)
        self.last = now
        inputs: Dict[str, Dict[str, object]] = {}
        if self.captured and not self.foreground():
            self.release_capture()
        if self.captured:
            self.control(dt, inputs)
        if self.vr:
            self.read_steamvr(now)
            if not self.hmd_enabled and self.real_hmd:
                # Without our HMD the head is the real one: trackers in the "body"
                # frame and hands in the "head" frame follow the person wearing it.
                self.head.pos = list(self.real_hmd[0])
                self.head.set_rot(self.real_hmd[1])
        if self.sending:
            self.send(inputs)
        if int(now * 10) != int((now - dt) * 10):   # 10 Hz is enough for the panel
            self.refresh_table()
            self.refresh_reports()
            self.draw_map()
        self.next_t += 1.0 / self.rate
        if self.next_t < time.perf_counter() - 0.1:   # fell far behind: do not catch up
            self.next_t = time.perf_counter()
        self.root.after(max(1, round((self.next_t - time.perf_counter()) * 1000)), self.tick)

    def control(self, dt: float, inputs: Dict[str, Dict[str, object]]) -> None:
        h = self.head
        cx, cy = self.warp_point()
        pt = ctypes.wintypes.POINT()
        user32.GetCursorPos(ctypes.byref(pt))
        dx, dy = pt.x - cx, pt.y - cy
        user32.SetCursorPos(cx, cy)
        if self.target == "hmd":
            s = self.sens.get()
            h.yaw = (h.yaw - dx * s + 180.0) % 360.0 - 180.0
            h.pitch = max(-89.0, min(89.0, h.pitch - dy * s))
        else:
            self.move_local(self.devices[self.target], (dx * 0.001, -dy * 0.001, 0.0))

        speed = self.speed.get() * (3.0 if key("SHIFT") else 1.0) * (0.25 if key("ALT") else 1.0)
        fwd = (key("W") - key("S"))
        side = (key("D") - key("A"))
        if self.loco_stick:
            inputs.setdefault("left_hand", {}).update(
                joystick_x=float(side), joystick_y=float(fwd), joystick_touch=bool(side or fwd))
        else:
            y = math.radians(h.yaw)
            # forward is -Z turned by yaw; right is +X turned by yaw
            h.pos[0] += (-math.sin(y) * fwd + math.cos(y) * side) * speed * dt
            h.pos[2] += (-math.cos(y) * fwd - math.sin(y) * side) * speed * dt
        h.pos[1] = max(0.05, h.pos[1] + (key("SPACE") - key("C")) * speed * dt)
        h.roll += (key("Q") - key("E")) * 60.0 * dt
        if key("R"):
            h.roll = 0.0

        rx = key("RIGHT") - key("LEFT")
        ry = key("UP") - key("DOWN")
        right = inputs.setdefault("right_hand", {})
        left = inputs.setdefault("left_hand", {})
        rt, lt, rg = key("LBUTTON"), key("RBUTTON"), key("MBUTTON") or key("G")
        right.update(trigger=1.0 if rt else 0.0, trigger_click=rt, trigger_touch=rt,
                     grip=1.0 if rg else 0.0, grip_click=rg,
                     a=key("1"), b=key("2"), joystick_click=key("X"),
                     joystick_x=float(rx), joystick_y=float(ry), joystick_touch=bool(rx or ry))
        lg = key("G")
        left.update(trigger=1.0 if lt else 0.0, trigger_click=lt, trigger_touch=lt,
                    grip=1.0 if lg else 0.0, grip_click=lg,
                    x=key("3"), y=key("4"), menu=key("M"), joystick_click=key("Z"))
        if not self.loco_stick:
            left.update(joystick_x=0.0, joystick_y=0.0, joystick_touch=False)

    def send(self, inputs: Dict[str, Dict[str, object]]) -> None:
        try:
            with self.c.batch():
                if self.hmd_enabled:
                    pos, rot = self.world("hmd")
                    self.c.pose("hmd", pos, rot)
                for dev_id in self.devices:
                    pos, rot = self.world(dev_id)
                    self.c.pose(dev_id, pos, rot)
                for dev_id, values in inputs.items():
                    if dev_id in self.devices:
                        self.c.inputs(dev_id, values)
        except OSError:
            pass

    # -- SteamVR readback -----------------------------------------------------
    def read_steamvr(self, now: float) -> None:
        ovr = self.openvr
        try:
            poses = self.vr.getDeviceToAbsoluteTrackingPose(ovr.TrackingUniverseStanding, 0, ovr.k_unMaxTrackedDeviceCount)
        except Exception:
            return
        if now >= self.vr_next_scan:          # serials of devices that appeared since
            self.vr_next_scan = now + 2.0
            self.vr_index.clear()
            self.vr_names = {}
            for i in range(ovr.k_unMaxTrackedDeviceCount):
                if not self.vr.isTrackedDeviceConnected(i):
                    continue
                try:
                    serial = self.vr.getStringTrackedDeviceProperty(i, ovr.Prop_SerialNumber_String)
                except Exception:
                    continue
                self.vr_names[i] = serial
                for dev_id, info in self.status_devices.items():
                    if info.get("serial") == serial:
                        self.vr_index[dev_id] = i
                if serial in ("VHMD-HAND-L", "VHMD-HAND-R"):
                    self.vr_index["left_hand" if serial.endswith("L") else "right_hand"] = i
        self.reported.clear()
        self.others = []
        ours = set(self.vr_index.values())
        self.real_hmd = None
        for i, serial in self.vr_names.items():
            p = poses[i]
            pos, rot = math3d.from_matrix34(p.mDeviceToAbsoluteTracking)
            if i in ours:
                continue
            if p.bPoseIsValid:
                self.others.append((serial, pos, rot))
                if i == ovr.k_unTrackedDeviceIndex_Hmd:
                    self.real_hmd = (pos, rot)
        for dev_id, i in self.vr_index.items():
            p = poses[i]
            pos, rot = math3d.from_matrix34(p.mDeviceToAbsoluteTracking)
            self.reported[dev_id] = (pos, rot, bool(p.bPoseIsValid))

    def refresh_reports(self) -> None:
        for dev_id, lbl in self.report_labels.items():
            r = self.reported.get(dev_id)
            if not self.vr:
                text = "-"
            elif r is None:
                text = self.t["absent"]
            else:
                pos, rot, valid = r
                yaw, pitch, roll = math3d.to_euler(rot)
                text = (f"{pos[0]:6.3f} {pos[1]:6.3f} {pos[2]:6.3f} | {yaw:6.1f} {pitch:5.1f} {roll:6.1f}"
                        if valid else self.t["invalid"])
            lbl.config(text=text)

    def take_over(self) -> None:
        if not self.vr:
            return
        self.read_steamvr(time.perf_counter())
        for dev_id, (pos, rot, valid) in list(self.reported.items()):
            if not valid:
                continue
            if dev_id == "hmd" and self.hmd_enabled:
                self.head.pos = list(pos)
                yaw, pitch, roll = math3d.to_euler(rot)
                self.head.yaw, self.head.pitch, self.head.roll = yaw, max(-89.0, min(89.0, pitch)), roll
        for dev_id, (pos, rot, valid) in list(self.reported.items()):
            if valid and dev_id in self.devices:
                self.set_world(dev_id, pos, rot)   # relative to the head adopted above
        self.refresh_table(force=True)

    # -- map ------------------------------------------------------------------
    def to_screen(self, x: float, z: float) -> Tuple[float, float]:
        w, h = self.canvas.winfo_width(), self.canvas.winfo_height()
        return w / 2 + x * self.scale, h / 2 + z * self.scale

    def to_world(self, sx: float, sy: float) -> Tuple[float, float]:
        w, h = self.canvas.winfo_width(), self.canvas.winfo_height()
        return (sx - w / 2) / self.scale, (sy - h / 2) / self.scale

    def draw_map(self) -> None:
        cv = self.canvas
        cv.delete("all")
        w, h = cv.winfo_width(), cv.winfo_height()
        step = 0.5 if self.scale >= 40 else 1.0 if self.scale >= 20 else 2.0
        x0, z0 = self.to_world(0, 0)
        x1, z1 = self.to_world(w, h)
        k = math.floor(x0 / step)
        while k * step <= x1:
            sx = self.to_screen(k * step, 0)[0]
            cv.create_line(sx, 0, sx, h, fill="#bbb" if k * step == 0 else "#eee")
            k += 1
        k = math.floor(z0 / step)
        while k * step <= z1:
            sy = self.to_screen(0, k * step)[1]
            cv.create_line(0, sy, w, sy, fill="#bbb" if k * step == 0 else "#eee")
            k += 1
        cv.create_text(w - 6, h / 2 - 8, text="+X", anchor="e", fill="#888")
        cv.create_text(w / 2 + 6, 8, text=self.t["forward"], anchor="nw", fill="#888")
        cv.create_text(6, h - 6, text=f"grid {step:g} m", anchor="sw", fill="#888")

        for serial, pos, rot in self.others:      # e.g. a real HMD and its controllers
            sx, sy = self.to_screen(pos[0], pos[2])
            cv.create_oval(sx - 5, sy - 5, sx + 5, sy + 5, outline=OTHER_COLOR, dash=(2, 2))
            cv.create_text(sx + 8, sy, text=f"{serial} y={pos[1]:.2f}", anchor="w", fill=OTHER_COLOR,
                           font=("", 8))
        for dev_id, (pos, rot, valid) in self.reported.items():   # where SteamVR has ours
            if valid:
                sx, sy = self.to_screen(pos[0], pos[2])
                cv.create_oval(sx - 9, sy - 9, sx + 9, sy + 9, outline=COLORS.get(dev_id, TRACKER_COLOR))

        ids = list(self.devices) + (["hmd"] if self.hmd_enabled else [])
        for dev_id in ids:
            pos, rot = self.world(dev_id)
            color = COLORS.get(dev_id, TRACKER_COLOR)
            sx, sy = self.to_screen(pos[0], pos[2])
            f = math3d.rotate(rot, (0.0, 0.0, -1.0))
            n = math.hypot(f[0], f[2]) or 1.0
            ex, ey = sx + f[0] / n * 18, sy + f[2] / n * 18
            if dev_id == "hmd":
                cv.create_line(sx, sy, ex, ey, fill=color, width=3, arrow="last")
                cv.create_oval(sx - 7, sy - 7, sx + 7, sy + 7, fill=color, outline="")
            elif self.devices[dev_id].kind == "controller":
                cv.create_line(sx, sy, ex, ey, fill=color, width=2)
                cv.create_oval(sx - 5, sy - 5, sx + 5, sy + 5, fill=color, outline="")
            else:
                cv.create_rectangle(sx - 5, sy - 5, sx + 5, sy + 5, fill=color, outline="")
            cv.create_text(sx + 9, sy - 9, text=f"{dev_id} y={pos[1]:.2f}", anchor="w", fill=color, font=("", 8))

    def map_press(self, ev) -> None:
        if self.captured:
            return
        best, dist = None, 12.0
        for dev_id in (["hmd"] if self.hmd_enabled else []) + list(self.devices):
            pos, _ = self.world(dev_id)
            sx, sy = self.to_screen(pos[0], pos[2])
            d = math.hypot(sx - ev.x, sy - ev.y)
            if d < dist:
                best, dist = dev_id, d
        self.drag = best

    def map_drag(self, ev) -> None:
        if not self.drag or self.captured:
            return
        x, z = self.to_world(ev.x, ev.y)
        pos, rot = self.world(self.drag)
        self.set_world(self.drag, (x, pos[1], z), rot)
        self.refresh_table(force=True)
        self.draw_map()

    # -- misc -----------------------------------------------------------------
    def copy_python(self) -> None:
        lines = []
        for dev_id in (["hmd"] if self.hmd_enabled else []) + list(self.devices):
            v = self.row_values(dev_id)
            lines.append(f'c.pose("{dev_id}", ({v[0]:.3f}, {v[1]:.3f}, {v[2]:.3f}), '
                         f"math3d.from_euler(yaw={v[3]:.1f}, pitch={v[4]:.1f}, roll={v[5]:.1f}))")
        self.root.clipboard_clear()
        self.root.clipboard_append("\n".join(lines) + "\n")
        self.message.set(self.t["copied"].format(len(lines)))

    def close(self) -> None:
        self.release_capture()
        ctypes.windll.winmm.timeEndPeriod(1)
        if self.vr:
            try:
                self.openvr.shutdown()
            except Exception:
                pass
        self.root.destroy()


def main() -> None:
    ap = argparse.ArgumentParser(description="FPS-style keyboard/mouse control of the VHMD devices (Windows)")
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--rate", type=float, default=90.0, help="poses sent per second")
    ap.add_argument("--lang", choices=("en", "ja"), default=None)
    args = ap.parse_args()
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)   # cursor and window in the same pixels
    except Exception:
        pass
    root = tk.Tk()
    with VHMDClient(args.host, args.port) as c:
        App(root, c, args.rate, args.lang or ui_language())
        root.mainloop()


if __name__ == "__main__":
    main()
