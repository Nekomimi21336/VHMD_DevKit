"""VHMD Link for the PC, without a console window (double-click, or pythonw).
Same as `python -m vhmd_client.link`."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from vhmd_client.link import main  # noqa: E402

main()
