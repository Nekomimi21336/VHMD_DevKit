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

from vhmd_client import bindings, headset_bridge, native_relay, offsets, tracking  # noqa: E402


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


def test_body_tracking_state_logged_on_change():
    logs = []
    b = headset_bridge.Bridge(headset_bridge.BridgeOptions(), log=logs.append)
    for state in (0, 1, 1, 2, 2, 1):
        b._own_message({"type": "link_stats", "body": state})
    assert len(logs) == 3 and "tracked" in logs[1] and "waiting" in logs[2]
    assert headset_bridge.BridgeOptions().body_tracking is True


def test_wear_detect_is_on_unless_turned_off():
    assert headset_bridge.BridgeOptions().wear_detect is True


def test_mic_state():
    b = object.__new__(headset_bridge.Bridge)
    b.o = headset_bridge.BridgeOptions()
    b.stats = headset_bridge.Stats()
    b.mic_driver = {"state": "off"}
    b._mic_packets = (5, 5)
    assert b.o.mic is False and b.mic_state() == "off"      # off until asked for
    b.o.mic = True
    assert b.mic_state() == "waiting"
    b.stats.headset = {"mic": 1}
    assert b.mic_state() == "permission"
    b.stats.headset = {"mic": 2}
    b._mic_packets = (9, 5)
    assert b.mic_state() == "on"
    b.mic_driver = {"state": "no device"}
    assert b.mic_state() == "no_device"
    b.mic_driver = {"state": "device busy"}
    assert b.mic_state() == "busy"


def test_headset_mics():
    b = object.__new__(headset_bridge.Bridge)
    b.info = {}
    assert b.mics() == [] and b.mic_using() == ""            # an older headset app lists none
    b.info = {"mics": [{"key": "15:bottom", "type": 15, "name": "PICO 4 Ultra"}, {"type": 7}, "x"],
              "mic_using": "15:bottom"}
    assert [m["key"] for m in b.mics()] == ["15:bottom"]       # entries without a key are left out
    assert b.mic_using() == "15:bottom"


# -- Linux ---------------------------------------------------------------------
def test_data_dir_per_os(monkeypatch, tmp_path):
    from vhmd_client import paths
    monkeypatch.setattr(paths, "IS_WINDOWS", False)
    monkeypatch.setenv("XDG_STATE_HOME", str(tmp_path))
    assert paths.data_dir() == tmp_path / "vhmd"
    monkeypatch.delenv("XDG_STATE_HOME")
    assert paths.data_dir() == paths.Path.home() / ".local" / "state" / "vhmd"
    monkeypatch.setattr(paths, "IS_WINDOWS", True)
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path))
    assert paths.data_dir() == tmp_path / "VHMD"


def test_linux_mic_sink_made_once(monkeypatch):
    from vhmd_client import linux_audio
    loaded = []
    sinks, sources = ["1\tRDPSink"], ["1\tRDPSink.monitor"]

    def pactl(*args):
        if args[:3] == ("list", "short", "sinks"):
            return "\n".join(sinks)
        if args[:3] == ("list", "short", "sources"):
            return "\n".join(sources)
        loaded.append(args)
        name = next(a.partition("=")[2] for a in args if a.startswith(("sink_name=", "source_name=")))
        (sinks if args[1] == "module-null-sink" else sources).append(f"9\t{name}")
        return "27"

    monkeypatch.setattr(linux_audio.shutil, "which", lambda name: "/usr/bin/" + name)
    monkeypatch.setattr(linux_audio, "_pactl", pactl)
    assert linux_audio.ensure_mic_sink() == ""
    assert [a[1] for a in loaded] == ["module-null-sink", "module-remap-source"]
    assert "master=vhmd_mic.monitor" in loaded[1]
    assert all(" " not in arg for a in loaded for arg in a)   # PulseAudio 16 refuses quoted descriptions
    assert linux_audio.ensure_mic_sink() == "" and len(loaded) == 2      # there already: nothing loaded again
    monkeypatch.setattr(linux_audio.shutil, "which", lambda name: None)
    assert "pactl" in linux_audio.ensure_mic_sink()


