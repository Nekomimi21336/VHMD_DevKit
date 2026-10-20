"""Reader for the eye-image preview the Linux direct-mode HMD publishes.

The driver writes the latest compositor output (left | right eye side by
side, RGBA8, sRGB encoded) to a shared-memory file, default
``/dev/shm/vhmd_preview``. Layout: ``driver/src/util/preview_shm.h``.
Standard library only.

    from vhmd_client.preview import PreviewReader
    with PreviewReader() as r:
        frame = r.read()            # None until the first frame
        if frame:
            frame.save_png("eyes.png")
"""

from __future__ import annotations

import math
import mmap
import os
import struct
import time
import zlib
from dataclasses import dataclass
from typing import Optional, Tuple

# Linux: a file in /dev/shm. Windows (display.mode = direct): a named shared-memory mapping.
DEFAULT_PATH = "vhmd_preview" if os.name == "nt" else "/dev/shm/vhmd_preview"
MAGIC = b"VHMDPRV1"
# magic, version, header_size, width, height, stride, format, seq,
# frame_index, timestamp_ns, eye_width, source_format, reserved[2]
_HEADER = struct.Struct("<8s6I3Q2I2I")
assert _HEADER.size == 72
_SEQ_OFFSET = 8 + 6 * 4


@dataclass
class Frame:
    width: int
    height: int
    eye_width: int
    frame_index: int
    timestamp_ns: int
    source_format: int
    rgba: bytes  # width * height * 4

    @property
    def age_s(self) -> float:
        return time.time() - self.timestamp_ns / 1e9

    def eye(self, which: str) -> "Frame":
        """Crop to 'left' or 'right'."""
        x0 = 0 if which == "left" else self.eye_width
        row = self.width * 4
        out = bytearray()
        for y in range(self.height):
            start = y * row + x0 * 4
            out += self.rgba[start:start + self.eye_width * 4]
        return Frame(self.eye_width, self.height, self.eye_width, self.frame_index, self.timestamp_ns,
                     self.source_format, bytes(out))

    def rgb(self) -> bytes:
        """Drops alpha (fast, C-level slicing)."""
        n = self.width * self.height
        out = bytearray(n * 3)
        out[0::3] = self.rgba[0::4]
        out[1::3] = self.rgba[1::4]
        out[2::3] = self.rgba[2::4]
        return bytes(out)

    def ppm(self) -> bytes:
        return b"P6 %d %d 255\n" % (self.width, self.height) + self.rgb()

    def save_png(self, path: str) -> None:
        raw = bytearray()
        row = self.width * 4
        for y in range(self.height):
            raw.append(0)  # filter: none
            raw += self.rgba[y * row:(y + 1) * row]

        def chunk(tag: bytes, data: bytes) -> bytes:
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

        ihdr = struct.pack(">IIBBBBB", self.width, self.height, 8, 6, 0, 0, 0)  # 8-bit RGBA
        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
        with open(path, "wb") as f:
            f.write(png)

    def mean_rgb(self) -> tuple:
        """Average colour (sampled), handy for automated checks."""
        step = max(1, (self.width * self.height) // 4096) * 4
        px = self.rgba[0::step], self.rgba[1::step], self.rgba[2::step]
        n = max(1, len(px[0]))
        return tuple(sum(c) / n for c in px)


class _WinMapping:
    """Read-only view of a named Windows file mapping (opened, never created)."""

    def __init__(self, name: str):
        import ctypes
        from ctypes import wintypes
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.OpenFileMappingW.restype = wintypes.HANDLE
        k32.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
        k32.MapViewOfFile.restype = ctypes.c_void_p
        k32.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]
        k32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
        k32.CloseHandle.argtypes = [wintypes.HANDLE]

        class MBI(ctypes.Structure):
            _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                        ("AllocationProtect", wintypes.DWORD), ("PartitionId", wintypes.WORD),
                        ("RegionSize", ctypes.c_size_t), ("State", wintypes.DWORD), ("Protect", wintypes.DWORD),
                        ("Type", wintypes.DWORD)]

        self._k32 = k32
        self.handle = k32.OpenFileMappingW(0x0004, False, name)  # FILE_MAP_READ
        if not self.handle:
            raise FileNotFoundError(name)
        self.addr = k32.MapViewOfFile(self.handle, 0x0004, 0, 0, 0)
        if not self.addr:
            k32.CloseHandle(self.handle)
            raise OSError("MapViewOfFile failed")
        mbi = MBI()
        k32.VirtualQuery(ctypes.c_void_p(self.addr), ctypes.byref(mbi), ctypes.sizeof(mbi))
        self.size = mbi.RegionSize
        self.buf = (ctypes.c_char * self.size).from_address(self.addr)

    def close(self) -> None:
        if self.addr:
            self._k32.UnmapViewOfFile(self.addr)
            self._k32.CloseHandle(self.handle)
        self.addr = None


