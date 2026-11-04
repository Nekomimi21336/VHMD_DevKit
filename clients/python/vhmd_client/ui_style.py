"""The look shared by VHMD's windows (VHMD Link, the keyboard / mouse tool):
palettes, ttk styles, fonts, and the desktop's theme and language. Windows and Linux.
"""

from __future__ import annotations

import ctypes
import locale
import os
import subprocess
import sys
import tkinter as tk
from tkinter import font as tkfont, ttk
from typing import Dict, Sequence

PALETTES = {
    "dark": dict(bg="#16171a", card="#212328", card2="#2b2e34", border="#363a41", fg="#e8eaed", muted="#9aa1aa",
                 accent="#4c8dff", accent_fg="#ffffff", accent_hover="#3a74d8", entry="#17181c", sel="#2f5fbf",
                 ok="#173d2c", ok_fg="#9ff0c4", warn="#4a3a10", warn_fg="#ffe08a", bad="#4a1d24", bad_fg="#ffb3bd",
                 map_bg="#1a1b1f", grid="#26282d", axis="#474c55", view_bg="#0c0d0f", key_bg="#30343b"),
    "light": dict(bg="#eceef2", card="#ffffff", card2="#f3f4f7", border="#dde1e7", fg="#1c2026", muted="#68707b",
                  accent="#2f6fed", accent_fg="#ffffff", accent_hover="#255ccc", entry="#ffffff", sel="#cddfff",
                  ok="#ddf5e9", ok_fg="#12603a", warn="#fff1c4", warn_fg="#7a5300", bad="#fde1e4", bad_fg="#8f1d2c",
                  map_bg="#fbfcfd", grid="#eceef2", axis="#c2c8d0", view_bg="#101114", key_bg="#eef0f4"),
}


def os_theme() -> str:
    """'dark' or 'light': Windows' app theme, or GNOME's color scheme on Linux."""
    if sys.platform == "win32":
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                                r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize") as k:
                return "light" if winreg.QueryValueEx(k, "AppsUseLightTheme")[0] else "dark"
        except OSError:
            return "light"
    try:
        out = subprocess.run(["gsettings", "get", "org.gnome.desktop.interface", "color-scheme"],
                             capture_output=True, text=True, timeout=2).stdout
        return "dark" if "dark" in out else "light"
    except (OSError, subprocess.SubprocessError):
        return "light"


windows_theme = os_theme   # the older name


def ui_language() -> str:
    """'ja' when the desktop speaks Japanese, else 'en'."""
    if sys.platform == "win32":
        try:
            return "ja" if ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3FF == 0x11 else "en"
        except Exception:
            return "en"
    for name in ("LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"):
        value = os.environ.get(name, "")
        if value:
            return "ja" if value.lower().startswith("ja") else "en"
    lang = (locale.getlocale()[0] or "").lower()
    return "ja" if lang.startswith("ja") else "en"


def _first_family(root: tk.Misc, names: Sequence[str]) -> str:
    have = {f.lower() for f in tkfont.families(root)}
    return next((n for n in names if n.lower() in have), names[-1])


def ui_fonts(root: tk.Misc, lang: str) -> Dict[str, tuple]:
    """The fonts of VHMD's windows: the platform's UI font (one with Japanese for ja)."""
    if sys.platform == "win32":
        family, mono = ("Yu Gothic UI" if lang == "ja" else "Segoe UI"), "Consolas"
    else:
        family = _first_family(root, ["Noto Sans CJK JP", "Noto Sans JP", "IPAexGothic", "TakaoPGothic", "Noto Sans",
                                      "Ubuntu", "Cantarell", "DejaVu Sans"] if lang == "ja" else
                               ["Noto Sans", "Ubuntu", "Cantarell", "DejaVu Sans"])
        mono = _first_family(root, ["Noto Sans Mono", "Ubuntu Mono", "DejaVu Sans Mono", "Monospace"])
    return {"ui": (family, 10), "small": (family, 9), "small_bold": (family, 9, "bold"),
            "head": (family, 11, "bold"), "title": (family, 16, "bold"), "big": (family, 13, "bold"),
            "mono": (mono, 9), "mono_small": (mono, 9)}


