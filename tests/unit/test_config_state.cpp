#include "test_framework.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "config/driver_config.h"
#include "config/roles.h"
#include "net/udp_socket.h"
#include "state/device_state.h"
#include "state/mic_buffer.h"
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

TEST(config_trackers_on_demand_by_default) {
    auto c = DefaultConfig();
    const auto* hmd = c.Find("hmd");
    const auto* trk = c.Find("tracker_1");
    CHECK(hmd && !hmd->on_demand);
    CHECK(trk && trk->on_demand);
    CHECK(c.EffectivePolicy(*trk) == TimeoutPolicy::Disconnect);
    CHECK(c.EffectivePolicy(*hmd) == TimeoutPolicy::Hold);

    auto off = LoadConfigFromJson(R"({"trackersOnDemand": false, "trackers": [{"role": "waist"}, {"role": "chest", "onDemand": true, "timeoutPolicy": "hold"}]})");
    CHECK(!off.config.Find("tracker_0")->on_demand);
    CHECK(off.config.Find("tracker_1")->on_demand);
    CHECK(off.config.EffectivePolicy(*off.config.Find("tracker_1")) == TimeoutPolicy::Hold);
}

TEST(router_requests_on_demand_registration) {
    DriverConfig cfg = DefaultConfig();
    std::vector<std::shared_ptr<DeviceState>> states;
    for (const auto& d : cfg.devices) states.push_back(std::make_shared<DeviceState>(d, cfg));
    std::vector<std::string> requested;
    RouterHooks hooks;
    hooks.request_add = [&](const std::string& id) { requested.push_back(id); };
    MessageRouter router(cfg, states, hooks);
    const auto now = Clock::now();

    router.HandleDatagram(R"({"type":"pose","id":"hmd","pos":[0,1.7,0],"rot":[1,0,0,0]})", now);
    CHECK(requested.empty()); // base devices are always registered
    router.HandleDatagram(R"({"type":"pose","id":"left_foot","pos":[0,0.1,0],"rot":[1,0,0,0]})", now);
    CHECK(requested.size() == 1 && requested[0] == "tracker_1");
    CHECK_NEAR(router.Find("tracker_1")->ComputeOutput(now).pos.y, 0.1, 1e-12); // buffered before add
    router.HandleDatagram(R"({"type":"pose","id":"tracker_2","valid":false})", now);
    CHECK(requested.size() == 1); // an invalidation does not create a device

    auto reply = router.HandleDatagram(R"({"type":"config","op":"add","id":"waist"})", now);
    CHECK(reply && reply->find("\"ok\":true") != std::string::npos);
    CHECK(requested.back() == "tracker_0");
    auto all = router.HandleDatagram(R"({"type":"config","op":"add"})", now);
    CHECK(all && all->find("tracker_7") != std::string::npos);
    auto bad = router.HandleDatagram(R"({"type":"config","op":"add","id":"nope"})", now);
    CHECK(bad && bad->find("\"ok\":false") != std::string::npos);
}

TEST(config_display_and_preview) {
    auto d = DefaultConfig();
    CHECK(d.display_mode == "auto" && d.preview.enabled && d.preview.fps == 15.0);
    auto r = LoadConfigFromJson(R"({"display":{"mode":"direct"},"preview":{"enabled":false,"path":"/tmp/x","fps":500,"maxEyeWidth":4,"vulkanDevice":1}})");
    CHECK(r.config.display_mode == "direct");
    CHECK(!r.config.preview.enabled && r.config.preview.path == "/tmp/x");
    CHECK(r.config.preview.fps == 120.0);          // clamped
    CHECK(r.config.preview.max_eye_width == 16);   // clamped
    CHECK(r.config.preview.vulkan_device == 1);
    auto bad = LoadConfigFromJson(R"({"display":{"mode":"hologram"}})");
    CHECK(bad.config.display_mode == "auto" && !bad.warnings.empty());
}

