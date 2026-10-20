"""Video for the VHMD Link headset app: VHMD's eye images (the direct-mode
preview in shared memory, both eyes side by side) encoded to H.264 by ffmpeg
with the GPU's encoder and sent to the headset over TCP.

ffmpeg sends RTP to a local UDP port: the RTP marker bit tells exactly where
each frame ends, so a frame goes out as soon as it is encoded (Annex B access
unit). The headset gets them on TCP port 39581 (adb reverse) as messages:

    header (64 bytes, little endian):
        magic  b"VHV1"      u32 kind (1 = config JSON, 2 = H.264 access unit)
        u32 payload size    u32 flags (bit 0: pose valid)
        u64 frame index     u64 capture time (microseconds, PC clock)
        f32 pose[7]         head pose the frame was rendered for (x y z, qw qx qy qz)
        u32 reserved
    payload

The config message (JSON) gives the codec, the frame size and each eye's field
of view as OpenXR angles (radians) taken from SteamVR's projection for the
VHMD HMD, so the headset shows the image with the geometry it was drawn with.
"""

from __future__ import annotations

import json
import math
import shutil
import socket
import struct
import subprocess
import threading
import time
from typing import List, Optional

from .client import VHMDClient
from .preview import PreviewReader

VIDEO_PORT = 39581
HEADER = struct.Struct("<4sIIIQQ7fI")
assert HEADER.size == 64
KIND_CONFIG, KIND_FRAME = 1, 2


def eye_fovs() -> Optional[List[List[float]]]:
    """[left, right] fields of view [angleLeft, angleRight, angleUp, angleDown]
    of SteamVR's HMD (pyopenvr), or None."""
    try:
        import openvr
    except ImportError:
        return None
    try:
        vr = openvr.init(openvr.VRApplication_Background)
    except Exception:
        return None
    try:
        out = []
        for eye in (openvr.Eye_Left, openvr.Eye_Right):
            left, right, top, bottom = vr.getProjectionRaw(eye)
            # OpenVR: tangents with y pointing down (top < 0). OpenXR: angles, up > 0.
            out.append([math.atan(left), math.atan(right), math.atan(-top), math.atan(-bottom)])
        return out
    finally:
        openvr.shutdown()


class RtpDepacketizer:
    """H.264 over RTP (RFC 6184): single NAL, STAP-A and FU-A packets to Annex B
    access units, one per RTP marker bit."""

    def __init__(self) -> None:
        self.au = bytearray()

    def push(self, pkt: bytes) -> Optional[bytes]:
        if len(pkt) < 12:
            return None
        cc = pkt[0] & 0x0F
        ext = pkt[0] & 0x10
        marker = pkt[1] & 0x80
        off = 12 + 4 * cc
        if ext:
            off += 4 + 4 * struct.unpack_from(">H", pkt, off + 2)[0]
        p = pkt[off:]
        if not p:
            return None
        t = p[0] & 0x1F
        if 1 <= t <= 23:
            self.au += b"\x00\x00\x00\x01" + p
        elif t == 24:                                   # STAP-A
            i = 1
            while i + 2 <= len(p):
                n = struct.unpack_from(">H", p, i)[0]
                self.au += b"\x00\x00\x00\x01" + p[i + 2:i + 2 + n]
                i += 2 + n
        elif t == 28 and len(p) > 2:                    # FU-A
            if p[1] & 0x80:
                self.au += b"\x00\x00\x00\x01" + bytes([(p[0] & 0xE0) | (p[1] & 0x1F)])
            self.au += p[2:]
        if marker:
            au, self.au = bytes(self.au), bytearray()
            return au
        return None


