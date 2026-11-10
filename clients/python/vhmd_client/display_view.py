"""A window to change the shape of VHMD's display while SteamVR runs: the IPD,
the field of view and the size apps render at (config op "display").

It always shows the values SteamVR started with next to the current ones, and
one button goes back to the startup values. Nothing is saved: a SteamVR restart
returns to them too. The three controls are all relative to the startup
values, so "100 %" always means "as SteamVR started". Windows and Linux.

    python -m vhmd_client.display_view

VHMD Link opens the same window from its developer view.
"""

from __future__ import annotations

import argparse
import math
import threading
import time
import tkinter as tk
from tkinter import ttk
from typing import Callable, Dict, List, Optional, Tuple

from .ui_style import PALETTES, os_theme, setup_style, ui_fonts, ui_language

Request = Callable[..., dict]

# Limits of the controls (the driver has its own, wider ones).
IPD_MM = (40.0, 90.0)
FOV_PCT = (50, 120)
RENDER_PCT = (50, 150)
RENDER_SIZE = (64, 8192)
MAX_TAN = 10.0

TEXT = {
    "en": {"title": "VHMD display (developer)", "values": "Values", "startup": "As SteamVR started", "now": "Now",
           "size": "Render size per eye", "ipd": "IPD", "fov_h": "Field of view, across", "fov_v": "Field of view, up-down",
           "change": "Change", "c_ipd": "IPD", "c_fov": "Field of view", "c_size": "Render size",
           "apply": "Apply", "reset": "Back to the startup values", "will": "Apply sends: {what}",
           "nothing": "Apply sends nothing: the controls match the current values.",
           "changed": "Changed from the startup values", "same": "Same as the startup values",
           "live": "SteamVR has these values.", "not_live": "SteamVR has not taken the headset yet: the values wait in the driver.",
           "error": "No answer from the VHMD driver: is SteamVR running in VHMD mode?",
           "hint": "Nothing is saved: restarting SteamVR also goes back to the startup values. "
                   "Render size changes how finely apps draw, not the size of the video. "
                   "For a moment after a field of view change, frames drawn with the old one are shown with the new one.",
           "px": "{w} x {h}", "mm": "{v:.1f} mm", "deg": "{v:.1f} deg", "pct": "{v:.0f} %"},
    "ja": {"title": "VHMD の表示の形(開発者向け)", "values": "値", "startup": "SteamVR の起動時", "now": "いま",
           "size": "片目の描画の大きさ", "ipd": "IPD", "fov_h": "視野(左右)", "fov_v": "視野(上下)",
           "change": "変更", "c_ipd": "IPD", "c_fov": "視野", "c_size": "描画の大きさ",
           "apply": "適用", "reset": "起動時の値に戻す", "will": "適用で送る内容: {what}",
           "nothing": "適用しても何も送りません(つまみは、いまの値と同じです)。",
           "changed": "起動時の値から変更中", "same": "起動時の値のまま",
           "live": "SteamVR はこの値で動いています。", "not_live": "SteamVR はまだヘッドセットを受け取っていません。値はドライバーが持っています。",
           "error": "VHMD のドライバから応答がありません。SteamVR は VHMD モードで動いていますか?",
           "hint": "保存はしません。SteamVR を再起動しても起動時の値に戻ります。"
                   "描画の大きさは、アプリが描く細かさを変えます(映像の大きさは変わりません)。"
                   "視野を変えた直後の一瞬は、古い視野で描いた映像が新しい視野で表示されます。",
           "px": "{w} x {h}", "mm": "{v:.1f} mm", "deg": "{v:.1f}°", "pct": "{v:.0f} %"},
}


# -- the arithmetic (no tkinter: tested in tests/python) ---------------------------
def fov_degrees(tan: List[List[float]]) -> Tuple[float, float]:
    """(across, up-down) field of view of the left eye, in degrees."""
    left, right, up, down = tan[0]
    return (math.degrees(math.atan(right) - math.atan(left)), math.degrees(math.atan(up) - math.atan(down)))


