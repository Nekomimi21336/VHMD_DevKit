"""Unit tests for VHMD Link's PC side (no SteamVR, headset or relay needed).

    python -m pytest tests/python
"""

import json
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "clients" / "python"))

from vhmd_client import bindings, headset_bridge, native_relay  # noqa: E402


class FakeRelay:
    """NativeRelay's interface: records the VIDEO settings sent."""

    def __init__(self):
        self.sent = []
        self.stats = {}

    def video(self, settings):
        self.sent.append(dict(settings))


def video(relay, **kw):
    args = dict(bitrate=40.0, codec="hevc", buffer=0, encoder_backend="auto", eye_width=1920, eye_scale=1.0,
                adaptive=True)
    args.update(kw)
    return native_relay.NativeVideo(relay, **args)


# -- the relay's video settings ------------------------------------------------
def test_video_tcp_until_headset_offers_udp():
    r = FakeRelay()
    v = video(r, udp=True, fec=30)
    assert r.sent[-1]["transport"] == "tcp" and r.sent[-1]["udp_port"] == 0
    v.headset_changed({"video_udp_port": 40001})
    assert r.sent[-1]["transport"] == "udp" and r.sent[-1]["udp_port"] == 40001 and r.sent[-1]["fec"] == 30
    n = len(r.sent)
    v.headset_changed({"video_udp_port": 40001})   # nothing new: no reconnect
    assert len(r.sent) == n


def test_video_udp_off_stays_tcp():
    r = FakeRelay()
    v = video(r, udp=False)
    v.headset_changed({"video_udp_port": 40001})
    assert all(s["transport"] == "tcp" for s in r.sent)


def test_video_foveation_and_restart():
    r = FakeRelay()
    v = video(r, foveation=headset_bridge.FOVEATION[2])
    assert r.sent[-1]["foveation"] == [0.4, 4.0, 0.4, 4.0] and r.sent[-1]["restart"] is False
    v.foveation = headset_bridge.FOVEATION[0]
    v.restart()
    assert r.sent[-1]["foveation"] == [] and r.sent[-1]["restart"] is True


def test_video_eye_scale(monkeypatch):
    from vhmd_client import headset_info
    monkeypatch.setattr(headset_info, "load", lambda: {"render": [2160, 2160]})
    r = FakeRelay()
    video(r, eye_width=0, eye_scale=0.7)
    eye = r.sent[-1]["eye_width"]
    assert eye % 16 == 0 and 0.69 * 2160 <= eye <= 0.7 * 2160
    video(r, eye_width=0, eye_scale=0.1)
    assert r.sent[-1]["eye_width"] == 640   # never below


def test_foveation_pixel_shares():
    """The shares the PC app shows (light 49 %, strong 30 %): per axis the centre
    plus the squeezed edges, squared."""
    assert headset_bridge.FOVEATION[0] is None
    for level, share in ((1, 0.49), (2, 0.30)):
        cx, rx, cy, ry = headset_bridge.FOVEATION[level]
        assert abs((cx + (1 - cx) / rx) * (cy + (1 - cy) / ry) - share) < 0.005


# -- bitrate over UDP --------------------------------------------------------
def test_udp_adapt(monkeypatch):
    r = FakeRelay()
    b = object.__new__(headset_bridge.Bridge)
    b.o = headset_bridge.BridgeOptions(udp_video=True)
    b.video = video(r, udp=True)
    b.video.headset_changed({"video_udp_port": 40001})
    b._native = r
    logs = []
    b.log = logs.append
    clock = [100.0]
    monkeypatch.setattr(headset_bridge.time, "monotonic", lambda: clock[0])

    b._udp_adapt({"udp_lost": 0})
    assert "bitrate_mbps" not in r.sent[-1] or r.sent[-1]["bitrate_mbps"] == 40.0
    b._udp_adapt({"udp_lost": 3})              # more than 1 frame/s lost: down 20 %
    assert r.sent[-1] == {"bitrate_mbps": 32.0}
    clock[0] += 5
    b._udp_adapt({"udp_lost": 0.5})            # within 10 s of the loss: stays
    assert r.sent[-1] == {"bitrate_mbps": 32.0}
    clock[0] += 6
    b._udp_adapt({"udp_lost": 0})              # calm for 11 s: up 10 %
    assert r.sent[-1] == {"bitrate_mbps": 35.2}
    for _ in range(20):
        clock[0] += 11
        b._udp_adapt({"udp_lost": 0})
    assert r.sent[-1]["bitrate_mbps"] <= 40.0   # never above the chosen bitrate
    for _ in range(30):
        b._udp_adapt({"udp_lost": 10})
    assert r.sent[-1]["bitrate_mbps"] >= 8.0    # nor below 8
    assert logs


def test_udp_adapt_only_for_udp():
    r = FakeRelay()
    b = object.__new__(headset_bridge.Bridge)
    b.o = headset_bridge.BridgeOptions()
    b.video = video(r, udp=False)
    b._native = r
    b.log = lambda s: None
    n = len(r.sent)
    b._udp_adapt({"udp_lost": 10})
    assert len(r.sent) == n


