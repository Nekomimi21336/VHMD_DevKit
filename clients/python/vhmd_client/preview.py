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

import mmap
import os
import struct
import time
import zlib
from dataclasses import dataclass
from typing import Optional

DEFAULT_PATH = "/dev/shm/vhmd_preview"
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


class PreviewReader:
    def __init__(self, path: str = DEFAULT_PATH):
        self.path = path
        self._fd: Optional[int] = None
        self._mm: Optional[mmap.mmap] = None
        self._size = 0

    def close(self) -> None:
        if self._mm is not None:
            self._mm.close()
        if self._fd is not None:
            os.close(self._fd)
        self._mm = None
        self._fd = None
        self._size = 0

    def __enter__(self) -> "PreviewReader":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _map(self) -> bool:
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