TEST(config_hand_mode_and_skeleton_level) {
    auto d = DefaultConfig();
    CHECK(d.hand_mode == "controllers");
    CHECK(!d.Find("left_hand")->skeleton_full); // controllers default to a Partial skeleton
    auto r = LoadConfigFromJson(R"({"controllers":{"handMode":"hands","skeletonLevel":"full"}})");
    CHECK(r.config.hand_mode == "hands" && r.warnings.empty());
    CHECK(r.config.Find("right_hand")->skeleton_full);
    auto bad = LoadConfigFromJson(R"({"controllers":{"handMode":"feet"}})");
    CHECK(bad.config.hand_mode == "controllers" && !bad.warnings.empty());
}

TEST(router_hand_mode_op) {
    DriverConfig cfg = DefaultConfig();
    std::vector<std::shared_ptr<DeviceState>> states;
    for (const auto& d : cfg.devices) states.push_back(std::make_shared<DeviceState>(d, cfg));
    const auto now = Clock::now();
    {
        MessageRouter router(cfg, states); // no hook: reported as unavailable
        auto j = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"hand_mode","mode":"hands"})", now));
        CHECK(j["ok"] == false);
    }
    std::string mode = "controllers";
    RouterHooks hooks;
    hooks.set_hand_mode = [&](const std::string& m) -> std::string {
        if (m != "controllers" && m != "hands") return "bad mode";
        mode = m;
        return "";
    };
    hooks.hand_mode = [&] { return mode; };
    MessageRouter router(cfg, states, hooks);
    auto set = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"hand_mode","mode":"hands","seq":3})", now));
    CHECK(set["ok"] == true && set["mode"] == "hands" && mode == "hands");
    auto query = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"hand_mode"})", now));
    CHECK(query["ok"] == true && query["mode"] == "hands");
    auto bad = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"hand_mode","mode":"feet"})", now));
    CHECK(bad["ok"] == false && bad["mode"] == "hands");
    auto status = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"list"})", now));
    CHECK(status["hand_mode"] == "hands");
}

TEST(router_inspect_op_and_who_drives) {
    DriverConfig cfg = DefaultConfig();
    std::vector<std::shared_ptr<DeviceState>> states;
    for (const auto& d : cfg.devices) states.push_back(std::make_shared<DeviceState>(d, cfg));
    std::string asked;
    RouterHooks hooks;
    hooks.inspect = [&](const std::string& id) {
        asked = id;
        return std::string(R"([{"serial":"S","given":{"trigger":0.5}}])");
    };
    MessageRouter router(cfg, states, hooks);
    const auto now = Clock::now();
    bool drives = true;
    auto r = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"inspect","id":"waist","seq":4})", now, &drives));
    CHECK(r["ok"] == true && r["id"] == "tracker_0" && r["kind"] == "tracker" && asked == "tracker_0");
    CHECK(r["devices"][0]["given"]["trigger"] == 0.5);
    CHECK(!drives);   // asking does not drive anything (haptics stay with the pose sender)
    auto bad = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"inspect","id":"nope"})", now));
    CHECK(bad["ok"] == false);
    router.HandleDatagram(R"({"type":"input","id":"left_hand","values":{"trigger":1}})", now, &drives);
    CHECK(drives);
    router.HandleDatagram(R"({"type":"config","op":"list"})", now, &drives);
    CHECK(!drives);
    MessageRouter none(cfg, states);
    CHECK(nlohmann::json::parse(*none.HandleDatagram(R"({"type":"config","op":"inspect","id":"hmd"})", now))["ok"] == false);
}