# -- button bindings ---------------------------------------------------------
def hand_input(hand, **values):
    return {"type": "input", "id": f"{hand}_hand", "src": "manual", "values": {"trigger": 0.25, **values}}


def press(overrides, hand, **values):
    msg = hand_input(hand, **values)
    head = {"type": "pose", "id": "hmd"}
    msgs = [head, msg]
    bindings.apply(msgs, bindings.compiled(bindings.table(overrides)))
    assert msgs[0] == {"type": "pose", "id": "hmd"} and msg["values"]["trigger"] == 0.25
    return msg["values"]


@pytest.mark.parametrize("key", ["menu", "system"])
def test_left_menu_presses_system_by_default(key):
    """An older headset app sends "menu", a newer one "system": the same button."""
    v = press({}, "left", x=False, **{key: True})
    assert v["system"] is True and v["menu"] is False and v["x"] is False


def test_bind_menu_back_to_app_menu():
    v = press({"left.menu": "menu"}, "left", system=True)
    assert v["menu"] is True and v["system"] is False


def test_bind_swap_merge_none():
    v = press({"right.a": "b", "right.b": "a"}, "right", a=True, b=False)
    assert v["b"] is True and v["a"] is False
    v = press({"right.a": "b"}, "right", a=False, b=True)   # b keeps its own input
    assert v["b"] is True
    v = press({"left.x": "none"}, "left", x=True)
    assert v["x"] is False


def test_bind_table_and_wire():
    assert bindings.table() == bindings.DEFAULTS
    t = bindings.table({"right.a": "a", "left.y": "system", "left.nope": "x", "right.b": "x"})
    assert t == {"left.menu": "system", "left.y": "system"}   # identity and unknown ones dropped
    assert bindings.wire(t) == "left.menu=system\nleft.y=system"
    assert bindings.parse(["left.menu=menu"]) == {"left.menu": "menu"}
    with pytest.raises(ValueError):
        bindings.parse(["left.menu"])
    with pytest.raises(ValueError):
        bindings.parse(["right.x=a"])


def test_every_button_named_in_the_app():
    from vhmd_client import link
    for texts in link.TEXT.values():
        for hand, buttons in bindings.BUTTONS.items():
            assert all(f"b_{hand}_{b}" in texts for b in buttons)
            assert all("t_" + d in texts for d in bindings.TARGETS[hand])


def test_bridge_sends_and_changes_bindings():
    class Relay(FakeRelay):
        text = None

        def bindings(self, text):
            self.text = text

    b = object.__new__(headset_bridge.Bridge)
    b.o = headset_bridge.BridgeOptions()
    b._native = Relay()
    b.set_bindings({"left.x": "y"})
    assert b._native.text == "left.menu=system\nleft.x=y"
    assert set(b._hands) == {"left_hand"}


def test_python_data_path_moves_buttons(monkeypatch):
    b = object.__new__(headset_bridge.Bridge)
    b.link = headset_bridge.HeadsetLink()
    b.stats = headset_bridge.Stats()
    b._fwd_samples = []
    b._hands = bindings.compiled(bindings.table())
    sent = []

    class Udp:
        def sendto(self, data, addr):
            sent.append(json.loads(data))

    b._udp, b._driver = Udp(), ("127.0.0.1", 1)
    b._forward(json.dumps([hand_input("left", menu=True)], separators=(",", ":")).encode())
    assert sent[-1][0]["values"]["system"] is True


RELAY_EXE = Path(__file__).resolve().parents[2] / "link" / "target" / "release" / (
    "vhmd-link-relay.exe" if sys.platform == "win32" else "vhmd-link-relay")


def test_relay_moves_buttons_like_python():
    """The real relay on loopback, with a stand-in for the driver: the same
    result as bindings.apply."""
    if not RELAY_EXE.exists():
        pytest.skip("relay not built")
    probe = socket.socket()
    try:
        probe.bind(("127.0.0.1", 39580))
    except OSError:
        pytest.skip("VHMD Link is running")
    finally:
        probe.close()
    driver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    driver.bind(("127.0.0.1", 0))
    driver.settimeout(2.0)
    proc = subprocess.Popen([str(RELAY_EXE), "--listen", "127.0.0.1", "--no-discovery",
                             "--driver", f"127.0.0.1:{driver.getsockname()[1]}"],
                            stdout=subprocess.PIPE, stdin=subprocess.DEVNULL)
    try:
        port = int(proc.stdout.readline().decode().split(":")[-1])
        control = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        control.sendto(b"HELLO\n", ("127.0.0.1", port))
        pose = socket.create_connection(("127.0.0.1", 39580), timeout=2.0)
        cases = [({}, "left", dict(menu=True, x=False)), ({}, "left", dict(system=True, x=True)),
                 ({"left.menu": "menu"}, "left", dict(system=True)),
                 ({"right.a": "b", "right.b": "a"}, "right", dict(a=True, b=False)),
                 ({"left.y": "system"}, "left", dict(y=True, menu=False))]
        for overrides, hand, values in cases:
            t = bindings.table(overrides)
            control.sendto(b"BINDINGS\n" + bindings.wire(t).encode(), ("127.0.0.1", port))
            time.sleep(0.05)
            msgs = [hand_input(hand, **values)]
            pose.sendall(json.dumps(msgs, separators=(",", ":")).encode() + b"\n")
            got = json.loads(driver.recv(65536))
            bindings.apply(msgs, bindings.compiled(t))
            assert got == msgs
        pose.close()
    finally:
        proc.kill()
        proc.wait()
        driver.close()


