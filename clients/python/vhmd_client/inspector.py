"""A window that shows what one VHMD device hands SteamVR: its buttons, sticks
and triggers, the finger bones, the vibrations apps ask for and the gaze.

It asks the driver (config op "inspect") ten times a second, so it shows what
SteamVR got, derived clicks and button-made hand shapes included, whatever
sends the data (VHMD Link, a script, VMT). Windows and Linux.

    python -m vhmd_client.inspector [--device left_hand]

VHMD Link opens the same window from its developer view.
"""

from __future__ import annotations

import argparse
import math
import threading
import time
import tkinter as tk
from tkinter import ttk
from typing import Callable, Dict, List, Optional, Sequence, Tuple

from . import math3d
from .ui_style import PALETTES, os_theme, setup_style, ui_fonts, ui_language

Request = Callable[..., dict]

# OpenVR hand skeleton: parent of each of the 31 bones (-1 = none). The five
# aux bones (26..30) are left out of the drawing.
BONE_PARENTS = [-1, 0,
                1, 2, 3, 4,          # thumb
                1, 6, 7, 8, 9,       # index
                1, 11, 12, 13, 14,   # middle
                1, 16, 17, 18, 19,   # ring
                1, 21, 22, 23, 24,   # pinky
                0, 0, 0, 0, 0]
FINGERS = [(2, 5), (6, 10), (11, 15), (16, 20), (21, 25)]
FINGER_COLORS = ["#f59e0b", "#4c8dff", "#22c55e", "#a855f7", "#ef4444"]

# Inputs drawn as a stick (x, y) rather than two bars.
STICKS = [("joystick", "joystick_x", "joystick_y"), ("trackpad", "trackpad_x", "trackpad_y")]
TWO_SIDED = {"joystick_x", "joystick_y", "trackpad_x", "trackpad_y"}

TEXT = {
    "en": {"title": "VHMD inputs and hands", "device": "Device", "pause": "Pause", "given": "What SteamVR got",
           "received": "What arrived (as sent)", "hand": "Finger bones", "from_top": "from above",
           "from_side": "from the side", "haptic": "Vibration", "no_haptic": "no app has asked yet",
           "haptic_line": "{count} times; last {age:.1f} s ago: {amp:.2f} strength, {freq:.0f} Hz, {dur:.3f} s",
           "gaze": "Gaze", "pose": "Pose", "no_bones": "no finger bones (this device has no skeleton)",
           "source_bones": "sent as bones", "source_curl": "made from the sent finger curls",
           "source_buttons": "made from the buttons", "none": "nothing yet",
           "error": "No answer from the VHMD driver: is SteamVR running in VHMD mode?",
           "inactive": "(not connected in SteamVR)", "twin_hand": "hand", "twin_ctl": "controller"},
    "ja": {"title": "VHMD の入力と手", "device": "機器", "pause": "一時停止", "given": "SteamVR に渡した値",
           "received": "届いた値(送られたまま)", "hand": "指の骨", "from_top": "上から", "from_side": "横から",
           "haptic": "振動", "no_haptic": "アプリからの振動の要求はまだありません",
           "haptic_line": "{count} 回。最後は {age:.1f} 秒前: 強さ {amp:.2f}、{freq:.0f} Hz、{dur:.3f} 秒",
           "gaze": "視線", "pose": "姿勢", "no_bones": "指の骨はありません(この機器はスケルトンを持たない)",
           "source_bones": "送られた骨のまま", "source_curl": "送られた指の曲げから作った形",
           "source_buttons": "ボタンから作った形", "none": "まだありません",
           "error": "VHMD のドライバから応答がありません。SteamVR は VHMD モードで動いていますか?",
           "inactive": "(SteamVR では未接続)", "twin_hand": "手", "twin_ctl": "コントローラー"},
}