TEST(router_records_what_drives_devices) {
    DriverConfig cfg = DefaultConfig();
    std::vector<std::shared_ptr<DeviceState>> states;
    for (const auto& d : cfg.devices) states.push_back(std::make_shared<DeviceState>(d, cfg));
    const auto dir = std::filesystem::temp_directory_path() /
                     ("vhmd_rec_test_" + std::to_string(Clock::now().time_since_epoch().count()));
    RouterHooks hooks;
    hooks.recordings_dir = dir;
    MessageRouter router(cfg, states, hooks);
    const auto t0 = Clock::now();
    auto bad = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"record","mode":"start","name":"../x"})", t0));
    CHECK(bad["ok"] == false);   // no paths, only names
    auto start = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"record","mode":"start","name":"walk"})", t0));
    CHECK(start["ok"] == true && start["record"]["recording"] == true);
    router.HandleDatagram("[{\"type\":\"pose\",\"id\":\"hmd\",\"pos\":[0,1.6,0],\"rot\":[1,0,0,0]},\n"
                          "{\"type\":\"input\",\"id\":\"left_hand\",\"values\":{\"trigger\":1}}]", t0 + 100ms);
    router.HandleDatagram(R"({"type":"config","op":"list"})", t0 + 150ms);                       // asks, not recorded
    router.HandleDatagram(R"({"type":"mic","seq":1,"pcm":""})", t0 + 160ms);                     // sound, not recorded
    router.HandleDatagram(R"({"type":"pose","id":"nobody","pos":[0,0,0],"rot":[1,0,0,0]})", t0 + 170ms);   // rejected
    router.HandleDatagram(R"({"type":"pose","id":"left_foot","pos":[0,0.1,0],"rot":[1,0,0,0]})", t0 + 250ms);
    auto again = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"record","mode":"start"})", t0));
    CHECK(again["ok"] == false);   // one at a time
    auto stop = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"record","mode":"stop"})", t0 + 300ms));
    CHECK(stop["ok"] == true && stop["record"]["recording"] == false && stop["record"]["lines"] == 2);
    std::ifstream f(dir / "walk.jsonl");
    std::string line1, line2, extra;
    std::getline(f, line1);
    std::getline(f, line2);
    CHECK(!std::getline(f, extra));
    f.close();   // Windows: an open file keeps its folder from being removed
    const auto j1 = nlohmann::json::parse(line1), j2 = nlohmann::json::parse(line2);
    CHECK_NEAR(j1["t"].get<double>(), 0.1, 1e-6);
    CHECK(j1["d"].is_array() && j1["d"][1]["values"]["trigger"] == 1);
    CHECK_NEAR(j2["t"].get<double>(), 0.25, 1e-6);
    CHECK(j2["d"]["id"] == "left_foot");
    auto exists = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"record","mode":"start","name":"walk"})", t0));
    CHECK(exists["ok"] == false);   // never overwrites
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(config_preview_auto_start_and_op) {
    CHECK(!DefaultConfig().preview.auto_start);
    CHECK(LoadConfigFromJson(R"({"preview":{"autoStart":true}})").config.preview.auto_start);
    DriverConfig cfg = DefaultConfig();
    std::vector<std::shared_ptr<DeviceState>> states;
    for (const auto& d : cfg.devices) states.push_back(std::make_shared<DeviceState>(d, cfg));
    bool on = false;
    RouterHooks hooks;
    hooks.set_preview = [&](bool v) -> std::string { on = v; return ""; };
    MessageRouter router(cfg, states, hooks);
    const auto now = Clock::now();
    auto r = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"preview","mode":"on"})", now));
    CHECK(r["ok"] == true && on);
    auto bad = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"preview","mode":"maybe"})", now));
    CHECK(bad["ok"] == false && on);
    MessageRouter none(cfg, states);
    auto na = nlohmann::json::parse(*none.HandleDatagram(R"({"type":"config","op":"preview","mode":"on"})", now));
    CHECK(na["ok"] == false);
}

TEST(config_direct_mode_window_and_refresh) {
    auto d = DefaultConfig();
    CHECK(d.display_refresh_hz == 0.0 && !d.preview_window && d.preview.window_fps == 30.0);
    auto r = LoadConfigFromJson(R"({"display":{"mode":"direct","refreshHz":30,"previewWindow":true},"preview":{"windowFps":10}})");
    CHECK(r.config.display_mode == "direct" && r.config.display_refresh_hz == 30.0 && r.config.preview_window);
    CHECK(r.config.preview.window_fps == 10.0 && r.warnings.empty());
    CHECK(LoadConfigFromJson(R"({"display":{"refreshHz":1000}})").config.display_refresh_hz == 240.0);
    DriverConfig cfg = DefaultConfig();
    std::vector<std::shared_ptr<DeviceState>> states;
    for (const auto& dc : cfg.devices) states.push_back(std::make_shared<DeviceState>(dc, cfg));
    bool shown = false;
    RouterHooks hooks;
    hooks.set_preview_window = [&](bool v) -> std::string { shown = v; return ""; };
    MessageRouter router(cfg, states, hooks);
    auto j = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"preview_window","mode":"on"})", Clock::now()));
    CHECK(j["ok"] == true && shown);
}

