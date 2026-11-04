"""VHMD Link's statistics over the last minutes: a window with line charts and
a CSV export. The PC app adds a sample every half second (StatsHistory.add),
whether or not the window is open, so a stutter can be looked at afterwards.
"""

from __future__ import annotations

import collections
import csv
import time
import tkinter as tk
from pathlib import Path
from tkinter import font as tkfont, ttk
from typing import Deque, Dict, List, Optional, Tuple

from .paths import data_dir

KEEP = 600   # samples: 5 minutes at 2 a second

# Categorical slots in a fixed order (light, dark); checked for colour-blind
# separation against both card colours.
SERIES = [("#2a78d6", "#3987e5"), ("#eb6834", "#d95926"), ("#1baf7a", "#199e70")]

# (title key, [(sample key, label key), ...]): one unit per chart, at most three lines.
CHARTS = [
    ("c_frames", [("video_out", "s_video_out"), ("shown", "s_shown"), ("video_in", "s_video_in")]),
    ("c_bitrate", [("mbps", "s_mbps"), ("bitrate_now", "s_target")]),
    ("c_times", [("decode_ms", "s_decode"), ("wait_ms", "s_wait"), ("relay_ms", "s_relay")]),
]

TEXT = {
    "en": {"title": "VHMD Link statistics", "c_frames": "Pictures (per second)", "c_bitrate": "Video (Mbit/s)",
           "c_times": "Times (ms)", "s_video_out": "sent by the PC", "s_shown": "new on the headset",
           "s_video_in": "taken from SteamVR", "s_mbps": "sent", "s_target": "target", "s_decode": "decode",
           "s_wait": "wait to show", "s_relay": "relay", "export": "Export CSV", "saved": "Saved: {path}",
           "ago": "{s:.0f} s ago", "empty": "No samples yet", "minutes": "last {m:.0f} min"},
    "ja": {"title": "VHMD Link の統計", "c_frames": "画像の数(回/秒)", "c_bitrate": "映像(Mbit/s)",
           "c_times": "時間(ms)", "s_video_out": "PC が送った", "s_shown": "ヘッドセットで新しく表示",
           "s_video_in": "SteamVR から受け取った", "s_mbps": "送った量", "s_target": "目標", "s_decode": "復号",
           "s_wait": "表示までの待ち", "s_relay": "中継", "export": "CSV に書き出す", "saved": "保存しました: {path}",
           "ago": "{s:.0f} 秒前", "empty": "まだ記録がありません", "minutes": "直近 {m:.0f} 分"},
}

COLUMNS = ["time", "video_in", "video_out", "mbps", "bitrate_now", "poses", "shown", "display", "repeats",
           "wait_ms", "wait_p95_ms", "reprojected", "decode_ms", "decode_max_ms", "audio_packets",
           "audio_underruns", "relay_ms", "relay_p99_ms", "pose_forward_us", "predict_ms", "late_ms", "sync_shift_ms"]


def sample_from(st: dict) -> Dict[str, float]:
    """One sample from the bridge's status (Bridge.status())."""
    h = st.get("headset_stats", {})
    relay = st.get("relay_ms", (0, 0, 0))
    return {"time": time.time(), "video_in": st.get("video_in", 0), "video_out": st.get("video_out", 0),
            "mbps": st.get("mbps", 0), "bitrate_now": st.get("bitrate_now", 0), "poses": st.get("msgs", 0),
            "shown": h.get("shown", 0), "display": h.get("display", 0), "repeats": h.get("repeats", 0),
            "wait_ms": h.get("wait_ms", 0), "wait_p95_ms": h.get("wait_p95_ms", 0),
            "reprojected": h.get("reprojected", 0), "decode_ms": h.get("decode_ms", 0),
            "decode_max_ms": h.get("decode_max_ms", 0), "audio_packets": h.get("audio_packets", 0),
            "audio_underruns": h.get("audio_underruns", 0), "relay_ms": relay[0], "relay_p99_ms": relay[1],
            "pose_forward_us": st.get("forward_us", (0, 0))[0], "predict_ms": h.get("predict_ms", 0),
            "late_ms": h.get("late_ms", 0), "sync_shift_ms": (st.get("sync") or {}).get("shift_ms", 0)}


