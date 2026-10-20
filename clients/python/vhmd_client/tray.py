"""A Windows notification-area (tray) icon without extra packages (ctypes).

The icon lives on its own thread with a hidden window that receives the shell's
callbacks; clicks are handed over through a queue (Tk is not thread-safe):

    tray = TrayIcon("VHMD Link", icon_path, menu=[("open", "Open"), ("quit", "Quit")])
    tray.start()
    ... tray.events.get_nowait() -> "open" (left click or menu) / "quit"
    tray.set_tooltip("VHMD Link: connected")
    tray.stop()
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import queue
import threading
from typing import List, Optional, Tuple

WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t)
WM_APP_TRAY = 0x8000 + 1
WM_CLOSE, WM_DESTROY, WM_LBUTTONUP, WM_RBUTTONUP, WM_LBUTTONDBLCLK = 0x0010, 0x0002, 0x0202, 0x0205, 0x0203
NIM_ADD, NIM_MODIFY, NIM_DELETE = 0, 1, 2
NIF_MESSAGE, NIF_ICON, NIF_TIP = 1, 2, 4
TPM_RETURNCMD, TPM_RIGHTBUTTON = 0x0100, 0x0002


class WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", ctypes.c_uint), ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int), ("hInstance", ctypes.c_void_p), ("hIcon", ctypes.c_void_p),
                ("hCursor", ctypes.c_void_p), ("hbrBackground", ctypes.c_void_p),
                ("lpszMenuName", ctypes.c_wchar_p), ("lpszClassName", ctypes.c_wchar_p)]


class NOTIFYICONDATAW(ctypes.Structure):
    _fields_ = [("cbSize", ctypes.c_uint32), ("hWnd", ctypes.c_void_p), ("uID", ctypes.c_uint32),
                ("uFlags", ctypes.c_uint32), ("uCallbackMessage", ctypes.c_uint32), ("hIcon", ctypes.c_void_p),
                ("szTip", ctypes.c_wchar * 128), ("dwState", ctypes.c_uint32), ("dwStateMask", ctypes.c_uint32),
                ("szInfo", ctypes.c_wchar * 256), ("uVersion", ctypes.c_uint32),
                ("szInfoTitle", ctypes.c_wchar * 64), ("dwInfoFlags", ctypes.c_uint32),
                ("guidItem", ctypes.c_byte * 16), ("hBalloonIcon", ctypes.c_void_p)]


class TrayIcon:
    def __init__(self, tooltip: str, icon_path: Optional[str], menu: List[Tuple[str, str]]) -> None:
        self.tooltip = tooltip[:127]
        self.icon_path = icon_path
        self.menu = menu                 # (event name, label); the first is also the left-click action
        self.events: "queue.Queue[str]" = queue.Queue()
        self._hwnd = None
        self._ready = threading.Event()
        self.ok = False

    def start(self) -> None:
        threading.Thread(target=self._run, name="vhmd-tray", daemon=True).start()
        self._ready.wait(3.0)

    def set_tooltip(self, text: str) -> None:
        self.tooltip = text[:127]
        if self._hwnd:
            self._u32.PostMessageW(self._hwnd, WM_APP_TRAY + 1, 0, 0)   # updated on the tray thread

    def stop(self) -> None:
        if self._hwnd:
            self._u32.PostMessageW(self._hwnd, WM_CLOSE, 0, 0)

    def _run(self) -> None:
        u32 = ctypes.WinDLL("user32")
        k32 = ctypes.WinDLL("kernel32")
        sh = ctypes.WinDLL("shell32")
        self._u32 = u32
        u32.DefWindowProcW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
        u32.DefWindowProcW.restype = ctypes.c_ssize_t
        u32.RegisterClassW.argtypes = [ctypes.POINTER(WNDCLASSW)]
        u32.RegisterClassW.restype = ctypes.c_ushort
        u32.CreateWindowExW.argtypes = [ctypes.c_uint32, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32,
                                        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
        u32.CreateWindowExW.restype = ctypes.c_void_p
        u32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
        u32.LoadImageW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_uint]
        u32.LoadImageW.restype = ctypes.c_void_p
        u32.LoadIconW.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        u32.LoadIconW.restype = ctypes.c_void_p
        u32.CreatePopupMenu.restype = ctypes.c_void_p
        u32.AppendMenuW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_wchar_p]
        u32.TrackPopupMenu.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                       ctypes.c_void_p, ctypes.c_void_p]
        u32.DestroyMenu.argtypes = [ctypes.c_void_p]
        u32.SetForegroundWindow.argtypes = [ctypes.c_void_p]
        u32.GetCursorPos.argtypes = [ctypes.POINTER(wt.POINT)]
        u32.DestroyWindow.argtypes = [ctypes.c_void_p]
        u32.GetMessageW.argtypes = [ctypes.POINTER(wt.MSG), ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint]
        u32.DispatchMessageW.argtypes = [ctypes.POINTER(wt.MSG)]
        sh.Shell_NotifyIconW.argtypes = [ctypes.c_uint32, ctypes.POINTER(NOTIFYICONDATAW)]
        k32.GetModuleHandleW.restype = ctypes.c_void_p

        nid = NOTIFYICONDATAW()

        def show_menu(hwnd) -> None:
            menu = u32.CreatePopupMenu()
            for i, (_, label) in enumerate(self.menu):
                u32.AppendMenuW(menu, 0, i + 1, label)
            pt = wt.POINT()
            u32.GetCursorPos(ctypes.byref(pt))
            u32.SetForegroundWindow(hwnd)   # so the menu closes when clicking elsewhere
            cmd = u32.TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, None)
            u32.DestroyMenu(menu)
            if cmd:
                self.events.put(self.menu[cmd - 1][0])

        def proc(hwnd, msg, wparam, lparam):
            if msg == WM_APP_TRAY:
                if lparam in (WM_LBUTTONUP, WM_LBUTTONDBLCLK) and self.menu:
                    self.events.put(self.menu[0][0])
                elif lparam == WM_RBUTTONUP:
                    show_menu(hwnd)
                return 0
            if msg == WM_APP_TRAY + 1:
                nid.szTip = self.tooltip
                sh.Shell_NotifyIconW(NIM_MODIFY, ctypes.byref(nid))
                return 0
            if msg == WM_CLOSE:
                sh.Shell_NotifyIconW(NIM_DELETE, ctypes.byref(nid))
                u32.DestroyWindow(hwnd)
                return 0
            if msg == WM_DESTROY:
                u32.PostQuitMessage(0)
                return 0
            return u32.DefWindowProcW(hwnd, msg, wparam, lparam)

        self._proc = WNDPROC(proc)   # keep alive
        hinst = k32.GetModuleHandleW(None)
        wc = WNDCLASSW(0, self._proc, 0, 0, hinst, None, None, None, None, "VHMDLinkTray")
        u32.RegisterClassW(ctypes.byref(wc))
        hwnd = u32.CreateWindowExW(0, "VHMDLinkTray", "VHMD Link", 0, 0, 0, 0, 0, None, None, hinst, None)
        if not hwnd:
            self._ready.set()
            return
        icon = None
        if self.icon_path:
            icon = u32.LoadImageW(None, self.icon_path, 1, 0, 0, 0x10 | 0x40)   # IMAGE_ICON, LOADFROMFILE|DEFAULTSIZE
        if not icon:
            icon = u32.LoadIconW(None, ctypes.c_void_p(32512))                   # IDI_APPLICATION
        nid.cbSize = ctypes.sizeof(NOTIFYICONDATAW)
        nid.hWnd = hwnd
        nid.uID = 1
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP
        nid.uCallbackMessage = WM_APP_TRAY
        nid.hIcon = icon
        nid.szTip = self.tooltip
        self.ok = bool(sh.Shell_NotifyIconW(NIM_ADD, ctypes.byref(nid)))
        self._hwnd = hwnd
        self._ready.set()
        msg = wt.MSG()
        while u32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            u32.DispatchMessageW(ctypes.byref(msg))
        self._hwnd = None