TEST(router_refresh_rate_op) {
    auto warn = LoadConfigFromJson(R"({"display":{"refreshHz":500}})");
    CHECK(warn.config.display_refresh_hz == kMaxRefreshHz && !warn.warnings.empty());
    DriverConfig cfg = DefaultConfig();
    std::vector<std::shared_ptr<DeviceState>> states;
    for (const auto& dc : cfg.devices) states.push_back(std::make_shared<DeviceState>(dc, cfg));
    double hz = 90.0;
    RouterHooks hooks;
    hooks.set_refresh_rate = [&](double v) -> std::string { hz = v; return ""; };
    hooks.refresh_rate = [&] { return hz; };
    MessageRouter router(cfg, states, hooks);
    const auto now = Clock::now();
    auto set = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"refresh_rate","hz":30})", now));
    CHECK(set["ok"] == true && set["hz"] == 30.0 && hz == 30.0);
    auto query = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"refresh_rate"})", now));
    CHECK(query["ok"] == true && query["hz"] == 30.0);
    auto bad = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"refresh_rate","hz":0.5})", now));
    CHECK(bad["ok"] == false && hz == 30.0);
    auto bad2 = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"refresh_rate","hz":1000})", now));
    CHECK(bad2["ok"] == false);
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
    // Processes vrserver starts must not inherit the port (it would outlive a crashed vrserver).
    CHECK(!server.IsInheritable());
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

TEST(config_hmd_proximity_and_play_area) {
    auto d = DefaultConfig();
    for (const auto& dev : d.devices) {
        if (dev.kind == DeviceKind::Hmd) CHECK(dev.proximity && dev.play_area_width == 2.0 && dev.play_area_depth == 2.0);
    }
    auto r = LoadConfigFromJson(R"({"hmd":{"proximity":false,"playArea":[3.5,2.5]}})");
    for (const auto& dev : r.config.devices) {
        if (dev.kind == DeviceKind::Hmd) CHECK(!dev.proximity && dev.play_area_width == 3.5 && dev.play_area_depth == 2.5);
    }
    auto bad = LoadConfigFromJson(R"({"hmd":{"playArea":[0.1,500]}})");
    CHECK(!bad.warnings.empty());
}

TEST(state_pose_age_becomes_time_offset) {
    auto c = DefaultConfig();
    DeviceConfig cfg;
    for (const auto& dev : c.devices) {
        if (dev.id == "tracker_0") cfg = dev;
    }
    DeviceState s(cfg, c);
    PoseMessage m;
    m.id = "tracker_0";
    m.pos = {0, 1, 0};
    m.age = 0.05;
    m.acc = Vec3{0, -1, 0};
    const auto t0 = Clock::now();
    s.SubmitPose(m, t0);
    auto out = s.ComputeOutput(t0 + std::chrono::milliseconds(10));
    CHECK_NEAR(out.time_offset, -0.06, 1e-6);
    CHECK_NEAR(out.acc.y, -1.0, 1e-12);
}

TEST(state_parented_pose_does_not_time_out) {
    auto c = DefaultConfig();
    DeviceConfig cfg;
    for (const auto& dev : c.devices) {
        if (dev.id == "tracker_2") cfg = dev;
    }
    DeviceState s(cfg, c);
    PoseMessage m;
    m.id = "tracker_2";
    m.parent = "HMD";
    const auto t0 = Clock::now();
    s.SubmitPose(m, t0);
    auto out = s.ComputeOutput(t0 + std::chrono::seconds(10));
    CHECK(!out.timed_out && out.pose_valid && out.parent == "HMD");
    PoseMessage plain;
    plain.id = "tracker_2";
    s.SubmitPose(plain, t0);
    CHECK(s.ComputeOutput(t0 + std::chrono::seconds(10)).timed_out);
}

