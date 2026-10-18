// Transport independent message model. Produced by the JSON parser
// (protocol/parser.*) and the VMT compatible OSC parser (protocol/osc.*),
// consumed by the device provider. No OpenVR dependency.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "math/pose_math.h"

namespace vhmd {

inline constexpr size_t kSkeletonBoneCount = 31;

struct PoseMessage {
    std::string id;
    Vec3 pos{};
    Quat rot{};
    std::optional<Vec3> vel;
    std::optional<Vec3> angvel;
    std::optional<double> ts; // sender timestamp (unix seconds), diagnostics only
    // false => the sender explicitly reports the device as not tracked.
    bool valid = true;
};

struct InputMessage {
    std::string id;
    // name -> value. Booleans are sent as 0/1.
    std::vector<std::pair<std::string, float>> values;
};

struct SkeletonMessage {
    std::string id;
    // Either explicit bones (31 rotations, optional 31 positions) ...
    std::vector<Quat> rotations;
    std::vector<Vec3> positions;
    // ... or finger curl/splay (thumb, index, middle, ring, pinky).
    std::optional<std::array<float, 5>> curl;
    std::optional<std::array<float, 5>> splay;
};

struct GazeMessage {
    std::string id;
    Vec3 origin{};
    Vec3 target{};
    bool valid = true;
};

struct ConfigMessage {
    std::string op;   // "list", "ping", "set_role", "reset", ...
    std::string id;   // optional device id argument
    std::string role; // optional role argument
    std::optional<double> seq; // echoed back in replies
};

using Message = std::variant<PoseMessage, InputMessage, SkeletonMessage, GazeMessage, ConfigMessage>;

struct ParseLimits {
    double max_abs_position = 1000.0;  // meters
    double max_abs_velocity = 100.0;   // m/s and rad/s
    float max_abs_input = 1.0f;
    size_t max_batch = 256;
    size_t max_packet_bytes = 65507;
};

struct ParseResult {
    std::vector<Message> messages;
    std::vector<std::string> errors; // human readable, one per rejected message
};

} // namespace vhmd
