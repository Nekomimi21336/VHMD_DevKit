#include "protocol/parser.h"

#include <cmath>

#include <nlohmann/json.hpp>

namespace vhmd {
namespace {

using json = nlohmann::json;

// Each helper returns an empty string on success, or an error description.

std::string ReadNumber(const json& j, double& out) {
    if (!j.is_number()) return "not a number";
    out = j.get<double>();
    if (!std::isfinite(out)) return "not finite";
    return {};
}

std::string ReadVec3(const json& j, Vec3& out) {
    if (!j.is_array() || j.size() != 3) return "expected [x,y,z]";
    double v[3];
    for (size_t i = 0; i < 3; ++i) {
        if (auto e = ReadNumber(j[i], v[i]); !e.empty()) return e;
    }
    out = {v[0], v[1], v[2]};
    return {};
}

std::string ReadQuat(const json& j, Quat& out) {
    if (!j.is_array() || j.size() < 4) return "expected [w,x,y,z]";
    double v[4];
    for (size_t i = 0; i < 4; ++i) {
        if (auto e = ReadNumber(j[i], v[i]); !e.empty()) return e;
    }
    Quat q{v[0], v[1], v[2], v[3]};
    if (!Normalize(q)) return "degenerate quaternion";
    out = q;
    return {};
}

bool WithinAbs(const Vec3& v, double limit) {
    return std::abs(v.x) <= limit && std::abs(v.y) <= limit && std::abs(v.z) <= limit;
}

std::string ReadId(const json& obj, std::string& out) {
    auto it = obj.find("id");
    if (it == obj.end() || !it->is_string()) return "missing string field 'id'";
    out = it->get<std::string>();
    if (out.empty() || out.size() > 64) return "invalid 'id'";
    return {};
}

std::string ReadFloatArray5(const json& j, std::array<float, 5>& out, float lo, float hi) {
    if (!j.is_array() || j.size() != 5) return "expected 5 numbers";
    for (size_t i = 0; i < 5; ++i) {
        double d = 0.0;
        if (auto e = ReadNumber(j[i], d); !e.empty()) return e;
        if (d < lo || d > hi) return "value out of range";
        out[i] = static_cast<float>(d);
    }
    return {};
}

std::string ParsePose(const json& obj, const ParseLimits& lim, Message& out) {
    PoseMessage m;
    if (auto e = ReadId(obj, m.id); !e.empty()) return e;
    auto valid = obj.find("valid");
    if (valid != obj.end() && valid->is_boolean()) m.valid = valid->get<bool>();

    auto pos = obj.find("pos");
    auto rot = obj.find("rot");
    if (pos == obj.end() || rot == obj.end()) {
        if (!m.valid) { // {"type":"pose","id":..,"valid":false} is allowed
            out = std::move(m);
            return {};
        }
        return "missing 'pos' or 'rot'";
    }
    if (auto e = ReadVec3(*pos, m.pos); !e.empty()) return "pos: " + e;
    if (!WithinAbs(m.pos, lim.max_abs_position)) return "pos: out of range";
    if (auto e = ReadQuat(*rot, m.rot); !e.empty()) return "rot: " + e;

    for (const char* key : {"vel", "angvel"}) {
        auto it = obj.find(key);
        if (it == obj.end() || it->is_null()) continue;
        Vec3 v;
        if (auto e = ReadVec3(*it, v); !e.empty()) return std::string(key) + ": " + e;
        if (!WithinAbs(v, lim.max_abs_velocity)) return std::string(key) + ": out of range";
        (key[0] == 'v' ? m.vel : m.angvel) = v;
    }
    auto ts = obj.find("ts");
    if (ts != obj.end() && ts->is_number()) {
        double t = ts->get<double>();
        if (std::isfinite(t)) m.ts = t;
    }
    out = std::move(m);
    return {};
}

std::string ReadInputValue(const json& j, float limit, float& out) {
    if (j.is_boolean()) {
        out = j.get<bool>() ? 1.0f : 0.0f;
        return {};
    }
    double d = 0.0;
    if (auto e = ReadNumber(j, d); !e.empty()) return e;
    if (std::abs(d) > limit) return "out of range";
    out = static_cast<float>(d);
    return {};
}

std::string ParseInput(const json& obj, const ParseLimits& lim, Message& out) {
    InputMessage m;
    if (auto e = ReadId(obj, m.id); !e.empty()) return e;
    auto name = obj.find("name");
    auto values = obj.find("values");
    if (name != obj.end()) {
        if (!name->is_string() || name->get_ref<const std::string&>().empty()) return "invalid 'name'";
        auto value = obj.find("value");
        if (value == obj.end()) return "missing 'value'";
        float v;
        if (auto e = ReadInputValue(*value, lim.max_abs_input, v); !e.empty()) return "value: " + e;
        m.values.emplace_back(name->get<std::string>(), v);
    }
    if (values != obj.end()) {
        if (!values->is_object()) return "'values' must be an object";
        for (auto it = values->begin(); it != values->end(); ++it) {
            float v;
            if (auto e = ReadInputValue(it.value(), lim.max_abs_input, v); !e.empty()) {
                return "values." + it.key() + ": " + e;
            }
            m.values.emplace_back(it.key(), v);
        }
    }
    if (m.values.empty()) return "no input values";
    out = std::move(m);
    return {};
}

std::string ParseSkeleton(const json& obj, const ParseLimits& lim, Message& out) {
    SkeletonMessage m;
    if (auto e = ReadId(obj, m.id); !e.empty()) return e;

    auto bones = obj.find("bones");
    if (bones != obj.end()) {
        if (!bones->is_array() || bones->size() != kSkeletonBoneCount) return "'bones' must have 31 entries";
        for (const auto& b : *bones) {
            Quat q;
            if (auto e = ReadQuat(b, q); !e.empty()) return "bones: " + e;
            m.rotations.push_back(q);
            // Optional inline position: [w,x,y,z,px,py,pz]
            if (b.size() == 7) {
                Vec3 p;
                if (auto e = ReadVec3(json::array({b[4], b[5], b[6]}), p); !e.empty()) return "bones: " + e;
                m.positions.push_back(p);
            }
        }
        if (!m.positions.empty() && m.positions.size() != kSkeletonBoneCount) {
            return "bones: inline positions must be given for all bones or none";
        }
    }
    auto positions = obj.find("positions");
    if (positions != obj.end()) {
        if (!positions->is_array() || positions->size() != kSkeletonBoneCount) return "'positions' must have 31 entries";
        m.positions.clear();
        for (const auto& p : *positions) {
            Vec3 v;
            if (auto e = ReadVec3(p, v); !e.empty()) return "positions: " + e;
            if (!WithinAbs(v, 1.0)) return "positions: out of range";
            m.positions.push_back(v);
        }
    }
    if (!m.positions.empty() && m.rotations.empty()) return "'positions' requires 'bones'";

    auto curl = obj.find("curl");
    if (curl != obj.end()) {
        std::array<float, 5> c{};
        if (auto e = ReadFloatArray5(*curl, c, 0.0f, 1.0f); !e.empty()) return "curl: " + e;
        m.curl = c;
    }
    auto splay = obj.find("splay");
    if (splay != obj.end()) {
        std::array<float, 5> s{};
        if (auto e = ReadFloatArray5(*splay, s, -1.0f, 1.0f); !e.empty()) return "splay: " + e;
        m.splay = s;
    }
    if (m.rotations.empty() && !m.curl) return "skeleton needs 'bones' or 'curl'";
    (void)lim;
    out = std::move(m);
    return {};
}

std::string ParseGaze(const json& obj, const ParseLimits& lim, Message& out) {
    GazeMessage m;
    if (auto e = ReadId(obj, m.id); !e.empty()) return e;
    auto valid = obj.find("valid");
    if (valid != obj.end() && valid->is_boolean()) m.valid = valid->get<bool>();
    auto origin = obj.find("origin");
    if (origin != obj.end()) {
        if (auto e = ReadVec3(*origin, m.origin); !e.empty()) return "origin: " + e;
    }
    auto target = obj.find("target");
    auto dir = obj.find("dir");
    if (target != obj.end()) {
        if (auto e = ReadVec3(*target, m.target); !e.empty()) return "target: " + e;
    } else if (dir != obj.end()) {
        Vec3 d;
        if (auto e = ReadVec3(*dir, d); !e.empty()) return "dir: " + e;
        if (Length(d) < 1e-9) return "dir: zero length";
        m.target = m.origin + d * (1.0 / Length(d));
    } else if (m.valid) {
        return "gaze needs 'target' or 'dir'";
    }
    if (!WithinAbs(m.origin, lim.max_abs_position) || !WithinAbs(m.target, lim.max_abs_position)) {
        return "gaze: out of range";
    }
    out = std::move(m);
    return {};
}

std::string ParseConfig(const json& obj, Message& out) {
    ConfigMessage m;
    auto op = obj.find("op");
    if (op == obj.end() || !op->is_string()) return "missing string field 'op'";
    m.op = op->get<std::string>();
    if (auto id = obj.find("id"); id != obj.end() && id->is_string()) m.id = id->get<std::string>();
    if (auto role = obj.find("role"); role != obj.end() && role->is_string()) m.role = role->get<std::string>();
    if (auto mode = obj.find("mode"); mode != obj.end() && mode->is_string()) m.mode = mode->get<std::string>();
    if (auto seq = obj.find("seq"); seq != obj.end() && seq->is_number()) m.seq = seq->get<double>();
    out = std::move(m);
    return {};
}

std::string ParseOne(const json& obj, const ParseLimits& lim, Message& out) {
    if (!obj.is_object()) return "message is not an object";
    auto type = obj.find("type");
    if (type == obj.end() || !type->is_string()) return "missing string field 'type'";
    const auto& t = type->get_ref<const std::string&>();
    if (t == "pose") return ParsePose(obj, lim, out);
    if (t == "input") return ParseInput(obj, lim, out);
    if (t == "skeleton") return ParseSkeleton(obj, lim, out);
    if (t == "gaze") return ParseGaze(obj, lim, out);
    if (t == "config") return ParseConfig(obj, out);
    return "unknown type '" + t.substr(0, 32) + "'";
}

} // namespace

bool LooksLikeJson(std::string_view data) {
    for (char c : data) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        return c == '{' || c == '[';
    }
    return false;
}

ParseResult ParseJsonPacket(std::string_view data, const ParseLimits& limits) {
    ParseResult result;
    if (data.size() > limits.max_packet_bytes) {
        result.errors.emplace_back("packet too large");
        return result;
    }
    json root = json::parse(data.begin(), data.end(), nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) {
        result.errors.emplace_back("malformed JSON");
        return result;
    }
    auto handle = [&](const json& obj, size_t index) {
        Message m;
        std::string err = ParseOne(obj, limits, m);
        if (err.empty()) {
            result.messages.push_back(std::move(m));
        } else {
            result.errors.push_back("[" + std::to_string(index) + "] " + err);
        }
    };
    try {
        if (root.is_array()) {
            if (root.size() > limits.max_batch) {
                result.errors.emplace_back("batch too large");
                return result;
            }
            for (size_t i = 0; i < root.size(); ++i) handle(root[i], i);
        } else {
            handle(root, 0);
        }
    } catch (const std::exception& e) {
        // Defensive: nlohmann accessors should not throw given the type checks
        // above, but a malformed packet must never take the driver down.
        result.errors.emplace_back(std::string("internal parse error: ") + e.what());
    }
    return result;
}

} // namespace vhmd