TEST(router_control_ownership) {
    DriverConfig cfg = DefaultConfig();
    std::vector<std::shared_ptr<DeviceState>> states;
    for (const auto& d : cfg.devices) states.push_back(std::make_shared<DeviceState>(d, cfg));
    MessageRouter router(cfg, states);
    const auto now = Clock::now();
    auto hmd_x = [&] { return router.Find("hmd")->ComputeOutput(now).pos.x; };

    // Nobody owns the devices: every sender is accepted, named or not.
    router.HandleDatagram(R"({"type":"pose","id":"hmd","pos":[1,1.7,0],"rot":[1,0,0,0],"src":"manual"})", now);
    CHECK_NEAR(hmd_x(), 1.0, 1e-9);
    router.HandleDatagram(R"({"type":"pose","id":"hmd","pos":[2,1.7,0],"rot":[1,0,0,0]})", now);
    CHECK_NEAR(hmd_x(), 2.0, 1e-9);

    // Manual holds the trigger, then control goes to "auto".
    router.HandleDatagram(R"({"type":"input","id":"right_hand","values":{"trigger":1,"a":1},"src":"manual"})", now);
    router.Find("right_hand")->TakeChangedInputs();
    auto r = router.HandleDatagram(R"({"type":"config","op":"control","source":"auto","seq":1})", now);
    CHECK(r && r->find("\"owner\":\"auto\"") != std::string::npos);
    CHECK(r->find("\"poses\"") != std::string::npos && r->find("\"hmd\"") != std::string::npos);
    // The hand-over released what manual was pressing.
    auto released = router.Find("right_hand")->TakeChangedInputs();
    CHECK(released.size() == 2);
    for (const auto& [name, value] : released) CHECK(value == 0.0f);

    // Only "auto" moves the devices now.
    router.HandleDatagram(R"({"type":"pose","id":"hmd","pos":[3,1.7,0],"rot":[1,0,0,0],"src":"manual"})", now);
    router.HandleDatagram(R"({"type":"pose","id":"hmd","pos":[4,1.7,0],"rot":[1,0,0,0]})", now);
    CHECK_NEAR(hmd_x(), 2.0, 1e-9);
    router.HandleDatagram(R"({"type":"pose","id":"hmd","pos":[5,1.7,0],"rot":[1,0,0,0],"src":"auto"})", now);
    CHECK_NEAR(hmd_x(), 5.0, 1e-9);

    // Per device: the head back to manual, the hands stay with auto.
    router.HandleDatagram(R"({"type":"config","op":"control","source":"manual","devices":["hmd"]})", now);
    router.HandleDatagram(R"({"type":"pose","id":"hmd","pos":[6,1.7,0],"rot":[1,0,0,0],"src":"manual"})", now);
    CHECK_NEAR(hmd_x(), 6.0, 1e-9);
    router.HandleDatagram(R"({"type":"pose","id":"left_hand","pos":[7,1,0],"rot":[1,0,0,0],"src":"manual"})", now);
    CHECK(router.Find("left_hand")->ComputeOutput(now).pos.x != 7.0);

    // Status reports the owners and who has been sending.
    const std::string st = router.StatusJson(now, std::nullopt);
    CHECK(st.find("\"control\"") != std::string::npos && st.find("\"ignored\"") != std::string::npos);

    // "" lets anyone drive again.
    router.HandleDatagram(R"({"type":"config","op":"control","source":""})", now);
    router.HandleDatagram(R"({"type":"pose","id":"left_hand","pos":[8,1,0],"rot":[1,0,0,0]})", now);
    CHECK_NEAR(router.Find("left_hand")->ComputeOutput(now).pos.x, 8.0, 1e-9);
}

