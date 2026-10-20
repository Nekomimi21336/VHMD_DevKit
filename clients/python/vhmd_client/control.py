"""Who drives the VHMD devices: a person ("manual") or a program ("auto").
SteamVR and the VR apps keep running; only the source the driver listens to
changes.

    python -m vhmd_client.control                 # show owners and senders
    python -m vhmd_client.control auto            # hand everything to the automation
    python -m vhmd_client.control manual          # take it back
    python -m vhmd_client.control auto --devices left_hand right_hand   # just the hands
    python -m vhmd_client.control any             # no owner: every sender is applied

Senders name themselves with "src" in their messages (VHMDClient(source=...)).
The FPS tool and the VHMD Link headset send as "manual"; on the headset,
holding X and A for 1.5 s takes control back to "manual".
"""

from __future__ import annotations

import argparse
import json

from .client import DEFAULT_HOST, DEFAULT_PORT, VHMDClient


def describe(reply: dict) -> str:
    c = reply.get("control", {})
    lines = [f"owner: {c.get('owner') or '(anyone)'}"]
    for dev, owner in sorted(c.get("devices", {}).items()):
        lines.append(f"  {dev}: {owner or '(anyone)'}")
    for name, s in sorted(c.get("sources", {}).items()):
        lines.append(f"sender {name}: last {s['age_s']:.1f} s ago, applied {s['accepted']}, ignored {s['ignored']}")
    return "\n".join(lines)


def main() -> None:
    ap = argparse.ArgumentParser(description="Give control of the VHMD devices to a sender")
    ap.add_argument("source", nargs="?", help='"manual", "auto", any sender name, or "any" for no owner')
    ap.add_argument("--devices", nargs="+", help="only these devices (default: all)")
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--json", action="store_true", help="print the raw reply")
    args = ap.parse_args()
    with VHMDClient(args.host, args.port, timeout=2.0) as c:
        source = None if args.source is None else ("" if args.source == "any" else args.source)
        reply = c.control(source, args.devices)
    print(json.dumps(reply, indent=1) if args.json else describe(reply))


if __name__ == "__main__":
    main()