class StatsHistory:
    def __init__(self, keep: int = KEEP) -> None:
        self.samples: Deque[Dict[str, float]] = collections.deque(maxlen=keep)

    def add(self, sample: Dict[str, float]) -> None:
        self.samples.append(sample)

    def to_csv(self, path: Optional[Path] = None) -> Path:
        if path is None:
            folder = data_dir() / "stats"
            folder.mkdir(parents=True, exist_ok=True)
            path = folder / time.strftime("stats_%Y%m%d_%H%M%S.csv")
        with open(path, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(COLUMNS)
            for s in list(self.samples):
                w.writerow([time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(s["time"])) +
                            f".{int(s['time'] * 10) % 10}" if c == "time" else round(float(s.get(c, 0) or 0), 3)
                            for c in COLUMNS])
        return path


def nice_max(v: float) -> float:
    """A round top for the axis, at or above v."""
    if v <= 0:
        return 1.0
    for step in (1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000):
        if v <= step:
            return float(step)
    return float(int(v / 1000 + 1) * 1000)


class StatsWindow:
    def __init__(self, master: tk.Misc, history: StatsHistory, lang: str, pal: Dict[str, str],
                 fonts: Dict[str, tuple], dark: bool) -> None:
        self.h, self.t, self.pal, self.f = history, TEXT.get(lang, TEXT["en"]), pal, fonts
        self.colors = [d if dark else lt for lt, d in SERIES]
        self.win = tk.Toplevel(master)
        self.win.title(self.t["title"])
        self.win.configure(bg=pal["bg"])
        self.win.geometry("760x620")
        self.closed = False
        self.hover_x: Optional[float] = None
        self.win.protocol("WM_DELETE_WINDOW", self.close)
        small, bold = tkfont.Font(font=fonts["small"]), tkfont.Font(font=fonts["small_bold"])
        self.measure, self.measure_bold = small.measure, bold.measure

        top = tk.Frame(self.win, bg=pal["bg"])
        top.pack(fill="x", padx=12, pady=(10, 6))
        ttk.Button(top, text=self.t["export"], command=self.export).pack(side="left")
        self.msg = ttk.Label(top, text="", style="Muted.TLabel")
        self.msg.pack(side="left", padx=(10, 0))
        self.span_lbl = ttk.Label(top, text="", style="Muted.TLabel")
        self.span_lbl.pack(side="right")
        self.canvases: List[tk.Canvas] = []
        for _ in CHARTS:
            outer = tk.Frame(self.win, bg=pal["border"], padx=1, pady=1)
            outer.pack(fill="both", expand=True, padx=12, pady=(0, 8))
            c = tk.Canvas(outer, bg=pal["card"], highlightthickness=0, height=160)
            c.pack(fill="both", expand=True)
            c.bind("<Motion>", self._hover)
            c.bind("<Leave>", lambda _e: setattr(self, "hover_x", None))
            self.canvases.append(c)
        self._draw()

    def close(self) -> None:
        self.closed = True
        self.win.destroy()

    def export(self) -> None:
        try:
            path = self.h.to_csv()
            self.msg.config(text=self.t["saved"].format(path=path))
        except OSError as e:
            self.msg.config(text=str(e))

    def _hover(self, e) -> None:
        self.hover_x = e.x

    # Plot area inside a canvas: left for the axis, right for direct labels.
    def _area(self, c: tk.Canvas) -> Tuple[float, float, float, float]:
        w, h = max(300, c.winfo_width()), max(120, c.winfo_height())
        return 44, 30, w - 150, h - 22

    def _draw(self) -> None:
        if self.closed:
            return
        samples = list(self.h.samples)
        pal, f = self.pal, self.f
        now = time.time()
        span = max(60.0, now - samples[0]["time"]) if samples else 60.0
        self.span_lbl.config(text=self.t["minutes"].format(m=max(1, round(span / 60))))
        for c, (title_key, series) in zip(self.canvases, CHARTS):
            c.delete("all")
            x0, y0, x1, y1 = self._area(c)
            c.create_text(10, 12, text=self.t[title_key], anchor="w", fill=pal["fg"], font=f["small_bold"])
            # Legend (always, these charts have two or three lines) after the title.
            lx = 10 + self.measure_bold(self.t[title_key]) + 24
            for i, (_, label_key) in enumerate(series):
                c.create_line(lx, 12, lx + 14, 12, fill=self.colors[i], width=2)
                c.create_text(lx + 18, 12, text=self.t[label_key], anchor="w", fill=pal["muted"], font=f["small"])
                lx += 34 + self.measure(self.t[label_key])
            if not samples:
                c.create_text((x0 + x1) / 2, (y0 + y1) / 2, text=self.t["empty"], fill=pal["muted"], font=f["small"])
                continue
            top = nice_max(max(float(s.get(k, 0) or 0) for s in samples for k, _ in series))
            for frac in (0, 0.5, 1):   # recessive grid and axis labels
                y = y1 - (y1 - y0) * frac
                c.create_line(x0, y, x1, y, fill=pal["grid"] if frac else pal["axis"])
                c.create_text(x0 - 6, y, text=f"{top * frac:g}", anchor="e", fill=pal["muted"], font=f["small"])
            c.create_text(x0, y1 + 12, text=self.t["ago"].format(s=span), anchor="w", fill=pal["muted"],
                          font=f["small"])
            c.create_text(x1, y1 + 12, text="0", anchor="e", fill=pal["muted"], font=f["small"])

            def px(s, x0=x0, x1=x1):
                return x1 - (now - s["time"]) / span * (x1 - x0)

            def py(v, y0=y0, y1=y1, top=top):
                return y1 - min(1.0, max(0.0, v / top)) * (y1 - y0)

            ends = []
            for i, (key, label_key) in enumerate(series):
                pts = [v for s in samples for v in (px(s), py(float(s.get(key, 0) or 0)))]
                if len(pts) >= 4:
                    c.create_line(*pts, fill=self.colors[i], width=2, joinstyle="round")
                last = float(samples[-1].get(key, 0) or 0)
                ends.append([py(last), i, f"{last:.1f} {self.t[label_key]}"])
            # Direct labels at the lines' ends, in text ink (the swatch carries the
            # identity), pushed apart when the values are close.
            ends.sort()
            for j in range(1, len(ends)):
                ends[j][0] = max(ends[j][0], ends[j - 1][0] + 14)
            shift = max(0.0, ends[-1][0] - (y1 + 6)) if ends else 0.0
            for ly, i, text in ends:
                ly -= shift
                c.create_line(x1 + 4, ly, x1 + 12, ly, fill=self.colors[i], width=2)
                c.create_text(x1 + 16, ly, text=text, anchor="w", fill=pal["fg"], font=f["small"])
            if self.hover_x is not None and x0 <= self.hover_x <= x1:
                t_at = now - (x1 - self.hover_x) / (x1 - x0) * span
                s = min(samples, key=lambda s: abs(s["time"] - t_at))
                hx = px(s)
                c.create_line(hx, y0, hx, y1, fill=pal["muted"], dash=(2, 2))
                lines = [self.t["ago"].format(s=now - s["time"])] + \
                        [f"{self.t[lk]}: {float(s.get(k, 0) or 0):.2f}" for k, lk in series]
                for i, (k, _) in enumerate(series):
                    vy = py(float(s.get(k, 0) or 0))
                    c.create_oval(hx - 4, vy - 4, hx + 4, vy + 4, fill=self.colors[i], outline=pal["card"], width=2)
                bx = hx + 10 if hx < (x0 + x1) / 2 else hx - 170
                c.create_rectangle(bx, y0, bx + 160, y0 + 16 * len(lines) + 6, fill=pal["card2"], outline=pal["border"])
                for j, line in enumerate(lines):
                    c.create_text(bx + 8, y0 + 12 + 16 * j, text=line, anchor="w",
                                  fill=pal["fg"] if j else pal["muted"], font=f["small"])
        self.win.after(500, self._draw)