TEST(offset_moves_along_the_device_or_the_room) {
    const double half = std::sqrt(0.5);
    OutputPose out;
    out.pos = {1, 2, 3};
    out.rot = {half, 0, half, 0};   // 90 degrees around +Y: the device's -Z points to -X
    out.angvel = {0, 2, 0};
    DriverConfig cfg = DefaultConfig();
    DeviceState state(cfg.devices[1], cfg);

    state.SetOffset({0, 0, -0.5}, {}, false);
    OutputPose a = out;
    ApplyOffset(a, state.Offset());
    CHECK(std::abs(a.pos.x - 0.5) < 1e-9 && std::abs(a.pos.y - 2) < 1e-9 && std::abs(a.pos.z - 3) < 1e-9);
    CHECK(std::abs(a.vel.z - 1.0) < 1e-9);   // swings around +Y: omega x arm
    CHECK(std::abs(a.rot.w - out.rot.w) < 1e-9 && std::abs(a.rot.y - out.rot.y) < 1e-9);

    state.SetOffset({0, 0, -0.5}, {}, true);
    OutputPose b = out;
    ApplyOffset(b, state.Offset());
    CHECK(std::abs(b.pos.x - 1) < 1e-9 && std::abs(b.pos.z - 2.5) < 1e-9);
    CHECK(b.vel.x == 0 && b.vel.y == 0 && b.vel.z == 0);

    state.SetOffset({}, {90, 0, 0}, false);   // yaw on top of the device's rotation: 180 degrees in all
    OutputPose c = out;
    ApplyOffset(c, state.Offset());
    CHECK(std::abs(c.rot.w) < 1e-9 && std::abs(std::abs(c.rot.y) - 1) < 1e-9);
    CHECK(std::abs(c.pos.x - 1) < 1e-9);

    state.SetOffset({}, {}, false);
    CHECK(state.Offset().IsZero());
    OutputPose d = out;
    ApplyOffset(d, state.Offset());
    CHECK(d.rot.w == out.rot.w && d.pos.x == out.pos.x);
}

TEST(router_offset_op) {
    DriverConfig cfg = DefaultConfig();
    std::vector<std::shared_ptr<DeviceState>> states;
    for (const auto& d : cfg.devices) states.push_back(std::make_shared<DeviceState>(d, cfg));
    MessageRouter router(cfg, states);
    const auto now = Clock::now();
    auto ask = [&](const char* text) { return nlohmann::json::parse(*router.HandleDatagram(text, now)); };

    auto set = ask(R"({"type":"config","op":"offset","id":"left_hand","pos":[0.01,0,-0.02],"ypr":[0,-30,0],"space":"device"})");
    CHECK(set["ok"].get<bool>());
    CHECK(set["offset"]["ypr"][1].get<double>() == -30.0);
    auto only_angle = ask(R"({"type":"config","op":"offset","id":"left_hand","ypr":[5,0,0]})");
    CHECK(only_angle["offset"]["pos"][2].get<double>() == -0.02);   // the rest stays
    CHECK(only_angle["offset"]["space"] == "device");
    CHECK(ask(R"({"type":"config","op":"offset","id":"left_hand"})")["offset"]["ypr"][0].get<double>() == 5.0);

    auto foot = ask(R"({"type":"config","op":"offset","id":"left_foot","pos":[0,-0.03,0],"space":"room"})");
    CHECK(foot["ok"].get<bool>() && foot["offset"]["space"] == "room");

    CHECK(!ask(R"({"type":"config","op":"offset","id":"hmd","pos":[0,1,0]})")["ok"].get<bool>());
    CHECK(!ask(R"({"type":"config","op":"offset","id":"nope","pos":[0,1,0]})")["ok"].get<bool>());
    CHECK(!ask(R"({"type":"config","op":"offset","id":"left_hand","pos":[0,3,0]})")["ok"].get<bool>());
    CHECK(!ask(R"({"type":"config","op":"offset","id":"left_hand","space":"moon"})")["ok"].get<bool>());

    auto list = ask(R"({"type":"config","op":"list"})");
    int with_offset = 0;
    for (const auto& d : list["devices"]) with_offset += d.contains("offset") ? 1 : 0;
    CHECK(with_offset == 2);
}

