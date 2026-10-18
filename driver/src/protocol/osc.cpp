#include "protocol/osc.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace vhmd {
namespace {

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
constexpr int kMaxBundleDepth = 4;

struct OscArg {
    char type = 0;
    int32_t i = 0;
    float f = 0.0f;
    std::string s;

    bool IsNumber() const { return type == 'i' || type == 'f'; }
    double Number() const { return type == 'i' ? static_cast<double>(i) : static_cast<double>(f); }
};

struct OscMessage {
    std::string address;
    std::vector<OscArg> args;
};

class Reader {
public:
    explicit Reader(std::string_view d) : d_(d) {}

    bool ReadString(std::string& out) {
        size_t end = d_.find('\0', pos_);
        if (end == std::string_view::npos) return false;
        out.assign(d_.substr(pos_, end - pos_));
        pos_ = (end + 4) & ~size_t{3}; // skip terminator + padding to 4 bytes
        return pos_ <= d_.size();
    }
    bool ReadU32(uint32_t& out) {
        if (pos_ + 4 > d_.size()) return false;
        const auto* p = reinterpret_cast<const unsigned char*>(d_.data() + pos_);
        out = (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
        pos_ += 4;
        return true;
    }
    bool Skip(size_t n) {
        if (pos_ + n > d_.size()) return false;
        pos_ += n;
        return true;
    }
    std::string_view Take(size_t n) {
        auto s = d_.substr(pos_, n);
        pos_ += n;
        return s;
    }
    size_t Remaining() const { return d_.size() - pos_; }

private:
    std::string_view d_;
    size_t pos_ = 0;
};

bool DecodeMessage(std::string_view data, OscMessage& out) {
    Reader r(data);
    if (!r.ReadString(out.address) || out.address.empty() || out.address[0] != '/') return false;
    std::string tags;
    if (r.Remaining() == 0) return true; // no type tag string: zero arguments
    if (!r.ReadString(tags) || tags.empty() || tags[0] != ',') return false;
    for (size_t k = 1; k < tags.size(); ++k) {
        OscArg a;
        a.type = tags[k];
        uint32_t u;
        switch (a.type) {
        case 'i':
            if (!r.ReadU32(u)) return false;
            a.i = static_cast<int32_t>(u);
            break;
        case 'f':
            if (!r.ReadU32(u)) return false;
            std::memcpy(&a.f, &u, sizeof(float));
            break;
        case 's':
            if (!r.ReadString(a.s)) return false;
            break;
        case 'T':
        case 'F':
            a.type = 'i';
            a.i = tags[k] == 'T' ? 1 : 0;
            break;
        case 'd': // double: read and convert to float
        {
            uint32_t hi, lo;
            if (!r.ReadU32(hi) || !r.ReadU32(lo)) return false;
            uint64_t bits = (uint64_t{hi} << 32) | lo;
            double d;
            std::memcpy(&d, &bits, sizeof(double));
            a.type = 'f';
            a.f = static_cast<float>(d);
            break;
        }
        default:
            return false; // blobs, timetags etc. are not used by VMT
        }
        out.args.push_back(std::move(a));
    }
    return true;
}

void Decode(std::string_view data, int depth, std::vector<OscMessage>& out, std::vector<std::string>& errors) {
    if (data.size() >= 8 && data.substr(0, 8) == std::string_view("#bundle\0", 8)) {
        if (depth >= kMaxBundleDepth) {
            errors.emplace_back("osc: bundle nesting too deep");
            return;
        }
        Reader r(data);
        if (!r.Skip(16)) { // "#bundle\0" + 8 byte timetag
            errors.emplace_back("osc: truncated bundle");
            return;
        }
        while (r.Remaining() >= 4) {
            uint32_t size = 0;
            if (!r.ReadU32(size) || size > r.Remaining() || size % 4 != 0) {
                errors.emplace_back("osc: bad bundle element size");
                return;
            }
            Decode(r.Take(size), depth + 1, out, errors);
        }
        return;
    }
    OscMessage m;
    if (DecodeMessage(data, m)) {
        out.push_back(std::move(m));
    } else {
        errors.emplace_back("osc: malformed message");
    }
}

bool ArgsMatch(const OscMessage& m, size_t numeric_count) {
    if (m.args.size() < numeric_count) return false;
    for (size_t i = 0; i < numeric_count; ++i) {
        if (!m.args[i].IsNumber()) return false;
        if (!std::isfinite(m.args[i].Number())) return false;
    }
    return true;
}

const char* ButtonName(int index) {
    static const char* kNames[] = {"system", "a", "b", "menu", "joystick_click", "trigger_click", "grip_click"};
    if (index < 0 || index >= static_cast<int>(sizeof(kNames) / sizeof(kNames[0]))) return nullptr;
    return kNames[index];
}

const char* TriggerName(int index) {
    if (index == 0) return "trigger";
    if (index == 1) return "grip";
    return nullptr;
}

std::string DeviceFor(const VmtMapping& map, int index, int enable) {
    if (enable == 2 || enable == 5) return map.left_hand_id;
    if (enable == 3 || enable == 6) return map.right_hand_id;
    if (index >= 0 && static_cast<size_t>(index) < map.index_to_id.size() && !map.index_to_id[index].empty()) {
        return map.index_to_id[index];
    }
    if (index >= 0 && index < 58) return "tracker_" + std::to_string(index);
    return {};
}

// Input messages carry no `enable`, so the index alone selects the device.
// Index 0/1 do not imply hands: callers map controllers via index_to_id.
std::string InputDeviceFor(const VmtMapping& map, int index) { return DeviceFor(map, index, 1); }

std::string HandlePose(const OscMessage& m, const VmtMapping& map, const ParseLimits& lim, std::vector<Message>& out) {
    const std::string& a = m.address;
    const bool unity = a.size() > 6 && a.compare(a.size() - 6, 6, "/Unity") == 0;
    const bool euler = a.size() > 7 && a.compare(a.size() - 7, 7, "/UEuler") == 0;
    const size_t n = euler ? 9 : 10;
    if (!ArgsMatch(m, n)) return "osc " + a + ": bad arguments";

    const int index = static_cast<int>(m.args[0].Number());
    const int enable = static_cast<int>(m.args[1].Number());
    PoseMessage p;
    p.id = DeviceFor(map, index, enable);
    if (p.id.empty()) return "osc " + a + ": index out of range";
    if (enable == 0) {
        p.valid = false;
        out.emplace_back(std::move(p));
        return {};
    }
    p.pos = {m.args[3].Number(), m.args[4].Number(), m.args[5].Number()};
    if (euler) {
        // VMT UEuler: Unity Euler angles in degrees (Unity applies Z, X, Y).
        const Quat qy = FromAxisAngle({0, 1, 0}, m.args[7].Number() * kDegToRad);
        const Quat qx = FromAxisAngle({1, 0, 0}, m.args[6].Number() * kDegToRad);
        const Quat qz = FromAxisAngle({0, 0, 1}, m.args[8].Number() * kDegToRad);
        p.rot = Multiply(Multiply(qy, qx), qz);
    } else {
        // OSC order is qx, qy, qz, qw
        p.rot = {m.args[9].Number(), m.args[6].Number(), m.args[7].Number(), m.args[8].Number()};
    }
    if (!Normalize(p.rot)) return "osc " + a + ": degenerate quaternion";
    if (unity || euler) {
        p.pos = UnityToDriver(p.pos);
        p.rot = UnityToDriver(p.rot);
    }
    if (std::abs(p.pos.x) > lim.max_abs_position || std::abs(p.pos.y) > lim.max_abs_position ||
        std::abs(p.pos.z) > lim.max_abs_position) {
        return "osc " + a + ": position out of range";
    }
    out.emplace_back(std::move(p));
    return {};
}

std::string HandleInput(const OscMessage& m, const VmtMapping& map, std::vector<Message>& out) {
    const std::string& a = m.address;
    InputMessage in;
    auto suffix = [&](const char* s) {
        const size_t n = std::strlen(s);
        return a.size() >= n && a.compare(a.size() - n, n, s) == 0;
    };

    if (a.rfind("/VMT/Input/Joystick", 0) == 0 && !suffix("/Touch") && !suffix("/Click")) {
        if (!ArgsMatch(m, 5)) return "osc " + a + ": bad arguments";
        if (static_cast<int>(m.args[1].Number()) != 0) return "osc " + a + ": only joystick 0 is mapped";
        in.id = InputDeviceFor(map, static_cast<int>(m.args[0].Number()));
        const float x = static_cast<float>(m.args[3].Number());
        const float y = static_cast<float>(m.args[4].Number());
        if (std::abs(x) > 1.0f || std::abs(y) > 1.0f) return "osc " + a + ": value out of range";
        in.values = {{"joystick_x", x}, {"joystick_y", y}};
        out.emplace_back(std::move(in));
        return {};
    }

    if (!ArgsMatch(m, 4)) return "osc " + a + ": bad arguments";
    const int index = static_cast<int>(m.args[0].Number());
    const int sub = static_cast<int>(m.args[1].Number());
    const float value = static_cast<float>(m.args[3].Number());
    if (std::abs(value) > 1.0f) return "osc " + a + ": value out of range";
    in.id = InputDeviceFor(map, index);

    std::string name;
    if (a.rfind("/VMT/Input/Button", 0) == 0) {
        const char* base = ButtonName(sub);
        if (!base) return "osc " + a + ": button index not mapped";
        name = base;
        if (suffix("/Touch")) name += "_touch";
    } else if (a.rfind("/VMT/Input/Trigger", 0) == 0) {
        const char* base = TriggerName(sub);
        if (!base) return "osc " + a + ": trigger index not mapped";
        name = base;
        if (suffix("/Touch")) name += "_touch";
        if (suffix("/Click")) name += "_click";
    } else if (a.rfind("/VMT/Input/Joystick", 0) == 0) {
        name = suffix("/Touch") ? "joystick_touch" : "joystick_click";
    } else {
        return "osc " + a + ": unsupported";
    }
    in.values = {{name, value}};
    out.emplace_back(std::move(in));
    return {};
}

} // namespace

Vec3 UnityToDriver(const Vec3& p) { return {p.x, p.y, -p.z}; }

Quat UnityToDriver(const Quat& q) { return {q.w, -q.x, -q.y, q.z}; }

bool LooksLikeOsc(std::string_view data) { return !data.empty() && (data[0] == '/' || data[0] == '#'); }

ParseResult ParseOscPacket(std::string_view data, const VmtMapping& mapping, const ParseLimits& limits) {
    ParseResult result;
    if (data.size() > limits.max_packet_bytes) {
        result.errors.emplace_back("osc: packet too large");
        return result;
    }
    std::vector<OscMessage> msgs;
    Decode(data, 0, msgs, result.errors);
    if (msgs.size() > limits.max_batch) {
        result.errors.emplace_back("osc: too many messages");
        return result;
    }
    for (const auto& m : msgs) {
        std::string err;
        const std::string& a = m.address;
        if (a.rfind("/VMT/Room/", 0) == 0 || a.rfind("/VMT/Raw/", 0) == 0) {
            err = HandlePose(m, mapping, limits, result.messages);
        } else if (a.rfind("/VMT/Input/", 0) == 0) {
            err = HandleInput(m, mapping, result.messages);
        } else {
            err = "osc " + a.substr(0, 64) + ": unsupported address";
        }
        if (!err.empty()) result.errors.push_back(std::move(err));
    }
    return result;
}

} // namespace vhmd