def scaled_render(startup: dict, pct: float) -> List[int]:
    """The startup render size times pct / 100: multiples of 16, within the limits."""
    return [min(RENDER_SIZE[1], max(RENDER_SIZE[0], int(round(v * pct / 100.0 / 16.0)) * 16)) for v in startup["render"]]


def scaled_tan(startup: dict, pct: float) -> List[List[float]]:
    """The startup tangents times pct / 100 (within the driver's limit)."""
    return [[max(-MAX_TAN, min(MAX_TAN, round(x * pct / 100.0, 6))) for x in eye] for eye in startup["tan"]]


def controls_from(startup: dict, now: dict) -> Tuple[float, float, float]:
    """Where the controls stand for the current values: (IPD in mm, field of view
    in % of the startup one, render size in % of the startup one)."""
    fov = 100.0 * now["tan"][0][1] / startup["tan"][0][1] if startup["tan"][0][1] else 100.0
    size = 100.0 * now["render"][0] / startup["render"][0] if startup["render"][0] else 100.0
    return round(now["ipd"] * 1000.0, 1), round(fov), round(size)


def proposal(startup: dict, now: dict, ipd_mm: float, fov_pct: float, render_pct: float) -> dict:
    """The arguments of op "display" that take the current values to what the
    controls say. Only what differs from now is named; {} = nothing to send."""
    out: dict = {}
    ipd = round(min(IPD_MM[1], max(IPD_MM[0], ipd_mm)) / 1000.0, 5)
    if abs(ipd - now["ipd"]) > 0.00004:
        out["ipd"] = ipd
    tan = scaled_tan(startup, fov_pct)
    if any(abs(a - b) > 1e-4 for e in (0, 1) for a, b in zip(tan[e], now["tan"][e])):
        out["tan"] = tan
    render = scaled_render(startup, render_pct)
    if render != [int(v) for v in now["render"]]:
        out["render"] = render
    return out


def describe(t: Dict[str, str], args: dict) -> str:
    """What `args` (of op "display") would change, in words."""
    parts = []
    if "ipd" in args:
        parts.append(f"{t['c_ipd']} {t['mm'].format(v=args['ipd'] * 1000.0)}")
    if "tan" in args:
        h, v = fov_degrees(args["tan"])
        parts.append(f"{t['c_fov']} {t['deg'].format(v=h)} / {t['deg'].format(v=v)}")
    if "render" in args:
        parts.append(f"{t['c_size']} {t['px'].format(w=args['render'][0], h=args['render'][1])}")
    return ", ".join(parts)