class PreviewReader:
    """Reads the eye-image preview the direct-mode HMD publishes (Linux, or Windows
    with display.mode = direct). The driver only writes while a tool has asked
    for it: call VHMDClient().preview(True) first (preview_viewer.py does)."""

    def __init__(self, path: str = DEFAULT_PATH):
        self.path = path
        self._fd: Optional[int] = None
        self._mm = None
        self._win: Optional[_WinMapping] = None
        self._size = 0

    def close(self) -> None:
        if self._win is not None:
            self._win.close()
        elif self._mm is not None:
            self._mm.close()
        if self._fd is not None:
            os.close(self._fd)
        self._mm = None
        self._win = None
        self._fd = None
        self._size = 0

    def __enter__(self) -> "PreviewReader":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _map(self) -> bool:
        if os.name == "nt" and not os.path.dirname(self.path):  # a mapping name, not a file
            if self._win is not None:
                return True
            try:
                self._win = _WinMapping(self.path)
            except (FileNotFoundError, OSError):
                return False
            self._mm = self._win.buf
            self._size = self._win.size
            return self._size >= _HEADER.size
        try:
            st = os.stat(self.path)
        except FileNotFoundError:
            self.close()
            return False
        if self._mm is not None and st.st_size == self._size and st.st_size >= _HEADER.size:
            return True
        self.close()
        if st.st_size < _HEADER.size:
            return False
        self._fd = os.open(self.path, os.O_RDONLY)
        self._mm = mmap.mmap(self._fd, st.st_size, access=mmap.ACCESS_READ)
        self._size = st.st_size
        return True

    def read(self, retries: int = 50) -> Optional[Frame]:
        """Latest complete frame, or None if no preview is being published."""
        for _ in range(retries):
            if not self._map():
                return None
            mm = self._mm
            s1 = struct.unpack_from("<Q", mm, _SEQ_OFFSET)[0]
            if s1 & 1:
                time.sleep(0.0005)
                continue
            (magic, version, header_size, width, height, stride, fmt, _seq, frame_index, ts, eye_w, src_fmt,
             _r0, _r1) = _HEADER.unpack_from(mm, 0)
            if magic != MAGIC or version != 1:
                return None
            need = header_size + stride * height
            if need > self._size:  # resized under us: re-map and retry
                self.close()
                continue
            data = mm[header_size:header_size + stride * height]
            if struct.unpack_from("<Q", mm, _SEQ_OFFSET)[0] != s1:
                continue  # torn read
            if stride != width * 4:
                data = b"".join(data[y * stride:y * stride + width * 4] for y in range(height))
            return Frame(width, height, eye_w, frame_index, ts, src_fmt, bytes(data))
        return None

    def read_scaled(self, max_w: int, max_h: int, eye: str = "left", skip_index: int = -1,
                    pixel_format: str = "rgb", retries: int = 50) -> Optional[Tuple[int, int, int, bytes]]:
        """One eye shrunk by an integer factor to fit max_w x max_h:
        (width, height, frame_index, pixels). pixel_format "rgb" (3 bytes per
        pixel) or "bgrx" (4 bytes, the layout of a 32-bit Windows DIB). Copies
        only the rows and pixels it needs, so it is cheap enough for a live view.
        Returns None when there is no frame or the latest one is still skip_index."""
        for _ in range(retries):
            if not self._map():
                return None
            mm = self._mm
            s1 = struct.unpack_from("<Q", mm, _SEQ_OFFSET)[0]
            if s1 & 1:
                time.sleep(0.0005)
                continue
            (magic, version, header_size, width, height, stride, fmt, _seq, frame_index, ts, eye_w, src_fmt,
             _r0, _r1) = _HEADER.unpack_from(mm, 0)
            if magic != MAGIC or version != 1:
                return None
            if frame_index == skip_index:
                return None
            if header_size + stride * height > self._size:
                self.close()
                continue
            ew = eye_w or width
            x0 = ew if eye == "right" else 0
            k = max(1, math.ceil(ew / max_w), math.ceil(height / max_h))
            ow, oh = len(range(0, ew, k)), len(range(0, height, k))
            bpp = 4 if pixel_format == "bgrx" else 3
            r, b = (2, 0) if bpp == 4 else (0, 2)          # BGRX swaps red and blue
            out = bytearray(ow * oh * bpp)
            step, o, n = 4 * k, 0, ow * bpp
            for y in range(0, height, k):
                start = header_size + y * stride + x0 * 4
                row = mm[start:start + ew * 4]
                out[o + r:o + n:bpp] = row[0::step]
                out[o + 1:o + n:bpp] = row[1::step]
                out[o + b:o + n:bpp] = row[2::step]
                o += n
            if struct.unpack_from("<Q", mm, _SEQ_OFFSET)[0] != s1:
                continue  # torn read
            return ow, oh, frame_index, bytes(out)
        return None