def test_pick_encoder(monkeypatch):
    from vhmd_client import headset_stream
    assert headset_stream.pick_encoder("h264_nvenc") == "h264_nvenc"
    monkeypatch.setattr(headset_stream.sys, "platform", "win32")
    assert headset_stream.pick_encoder("auto") == "h264_amf"
    monkeypatch.setattr(headset_stream.sys, "platform", "linux")

    class Done:
        def __init__(self, out):
            self.stdout = out
    monkeypatch.setattr(headset_stream.subprocess, "run", lambda *a, **k: Done(" V....D h264_nvenc  NVIDIA\n"))
    assert headset_stream.pick_encoder("auto") == "h264_nvenc"
    monkeypatch.setattr(headset_stream.subprocess, "run", lambda *a, **k: Done(" V....D libx264  x264\n"))
    assert headset_stream.pick_encoder("auto") == "libx264"


# -- position and angle corrections ------------------------------------------
HAND = {"id": "left_hand", "kind": "controller", "index": 1}
FOOT = {"id": "tracker_1", "kind": "tracker", "index": 4, "role": "left_foot"}
HEAD = {"id": "hmd", "kind": "hmd", "index": 0}
UNUSED = {"id": "tracker_5", "kind": "tracker", "index": -1, "role": "left_elbow"}


def test_offset_space_and_limits():
    assert offsets.space("left_hand") == "device" and offsets.space("tracker_1") == "room"
    assert offsets.clean({"pos": [3, -3, 0.1], "ypr": [200, -200, 5]}) == {"pos": [2, -2, 0.1], "ypr": [180, -180, 5]}
    assert offsets.message("tracker_1", {"pos": [0, -0.03, 0], "ypr": [0, 0, 0]})["space"] == "room"


def test_offset_sent_again_after_steamvr_restarts():
    wanted = {"left_hand": {"pos": [0, 0, -0.02], "ypr": [0, -30, 0]}}
    fresh = [HEAD, HAND, FOOT]                         # the driver has none
    assert offsets.pending(fresh, wanted) == [{"id": "left_hand", "pos": [0, 0, -0.02], "ypr": [0, -30, 0],
                                               "space": "device"}]
    applied = [HEAD, {**HAND, "offset": {**wanted["left_hand"], "space": "device"}}, FOOT]
    assert offsets.pending(applied, wanted) == []
    assert [m["id"] for m in offsets.pending(applied, {})] == ["left_hand"]   # cleared here: cleared there


def test_offset_choices():
    assert offsets.choices([]) == [("left_hand", "left_hand"), ("right_hand", "right_hand")]
    assert offsets.choices([HEAD, HAND, FOOT, UNUSED])[2:] == [("tracker_1", "left_foot")]


def test_bridge_set_offset():
    sent = []
    b = object.__new__(headset_bridge.Bridge)
    b.o = headset_bridge.BridgeOptions()
    b._send_driver = lambda data: sent.append(json.loads(data))
    b.set_offset("tracker_1", {"pos": [0, -0.03, 0], "ypr": [0, 0, 10]})
    assert sent[-1] == {"type": "config", "op": "offset", "id": "tracker_1", "pos": [0, -0.03, 0], "ypr": [0, 0, 10],
                        "space": "room"}
    assert "tracker_1" in b.o.offsets
    b.set_offset("tracker_1", offsets.ZERO)
    assert sent[-1]["pos"] == [0, 0, 0] and b.o.offsets == {}


def test_every_device_and_field_named_in_the_app():
    from vhmd_client import link
    for texts in link.TEXT.values():
        for key in ("off", "off_hint", "off_device", "off_x", "off_y", "off_z", "off_yaw", "off_pitch", "off_roll",
                    "off_reset", "d_left_hand", "d_right_hand", "d_waist", "d_left_foot", "d_right_foot"):
            assert key in texts


def test_app_field_values_become_a_correction(monkeypatch):
    """What the PC app's fields do: centimeters and degrees to the driver's units,
    kept in the settings, nothing sent when nothing changed or on bad input."""
    from types import SimpleNamespace
    from vhmd_client import link

    class Var:
        def __init__(self, v):
            self.v = v

        def get(self):
            return self.v

        def set(self, v):
            self.v = v

    calls = []
    saved = {}
    bridge = SimpleNamespace(o=SimpleNamespace(offsets={}))

    def set_offset(device, value):
        calls.append((device, value))
        bridge.o.offsets = {device: value}

    bridge.set_offset = set_offset
    app = SimpleNamespace(off_vars=[Var(x) for x in ("1.5", "0", "-2", "0", "-30", "")], s={"offsets": {}},
                          bridge=bridge, log=lambda _m: None, offset_id=lambda: "left_hand")
    app.show_offset = lambda: None
    app.set = lambda k, v: (saved.update({k: v}), app.s.update({k: v}))

    link.LinkApp.offset_changed(app)
    assert calls == [("left_hand", {"pos": [0.015, 0.0, -0.02], "ypr": [0.0, -30.0, 0.0]})]
    assert saved["offsets"]["left_hand"]["ypr"][1] == -30.0
    link.LinkApp.offset_changed(app)
    assert len(calls) == 1                       # nothing changed
    app.off_vars[0].v = "abc"
    link.LinkApp.offset_changed(app)
    assert len(calls) == 1                       # not a number

    link.LinkApp.reset_offset(app)
    assert calls[-1] == ("left_hand", offsets.DEFAULTS["left_hand"])
    app.offset_id = lambda: "tracker_1"
    link.LinkApp.reset_offset(app)
    assert calls[-1] == ("tracker_1", offsets.ZERO)


