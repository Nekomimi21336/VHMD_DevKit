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
    std::optional<Vec3> acc;    // linear acceleration (m/s^2)
    std::optional<Vec3> angacc; // angular acceleration (rad/s^2)
    // How old the sample already is when sent (seconds). SteamVR then predicts
    // from the time the sample was taken (DriverPose_t::poseTimeOffset).
    std::optional<double> age;
    std::optional<double> ts; // sender timestamp (unix seconds), diagnostics only
    // Pose relative to another device (VMT Joint / Follow): serial of the parent
    // ("HMD" = the active HMD). Joint: pos/rot are in the parent's frame.
    // Follow: pos is added to the parent's position, rot is absolute.
    std::string parent;
    bool follow = false;
    // false => the sender explicitly reports the device as not tracked.
    bool valid = true;
    std::string src; // sender name ("src"), for control ownership; "" = unnamed
};

struct InputMessage {
    std::string id;
    // name -> value. Booleans are sent as 0/1.
    std::vector<std::pair<std::string, float>> values;
    std::string src;
};

struct SkeletonMessage {
    std::string id;
    // Either explicit bones (31 rotations, optional 31 positions) ...
    std::vector<Quat> rotations;
    std::vector<Vec3> positions;
    // ... or finger curl/splay (thumb, index, middle, ring, pinky).
    std::optional<std::array<float, 5>> curl;
    std::optional<std::array<float, 5>> splay;
    std::string src;
};

struct GazeMessage {
    std::string id;
    Vec3 origin{};
    Vec3 target{};
    bool valid = true;
    std::string src;
};

struct ConfigMessage {
    std::string op;   // "list", "ping", "set_role", "reset", ...
    std::string id;   // optional device id argument
    std::string role; // optional role argument
    std::string mode; // optional mode argument ("hand_mode")
    std::optional<double> hz; // optional number ("refresh_rate")
    // "display_sync": the headset's frame wait (circular mean), its display
    // period, the wanted wait (ms) and how consistent the measurement was (0..1).
    std::optional<double> slack_ms, period_ms, target_ms, weight;
    std::optional<double> seq; // echoed back in replies
    // "control": the sender that drives the devices ("" = anyone) and, optionally,
    // which devices (none = all). Without "source" the op only reports.
    std::optional<std::string> source;
    std::vector<std::string> devices;
};

using Message = std::variant<PoseMessage, InputMessage, SkeletonMessage, GazeMessage, ConfigMessage>;

struct ParseLimits {
    double max_abs_position = 1000.0;  // meters
    double max_abs_velocity = 100.0;   // m/s and rad/s
    double max_abs_acceleration = 1000.0; // m/s^2 and rad/s^2
    double max_pose_age = 0.5;         // s
    float max_abs_input = 1.0f;
    size_t max_batch = 256;
    size_t max_packet_bytes = 65507;
};

struct ParseResult {
    std::vector<Message> messages;
    std::vector<std::string> errors; // human readable, one per rejected message
};

} // namespace vhmd
