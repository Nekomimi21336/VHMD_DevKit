#include "test_framework.h"

#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

#include "config/driver_config.h"
#include "config/roles.h"
#include "net/udp_socket.h"
#include "state/device_state.h"
#include "state/router.h"

using namespace vhmd;
using namespace std::chrono_literals;

TEST(roles_normalize_all_forms) {
    for (const char* s : {"waist", "vive_tracker_waist", "TrackerRole_Waist", "Waist"}) {
        auto r = NormalizeRole(s);
        CHECK(r && r->steamvr_role == "TrackerRole_Waist");
    }
    auto lf = NormalizeRole("vive_tracker_left_foot");
    CHECK(lf && lf->short_name == "left_foot");
    CHECK(!NormalizeRole("tail"));
}

TEST(config_defaults) {
    auto c = DefaultConfig();
    int hmd = 0, ctl = 0, trk = 0;
    for (const auto& d : c.devices) {
        hmd += d.kind == DeviceKind::Hmd;
        ctl += d.kind == DeviceKind::Controller;
        trk += d.kind == DeviceKind::Tracker;
    }
    CHECK(hmd == 1 && ctl == 2 && trk == 8);
    CHECK(c.bind_address == "127.0.0.1" && c.port == 39570);
}

TEST(config_shipped_file_parses_cleanly) {
    std::ifstream f(VHMD_SOURCE_DIR "/driver/resources/settings/vhmd_config.json");
    CHECK(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    auto r = LoadConfigFromJson(ss.str());
    CHECK(r.ok);
    for (const auto& w : r.warnings) std::printf("  warning: %s\n", w.c_str());
    CHECK(r.warnings.empty());
    CHECK(r.config.Find("tracker_7") != nullptr);
}

TEST(config_overrides_and_validation) {
    auto r = LoadConfigFromJson(R"({
        // comment allowed
        "network": {"bind": "0.0.0.0", "port": 40000},
        "timeout": {"ms": 250, "policy": "disconnect"},
        "hmd": {"enabled": false},
        "trackers": [
            {"role": "vive_tracker_waist"},
            {"id": "foot", "role": "TrackerRole_LeftFoot", "timeoutPolicy": "invalid"},
            {"id": "foot", "role": "bogus"}
        ]
    })");
    CHECK(r.ok);
    const auto& c = r.config;
    CHECK(c.port == 40000);
    CHECK(c.bind_address == "127.0.0.1"); // 0.0.0.0 without allowRemote is refused
    CHECK(c.timeout_policy == TimeoutPolicy::Disconnect);
    CHECK(!c.Find("hmd")->enabled);
    CHECK(c.Find("tracker_0")->role == "waist");
    CHECK(c.Find("foot")->timeout_policy == TimeoutPolicy::Invalid);
    int trackers = 0, enabled_foot = 0;
    for (const auto& d : c.devices) {
        trackers += d.kind == DeviceKind::Tracker;
        enabled_foot += d.id == "foot" && d.enabled;
    }
    CHECK(trackers == 3);
    CHECK(enabled_foot == 1); // duplicate id disabled
    CHECK(r.warnings.size() >= 3);
}

TEST(config_allow_remote) {
    auto r = LoadConfigFromJson(R"({"network": {"bind": "0.0.0.0", "allowRemote": true}})");
    CHECK(r.config.bind_address == "0.0.0.0");
}

TEST(config_garbage_falls_back) {
    auto r = LoadConfigFromJson("not json");
    CHECK(!r.ok);
    CHECK(r.config.devices.size() == 11);
}

TEST(config_tracker_limit) {
    std::string s = R"({"trackers":[)";
    for (int i = 0; i < 20; ++i) s += std::string(i ? "," : "") + "{}";
    s += "]}";
    auto r = LoadConfigFromJson(s);
    int trackers = 0;
    for (const auto& d : r.config.devices) trackers += d.kind == DeviceKind::Tracker;
    CHECK(trackers == 16);
}

namespace {
DeviceConfig TrackerCfg(TimeoutPolicy p) {
    DeviceConfig d;
    d.id = "tracker_0";
    d.serial = "S";
    d.default_pos = {0, 1, 0};
    d.timeout_policy = p;
    return d;
}
PoseMessage Pose(double x) {
    PoseMessage m;
    m.id = "tracker_0";
    m.pos = {x, 1, 0};
    m.vel = Vec3{1, 0, 0};
    return m;
}
} // namespace

TEST(state_timeout_policies) {
    DriverConfig g;
    g.timeout_ms = 100;
    const auto t0 = Clock::now();
    for (auto policy : {TimeoutPolicy::Hold, TimeoutPolicy::Invalid, TimeoutPolicy::Disconnect}) {
        DeviceState s(TrackerCfg(policy), g);
        s.SubmitPose(Pose(0.5), t0);
        auto fresh = s.ComputeOutput(t0 + 50ms);
        CHECK(fresh.pose_valid && fresh.connected && !fresh.timed_out);
        CHECK_NEAR(fresh.pos.x, 0.5, 1e-12);
        CHECK_NEAR(fresh.vel.x, 1.0, 1e-12);

        auto stale = s.ComputeOutput(t0 + 200ms);
        CHECK(stale.timed_out);
        CHECK_NEAR(stale.pos.x, 0.5, 1e-12); // last pose kept
        CHECK_NEAR(stale.vel.x, 0.0, 1e-12); // no extrapolation when stale
        CHECK(stale.pose_valid == (policy == TimeoutPolicy::Hold));
        CHECK(stale.connected == (policy != TimeoutPolicy::Disconnect));

        s.SubmitPose(Pose(0.7), t0 + 210ms); // recovers
        auto back = s.ComputeOutput(t0 + 220ms);
        CHECK(back.pose_valid && back.connected);
    }
}

