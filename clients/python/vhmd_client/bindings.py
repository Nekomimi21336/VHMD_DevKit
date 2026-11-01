"""Button bindings: which SteamVR input each headset button presses, on the same
hand. Written "left.menu=system". The native data path does this in
link/relay/src/bindings.rs; this is the same for Python's data path, and the
table both start from.

The left menu button comes from the headset as "menu" or, from newer headset
apps, "system": one physical button, named "menu" here. It presses "system" by
default, which is what opens the SteamVR dashboard.
"""

from __future__ import annotations

from typing import Dict, Iterable, List, Optional, Tuple

# Buttons that can be bound, as the headset names them. The Pico button on the
# right controller belongs to the headset's system: apps never see it.
BUTTONS = {"left": ("menu", "x", "y", "joystick_click"), "right": ("a", "b", "joystick_click")}
COMMON = ("system", "menu", "joystick_click", "trigger_click", "grip_click", "none")
TARGETS = {"left": COMMON[:2] + ("x", "y") + COMMON[2:], "right": COMMON[:2] + ("a", "b") + COMMON[2:]}
DEFAULTS = {"left.menu": "system"}

Hand = Tuple[Dict[str, str], List[str]]   # source -> target, names the message then carries


def table(overrides: Optional[Dict[str, str]] = None) -> Dict[str, str]:
    """The bindings with these changes from the defaults (unknown ones ignored)."""
    t = dict(DEFAULTS)
    for name, dst in (overrides or {}).items():
        hand, _, src = name.partition(".")
        if src in BUTTONS.get(hand, ()) and dst in TARGETS[hand]:
            t[name] = dst
    return {k: v for k, v in t.items() if k in DEFAULTS or k.partition(".")[2] != v}


def wire(t: Dict[str, str]) -> str:
    """As the relay's BINDINGS message."""
    return "\n".join(f"{k}={v}" for k, v in t.items())


def parse(items: Iterable[str]) -> Dict[str, str]:
    """From "left.menu=system" strings."""
    out = {}
    for item in items:
        name, sep, dst = item.partition("=")
        hand, _, src = name.partition(".")
        if not sep or src not in BUTTONS.get(hand, ()) or dst not in TARGETS[hand]:
            raise ValueError(f"not a binding: {item!r} (left.menu=system; left {BUTTONS['left']}, right {BUTTONS['right']}, "
                             f"to {TARGETS['left']} or {TARGETS['right']})")
        out[name] = dst
    return out


def compiled(t: Dict[str, str]) -> Dict[str, Hand]:
    """The table by message id, for apply()."""
    out: Dict[str, Hand] = {}
    for hand in BUTTONS:
        m = {k.partition(".")[2]: v for k, v in t.items() if k.startswith(hand + ".")}
        for dst in list(m.values()):   # a target that is no source stays pressable by its own button
            src = "menu" if dst == "system" else dst
            if dst != "none":
                m.setdefault(src, dst)
        if not m:
            continue
        emit = [n for n in dict.fromkeys(list(m) + list(m.values())) if n != "none"]
        if "menu" in m and "system" not in emit:
            emit.append("system")
        out[f"{hand}_hand"] = (m, emit)
    return out


def apply(messages: List[dict], hands: Dict[str, Hand]) -> None:
    """Moves the buttons of the input messages, in place."""
    for msg in messages:
        hand = hands.get(msg.get("id")) if msg.get("type") == "input" else None
        values = msg.get("values")
        if hand is None or not isinstance(values, dict):
            continue
        m, emit = hand
        pressed = set()
        for key in list(values):
            src = "menu" if key == "system" else key
            if src in m:
                if values.pop(key) is True:
                    pressed.add(m[src])
        for name in emit:
            values[name] = name in pressed
