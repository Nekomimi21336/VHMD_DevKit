"""Show what SteamVR renders into the virtual HMD (Linux direct mode).

Reads the shared-memory preview the driver writes (default
/dev/shm/vhmd_preview) and displays it in a Tk window. Standard library only.

    python preview_viewer.py                  # live window
    python preview_viewer.py --eye left       # one eye
    python preview_viewer.py --snapshot eyes.png [--wait 10]   # save one frame and exit
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from vhmd_client.preview import DEFAULT_PATH, PreviewReader  # noqa: E402


def snapshot(reader: PreviewReader, path: str, eye: str, wait: float) -> int:
    deadline = time.time() + wait
    while time.time() < deadline:
        frame = reader.read()
        if frame:
            if eye != "both":
                frame = frame.eye(eye)
            frame.save_png(path)
            r, g, b = frame.mean_rgb()
            print(f"saved {path}: {frame.width}x{frame.height}, frame #{frame.frame_index}, "
                  f"age {frame.age_s * 1000:.0f} ms, mean RGB ({r:.0f}, {g:.0f}, {b:.0f})")
            return 0
        time.sleep(0.1)
    print(f"no preview frame in {wait:.0f} s at {reader.path} "
          "(is SteamVR running with the vhmd HMD in direct mode, and preview.enabled true?)", file=sys.stderr)
    return 1


def live(reader: PreviewReader, eye: str, max_width: int) -> int:
    import tkinter as tk

    root = tk.Tk()
    root.title("VHMD preview")
    label = tk.Label(root, bg="black")
    label.pack(fill="both", expand=True)
    status = tk.Label(root, anchor="w", font=("TkFixedFont", 9))
    status.pack(fill="x")
    state = {"img": None, "last": 0, "t": time.time(), "n": 0}

    def tick():
        frame = reader.read()
        if frame is None:
            status.config(text=f"waiting for {reader.path} ...")
        elif frame.frame_index != state["last"]:
            state["last"] = frame.frame_index
            if eye != "both":
                frame = frame.eye(eye)
            img = tk.PhotoImage(data=frame.ppm(), format="PPM")
            factor = max(1, -(-frame.width // max_width))  # ceil
            if factor > 1:
                img = img.subsample(factor, factor)
            label.config(image=img)
            state["img"] = img  # keep a reference
            state["n"] += 1
            now = time.time()
            fps = state["n"] / max(1e-6, now - state["t"])
            if now - state["t"] > 2:
                state["t"], state["n"] = now, 0
            status.config(text=f"#{frame.frame_index}  {frame.width}x{frame.height}  "
                               f"{fps:4.1f} fps  age {frame.age_s * 1000:4.0f} ms  src VkFormat {frame.source_format}")
        root.after(15, tick)

    tick()
    root.mainloop()
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--path", default=DEFAULT_PATH)
    ap.add_argument("--eye", choices=("both", "left", "right"), default="both")
    ap.add_argument("--max-width", type=int, default=1600, help="downscale the window to at most this width")
    ap.add_argument("--snapshot", metavar="PNG", help="save one frame to PNG and exit")
    ap.add_argument("--wait", type=float, default=10.0, help="seconds to wait for a frame with --snapshot")
    args = ap.parse_args()
    with PreviewReader(args.path) as reader:
        if args.snapshot:
            return snapshot(reader, args.snapshot, args.eye, args.wait)
        return live(reader, args.eye, args.max_width)


if __name__ == "__main__":
    sys.exit(main())