# -- the window --------------------------------------------------------------------
class DisplayWindow:
    """`request(op, **args)` asks the driver (VHMDClient.request or the bridge's)."""

    def __init__(self, master: tk.Misc, request: Request, lang: str = "en", pal: Optional[Dict[str, str]] = None,
                 fonts: Optional[Dict[str, tuple]] = None, own_window: bool = True,
                 log: Optional[Callable[[str], None]] = None) -> None:
        self.request, self.t, self.log = request, TEXT.get(lang, TEXT["en"]), log or (lambda _s: None)
        self.pal = pal or PALETTES[os_theme()]
        self.f = fonts or ui_fonts(master, lang)
        self.win = tk.Toplevel(master) if own_window else master
        self.win.title(self.t["title"])
        self.win.configure(bg=self.pal["bg"])
        self.win.geometry("600x560")
        self.win.minsize(540, 520)
        self.reply: Optional[dict] = None    # the driver's last "display" report
        self.error = ""
        self.closed = False
        self.shown_now: Optional[dict] = None    # the "now" the controls were last set from
        self._build()
        self.win.protocol("WM_DELETE_WINDOW", self.close)
        threading.Thread(target=self._poll, daemon=True).start()
        self._refresh()

    # -- layout ------------------------------------------------------------------
    def _card(self, title: str) -> ttk.Frame:
        outer = tk.Frame(self.win, bg=self.pal["border"], padx=1, pady=1)
        outer.pack(fill="x", padx=12, pady=(10, 0))
        inner = ttk.Frame(outer, style="Card.TFrame", padding=(12, 8))
        inner.pack(fill="both", expand=True)
        ttk.Label(inner, text=title, style="CardGroup.TLabel").pack(anchor="w")
        return inner

    def _build(self) -> None:
        t, pal, f = self.t, self.pal, self.f
        self.status_lbl = ttk.Label(self.win, text="", style="Muted.TLabel", wraplength=560, justify="left")
        self.status_lbl.pack(fill="x", padx=14, pady=(10, 0))

        c = self._card(t["values"])
        grid = ttk.Frame(c, style="Card.TFrame")
        grid.pack(fill="x", pady=(4, 0))
        for col, key in ((1, "startup"), (2, "now")):
            ttk.Label(grid, text=t[key], style="CardMuted.TLabel").grid(row=0, column=col, sticky="w", padx=(16, 0))
        self.cells: Dict[Tuple[str, str], tk.Label] = {}
        for row, key in enumerate(("size", "ipd", "fov_h", "fov_v"), start=1):
            ttk.Label(grid, text=t[key], style="Card.TLabel").grid(row=row, column=0, sticky="w", pady=1)
            for col, which in ((1, "startup"), (2, "now")):
                lbl = tk.Label(grid, text="", bg=pal["card"], fg=pal["fg"], font=f["mono"], anchor="w")
                lbl.grid(row=row, column=col, sticky="w", padx=(16, 0))
                self.cells[(key, which)] = lbl
        self.state_lbl = ttk.Label(c, text="", style="CardMuted.TLabel")
        self.state_lbl.pack(anchor="w", pady=(6, 0))

        c = self._card(t["change"])
        ctl = ttk.Frame(c, style="Card.TFrame")
        ctl.pack(fill="x", pady=(4, 0))
        ctl.columnconfigure(1, weight=1)
        self.vars: Dict[str, tk.DoubleVar] = {}
        self.value_lbls: Dict[str, tk.Label] = {}
        for row, (key, lo, hi) in enumerate((("c_ipd", *IPD_MM), ("c_fov", *FOV_PCT), ("c_size", *RENDER_PCT))):
            ttk.Label(ctl, text=t[key], style="Card.TLabel").grid(row=row, column=0, sticky="w", pady=4)
            var = tk.DoubleVar(value=lo)
            ttk.Scale(ctl, from_=lo, to=hi, variable=var, command=lambda _v: self._controls_moved()).grid(
                row=row, column=1, sticky="ew", padx=(12, 12))
            lbl = tk.Label(ctl, text="", bg=pal["card"], fg=pal["fg"], font=f["mono"], width=18, anchor="w")
            lbl.grid(row=row, column=2, sticky="w")
            self.vars[key], self.value_lbls[key] = var, lbl
        self.will_lbl = ttk.Label(c, text="", style="CardMuted.TLabel", wraplength=540, justify="left")
        self.will_lbl.pack(anchor="w", pady=(6, 0))
        btns = ttk.Frame(c, style="Card.TFrame")
        btns.pack(fill="x", pady=(8, 0))
        self.apply_btn = ttk.Button(btns, text=t["apply"], command=self.apply)
        self.apply_btn.pack(side="left")
        self.reset_btn = ttk.Button(btns, text=t["reset"], command=self.reset)
        self.reset_btn.pack(side="left", padx=(8, 0))
        ttk.Label(self.win, text=t["hint"], style="Muted.TLabel", wraplength=560, justify="left").pack(
            fill="x", padx=14, pady=(10, 12))

    # -- data --------------------------------------------------------------------
    def _ask(self, **args) -> None:
        """One "display" request; the reply (or the error) is what the window shows."""
        try:
            r = self.request("display", **args)
            if "display" in r:
                self.reply = r["display"]
            self.error = "" if r.get("ok") else r.get("error", "?")
        except (OSError, TimeoutError, ValueError, RuntimeError):
            self.error = self.t["error"]

    def _poll(self) -> None:
        while not self.closed:
            self._ask()
            time.sleep(1.0)

    def controls(self) -> Tuple[float, float, float]:
        """The controls, rounded to what they stand for: 0.1 mm, 1 %, 5 %."""
        return (round(self.vars["c_ipd"].get(), 1), round(self.vars["c_fov"].get()),
                round(self.vars["c_size"].get() / 5.0) * 5.0)

    def wanted(self) -> dict:
        d = self.reply
        return proposal(d["startup"], d["now"], *self.controls()) if d else {}

    def apply(self) -> None:
        args = self.wanted()
        if args:
            self.log("display: " + describe(TEXT["en"], args))
            threading.Thread(target=lambda: self._ask(**args), daemon=True).start()

    def reset(self) -> None:
        self.log("display: back to the startup values")
        threading.Thread(target=lambda: self._ask(reset=True), daemon=True).start()

    def close(self) -> None:
        self.closed = True
        self.win.destroy()

    # -- drawing -----------------------------------------------------------------
    def _controls_moved(self) -> None:
        t, d = self.t, self.reply
        ipd, fov, size = self.controls()
        self.value_lbls["c_ipd"].configure(text=t["mm"].format(v=ipd))
        if not d:
            return
        h, _v = fov_degrees(scaled_tan(d["startup"], fov))
        self.value_lbls["c_fov"].configure(text=f"{t['pct'].format(v=fov)}  {t['deg'].format(v=h)}")
        w, hgt = scaled_render(d["startup"], size)
        self.value_lbls["c_size"].configure(text=f"{t['pct'].format(v=size)}  {t['px'].format(w=w, h=hgt)}")
        args = self.wanted()
        self.will_lbl.configure(text=t["will"].format(what=describe(t, args)) if args else t["nothing"])
        self.apply_btn.state(["!disabled"] if args else ["disabled"])

    def _refresh(self) -> None:
        if self.closed:
            return
        t, d = self.t, self.reply
        self.status_lbl.configure(text=self.error)
        if d:
            for which in ("startup", "now"):
                v = d[which]
                h, vert = fov_degrees(v["tan"])
                self.cells[("size", which)].configure(text=t["px"].format(w=v["render"][0], h=v["render"][1]))
                self.cells[("ipd", which)].configure(text=t["mm"].format(v=v["ipd"] * 1000.0))
                self.cells[("fov_h", which)].configure(text=t["deg"].format(v=h))
                self.cells[("fov_v", which)].configure(text=t["deg"].format(v=vert))
            self.state_lbl.configure(text=(t["changed"] if d.get("changed") else t["same"]) + "  " +
                                     (t["live"] if d.get("live") else t["not_live"]))
            self.reset_btn.state(["!disabled"] if d.get("changed") else ["disabled"])
            if d["now"] != self.shown_now:   # first values, after Apply / reset, or changed by another tool
                self.shown_now = d["now"]
                for key, v in zip(("c_ipd", "c_fov", "c_size"), controls_from(d["startup"], d["now"])):
                    self.vars[key].set(v)
            self._controls_moved()
        else:
            self.apply_btn.state(["disabled"])
            self.reset_btn.state(["disabled"])
        self.win.after(200, self._refresh)


def main() -> None:
    from .client import DEFAULT_HOST, DEFAULT_PORT, VHMDClient

    ap = argparse.ArgumentParser(description="Change VHMD's IPD, field of view and render size while SteamVR runs.")
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
    DisplayWindow(root, request, lang, pal, fonts, own_window=False, log=print)
    root.mainloop()


if __name__ == "__main__":
    main()