class VideoServer(threading.Thread):
    def __init__(self, port: int = VIDEO_PORT, bitrate_mbps: float = 30.0, fps: float = 90.0,
                 encoder: str = "h264_amf", log=print) -> None:
        super().__init__(name="vhmd-video", daemon=True)
        self.port, self.bitrate, self.fps, self.encoder, self.log = port, bitrate_mbps, fps, encoder, log
        self.frames_in = self.frames_out = self.bytes_out = 0

    def run(self) -> None:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", self.port))
        srv.listen(1)
        self.log(f"video: waiting for the headset on 127.0.0.1:{self.port}")
        while True:
            conn, _ = srv.accept()
            self.log("video: headset connected")
            try:
                self.stream(conn)
            except (OSError, ValueError) as e:
                self.log(f"video: stopped ({e})")
            finally:
                conn.close()

    def stream(self, conn: socket.socket) -> None:
        if not shutil.which("ffmpeg"):
            raise OSError("ffmpeg not found on PATH")
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        client = VHMDClient(timeout=2.0)
        client.preview(True)
        reader = PreviewReader()
        ffmpeg = None
        try:
            frame = None
            t0 = time.time()
            while frame is None and time.time() - t0 < 5:
                frame = reader.read()
                time.sleep(0.02)
            if frame is None:
                raise OSError("no preview frames (is the driver in direct mode?)")
            w, h = frame.width, frame.height
            config = {"codec": "h264", "width": w, "height": h, "eye_width": frame.eye_width, "fov": eye_fovs()}
            self.send(conn, KIND_CONFIG, 0, json.dumps(config).encode())
            self.log(f"video: {w}x{h}, {self.encoder} {self.bitrate:g} Mbit/s, fov {config['fov'] is not None}")

            rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            rtp.bind(("127.0.0.1", 0))
            rtp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
            rtp_port = rtp.getsockname()[1]
            br = f"{self.bitrate:g}M"
            cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-fflags", "nobuffer",
                   "-f", "rawvideo", "-pix_fmt", "rgba", "-s", f"{w}x{h}", "-r", f"{self.fps:g}", "-i", "pipe:0",
                   # Limited-range BT.709 YUV: headset decoders (and the GPU's YUV sampler) assume
                   # limited range and crush the dark end of a full-range stream.
                   "-vf", "scale=out_color_matrix=bt709:out_range=tv:flags=fast_bilinear,format=nv12",
                   "-color_range", "tv", "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709",
                   "-c:v", self.encoder]
            if self.encoder.endswith("_amf"):
                cmd += ["-usage", "ultralowlatency", "-rc", "cbr", "-async_depth", "1", "-header_spacing", "30"]
            cmd += ["-b:v", br, "-maxrate", br, "-bufsize", br, "-g", "30", "-bf", "0",
                    "-f", "rtp", "-payload_type", "96", f"rtp://127.0.0.1:{rtp_port}?pkt_size=1300"]
            ffmpeg = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                      creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            stop = threading.Event()
            index = {"n": 0}

            def out_loop() -> None:
                dep = RtpDepacketizer()
                rtp.settimeout(0.5)
                while not stop.is_set():
                    try:
                        pkt = rtp.recv(65536)
                    except socket.timeout:
                        continue
                    au = dep.push(pkt)
                    if au:
                        index["n"] += 1
                        self.send(conn, KIND_FRAME, index["n"], au)
                        self.frames_out += 1
                        self.bytes_out += len(au)

            out = threading.Thread(target=out_loop, daemon=True)
            out.start()
            last = -1
            try:
                while ffmpeg.poll() is None and out.is_alive():
                    hdr = reader.peek()
                    if hdr is None or hdr[4] == last:
                        time.sleep(0.001)
                        continue
                    f = reader.read()
                    if f is None or (f.width, f.height) != (w, h):
                        continue
                    last = f.frame_index
                    ffmpeg.stdin.write(f.rgba)
                    ffmpeg.stdin.flush()
                    self.frames_in += 1
            finally:
                stop.set()
                rtp.close()
        finally:
            if ffmpeg:
                try:
                    ffmpeg.stdin.close()
                except OSError:
                    pass
                ffmpeg.kill()
            try:
                client.preview(False)
            except Exception:
                pass
            reader.close()

    def send(self, conn: socket.socket, kind: int, index: int, payload: bytes, pose=None) -> None:
        flags = 1 if pose else 0
        p = pose or (0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0)
        conn.sendall(HEADER.pack(b"VHV1", kind, len(payload), flags, index, int(time.time() * 1e6), *p, 0) + payload)

    def take_stats(self):
        out = (self.frames_in, self.frames_out, self.bytes_out)
        self.frames_in = self.frames_out = self.bytes_out = 0
        return out