def setup_style(root: tk.Tk, pal: Dict[str, str], fonts: Dict[str, tuple]) -> None:
    st = ttk.Style(root)
    st.theme_use("clam")
    root.configure(bg=pal["bg"])
    ui, small, head = fonts["ui"], fonts["small"], fonts["head"]
    st.configure(".", background=pal["bg"], foreground=pal["fg"], font=ui, bordercolor=pal["border"],
                 lightcolor=pal["card2"], darkcolor=pal["card2"], troughcolor=pal["card2"],
                 fieldbackground=pal["entry"], insertcolor=pal["fg"], selectbackground=pal["sel"],
                 selectforeground=pal["fg"], focuscolor=pal["accent"])
    st.configure("Card.TFrame", background=pal["card"])
    st.configure("Card.TLabel", background=pal["card"], foreground=pal["fg"])
    st.configure("CardMuted.TLabel", background=pal["card"], foreground=pal["muted"], font=small)
    st.configure("CardHead.TLabel", background=pal["card"], foreground=pal["fg"], font=head)
    st.configure("CardGroup.TLabel", background=pal["card"], foreground=pal["muted"], font=small)
    st.configure("Muted.TLabel", background=pal["bg"], foreground=pal["muted"], font=small)
    st.configure("Error.TLabel", background=pal["bg"], foreground=pal["bad_fg"], font=small)
    st.configure("TButton", background=pal["card"], foreground=pal["fg"], padding=(12, 6), borderwidth=1,
                 bordercolor=pal["border"], lightcolor=pal["card"], darkcolor=pal["card"], relief="flat")
    st.map("TButton", background=[("disabled", pal["bg"]), ("pressed", pal["border"]), ("active", pal["card2"])],
           foreground=[("disabled", pal["muted"])])
    st.configure("Accent.TButton", background=pal["accent"], foreground=pal["accent_fg"], bordercolor=pal["accent"],
                 lightcolor=pal["accent"], darkcolor=pal["accent"])
    st.map("Accent.TButton", background=[("pressed", pal["accent_hover"]), ("active", pal["accent_hover"])],
           foreground=[("disabled", pal["muted"])])
    for name in ("TEntry", "TSpinbox", "TCombobox"):
        st.configure(name, fieldbackground=pal["entry"], foreground=pal["fg"], background=pal["card2"],
                     bordercolor=pal["border"], lightcolor=pal["border"], darkcolor=pal["border"],
                     arrowcolor=pal["fg"], padding=(5, 3))
        st.map(name, bordercolor=[("focus", pal["accent"])], lightcolor=[("focus", pal["accent"])],
               darkcolor=[("focus", pal["accent"])])
    st.map("TCombobox", fieldbackground=[("readonly", pal["entry"])], foreground=[("readonly", pal["fg"])],
           selectbackground=[("readonly", pal["entry"])], selectforeground=[("readonly", pal["fg"])],
           background=[("active", pal["card2"]), ("readonly", pal["card2"])])
    root.option_add("*TCombobox*Listbox.background", pal["card"])
    root.option_add("*TCombobox*Listbox.foreground", pal["fg"])
    root.option_add("*TCombobox*Listbox.selectBackground", pal["sel"])
    root.option_add("*TCombobox*Listbox.selectForeground", pal["fg"])
    root.option_add("*TCombobox*Listbox.font", ui)
    for name in ("Card.TCheckbutton", "Card.TRadiobutton"):
        st.configure(name, background=pal["card"], foreground=pal["fg"], indicatorbackground=pal["entry"],
                     indicatorforeground=pal["accent_fg"], indicatorcolor=pal["entry"], padding=(2, 2))
        st.map(name, background=[("active", pal["card"])],
               indicatorcolor=[("selected", pal["accent"]), ("pressed", pal["card2"])],
               indicatorbackground=[("selected", pal["accent"])])