TEST(tracking_paused_holds_the_pose_and_disconnected_reports_off) {
    DriverConfig cfg = DefaultConfig();
    cfg.smoothing = 0.0;
    DeviceConfig dev = cfg.devices[1];
    dev.timeout_policy = TimeoutPolicy::Disconnect;
    DeviceState s(dev, cfg);
    auto now = Clock::now();
    auto pose = [&](double x) {
        PoseMessage m;
        m.id = dev.id;
        m.pos = {x, 1, 0};
        m.vel = Vec3{1, 0, 0};
        s.SubmitPose(m, now);
    };
    pose(0.1);
    s.ComputeOutput(now);

    s.SetTracking(TrackingMode::Paused, now);
    pose(0.5);
    InputMessage press;
    press.id = dev.id;
    press.values = {{"a", 1.0f}};
    s.SubmitInput(press, now);
    OutputPose held = s.ComputeOutput(now);
    CHECK_NEAR(held.pos.x, 0.1, 1e-9);
    CHECK_NEAR(held.vel.x, 0.0, 1e-9);                // no drift from the last speed
    CHECK(held.pose_valid && held.connected);
    CHECK(s.TakeChangedInputs().size() == 1);         // only the motion stops
    CHECK(s.Stats(now).pose_packets == 2);            // the sender still shows
    now += std::chrono::seconds(30);
    held = s.ComputeOutput(now);
    CHECK(held.pose_valid && held.connected);         // no timeout while held

    s.SetTracking(TrackingMode::Disconnected, now);
    auto released = s.TakeChangedInputs();
    CHECK(released.size() == 1 && released[0].second == 0.0f);   // nothing stays pressed
    pose(0.6);
    s.SubmitInput(press, now);
    OutputPose off = s.ComputeOutput(now);
    CHECK(!off.pose_valid && !off.connected);
    CHECK(s.TakeChangedInputs().empty());

    s.SetTracking(TrackingMode::Active, now);
    pose(0.7);
    OutputPose back = s.ComputeOutput(now);
    CHECK(back.pose_valid && back.connected);
    CHECK_NEAR(back.pos.x, 0.7, 1e-9);
}

