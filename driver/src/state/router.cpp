#include "state/router.h"

#include <nlohmann/json.hpp>

#include "protocol/osc.h"
#include "protocol/parser.h"

namespace vhmd {
namespace {

using json = nlohmann::json;

const char* KindName(DeviceKind k) {
    switch (k) {
    case DeviceKind::Hmd: return "hmd";
    case DeviceKind::Controller: return "controller";
    case DeviceKind::Tracker: return "tracker";
    }
    return "?";
}

json Reply(const char* type, std::optional<double> seq) {
    json j = {{"type", type}};
    if (seq) j["seq"] = *seq;
    return j;
}

} // namespace

MessageRouter::MessageRouter(const DriverConfig& config, std::vector<std::shared_ptr<DeviceState>> devices,
                             RouterHooks hooks)
    : config_(config), devices_(std::move(devices)), hooks_(std::move(hooks)),
      error_limiter_(config.log_errors_per_second, config.log_errors_per_second) {
    for (const auto& d : devices_) by_id_[d->Config().id] = d.get();
    // Role aliases ("waist" -> tracker_0) only where unambiguous and not shadowing an id.
    std::map<std::string, int> role_count;
    for (const auto& d : devices_) {
        if (d->Config().kind == DeviceKind::Tracker && d->Config().role != "none") role_count[d->Config().role]++;
    }
    for (const auto& d : devices_) {
        const auto& role = d->Config().role;
        if (d->Config().kind == DeviceKind::Tracker && role_count[role] == 1 && !by_id_.count(role)) {
            by_id_[role] = d.get();
        }
    }
}

DeviceState* MessageRouter::Find(std::string_view id) const {
    auto it = by_id_.find(id);
    return it == by_id_.end() ? nullptr : it->second;
}

void MessageRouter::ReportErrors(const std::vector<std::string>& errors) {
    for (const auto& e : errors) {
        ++rejected_;
        uint64_t suppressed = 0;
        if (error_limiter_.Allow(suppressed)) {
            if (suppressed > 0) {
                Log(LogLevel::Warning, "rejected message: %s (%llu similar suppressed)", e.c_str(),
                    static_cast<unsigned long long>(suppressed));
            } else {
                Log(LogLevel::Warning, "rejected message: %s", e.c_str());
            }
        }
    }
}

std::optional<std::string> MessageRouter::HandleDatagram(std::string_view data, Clock::time_point now) {
    ++total_packets_;
    ParseResult parsed;
    if (LooksLikeJson(data)) {
        parsed = ParseJsonPacket(data, config_.limits);
    } else if (config_.vmt_enabled && LooksLikeOsc(data)) {
        parsed = ParseOscPacket(data, config_.vmt_mapping, config_.limits);
    } else {
        parsed.errors.emplace_back(LooksLikeOsc(data) ? "OSC packet received but vmt.enabled is false"
                                                      : "unrecognized packet format");
    }

    std::optional<std::string> reply;
    for (const auto& msg : parsed.messages) {
        if (const auto* cfg = std::get_if<ConfigMessage>(&msg)) {
            reply = HandleConfig(*cfg, now);
            continue;
        }
        const std::string id = std::visit([](const auto& m) { return m.id; }, msg);
        DeviceState* dev = Find(id);
        if (!dev || !dev->Config().enabled) {
            parsed.errors.push_back("unknown or disabled device '" + id.substr(0, 64) + "'");
            continue;
        }
        // On-demand devices join SteamVR when a client first talks to them.
        // The value is buffered first, so the device starts in sync.
        const auto* pose = std::get_if<PoseMessage>(&msg);
        const bool wants_device = !(pose && !pose->valid);
        if (pose) dev->SubmitPose(*pose, now);
        else if (const auto* i = std::get_if<InputMessage>(&msg)) dev->SubmitInput(*i, now);
        else if (const auto* s = std::get_if<SkeletonMessage>(&msg)) dev->SubmitSkeleton(*s);
        else if (const auto* g = std::get_if<GazeMessage>(&msg)) dev->SubmitGaze(*g);
        if (wants_device && dev->Config().on_demand && hooks_.request_add) hooks_.request_add(dev->Config().id);
    }
    ReportErrors(parsed.errors);
    return reply;
}

std::string MessageRouter::StatusJson(Clock::time_point now, std::optional<double> seq) const {
    json j = Reply("status", seq);
    j["driver"] = "vhmd";
    j["version"] = kDriverVersion;
    j["port"] = config_.port;
    j["update_rate_hz"] = config_.update_rate_hz;
    j["timeout_ms"] = config_.timeout_ms;
    j["timeout_policy"] = ToString(config_.timeout_policy);
    j["vmt_enabled"] = config_.vmt_enabled;
    j["hand_mode"] = hooks_.hand_mode ? hooks_.hand_mode() : config_.hand_mode;
    if (hooks_.perf_status) {
        auto perf = json::parse(hooks_.perf_status(), nullptr, false);
        if (!perf.is_discarded()) j["perf"] = std::move(perf);
    }
    j["packets"] = {{"total", total_packets_.load()}, {"rejected", rejected_.load()}};
    json devs = json::array();
    for (const auto& d : devices_) {
        const auto& c = d->Config();
        const DeviceStats s = d->Stats(now);
        json e = {
            {"id", c.id},
            {"serial", c.serial},
            {"kind", KindName(c.kind)},
            {"enabled", c.enabled},
            {"index", hooks_.device_index ? hooks_.device_index(c.id) : -1},
            {"pose_packets", s.pose_packets},
            {"input_packets", s.input_packets},
            {"rate_hz", s.rate_hz},
        };
        if (!c.role.empty()) e["role"] = c.role;
        e["on_demand"] = c.on_demand;
        e["timeout_policy"] = ToString(config_.EffectivePolicy(c));
        e["age_s"] = s.seconds_since_update < 0 ? json(nullptr) : json(s.seconds_since_update);
        if (s.sender_latency_ms >= 0) e["latency_ms"] = s.sender_latency_ms;
        devs.push_back(std::move(e));
    }
    j["devices"] = std::move(devs);
    if (hooks_.display_status) {
        json d = json::parse(hooks_.display_status(), nullptr, false);
        if (!d.is_discarded()) j["display"] = std::move(d);
    }
    return j.dump();
}

std::optional<std::string> MessageRouter::HandleConfig(const ConfigMessage& m, Clock::time_point now) {
    if (m.op == "list" || m.op == "status") return StatusJson(now, m.seq);

    json r = Reply("result", m.seq);
    r["op"] = m.op;
    if (m.op == "ping") {
        r["ok"] = true;
        r["version"] = kDriverVersion;
    } else if (m.op == "roles") {
        json roles = json::array();
        for (const auto& role : AllRoles()) roles.push_back({{"name", role.short_name}, {"steamvr", role.steamvr_role}});
        r["ok"] = true;
        r["roles"] = std::move(roles);
    } else if (m.op == "reset") {
        int n = 0;
        for (const auto& d : devices_) {
            if (m.id.empty() || d->Config().id == m.id) {
                d->Reset(now);
                ++n;
            }
        }
        r["ok"] = n > 0;
        r["count"] = n;
    } else if (m.op == "add") {
        // Explicitly register on-demand devices (all of them if no id given).
        // Registration happens on SteamVR's next frame; poll "list" for the index.
        json added = json::array();
        for (const auto& d : devices_) {
            const auto& c = d->Config();
            const bool match = m.id.empty() ? c.on_demand : (d.get() == Find(m.id));
            if (!match || !c.enabled) continue;
            if (c.on_demand && hooks_.request_add) hooks_.request_add(c.id);
            added.push_back(c.id);
        }
        r["ok"] = !added.empty();
        if (added.empty()) r["error"] = "unknown or disabled device";
        r["ids"] = std::move(added);
    } else if (m.op == "set_role") {
        DeviceState* dev = Find(m.id);
        auto role = NormalizeRole(m.role);
        if (!dev || dev->Config().kind != DeviceKind::Tracker) {
            r["ok"] = false;
            r["error"] = "unknown tracker id";
        } else if (!role) {
            r["ok"] = false;
            r["error"] = "unknown role";
        } else if (!hooks_.set_role) {
            r["ok"] = false;
            r["error"] = "set_role not available";
        } else {
            std::string err = hooks_.set_role(dev->Config().id, *role);
            r["ok"] = err.empty();
            if (!err.empty()) r["error"] = err;
            r["role"] = role->steamvr_role;
        }
    } else if (m.op == "preview_window") {
        if (m.mode != "on" && m.mode != "off") {
            r["ok"] = false;
            r["error"] = "mode must be \"on\" or \"off\"";
        } else {
            const std::string err =
                hooks_.set_preview_window ? hooks_.set_preview_window(m.mode == "on") : "preview window not available";
            r["ok"] = err.empty();
            if (!err.empty()) r["error"] = err;
        }
    } else if (m.op == "preview") {
        if (m.mode != "on" && m.mode != "off") {
            r["ok"] = false;
            r["error"] = "mode must be \"on\" or \"off\"";
        } else {
            const std::string err = hooks_.set_preview ? hooks_.set_preview(m.mode == "on") : "preview not available";
            r["ok"] = err.empty();
            if (!err.empty()) r["error"] = err;
        }
    } else if (m.op == "hand_mode") {
        if (!hooks_.set_hand_mode) {
            r["ok"] = false;
            r["error"] = "hand_mode not available";
        } else if (m.mode.empty()) {
            r["ok"] = true; // query only
        } else {
            const std::string err = hooks_.set_hand_mode(m.mode);
            r["ok"] = err.empty();
            if (!err.empty()) r["error"] = err;
        }
        if (hooks_.hand_mode) r["mode"] = hooks_.hand_mode();
    } else {
        r["ok"] = false;
        r["error"] = "unknown op";
    }
    return r.dump();
}

} // namespace vhmd