def test_wheel_over_dropdown_scrolls_the_page():
    import tkinter as tk
    from tkinter import ttk
    from vhmd_client import link
    try:
        root = tk.Tk()
    except tk.TclError:
        pytest.skip("no display")
    try:
        plain = ttk.Combobox(root, values=["a", "b"], state="readonly")
        fixed = ttk.Combobox(root, values=["a", "b"], state="readonly")
        plain.current(0)
        fixed.current(0)
        plain.pack()
        fixed.pack()
        scrolled = []
        link.wheel_scrolls_page(fixed, scrolled.append)
        root.update()
        plain.event_generate("<MouseWheel>", delta=-120)
        fixed.event_generate("<MouseWheel>", delta=-120)
        root.update()
        assert plain.get() == "b"   # what ttk does by itself
        assert fixed.get() == "a" and len(scrolled) == 1
    finally:
        root.destroy()


def test_adb_reverse_only_over_usb():
    listing = ("List of devices attached\nPA92U0MGJ9041110G\tdevice\n192.168.0.15:5555\tdevice\n"
               "adb-PA92-x._adb-tls-connect._tcp\tdevice\nOTHER123\tunauthorized\n\n")
    assert headset_bridge.usb_serials(listing) == ["PA92U0MGJ9041110G"]
    assert headset_bridge.usb_serials("List of devices attached\n\n") == []


# -- switching SteamVR between VHMD and the original ---------------------------
@pytest.fixture
def steamvr(tmp_path, monkeypatch):
    pytest.importorskip("winreg")
    from vhmd_client import headset_info, mode
    settings = tmp_path / "steamvr.vrsettings"
    original = {"steamvr": {"enableHomeApp": False}, "driver_pico": {"other": 1},
                "vhmd_display": {"unrelated": 2}}
    settings.write_text(json.dumps(original), encoding="utf-8")
    config = tmp_path / "vhmd_config.json"
    config.write_text('{\n  // comment\n  "hmd": { "enabled": true },\n  "controllers": { "enabled": true }\n}\n',
                      encoding="utf-8")
    monkeypatch.setattr(mode, "settings_file", lambda: settings)
    monkeypatch.setattr(mode, "base_config", lambda: config)
    monkeypatch.setattr(mode, "PROFILE_DIR", tmp_path / "profiles")
    monkeypatch.setattr(mode, "ORIGINAL", tmp_path / "original_steamvr.json")
    monkeypatch.setattr(headset_info, "APPLIED", tmp_path / "headset_applied")
    return mode, settings, original


def read(p):
    return json.loads(p.read_text(encoding="utf-8"))


def test_mode_switch_and_back(steamvr):
    mode, settings, original = steamvr
    mode._apply_to_file(mode.wanted_settings("vhmd", headset=False))
    data = read(settings)
    assert data["driver_vhmd"]["enable"] is True and data["driver_pico"]["enable"] is False
    assert mode.current_mode() == "vhmd"
    assert "enabled\": false" not in (mode.PROFILE_DIR / "vhmd.json").read_text(encoding="utf-8")

    mode._apply_to_file(mode.wanted_settings("pico", headset=False))
    data = read(settings)
    assert "enable" not in data["driver_pico"]   # never set before: removed (on by default)
    assert mode.current_mode() == "pico"
    assert (mode.PROFILE_DIR / "pico.json").read_text(encoding="utf-8").count('"enabled": false') == 2

    mode._apply_to_file(mode.wanted_settings("off"))
    data = read(settings)
    assert mode.current_mode() == "off"
    expected = dict(original, driver_vhmd={"enable": False})
    assert data == expected   # everything else exactly as before VHMD


def test_mode_original_recorded_once(steamvr):
    mode, settings, original = steamvr
    rec = mode.original_settings()
    assert rec["driver_pico"]["enable"] is None and rec["driver_vhmd"] == {"enable": None, "configFile": None}
    mode._apply_to_file(mode.wanted_settings("vhmd", headset=False))
    assert mode.original_settings() == rec   # the record keeps the values from before the first switch
    assert read(mode.ORIGINAL)["inferred"] is False


def test_mode_original_inferred_after_earlier_switch(steamvr):
    mode, settings, original = steamvr
    mode.PROFILE_DIR.mkdir()
    (mode.PROFILE_DIR / "vhmd.json").write_text("{}", encoding="utf-8")   # switched before the record existed
    settings.write_text(json.dumps({"driver_pico": {"enable": False}}), encoding="utf-8")
    rec = mode.original_settings()
    assert rec["driver_pico"]["enable"] is None   # SteamVR's default (physical driver on)
    assert read(mode.ORIGINAL)["inferred"] is True