def bone_positions(bones: Sequence[Sequence[float]]) -> List[Tuple[float, float, float]]:
    """World positions of the bones ([x, y, z, qw, qx, qy, qz], each relative to
    its parent), in the skeleton's space (meters, +Y up, -Z forward)."""
    pos: List[Tuple[float, float, float]] = [(0.0, 0.0, 0.0)] * len(bones)
    rot: List[Tuple[float, float, float, float]] = [(1.0, 0.0, 0.0, 0.0)] * len(bones)
    for i, b in enumerate(bones):
        local_p, local_q = b[0:3], math3d.normalize(b[3:7])
        parent = BONE_PARENTS[i] if i < len(BONE_PARENTS) else 0
        if parent < 0:
            pos[i], rot[i] = tuple(local_p), local_q
            continue
        offset = math3d.rotate(rot[parent], local_p)
        pos[i] = tuple(pos[parent][k] + offset[k] for k in range(3))
        rot[i] = math3d.normalize(math3d.multiply(rot[parent], local_q))
    return pos


def fmt_value(name: str, v: float) -> str:
    return f"{v:+.2f}" if name in TWO_SIDED else f"{v:.2f}"


class InspectorWindow:
    """The window. `request(op, **args)` asks the driver (VHMDClient.request or
    the bridge's, which keeps haptic events going to the headset)."""

    def __init__(self, master: tk.Misc, request: Request, lang: str = "en", device: str = "left_hand",
                 pal: Optional[Dict[str, str]] = None, fonts: Optional[Dict[str, tuple]] = None,
                 own_window: bool = True) -> None:
        self.request, self.t = request, TEXT.get(lang, TEXT["en"])
        self.pal = pal or PALETTES[os_theme()]
        self.f = fonts or ui_fonts(master, lang)
        self.win = tk.Toplevel(master) if own_window else master
        self.win.title(self.t["title"])
        self.win.configure(bg=self.pal["bg"])
        self.win.geometry("900x640")
        self.win.minsize(720, 520)
        self.reply: Optional[dict] = None
        self.error = ""
        self.ids: List[str] = []
        self.closed = False
        self.last_haptics = 0
        self.flash_until = 0.0
        self._build(device)
        self.win.protocol("WM_DELETE_WINDOW", self.close)
        threading.Thread(target=self._poll, daemon=True).start()
        self._refresh()

    # -- layout ------------------------------------------------------------------
    def _build(self, device: str) -> None:
        t, pal, f = self.t, self.pal, self.f
        top = tk.Frame(self.win, bg=pal["bg"])
        top.pack(fill="x", padx=12, pady=(10, 6))
        ttk.Label(top, text=t["device"]).pack(side="left")
        self.dev_var = tk.StringVar(value=device)
        self.dev_box = ttk.Combobox(top, textvariable=self.dev_var, state="readonly", width=22, values=[device])
        self.dev_box.pack(side="left", padx=(8, 0))
        self.device = device   # read by the polling thread (tk variables belong to the UI thread)
        self.dev_box.bind("<<ComboboxSelected>>", lambda _e: setattr(self, "device", self.dev_var.get()))
        self.pause_var = tk.BooleanVar(value=False)
        self.paused = False
        ttk.Checkbutton(top, text=t["pause"], variable=self.pause_var,
                        command=lambda: setattr(self, "paused", self.pause_var.get())).pack(side="left", padx=(12, 0))
        self.status_lbl = ttk.Label(top, text="", style="Muted.TLabel")
        self.status_lbl.pack(side="left", padx=(12, 0))

        body = tk.Frame(self.win, bg=pal["bg"])
        body.pack(fill="both", expand=True, padx=12, pady=(0, 10))
        body.columnconfigure(0, weight=1, uniform="c")
        body.columnconfigure(1, weight=1, uniform="c")
        body.rowconfigure(0, weight=1)

        left = self._card(body, t["given"])
        left.master.grid(row=0, column=0, sticky="nsew", padx=(0, 6))
        self.inputs = tk.Canvas(left, bg=pal["card"], highlightthickness=0, height=300)
        self.inputs.pack(fill="both", expand=True, pady=(4, 0))
        ttk.Label(left, text=t["received"], style="CardGroup.TLabel").pack(anchor="w", pady=(8, 0))
        self.received_lbl = tk.Label(left, text="", bg=pal["card"], fg=pal["muted"], font=f["mono_small"],
                                     justify="left", anchor="w", wraplength=400)
        self.received_lbl.pack(fill="x")

        right = self._card(body, t["hand"])
        right.master.grid(row=0, column=1, sticky="nsew", padx=(6, 0))
        self.hand = tk.Canvas(right, bg=pal["card"], highlightthickness=0, height=300)
        self.hand.pack(fill="both", expand=True, pady=(4, 0))
        self.hand_lbl = ttk.Label(right, text="", style="CardMuted.TLabel")
        self.hand_lbl.pack(anchor="w")

        foot = self._card(self.win, "")
        foot.master.pack(fill="x", padx=12, pady=(0, 12))
        self.haptic_lbl = tk.Label(foot, text="", bg=pal["card"], fg=pal["fg"], font=f["ui"], anchor="w", justify="left")
        self.haptic_lbl.pack(fill="x")
        self.pose_lbl = tk.Label(foot, text="", bg=pal["card"], fg=pal["muted"], font=f["mono_small"], anchor="w",
                                 justify="left")
        self.pose_lbl.pack(fill="x", pady=(4, 0))

    def _card(self, parent, title: str) -> ttk.Frame:
        outer = tk.Frame(parent, bg=self.pal["border"], padx=1, pady=1)
        inner = ttk.Frame(outer, style="Card.TFrame", padding=(12, 8))
        inner.pack(fill="both", expand=True)
        if title:
            ttk.Label(inner, text=title, style="CardGroup.TLabel").pack(anchor="w")
        return inner

    # -- data --------------------------------------------------------------------
    def _poll(self) -> None:
        last_list = 0.0
        while not self.closed:
            if not self.paused:
                try:
                    if time.monotonic() - last_list > 2.0:
                        st = self.request("list")
                        self.ids = [d["id"] for d in st.get("devices", []) if d.get("enabled", True)]
                        last_list = time.monotonic()
                    r = self.request("inspect", id=self.device)
                    self.reply, self.error = (r, "") if r.get("ok") else (None, r.get("error", "?"))
                except (OSError, TimeoutError, ValueError, RuntimeError):
                    self.reply, self.error = None, self.t["error"]
            time.sleep(0.1)

    def close(self) -> None:
        self.closed = True
        self.win.destroy()

    # -- drawing -----------------------------------------------------------------
    def _refresh(self) -> None:
        if self.closed:
            return
        if self.ids and list(self.dev_box.cget("values")) != self.ids:
            self.dev_box.configure(values=self.ids)
        r = self.reply
        self.status_lbl.configure(text=self.error)
        if r and not self.paused:
            devices = r.get("devices", [])
            # The one SteamVR has connected (a controller or its hand twin), else the first.
            d = next((x for x in devices if x.get("active")), devices[0] if devices else {})
            self._draw_inputs(d.get("given", {}))
            self.received_lbl.configure(text="   ".join(f"{k}={fmt_value(k, v)}" for k, v in
                                                       sorted(d.get("received", {}).items())) or self.t["none"])
            self._draw_hand(d.get("skeleton"))
            self._show_haptic(d.get("haptic"))
            self._show_pose(d, devices)
        self.win.after(100, self._refresh)

    def _draw_inputs(self, given: Dict[str, float]) -> None:
        c, pal = self.inputs, self.pal
        c.delete("all")
        w = max(200, c.winfo_width())
        if not given:
            c.create_text(8, 12, text=self.t["none"], anchor="w", fill=pal["muted"], font=self.f["small"])
            return
        x0, y = 8, 10
        stick_names = set()
        sticks = [(n, xs, ys) for n, xs, ys in STICKS if xs in given or ys in given]
        for n, xs, ys in sticks:
            stick_names.update((xs, ys))
        # Sticks: a circle with the thumb's position.
        sx = x0
        for n, xs, ys in sticks:
            r = 34
            cx, cy = sx + r, y + r
            c.create_oval(cx - r, cy - r, cx + r, cy + r, outline=pal["axis"])
            c.create_line(cx - r, cy, cx + r, cy, fill=pal["grid"])
            c.create_line(cx, cy - r, cx, cy + r, fill=pal["grid"])
            px, py = given.get(xs, 0.0), given.get(ys, 0.0)
            dot = 6
            pressed = given.get(n + "_click", 0) >= 0.5
            touched = given.get(n + "_touch", 0) >= 0.5
            color = pal["accent"] if pressed else (pal["fg"] if touched else pal["muted"])
            c.create_oval(cx + px * r - dot, cy - py * r - dot, cx + px * r + dot, cy - py * r + dot, fill=color,
                          outline="")
            c.create_text(sx, cy + r + 9, text=f"{n} {px:+.2f} {py:+.2f}", anchor="w", fill=pal["muted"],
                          font=self.f["small"])
            sx += 2 * r + 110
        if sticks:
            y += 2 * 34 + 26
        # Scalars as bars, booleans as lamps.
        scalars = [(k, v) for k, v in sorted(given.items()) if k not in stick_names and
                   not (k.endswith(("_click", "_touch")) or k in ("system", "menu", "a", "b", "x", "y", "proximity",
                                                                 "charging"))]
        bools = [(k, v) for k, v in sorted(given.items()) if k not in stick_names and (k, v) not in scalars]
        bar_w = max(60, w - x0 - 170)
        for k, v in scalars:
            c.create_text(x0, y + 7, text=k, anchor="w", fill=pal["fg"], font=self.f["small"])
            bx = x0 + 110
            c.create_rectangle(bx, y + 2, bx + bar_w, y + 12, outline=pal["border"], fill=pal["card2"])
            frac = (v + 1) / 2 if k in TWO_SIDED else v
            c.create_rectangle(bx, y + 2, bx + bar_w * max(0.0, min(1.0, frac)), y + 12, outline="", fill=pal["accent"])
            c.create_text(bx + bar_w + 6, y + 7, text=fmt_value(k, v), anchor="w", fill=pal["muted"],
                          font=self.f["mono_small"])
            y += 18
        y += 6
        lx = x0
        for k, v in bools:
            label_w = 7 * len(k) + 24
            if lx + label_w > w:
                lx, y = x0, y + 20
            on = v >= 0.5
            c.create_oval(lx, y + 2, lx + 11, y + 13, fill=pal["accent"] if on else pal["card2"],
                          outline=pal["accent"] if on else pal["border"])
            c.create_text(lx + 16, y + 7, text=k, anchor="w", fill=pal["fg"] if on else pal["muted"],
                          font=self.f["small"])
            lx += label_w

    def _draw_hand(self, sk: Optional[dict]) -> None:
        c, pal = self.hand, self.pal
        c.delete("all")
        if not sk or len(sk.get("bones", [])) < 26:
            self.hand_lbl.configure(text=self.t["no_bones"])
            return
        self.hand_lbl.configure(text=f"{self.t.get('source_' + sk.get('source', ''), sk.get('source', ''))}  "
                                     f"({sk.get('age_s', 0):.1f} s)")
        pos = bone_positions(sk["bones"])[:26]
        w, h = max(200, c.winfo_width()), max(160, c.winfo_height())
        half = w / 2
        # Same scale in both views, fitted to the hand.
        xs = [p[0] for p in pos[1:]]
        ys = [p[1] for p in pos[1:]]
        zs = [p[2] for p in pos[1:]]
        span = max(0.12, max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs)) * 1.15
        scale = min(half - 20, h - 40) / span
        mid = lambda v: (min(v) + max(v)) / 2   # noqa: E731 - centre of the bounding box
        views = [(self.t["from_top"], 0, lambda p: (p[0], p[2]), (mid(xs), mid(zs))),
                 (self.t["from_side"], half, lambda p: (p[2], -p[1]), (mid(zs), -mid(ys)))]
        for title, ox, proj, center in views:
            c.create_text(ox + 8, 10, text=title, anchor="w", fill=pal["muted"], font=self.f["small"])
            cx, cy = ox + half / 2, h / 2 + 8

            def to_screen(p, proj=proj, center=center, cx=cx, cy=cy):
                u, v = proj(p)
                return cx + (u - center[0]) * scale, cy + (v - center[1]) * scale

            wrist = to_screen(pos[1])
            for fi, (a, b) in enumerate(FINGERS):
                pts = [wrist] + [to_screen(pos[i]) for i in range(a, b + 1)]
                c.create_line(*[v for pt in pts for v in pt], fill=FINGER_COLORS[fi], width=3, capstyle="round")
                tx, ty = pts[-1]
                c.create_oval(tx - 3, ty - 3, tx + 3, ty + 3, fill=FINGER_COLORS[fi], outline="")
            c.create_oval(wrist[0] - 5, wrist[1] - 5, wrist[0] + 5, wrist[1] + 5, fill=pal["fg"], outline="")
        # Distances that say whether the hand closes: thumb tip to index tip, fingertips to the wrist.
        thumb_index = math.dist(pos[5], pos[10]) * 100
        reach = sum(math.dist(pos[1], pos[i]) for i in (10, 15, 20, 25)) / 4 * 100
        c.create_text(8, h - 8, anchor="sw", fill=pal["muted"], font=self.f["small"],
                      text=f"thumb-index {thumb_index:.1f} cm   fingertips-wrist {reach:.1f} cm")

    def _show_haptic(self, hp: Optional[dict]) -> None:
        pal = self.pal
        if not hp:
            self.haptic_lbl.configure(text=f"{self.t['haptic']}: {self.t['no_haptic']}", bg=pal["card"])
            return
        if hp["count"] != self.last_haptics:
            self.last_haptics = hp["count"]
            self.flash_until = time.monotonic() + max(0.25, float(hp.get("duration", 0)))
        flashing = time.monotonic() < self.flash_until
        self.haptic_lbl.configure(
            text=f"{self.t['haptic']}: " + self.t["haptic_line"].format(count=hp["count"], age=hp.get("age_s", 0),
                                                                        amp=hp.get("amplitude", 0),
                                                                        freq=hp.get("frequency", 0),
                                                                        dur=hp.get("duration", 0)),
            bg=pal["warn"] if flashing else pal["card"], fg=pal["warn_fg"] if flashing else pal["fg"])

    def _show_pose(self, d: dict, devices: List[dict]) -> None:
        lines = []
        pose = d.get("pose")
        if pose:
            p, q = pose["pos"], pose["rot"]
            yaw, pitch, roll = math3d.to_euler(q)
            state = "" if d.get("active", True) else " " + self.t["inactive"]
            lines.append(f"{self.t['pose']}: {d.get('serial', '')}  pos ({p[0]:+.3f}, {p[1]:+.3f}, {p[2]:+.3f}) m  "
                         f"yaw {yaw:+.1f}  pitch {pitch:+.1f}  roll {roll:+.1f}  "
                         f"{'valid' if pose.get('valid') else 'invalid'}{state}")
        others = [x.get("serial", "") for x in devices if x is not d]
        if others:
            lines[-1:] = [lines[-1] + "   (" + ", ".join(others) + ")"] if lines else []
        gz = d.get("gaze")
        if gz:
            o, tg = gz["origin"], gz["target"]
            lines.append(f"{self.t['gaze']}: origin ({o[0]:+.3f}, {o[1]:+.3f}, {o[2]:+.3f})  "
                         f"target ({tg[0]:+.3f}, {tg[1]:+.3f}, {tg[2]:+.3f})  "
                         f"{'valid' if gz.get('valid') else 'invalid'}  {gz.get('age_s', 0):.1f} s")
        self.pose_lbl.configure(text="\n".join(lines))


def main() -> None:
    from .client import DEFAULT_HOST, DEFAULT_PORT, VHMDClient

    ap = argparse.ArgumentParser(description="Show what a VHMD device hands SteamVR (inputs, finger bones, haptics).")
    ap.add_argument("--device", default="left_hand", help="device id or tracker role (default left_hand)")
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--lang", choices=["en", "ja"], default=None)
    args = ap.parse_args()
    client = VHMDClient(args.host, args.port, timeout=0.5)
    lock = threading.Lock()

    def request(op: str, **kw) -> dict:
        with lock:
            return client.request(op, **kw)

    root = tk.Tk()
    lang = args.lang or ui_language()
    pal, fonts = PALETTES[os_theme()], ui_fonts(root, lang)
    setup_style(root, pal, fonts)
    InspectorWindow(root, request, lang, args.device, pal, fonts, own_window=False)
    root.mainloop()


if __name__ == "__main__":
    main()