def test_offset_defaults(tmp_path, monkeypatch):
    from vhmd_client import link

    monkeypatch.setattr(link, "SETTINGS", tmp_path / "link_settings.json")
    want = {"pos": [0.0, 0.0, -0.1], "ypr": [0.0, -10.0, 0.0]}
    assert link.load_settings()["offsets"] == {"left_hand": want, "right_hand": want}
    link.SETTINGS.write_text('{"offsets": {}}', encoding="utf-8")
    assert link.load_settings()["offsets"] == {}   # cleared by the user: stays cleared


def test_tracking_rows():
    devices = [{"id": "hmd", "kind": "hmd", "index": 0, "tracking": "active", "age_s": 0.01, "rate_hz": 71.6},
               {"id": "left_hand", "kind": "controller", "index": 1, "role": "left", "tracking": "paused",
                "age_s": 0.01, "rate_hz": 72},
               {"id": "tracker_0", "kind": "tracker", "index": 2, "role": "waist", "tracking": "disconnected",
                "age_s": 0.01, "rate_hz": 72},
               {"id": "tracker_1", "kind": "tracker", "index": 3, "role": "left_foot", "tracking": "active",
                "age_s": 4.0, "rate_hz": 0},
               {"id": "tracker_2", "kind": "tracker", "index": 4, "role": "right_foot", "age_s": None},
               {"id": "tracker_3", "kind": "tracker", "index": -1, "tracking": "active"}]
    rows = tracking.rows(devices)
    assert [(r["id"], r["name"], r["state"], r["rate"]) for r in rows] == [
        ("hmd", "hmd", "tracking", 72), ("left_hand", "left_hand", "paused", 72), ("tracker_0", "waist", "disconnected", 72),
        ("tracker_1", "left_foot", "silent", 0), ("tracker_2", "right_foot", "silent", 0)]   # not with SteamVR: left out
    assert rows[0]["actions"] == []                  # the HMD: the picture needs it
    assert rows[1]["actions"] == [("resume", "active"), ("disconnect", "disconnected")]
    assert rows[2]["actions"] == [("reconnect", "active")]
    assert rows[3]["actions"] == [("pause", "paused"), ("disconnect", "disconnected")]


def test_bridge_set_tracking():
    b = object.__new__(headset_bridge.Bridge)
    b.devices = [{"id": "tracker_1", "tracking": "active"}, {"id": "tracker_2", "tracking": "active"}]
    asked = []

    def request(op, **args):
        asked.append((op, args))
        return {"ok": True, "id": "tracker_1", "mode": "paused"}

    b.request = request
    assert b.set_tracking("left_foot", "paused") == ""   # a role also names the device
    assert asked == [("tracking", {"id": "left_foot", "mode": "paused"})]
    assert [d["tracking"] for d in b.devices] == ["paused", "active"]   # shown before the next status

    b.request = lambda *_a, **_k: {"ok": False, "error": "unknown op"}
    assert b.set_tracking("tracker_2", "paused") == "unknown op"
    assert b.devices[1]["tracking"] == "active"

    def timed_out(*_a, **_k):
        raise TimeoutError("no reply")

    b.request = timed_out
    assert b.set_tracking("tracker_2", "paused") == "no reply"


# -- switching SteamVR between VHMD and the original ---------------------------
@pytest.fixture
def steamvr(tmp_path, monkeypatch):
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


# -- the display window (IPD, field of view, render size while SteamVR runs) -------------
def _display_report(**now):
    startup = {"render": [1920, 1920], "ipd": 0.0718, "tan": [[-1.28, 1.28, 1.28, -1.28], [-1.28, 1.28, 1.28, -1.28]]}
    return {"startup": startup, "now": {**startup, **now}, "changed": bool(now), "live": True}


