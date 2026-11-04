"""Sound on Linux (PulseAudio, or PipeWire through pipewire-pulse), with pactl / parec.

- The headset's microphone: the driver plays it into the sink MIC_SINK. This
  makes that sink (a null sink) and a source on its monitor, MIC_SOURCE
  ("VHMD-Microphone"), which programs then choose as their microphone: what VB-CABLE
  is on Windows, with nothing to install. Both last until the sound server
  restarts (log out); VHMD Link makes them again when the microphone is on.
- The PC's sound for the headset: the default output's monitor, as 16-bit
  48 kHz stereo (pc_sound()).
"""

from __future__ import annotations

import shutil
import subprocess
from typing import Optional

MIC_SINK = "vhmd_mic"
MIC_SOURCE = "vhmd_mic_source"
MIC_NAME = "VHMD-Microphone"   # no spaces: PulseAudio 16 makes no sink for a quoted description
RATE, CHANNELS = 48000, 2


def _pactl(*args: str) -> str:
    return subprocess.run(["pactl", *args], capture_output=True, text=True, timeout=5).stdout


def ensure_mic_sink() -> str:
    """Makes MIC_SINK and MIC_SOURCE if they are not there. "" when done, else why not."""
    if not shutil.which("pactl"):
        return "pactl not found (install pulseaudio-utils)"
    try:
        sinks = _pactl("list", "short", "sinks").split()
        if MIC_SINK not in sinks:
            _pactl("load-module", "module-null-sink", f"sink_name={MIC_SINK}",
                   f"sink_properties=device.description={MIC_NAME}-Output")
        sources = _pactl("list", "short", "sources").split()
        if MIC_SOURCE not in sources:
            _pactl("load-module", "module-remap-source", f"master={MIC_SINK}.monitor", f"source_name={MIC_SOURCE}",
                   f"source_properties=device.description={MIC_NAME}")
        return "" if MIC_SINK in _pactl("list", "short", "sinks").split() else "the sink was not made"
    except (OSError, subprocess.SubprocessError) as e:
        return str(e)


def pc_sound() -> Optional[subprocess.Popen]:
    """parec on the default output's monitor (raw s16le), or None without parec."""
    if not shutil.which("parec"):
        return None
    return subprocess.Popen(["parec", "--raw", "--format=s16le", f"--rate={RATE}", f"--channels={CHANNELS}",
                             "--latency-msec=10", "-d", "@DEFAULT_MONITOR@"],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
