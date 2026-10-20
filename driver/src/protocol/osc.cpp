#include "protocol/osc.h"

#include <algorithm>
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

bool EndsWith(const std::string& a, const char* s) {
    const size_t n = std::strlen(s);
    return a.size() >= n && a.compare(a.size() - n, n, s) == 0;
}

bool StartsWith(const std::string& a, const char* s) { return a.rfind(s, 0) == 0; }

// Button index -> input base name ("<base>" is the click, "<base>_touch" the touch).
const char* ButtonBase(int index) {
    static const char* kNames[] = {"system", "a", "b", "menu", "joystick", "trigger", "grip"};
    if (index < 0 || index >= static_cast<int>(sizeof(kNames) / sizeof(kNames[0]))) return nullptr;
    return kNames[index];
}

const char* TriggerName(int index) {
    if (index == 0) return "trigger";
    if (index == 1) return "grip";
    if (index == 2) return "grip_force";
    return nullptr;
}

// Joystick 0 = thumbstick, 1 = trackpad.
const char* JoystickBase(int index) {
    if (index == 0) return "joystick";
    if (index == 1) return "trackpad";
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

// Messages without `enable` (inputs, skeleton) use the device the index last
// posed as (VMT fixes the device class with the first pose), else the mapping.
std::string IndexDevice(const VmtMapping& map, VmtState* state, int index) {
    if (state) {
        std::lock_guard<std::mutex> lock(state->mu);
        if (auto it = state->index_ids.find(index); it != state->index_ids.end()) return it->second;
    }
    return DeviceFor(map, index, 1);
}

Quat EulerUnity(double rx, double ry, double rz) {
    // Unity Quaternion.Euler: Z, then X, then Y (degrees).
    const Quat qy = FromAxisAngle({0, 1, 0}, ry * kDegToRad);
    const Quat qx = FromAxisAngle({1, 0, 0}, rx * kDegToRad);
    const Quat qz = FromAxisAngle({0, 0, 1}, rz * kDegToRad);
    return Multiply(Multiply(qy, qx), qz);
}

// Position + rotation from `args` starting at `first`: x y z, then qx qy qz qw
// (or rx ry rz for UEuler), converted to driver space for Unity / UEuler.
std::string ReadPose(const OscMessage& m, size_t first, bool unity, bool euler, Vec3& pos, Quat& rot) {
    pos = {m.args[first].Number(), m.args[first + 1].Number(), m.args[first + 2].Number()};
    if (euler) {
        rot = EulerUnity(m.args[first + 3].Number(), m.args[first + 4].Number(), m.args[first + 5].Number());
    } else {
        // OSC order is qx, qy, qz, qw
        rot = {m.args[first + 6].Number(), m.args[first + 3].Number(), m.args[first + 4].Number(),
               m.args[first + 5].Number()};
    }
    if (!Normalize(rot)) return "degenerate quaternion";
    if (unity || euler) {
        pos = UnityToDriver(pos);
        rot = UnityToDriver(rot);
    }
    return {};
}

// Rotation part of a row-major 3x4 matrix (assumed orthonormal).
Quat RoomRotation(const std::array<double, 12>& r) {
    const double m00 = r[0], m01 = r[1], m02 = r[2], m10 = r[4], m11 = r[5], m12 = r[6], m20 = r[8], m21 = r[9],
                 m22 = r[10];
    Quat q;
    const double trace = m00 + m11 + m22;
    if (trace > 0) {
        const double s = std::sqrt(trace + 1.0) * 2;
        q = {0.25 * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s};
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2;
        q = {(m21 - m12) / s, 0.25 * s, (m01 + m10) / s, (m02 + m20) / s};
    } else if (m11 > m22) {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2;
        q = {(m02 - m20) / s, (m01 + m10) / s, 0.25 * s, (m12 + m21) / s};
    } else {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2;
        q = {(m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25 * s};
    }
    if (!Normalize(q)) q = {};
    return q;
}

std::string HandlePose(const OscMessage& m, const VmtMapping& map, const ParseLimits& lim, VmtState* state,
                       std::vector<Message>& out) {
    const std::string& a = m.address;
    const bool unity = EndsWith(a, "/Unity");
    const bool euler = EndsWith(a, "/UEuler");
    const bool room = StartsWith(a, "/VMT/Room/");
    const bool joint = StartsWith(a, "/VMT/Joint/");
    const bool follow = StartsWith(a, "/VMT/Follow/");
    const size_t n = euler ? 9 : 10;
    if (!ArgsMatch(m, n)) return "osc " + a + ": bad arguments";
    if ((joint || follow) && (m.args.size() < n + 1 || m.args[n].type != 's')) return "osc " + a + ": missing serial";

    const int index = static_cast<int>(m.args[0].Number());
    const int enable = static_cast<int>(m.args[1].Number());
    PoseMessage p;
    p.id = DeviceFor(map, index, enable);
    if (p.id.empty()) return "osc " + a + ": index out of range";
    std::array<double, 12> r{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    if (state) {
        std::lock_guard<std::mutex> lock(state->mu);
        state->active = true;
        if (enable != 0) state->index_ids[index] = p.id;
        r = state->room;
    }
    if (enable == 0) {
        p.valid = false;
        out.emplace_back(std::move(p));
        return {};
    }
    if (auto e = ReadPose(m, 3, unity, euler, p.pos, p.rot); !e.empty()) return "osc " + a + ": " + e;
    if (room) {
        // Room space -> driver space with the room matrix (identity until set).
        const Vec3 v = p.pos;
        p.pos = {r[0] * v.x + r[1] * v.y + r[2] * v.z + r[3], r[4] * v.x + r[5] * v.y + r[6] * v.z + r[7],
                 r[8] * v.x + r[9] * v.y + r[10] * v.z + r[11]};
        p.rot = Multiply(RoomRotation(r), p.rot);
    } else if (joint || follow) {
        p.parent = m.args[n].s;
        p.follow = follow;
        if (follow) {
            // Follow: offset and rotation in room orientation (not the parent's).
            const Quat q = RoomRotation(r);
            p.pos = Rotate(q, p.pos);
            p.rot = Multiply(q, p.rot);
        }
    }
    if (std::abs(p.pos.x) > lim.max_abs_position || std::abs(p.pos.y) > lim.max_abs_position ||
        std::abs(p.pos.z) > lim.max_abs_position) {
        return "osc " + a + ": position out of range";
    }
    out.emplace_back(std::move(p));
    return {};
}

std::string HandleInput(const OscMessage& m, const VmtMapping& map, VmtState* state, std::vector<Message>& out) {
    const std::string& a = m.address;
    InputMessage in;
    const bool touch = EndsWith(a, "/Touch"), click = EndsWith(a, "/Click");

    if (StartsWith(a, "/VMT/Input/Joystick") && !touch && !click) {
        if (!ArgsMatch(m, 5)) return "osc " + a + ": bad arguments";
        const char* base = JoystickBase(static_cast<int>(m.args[1].Number()));
        if (!base) return "osc " + a + ": joystick index not mapped";
        in.id = IndexDevice(map, state, static_cast<int>(m.args[0].Number()));
        const float x = static_cast<float>(m.args[3].Number());
        const float y = static_cast<float>(m.args[4].Number());
        if (std::abs(x) > 1.0f || std::abs(y) > 1.0f) return "osc " + a + ": value out of range";
        in.values = {{std::string(base) + "_x", x}, {std::string(base) + "_y", y}};
        out.emplace_back(std::move(in));
        return {};
    }

    if (!ArgsMatch(m, 4)) return "osc " + a + ": bad arguments";
    const int index = static_cast<int>(m.args[0].Number());
    const int sub = static_cast<int>(m.args[1].Number());
    const float value = static_cast<float>(m.args[3].Number());
    if (std::abs(value) > 1.0f) return "osc " + a + ": value out of range";
    in.id = IndexDevice(map, state, index);

    std::string name;
    if (StartsWith(a, "/VMT/Input/Button")) {
        const char* base = ButtonBase(sub);
        if (!base) return "osc " + a + ": button index not mapped";
        const bool face = sub <= 3; // system/a/b/menu have no "_click" suffix
        name = std::string(base) + (touch ? "_touch" : (face ? "" : "_click"));
    } else if (StartsWith(a, "/VMT/Input/Trigger")) {
        const char* base = TriggerName(sub);
        if (!base) return "osc " + a + ": trigger index not mapped";
        name = base;
        if (touch) name += "_touch";
        if (click) name += "_click";
    } else if (StartsWith(a, "/VMT/Input/Joystick")) {
        const char* base = JoystickBase(sub);
        if (!base) return "osc " + a + ": joystick index not mapped";
        name = std::string(base) + (touch ? "_touch" : "_click");
    } else {
        return "osc " + a + ": unsupported";
    }
    in.values = {{name, value}};
    out.emplace_back(std::move(in));
    return {};
}

std::string HandleSkeleton(const OscMessage& m, const VmtMapping& map, VmtState* state, std::vector<Message>& out) {
    const std::string& a = m.address;
    if (!state) return "osc " + a + ": needs driver state";
    if (a == "/VMT/Skeleton/Apply") {
        if (!ArgsMatch(m, 1)) return "osc " + a + ": bad arguments";
        const int index = static_cast<int>(m.args[0].Number());
        SkeletonMessage s;
        s.id = IndexDevice(map, state, index);
        {
            std::lock_guard<std::mutex> lock(state->mu);
            auto it = state->skeletons.find(index);
            if (it == state->skeletons.end()) return "osc " + a + ": no bones sent for index " + std::to_string(index);
            s.rotations.assign(it->second.rot.begin(), it->second.rot.end());
            s.positions.assign(it->second.pos.begin(), it->second.pos.end());
            for (size_t i = 0; i < kSkeletonBoneCount; ++i) {
                if (!it->second.set[i]) s.rotations[i] = Quat{}; // bones never sent: identity
            }
        }
        out.emplace_back(std::move(s));
        return {};
    }
    if (a == "/VMT/Skeleton/Scalar") {
        if (!ArgsMatch(m, 3)) return "osc " + a + ": bad arguments";
        const int index = static_cast<int>(m.args[0].Number());
        const int set = static_cast<int>(m.args[1].Number());
        const double value = std::clamp(m.args[2].Number(), 0.0, 1.0);
        if (m.args.size() >= 4 && m.args[3].IsNumber() && m.args[3].Number() != 0) {
            return "osc " + a + ": only mode 0 is supported";
        }
        if (set < 0 || set > 5) return "osc " + a + ": bone set out of range";
        if (set == 0) return {}; // root + wrist: no finger to curl
        SkeletonMessage s;
        s.id = IndexDevice(map, state, index);
        std::lock_guard<std::mutex> lock(state->mu);
        auto& curl = state->curls.try_emplace(index, std::array<float, 5>{0, 0, 0, 0, 0}).first->second;
        curl[set - 1] = static_cast<float>(1.0 - value); // VMT: 0 = fist, 1 = open hand
        s.curl = curl;
        out.emplace_back(std::move(s));
        return {};
    }
    const bool unity = EndsWith(a, "/Unity"), euler = EndsWith(a, "/UEuler");
    if (!unity && !euler && !EndsWith(a, "/Driver")) return "osc " + a + ": unsupported address";
    const size_t n = euler ? 8 : 9;
    if (!ArgsMatch(m, n)) return "osc " + a + ": bad arguments";
    const int index = static_cast<int>(m.args[0].Number());
    const int bone = static_cast<int>(m.args[1].Number());
    if (bone < 0 || bone >= static_cast<int>(kSkeletonBoneCount)) return "osc " + a + ": bone index out of range";
    Vec3 pos;
    Quat rot;
    if (auto e = ReadPose(m, 2, unity, euler, pos, rot); !e.empty()) return "osc " + a + ": " + e;
    std::lock_guard<std::mutex> lock(state->mu);
    auto& b = state->skeletons[index];
    b.rot[bone] = rot;
    b.pos[bone] = pos;
    b.set[bone] = true;
    return {};
}

bool ParseIpv4(const std::string& s, uint32_t& out) {
    out = 0;
    int parts = 0;
    uint32_t value = 0;
    bool digit = false;
    for (size_t i = 0; i <= s.size(); ++i) {
        const char c = i < s.size() ? s[i] : '.';
        if (c >= '0' && c <= '9') {
            value = value * 10 + static_cast<uint32_t>(c - '0');
            if (value > 255) return false;
            digit = true;
        } else if (c == '.' && digit && parts < 4) {
            out = (out << 8) | value;
            ++parts;
            value = 0;
            digit = false;
        } else {
            return false;
        }
    }
    return parts == 4;
}

std::string HandleControl(const OscMessage& m, VmtState* state, std::vector<Message>& out) {
    const std::string& a = m.address;
    if (!state) return "osc " + a + ": needs driver state";
    std::lock_guard<std::mutex> lock(state->mu);
    state->active = true;
    if (a == "/VMT/SetRoomMatrix" || a == "/VMT/SetRoomMatrix/Temporary" || a == "/VMT/Set/RoomMatrix" ||
        a == "/VMT/Set/RoomMatrix/Temporary") {
        if (!ArgsMatch(m, 12)) return "osc " + a + ": bad arguments";
        for (size_t i = 0; i < 12; ++i) state->room[i] = m.args[i].Number();
        return {};
    }
    if (a == "/VMT/Reset") {
        // Every device VMT drove: pose invalid (devices stay registered), buffers cleared.
        for (const auto& entry : state->index_ids) {
            PoseMessage p;
            p.id = entry.second;
            p.valid = false;
            out.emplace_back(std::move(p));
        }
        state->skeletons.clear();
        state->curls.clear();
        return {};
    }
    if (a == "/VMT/Subscribe/Device" || a == "/VMT/Unsubscribe/Device") {
        if (m.args.empty() || m.args[0].type != 's') return "osc " + a + ": bad arguments";
        if (a == "/VMT/Subscribe/Device") {
            state->subscriptions.insert(m.args[0].s);
        } else {
            state->subscriptions.erase(m.args[0].s);
        }
        return {};
    }
    if (a == "/VMT/Get/Devices/List") {
        state->devices_list_requested = true;
        return {};
    }
    if (a == "/VMT/Set/Destination") {
        uint32_t ip = 0;
        if (m.args.size() < 2 || m.args[0].type != 's' || !m.args[1].IsNumber() || !ParseIpv4(m.args[0].s, ip)) {
            return "osc " + a + ": bad arguments";
        }
        const double port = m.args[1].Number();
        if (port < 1 || port > 65535) return "osc " + a + ": port out of range";
        state->dest_ip = ip;
        state->dest_port = static_cast<uint16_t>(port);
        return {};
    }
    if (a == "/VMT/LoadSetting") {
        state->logs.emplace_back(0, "Setting Loaded");
        return {};
    }
    // Accepted for compatibility; nothing to do in this driver.
    if (a == "/VMT/SetAutoPoseUpdate" || a == "/VMT/Set/AutoPoseUpdate" || a == "/VMT/SetDiagLog" ||
        a == "/VMT/Set/DiagLog" || a == "/VMT/RequestRestart" || a == "/VMT/Config" || a == "/VMT/Debug") {
        return {};
    }
    return "osc " + a.substr(0, 64) + ": unsupported address";
}

void Pad4(std::string& out, const std::string& s) {
    out += s;
    out.push_back('\0');
    while (out.size() % 4) out.push_back('\0');
}

void PutU32(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>(v >> 24));
    out.push_back(static_cast<char>(v >> 16));
    out.push_back(static_cast<char>(v >> 8));
    out.push_back(static_cast<char>(v));
}

} // namespace

Vec3 UnityToDriver(const Vec3& p) { return {p.x, p.y, -p.z}; }

Quat UnityToDriver(const Quat& q) { return {q.w, -q.x, -q.y, q.z}; }

bool LooksLikeOsc(std::string_view data) { return !data.empty() && (data[0] == '/' || data[0] == '#'); }

std::string EncodeOscMessage(const std::string& address, const std::vector<OscOutArg>& args) {
    std::string out, tags = ",", body;
    Pad4(out, address);
    for (const auto& a : args) {
        if (const auto* i = std::get_if<int32_t>(&a)) {
            tags += 'i';
            PutU32(body, static_cast<uint32_t>(*i));
        } else if (const auto* f = std::get_if<float>(&a)) {
            tags += 'f';
            uint32_t u;
            std::memcpy(&u, f, 4);
            PutU32(body, u);
        } else {
            tags += 's';
            Pad4(body, std::get<std::string>(a));
        }
    }
    Pad4(out, tags);
    return out + body;
}

ParseResult ParseOscPacket(std::string_view data, const VmtMapping& mapping, const ParseLimits& limits,
                           VmtState* state) {
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
        if (StartsWith(a, "/VMT/Room/") || StartsWith(a, "/VMT/Raw/") || StartsWith(a, "/VMT/Joint/") ||
            StartsWith(a, "/VMT/Follow/")) {
            err = HandlePose(m, mapping, limits, state, result.messages);
        } else if (StartsWith(a, "/VMT/Input/")) {
            err = HandleInput(m, mapping, state, result.messages);
        } else if (StartsWith(a, "/VMT/Skeleton/")) {
            err = HandleSkeleton(m, mapping, state, result.messages);
        } else if (StartsWith(a, "/VMT/")) {
            err = HandleControl(m, state, result.messages);
        } else {
            err = "osc " + a.substr(0, 64) + ": unsupported address";
        }
        if (!err.empty()) result.errors.push_back(std::move(err));
    }
    return result;
}

} // namespace vhmd