TEST(mic_buffer_waits_then_plays_and_catches_up) {
    MicBuffer b(4, 8);
    const int16_t two[2] = {1, 2}, ten[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    int16_t out[4];

    b.Push(two, 2, 0);
    CHECK(b.Pull(out, 4) == 0);                 // not enough yet: silence
    CHECK(out[0] == 0 && out[3] == 0);
    b.Push(two, 2, 1);
    CHECK(b.Pull(out, 3) == 3);                 // plays from here
    CHECK(out[0] == 1 && out[1] == 2 && out[2] == 1);
    CHECK(b.Pull(out, 4) == 1);                 // ran empty: the rest is silence
    CHECK(out[0] == 2 && out[1] == 0);
    CHECK(b.GetStats().underruns == 1);

    b.Push(ten, 10, 4);                         // packets 2 and 3 never came; over the limit
    CHECK(b.GetStats().lost == 2);
    CHECK(b.GetStats().skipped == 6);
    CHECK(b.Pull(out, 4) == 4);
    CHECK(out[0] == 7 && out[3] == 10);         // only the newest stays
}

TEST(router_mic) {
    DriverConfig cfg = DefaultConfig();
    std::vector<std::shared_ptr<DeviceState>> states;
    for (const auto& d : cfg.devices) states.push_back(std::make_shared<DeviceState>(d, cfg));
    size_t samples = 0;
    std::string device = "unset";
    RouterHooks hooks;
    hooks.mic = [&](const MicMessage& m) { samples += m.pcm.size(); };
    hooks.set_mic = [&](const std::string& d) { device = d; };
    bool monitor = false;
    hooks.set_mic_monitor = [&](bool on) { monitor = on; };
    hooks.mic_status = [&]() { return nlohmann::json{{"device", device}}.dump(); };
    MessageRouter router(cfg, states, hooks);
    const auto now = Clock::now();
    auto ask = [&](const char* text) { return nlohmann::json::parse(*router.HandleDatagram(text, now)); };

    CHECK(!router.HandleDatagram(R"([{"type":"mic","seq":0,"pcm":"AQD//wAB"}])", now));
    CHECK(samples == 3);
    CHECK(ask(R"({"type":"config","op":"mic"})")["mic"]["device"] == "unset");   // a query changes nothing
    auto set = ask(R"({"type":"config","op":"mic","device":"CABLE Input"})");
    CHECK(set["ok"].get<bool>() && set["mic"]["device"] == "CABLE Input");
    CHECK(device == "CABLE Input");
    CHECK(!monitor);
    ask(R"({"type":"config","op":"mic","mode":"monitor"})");
    CHECK(monitor && device == "CABLE Input");
    ask(R"({"type":"config","op":"mic","mode":"quiet"})");
    CHECK(!monitor);
    CHECK(ask(R"({"type":"config","op":"list"})")["mic"]["device"] == "CABLE Input");
    ask(R"({"type":"config","op":"mic","device":""})");
    CHECK(device.empty());

    MessageRouter no_mic(cfg, states);
    CHECK(!nlohmann::json::parse(*no_mic.HandleDatagram(R"({"type":"config","op":"mic"})", now))["ok"].get<bool>());
}

TEST(router_tracking_op) {
    DriverConfig cfg = DefaultConfig();
    std::vector<std::shared_ptr<DeviceState>> states;
    for (const auto& d : cfg.devices) states.push_back(std::make_shared<DeviceState>(d, cfg));
    MessageRouter router(cfg, states);
    const auto now = Clock::now();
    auto ask = [&](const char* text) { return nlohmann::json::parse(*router.HandleDatagram(text, now)); };

    auto set = ask(R"({"type":"config","op":"tracking","id":"left_hand","mode":"paused"})");
    CHECK(set["ok"].get<bool>() && set["id"] == "left_hand" && set["mode"] == "paused");
    CHECK(ask(R"({"type":"config","op":"tracking","id":"left_hand"})")["mode"] == "paused");
    CHECK(!ask(R"({"type":"config","op":"tracking","id":"hmd","mode":"paused"})")["ok"].get<bool>());
    CHECK(!ask(R"({"type":"config","op":"tracking","id":"nope","mode":"paused"})")["ok"].get<bool>());
    CHECK(!ask(R"({"type":"config","op":"tracking","id":"left_hand","mode":"nap"})")["ok"].get<bool>());

    for (const auto& d : ask(R"({"type":"config","op":"list"})")["devices"]) {
        CHECK(d["tracking"] == (d["id"] == "left_hand" ? "paused" : "active"));
    }
}

TEST(router_render_models_op) {
    DriverConfig cfg = DefaultConfig();
    CHECK(cfg.render_models == "labeled");
    CHECK(LoadConfigFromJson(R"({"renderModels":"axes"})").config.render_models == "axes");
    auto bad_cfg = LoadConfigFromJson(R"({"renderModels":"cubes"})");
    CHECK(bad_cfg.config.render_models == "labeled" && !bad_cfg.warnings.empty());
    std::vector<std::shared_ptr<DeviceState>> states;
    for (const auto& d : cfg.devices) states.push_back(std::make_shared<DeviceState>(d, cfg));
    const auto now = Clock::now();
    std::string mode = "labeled";
    RouterHooks hooks;
    hooks.set_render_models = [&](const std::string& m) -> std::string {
        mode = m;
        return "";
    };
    hooks.render_models = [&] { return mode; };
    MessageRouter router(cfg, states, hooks);
    auto set = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"render_models","mode":"axes"})", now));
    CHECK(set["ok"] == true && set["mode"] == "axes" && mode == "axes");
    auto bad = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"render_models","mode":"cubes"})", now));
    CHECK(bad["ok"] == false && bad["mode"] == "axes");
}

TEST(router_display_sync_op) {
    DriverConfig cfg = DefaultConfig();
    std::vector<std::shared_ptr<DeviceState>> states;
    for (const auto& d : cfg.devices) states.push_back(std::make_shared<DeviceState>(d, cfg));
    const auto now = Clock::now();
    double got_slack = 0, got_target = 0, got_weight = 0;
    RouterHooks hooks;
    hooks.display_sync = [&](double slack, double period, double target, double weight, double* shift) -> std::string {
        got_slack = slack;
        got_target = target;
        got_weight = weight;
        *shift = 1.25;
        return period > 0 ? "" : "bad period";
    };
    MessageRouter router(cfg, states, hooks);
    auto r = nlohmann::json::parse(*router.HandleDatagram(
        R"({"type":"config","op":"display_sync","slack_ms":3.5,"period_ms":10.0,"weight":0.5})", now));
    CHECK(r["ok"] == true && r["shift_ms"] == 1.25);
    CHECK(got_slack == 3.5 && got_target == 4.0 && got_weight == 0.5);   // target defaults to 0.4 period
    auto missing = nlohmann::json::parse(*router.HandleDatagram(R"({"type":"config","op":"display_sync"})", now));
    CHECK(missing["ok"] == false);
}