def test_display_controls_and_proposal():
    from vhmd_client import display_view as dv
    d = _display_report()
    s, now = d["startup"], d["now"]
    # The controls for the startup values, and nothing to send from there.
    assert dv.controls_from(s, now) == (71.8, 100, 100)
    assert dv.proposal(s, now, 71.8, 100, 100) == {}
    # Only what differs from now is named.
    assert dv.proposal(s, now, 66.0, 100, 100) == {"ipd": 0.066}
    fov = dv.proposal(s, now, 71.8, 90, 100)
    assert set(fov) == {"tan"} and fov["tan"][0] == [-1.152, 1.152, 1.152, -1.152] and fov["tan"][1] == fov["tan"][0]
    assert dv.proposal(s, now, 71.8, 100, 75) == {"render": [1440, 1440]}
    # Percentages are of the startup values, whatever the current ones are.
    changed = _display_report(render=[1440, 1440], ipd=0.066)
    assert dv.controls_from(changed["startup"], changed["now"]) == (66.0, 100, 75)
    assert dv.proposal(changed["startup"], changed["now"], 66.0, 100, 75) == {}
    assert dv.proposal(changed["startup"], changed["now"], 71.8, 100, 100) == {"ipd": 0.0718, "render": [1920, 1920]}
    # Sizes are multiples of 16 within the limits; the IPD stays within the control's range.
    assert dv.scaled_render({"render": [1832, 1920]}, 85) == [1552, 1632]
    assert dv.scaled_render({"render": [100, 8000]}, 50) == [64, 4000] and dv.scaled_render({"render": [8000, 8000]}, 150) == [8192, 8192]
    assert dv.proposal(s, now, 20.0, 100, 100) == {"ipd": 0.04}
    h, v = dv.fov_degrees([[-1.0, 1.0, 1.0, -1.0]] * 2)
    assert abs(h - 90.0) < 1e-6 and abs(v - 90.0) < 1e-6
    assert dv.describe(dv.TEXT["en"], {"ipd": 0.066, "render": [1440, 1440]}) == "IPD 66.0 mm, Render size 1440 x 1440"
    assert set(dv.TEXT["ja"]) == set(dv.TEXT["en"])


def test_display_window_applies_and_resets():
    tk = pytest.importorskip("tkinter")
    from vhmd_client import display_view as dv
    try:
        root = tk.Tk()
    except tk.TclError:
        pytest.skip("no display")
    root.withdraw()
    state = {"report": _display_report(), "sent": []}

    def request(op, **args):
        assert op == "display"
        if args:
            state["sent"].append(args)
            if args.get("reset"):
                state["report"] = _display_report()
            else:
                state["report"] = _display_report(**{**{k: v for k, v in state["report"]["now"].items()
                                                         if state["report"]["changed"]}, **args})
        return {"ok": True, "display": state["report"]}

    def pump(until, timeout=3.0):
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout and not until():
            root.update()
            time.sleep(0.02)
        return until()

    w = dv.DisplayWindow(root, request, "ja", own_window=False)   # in the hidden root: nothing shows on screen
    try:
        assert pump(lambda: w.cells[("ipd", "now")].cget("text") == "71.8 mm")
        assert w.cells[("size", "startup")].cget("text") == "1920 x 1920" and w.controls() == (71.8, 100, 100)
        assert "disabled" in w.apply_btn.state() and "disabled" in w.reset_btn.state()   # nothing to send, nothing to undo
        # Moving a control says what Apply would send, and Apply sends just that.
        w.vars["c_ipd"].set(66.0)
        w.vars["c_size"].set(76.0)   # rounds to 75 %
        assert pump(lambda: "disabled" not in w.apply_btn.state())
        assert "66.0 mm" in w.will_lbl.cget("text") and "1440 x 1440" in w.will_lbl.cget("text")
        w.apply()
        assert pump(lambda: w.cells[("ipd", "now")].cget("text") == "66.0 mm")
        assert state["sent"] == [{"ipd": 0.066, "render": [1440, 1440]}]
        assert w.cells[("ipd", "startup")].cget("text") == "71.8 mm"          # the startup column never moves
        assert pump(lambda: "disabled" not in w.reset_btn.state()) and "disabled" in w.apply_btn.state()
        # Back to the startup values with one button; the controls follow.
        w.reset()
        assert pump(lambda: w.cells[("size", "now")].cget("text") == "1920 x 1920")
        assert state["sent"][-1] == {"reset": True} and pump(lambda: w.controls() == (71.8, 100, 100))
    finally:
        w.close()   # destroys the root
