"""Python client for the VirtualHMD SteamVR development driver."""

from .client import DEFAULT_HOST, DEFAULT_PORT, VHMDClient
from . import math3d

TRACKER_IDS = [f"tracker_{i}" for i in range(16)]
HAND_IDS = ("left_hand", "right_hand")

__all__ = ["VHMDClient", "DEFAULT_HOST", "DEFAULT_PORT", "math3d", "TRACKER_IDS", "HAND_IDS"]
__version__ = "0.1.0"
