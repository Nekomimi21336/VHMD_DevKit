"""Where VHMD's PC side keeps its files, per OS.

Windows: %LOCALAPPDATA%/VHMD. Linux: $XDG_STATE_HOME/vhmd (~/.local/state/vhmd).
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

IS_WINDOWS = sys.platform == "win32"


def data_dir() -> Path:
    if IS_WINDOWS:
        return Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "VHMD"
    return Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local" / "state") / "vhmd"


def config_home() -> Path:
    """Linux: $XDG_CONFIG_HOME (~/.config), where OpenVR and autostart entries live."""
    return Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
