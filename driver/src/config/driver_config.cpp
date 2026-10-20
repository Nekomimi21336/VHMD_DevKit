#include "config/driver_config.h"

#include <algorithm>
#include <set>

#include <nlohmann/json.hpp>

#include "config/roles.h"

namespace vhmd {
namespace {

using json = nlohmann::json;

struct TrackerDefault {
    const char* role;
    Vec3 pos;
};

// Rough standing pose of a 1.7 m person facing -Z.
constexpr TrackerDefault kDefaultTrackers[] = {
    {"waist", {0.0, 1.00, 0.0}},
    {"left_foot", {-0.12, 0.08, 0.0}},
    {"right_foot", {0.12, 0.08, 0.0}},
    {"left_knee", {-0.11, 0.50, -0.03}},
    {"right_knee", {0.11, 0.50, -0.03}},
    {"left_elbow", {-0.30, 1.15, 0.0}},
    {"right_elbow", {0.30, 1.15, 0.0}},
    {"chest", {0.0, 1.35, -0.05}},
};

bool ReadVec3(const json& j, Vec3& out) {
    if (!j.is_array() || j.size() != 3) return false;
    for (const auto& v : j) {
        if (!v.is_number()) return false;
    }
    Vec3 v{j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
    if (!IsFinite(v)) return false;
    out = v;
    return true;
}

bool ReadQuat(const json& j, Quat& out) {
    if (!j.is_array() || j.size() != 4) return false;
    for (const auto& v : j) {
        if (!v.is_number()) return false;
    }
    Quat q{j[0].get<double>(), j[1].get<double>(), j[2].get<double>(), j[3].get<double>()};
    if (!Normalize(q)) return false;
    out = q;
    return true;
}

template <typename T>
void Get(const json& obj, const char* key, T& out, std::vector<std::string>& warnings, const std::string& ctx) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return;
    try {
        out = it->get<T>();
    } catch (const std::exception&) {
        warnings.push_back(ctx + key + ": wrong type, using default");
    }
}

void ApplyDeviceCommon(const json& obj, DeviceConfig& d, std::vector<std::string>& w, const std::string& ctx) {
    Get(obj, "enabled", d.enabled, w, ctx);
    Get(obj, "serial", d.serial, w, ctx);
    if (auto it = obj.find("pos"); it != obj.end() && !ReadVec3(*it, d.default_pos)) w.push_back(ctx + "pos: invalid");
    if (auto it = obj.find("rot"); it != obj.end() && !ReadQuat(*it, d.default_rot)) w.push_back(ctx + "rot: invalid");
    if (auto it = obj.find("timeoutPolicy"); it != obj.end()) {
        auto p = it->is_string() ? ParseTimeoutPolicy(it->get<std::string>()) : std::nullopt;
        if (p) d.timeout_policy = p;
        else w.push_back(ctx + "timeoutPolicy: expected hold|invalid|disconnect");
    }
}

DeviceConfig MakeTracker(size_t index, const std::string& role, const Vec3& pos) {
    DeviceConfig d;
    d.kind = DeviceKind::Tracker;
    d.id = "tracker_" + std::to_string(index);
    d.serial = "VHMD-TRK-" + std::to_string(index);
    d.role = role;
    d.default_pos = pos;
    d.on_demand = true;
    return d;
}

} // namespace

std::optional<TimeoutPolicy> ParseTimeoutPolicy(std::string_view s) {
    if (s == "hold") return TimeoutPolicy::Hold;
    if (s == "invalid") return TimeoutPolicy::Invalid;
    if (s == "disconnect") return TimeoutPolicy::Disconnect;
    return std::nullopt;
}

const char* ToString(TimeoutPolicy p) {
    switch (p) {
    case TimeoutPolicy::Hold: return "hold";
    case TimeoutPolicy::Invalid: return "invalid";
    case TimeoutPolicy::Disconnect: return "disconnect";
    }
    return "?";
}

TimeoutPolicy DriverConfig::EffectivePolicy(const DeviceConfig& d) const {
    if (d.timeout_policy) return *d.timeout_policy;
    if (d.on_demand) return TimeoutPolicy::Disconnect;
    return timeout_policy;
}

const DeviceConfig* DriverConfig::Find(std::string_view id) const {
    for (const auto& d : devices) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

DriverConfig DefaultConfig() {
    DriverConfig c;

    DeviceConfig hmd;
    hmd.kind = DeviceKind::Hmd;
    hmd.id = "hmd";
    hmd.serial = "VHMD-HMD-0";
    hmd.default_pos = {0.0, 1.7, 0.0};
    c.devices.push_back(hmd);

    for (const char* side : {"left", "right"}) {
        DeviceConfig ctl;
        ctl.kind = DeviceKind::Controller;
        ctl.id = std::string(side) + "_hand";
        ctl.serial = std::string("VHMD-CTL-") + (side[0] == 'l' ? "L" : "R");
        ctl.role = side;
        ctl.default_pos = {side[0] == 'l' ? -0.2 : 0.2, 1.2, -0.3};
        c.devices.push_back(ctl);
    }

    size_t i = 0;
    for (const auto& t : kDefaultTrackers) c.devices.push_back(MakeTracker(i++, t.role, t.pos));
    return c;
}

ConfigLoadResult LoadConfigFromJson(std::string_view text) {
    ConfigLoadResult r;
    r.config = DefaultConfig();
    DriverConfig& c = r.config;
    auto& w = r.warnings;

    json root = json::parse(text.begin(), text.end(), nullptr, false, /*ignore_comments=*/true);
    if (root.is_discarded() || !root.is_object()) {
        r.ok = false;
        w.emplace_back("config: not a JSON object, using built-in defaults");
        return r;
    }

    try {
        if (auto net = root.find("network"); net != root.end() && net->is_object()) {
            Get(*net, "bind", c.bind_address, w, "network.");
            int port = c.port;
            Get(*net, "port", port, w, "network.");
            if (port > 0 && port < 65536) c.port = static_cast<uint16_t>(port);
            else w.emplace_back("network.port: out of range");
            Get(*net, "allowRemote", c.allow_remote, w, "network.");
        }
        Get(root, "updateRateHz", c.update_rate_hz, w, "");
        c.update_rate_hz = std::clamp(c.update_rate_hz, 30.0, 1000.0);
        Get(root, "smoothing", c.smoothing, w, "");
        c.smoothing = std::clamp(c.smoothing, 0.0, 0.99);
        Get(root, "applyTrackerRoles", c.apply_tracker_roles, w, "");
        Get(root, "trackersOnDemand", c.trackers_on_demand, w, "");
        for (auto& d : c.devices) {
            if (d.kind == DeviceKind::Tracker) d.on_demand = c.trackers_on_demand;
        }

        if (auto d = root.find("display"); d != root.end() && d->is_object()) {
            Get(*d, "mode", c.display_mode, w, "display.");
            Get(*d, "refreshHz", c.display_refresh_hz, w, "display.");
            if (c.display_refresh_hz != 0.0) {
                if (c.display_refresh_hz < kMinRefreshHz || c.display_refresh_hz > kMaxRefreshHz)
                    w.emplace_back("display.refreshHz: clamped to " + std::to_string(int(kMinRefreshHz)) + ".." + std::to_string(int(kMaxRefreshHz)));
                c.display_refresh_hz = std::clamp(c.display_refresh_hz, kMinRefreshHz, kMaxRefreshHz);
            }
            Get(*d, "previewWindow", c.preview_window, w, "display.");
            if (c.display_mode != "auto" && c.display_mode != "window" && c.display_mode != "direct") {
                w.emplace_back("display.mode: expected auto|window|direct");
                c.display_mode = "auto";
            }
        }
        if (auto p = root.find("preview"); p != root.end() && p->is_object()) {
            Get(*p, "enabled", c.preview.enabled, w, "preview.");
            Get(*p, "path", c.preview.path, w, "preview.");
            Get(*p, "fps", c.preview.fps, w, "preview.");
            c.preview.fps = std::clamp(c.preview.fps, 1.0, 120.0);
            int max_w = static_cast<int>(c.preview.max_eye_width);
            Get(*p, "maxEyeWidth", max_w, w, "preview.");
            c.preview.max_eye_width = static_cast<uint32_t>(std::clamp(max_w, 16, 8192));
            Get(*p, "vulkanDevice", c.preview.vulkan_device, w, "preview.");
            Get(*p, "autoStart", c.preview.auto_start, w, "preview.");
            Get(*p, "windowFps", c.preview.window_fps, w, "preview.");
            c.preview.window_fps = std::clamp(c.preview.window_fps, 1.0, 240.0);
        }
        if (auto st = root.find("stream"); st != root.end() && st->is_object()) {
            Get(*st, "enabled", c.preview.stream_enabled, w, "stream.");
            int port = c.preview.stream_port;
            Get(*st, "port", port, w, "stream.");
            if (port < 1 || port > 65535) {
                w.emplace_back("stream.port: out of range");
                port = 39583;
            }
            c.preview.stream_port = static_cast<uint16_t>(port);
        }
        if (auto t = root.find("timeout"); t != root.end() && t->is_object()) {
            Get(*t, "ms", c.timeout_ms, w, "timeout.");
            c.timeout_ms = std::max(c.timeout_ms, 1.0);
            if (auto p = t->find("policy"); p != t->end()) {
                auto pol = p->is_string() ? ParseTimeoutPolicy(p->get<std::string>()) : std::nullopt;
                if (pol) c.timeout_policy = *pol;
                else w.emplace_back("timeout.policy: expected hold|invalid|disconnect");
            }
        }
        if (auto lg = root.find("log"); lg != root.end() && lg->is_object()) {
            Get(*lg, "errorsPerSecond", c.log_errors_per_second, w, "log.");
            Get(*lg, "verbose", c.verbose, w, "log.");
        }
        if (auto v = root.find("vmt"); v != root.end() && v->is_object()) {
            Get(*v, "enabled", c.vmt_enabled, w, "vmt.");
            if (auto m = v->find("indexMap"); m != v->end() && m->is_object()) {
                for (auto it = m->begin(); it != m->end(); ++it) {
                    int idx = -1;
                    try { idx = std::stoi(it.key()); } catch (...) {}
                    if (idx < 0 || idx > 57 || !it->is_string()) {
                        w.push_back("vmt.indexMap." + it.key() + ": invalid");
                        continue;
                    }
                    if (c.vmt_mapping.index_to_id.size() <= static_cast<size_t>(idx)) {
                        c.vmt_mapping.index_to_id.resize(idx + 1);
                    }
                    c.vmt_mapping.index_to_id[idx] = it->get<std::string>();
                }
            }
        }

        // Devices
        if (auto h = root.find("hmd"); h != root.end() && h->is_object()) {
            for (auto& d : c.devices) {
                if (d.kind != DeviceKind::Hmd) continue;
                ApplyDeviceCommon(*h, d, w, "hmd.");
                Get(*h, "eyeTracking", d.eye_tracking, w, "hmd.");
                Get(*h, "headModel", d.head_model, w, "hmd.");
                Get(*h, "proximity", d.proximity, w, "hmd.");
                if (auto au = h->find("audio"); au != h->end() && au->is_object()) {
                    Get(*au, "playbackDevice", d.audio_playback, w, "hmd.audio.");
                    Get(*au, "recordingDevice", d.audio_recording, w, "hmd.audio.");
                }
                if (auto pa = h->find("playArea"); pa != h->end()) {
                    if (pa->is_array() && pa->size() == 2 && (*pa)[0].is_number() && (*pa)[1].is_number() &&
                        (*pa)[0].get<double>() >= 0.5 && (*pa)[0].get<double>() <= 100.0 &&
                        (*pa)[1].get<double>() >= 0.5 && (*pa)[1].get<double>() <= 100.0) {
                        d.play_area_width = (*pa)[0].get<double>();
                        d.play_area_depth = (*pa)[1].get<double>();
                    } else {
                        w.push_back("hmd.playArea: expected [width, depth] in meters (0.5..100)");
                    }
                }
            }
        }
        if (auto ctl = root.find("controllers"); ctl != root.end() && ctl->is_object()) {
            bool all_enabled = true;
            Get(*ctl, "enabled", all_enabled, w, "controllers.");
            bool skeleton = true;
            Get(*ctl, "skeleton", skeleton, w, "controllers.");
            // Controllers report a Partial skeleton by default, like hardware
            // controllers; the hand twins ("hands" mode) are always Full.
            std::string level = "partial";
            Get(*ctl, "skeletonLevel", level, w, "controllers.");
            if (level != "full" && level != "partial") w.emplace_back("controllers.skeletonLevel: expected full|partial");
            Get(*ctl, "handMode", c.hand_mode, w, "controllers.");
            if (c.hand_mode != "controllers" && c.hand_mode != "hands") {
                w.emplace_back("controllers.handMode: expected controllers|hands");
                c.hand_mode = "controllers";
            }
            for (auto& d : c.devices) {
                if (d.kind != DeviceKind::Controller) continue;
                d.enabled = all_enabled;
                d.skeleton = skeleton;
                d.skeleton_full = level != "partial";
                if (auto side = ctl->find(d.role); side != ctl->end() && side->is_object()) {
                    ApplyDeviceCommon(*side, d, w, "controllers." + d.role + ".");
                }
            }
        }
        if (auto trk = root.find("trackers"); trk != root.end()) {
            if (!trk->is_array()) {
                w.emplace_back("trackers: expected array");
            } else {
                // Replace the default tracker list entirely.
                c.devices.erase(std::remove_if(c.devices.begin(), c.devices.end(),
                                               [](const DeviceConfig& d) { return d.kind == DeviceKind::Tracker; }),
                                c.devices.end());
                size_t index = 0;
                for (const auto& t : *trk) {
                    if (index >= kMaxTrackers) {
                        w.emplace_back("trackers: more than 16 entries, extra ignored");
                        break;
                    }
                    const std::string ctx = "trackers[" + std::to_string(index) + "].";
                    if (!t.is_object()) {
                        w.push_back(ctx + "expected object");
                        continue;
                    }
                    DeviceConfig d = MakeTracker(index, "none", {0.0, 1.0, 0.0});
                    d.on_demand = c.trackers_on_demand;
                    Get(t, "onDemand", d.on_demand, w, ctx);
                    if (index < std::size(kDefaultTrackers)) d.default_pos = kDefaultTrackers[index].pos;
                    Get(t, "id", d.id, w, ctx);
                    if (auto role = t.find("role"); role != t.end() && role->is_string()) {
                        if (auto info = NormalizeRole(role->get<std::string>())) d.role = info->short_name;
                        else w.push_back(ctx + "role: unknown '" + role->get<std::string>() + "'");
                    }
                    ApplyDeviceCommon(t, d, w, ctx);
                    c.devices.push_back(d);
                    ++index;
                }
            }
        }
    } catch (const std::exception& e) {
        w.push_back(std::string("config: unexpected error: ") + e.what());
    }

    // Validate network exposure: anything but loopback requires allowRemote.
    if (!c.allow_remote && c.bind_address != "127.0.0.1" && c.bind_address != "localhost") {
        w.push_back("network.bind '" + c.bind_address + "' requires allowRemote=true; falling back to 127.0.0.1");
        c.bind_address = "127.0.0.1";
    }

    // Ids and serials must be unique.
    std::set<std::string> ids, serials;
    for (auto& d : c.devices) {
        if (d.id.empty() || !ids.insert(d.id).second) {
            w.push_back("device id '" + d.id + "' is empty or duplicated; device disabled");
            d.enabled = false;
        }
        if (d.serial.empty() || !serials.insert(d.serial).second) {
            w.push_back("serial '" + d.serial + "' is empty or duplicated; device disabled");
            d.enabled = false;
        }
    }
    return r;
}

} // namespace vhmd