TEST(state_explicit_invalid_and_inputs) {
    DriverConfig g;
    DeviceState s(TrackerCfg(TimeoutPolicy::Hold), g);
    const auto t0 = Clock::now();
    s.SubmitPose(Pose(0.3), t0);
    PoseMessage off;
    off.id = "tracker_0";
    off.valid = false;
    s.SubmitPose(off, t0 + 1ms);
    auto out = s.ComputeOutput(t0 + 2ms);
    CHECK(!out.pose_valid);
    CHECK_NEAR(out.pos.x, 0.3, 1e-12);

    InputMessage in;
    in.id = "tracker_0";
    in.values = {{"trigger", 0.5f}, {"a", 1.0f}};
    s.SubmitInput(in, t0);
    CHECK(s.TakeChangedInputs().size() == 2);
    CHECK(s.TakeChangedInputs().empty());
    // Repeated values are re-reported (aliases may share a component) but
    // coalesced: two packets before a tick yield one entry per name.
    s.SubmitInput(in, t0);
    in.values = {{"trigger", 0.7f}};
    s.SubmitInput(in, t0);
    auto again = s.TakeChangedInputs();
    CHECK(again.size() == 2);
    for (const auto& [name, value] : again) {
        if (name == "trigger") CHECK_NEAR(value, 0.7, 1e-6);
    }
}

TEST(router_dispatch_status_and_roles) {
    DriverConfig cfg = DefaultConfig();
    std::vector<std::shared_ptr<DeviceState>> states;
    for (const auto& d : cfg.devices) states.push_back(std::make_shared<DeviceState>(d, cfg));
    std::string last_role;
    RouterHooks hooks;
    hooks.set_role = [&](const std::string& id, const RoleInfo& r) {
        last_role = id + "=" + r.steamvr_role;
        return std::string();
    };
    MessageRouter router(cfg, states, hooks);
    const auto now = Clock::now();

    CHECK(!router.HandleDatagram(R"({"type":"pose","id":"waist","pos":[0,1.1,0],"rot":[1,0,0,0]})", now));
    CHECK(router.Find("waist") == router.Find("tracker_0"));
    CHECK_NEAR(router.Find("tracker_0")->ComputeOutput(now).pos.y, 1.1, 1e-12);

    router.HandleDatagram(R"({"type":"pose","id":"ghost","pos":[0,1,0],"rot":[1,0,0,0]})", now);
    router.HandleDatagram("garbage", now);
    CHECK(router.RejectedMessages() == 2);

    auto status = router.HandleDatagram(R"({"type":"config","op":"list","seq":3})", now);
    CHECK(status && status->find("\"status\"") != std::string::npos && status->find("tracker_7") != std::string::npos);
    CHECK(status->find("\"seq\":3") != std::string::npos);

    auto role = router.HandleDatagram(R"({"type":"config","op":"set_role","id":"tracker_1","role":"chest"})", now);
    CHECK(role && role->find("\"ok\":true") != std::string::npos);
    CHECK(last_role == "tracker_1=TrackerRole_Chest");

    auto bad = router.HandleDatagram(R"({"type":"config","op":"set_role","id":"hmd","role":"chest"})", now);
    CHECK(bad && bad->find("\"ok\":false") != std::string::npos);
}

TEST(router_osc_requires_opt_in) {
    DriverConfig cfg = DefaultConfig();
    std::vector<std::shared_ptr<DeviceState>> states;
    for (const auto& d : cfg.devices) states.push_back(std::make_shared<DeviceState>(d, cfg));
    MessageRouter router(cfg, states);
    router.HandleDatagram(std::string("/VMT/Reset\0\0\0", 12), Clock::now());
    CHECK(router.RejectedMessages() == 1);
}

TEST(udp_loopback_roundtrip) {
    UdpSocket server, client;
    std::string err;
    CHECK(server.Open("127.0.0.1", 0, err));
    CHECK(client.Open("127.0.0.1", 0, err));
    Endpoint to{0x7f000001u, server.LocalPort()};
    CHECK(client.SendTo("hello", to));
    char buf[64];
    Endpoint from;
    const int n = server.Receive(buf, sizeof(buf), from, 1000);
    CHECK(n == 5);
    CHECK(from.port == client.LocalPort());
    // Timeout path
    CHECK(server.Receive(buf, sizeof(buf), from, 10) == 0);
    // Binding the same port twice must fail cleanly.
    UdpSocket dup;
    CHECK(!dup.Open("127.0.0.1", server.LocalPort(), err));
    CHECK(!dup.Open("not-an-ip", 0, err));
}
