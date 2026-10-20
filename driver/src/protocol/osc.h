// Optional VMT (Virtual Motion Tracker) compatible OSC receiver.
// Follows the VMT 0.15 API (see docs/protocol.md for what is mapped and how).
// OSC datagrams are told apart from JSON by their first byte ('/' or '#').
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "protocol/messages.h"

namespace vhmd {

struct VmtMapping {
    // VMT tracker index -> device id. Index i defaults to "tracker_<i>".
    std::vector<std::string> index_to_id;
    // Device ids used when VMT `enable` is 2/5 (left) or 3/6 (right).
    std::string left_hand_id = "left_hand";
    std::string right_hand_id = "right_hand";
};

// State the VMT API keeps between packets: room matrix, skeleton buffers,
// subscriptions and where /VMT/Out/... messages go. The receive thread writes
// it while parsing; the update thread reads it to send outputs (hence the mutex).
struct VmtState {
    struct Bones {
        std::array<Quat, kSkeletonBoneCount> rot{};
        std::array<Vec3, kSkeletonBoneCount> pos{};
        std::array<bool, kSkeletonBoneCount> set{};
    };

    std::mutex mu;
    // Room (standing) space -> driver space, row-major 3x4 (VMT m1..m12).
    std::array<double, 12> room{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::map<int, std::string> index_ids;          // VMT index -> device id, from pose messages
    std::map<int, Bones> skeletons;                 // /VMT/Skeleton/* buffers
    std::map<int, std::array<float, 5>> curls;      // /VMT/Skeleton/Scalar, thumb..pinky
    std::set<std::string> subscriptions;            // /VMT/Subscribe/Device serials
    bool devices_list_requested = false;            // /VMT/Get/Devices/List
    std::vector<std::pair<int, std::string>> logs;  // /VMT/Out/Log replies (stat, message)
    uint32_t dest_ip = 0x7F000001;                  // 127.0.0.1, host order
    uint16_t dest_port = 39571;                     // VMT's output port
    bool active = false;                            // any VMT message seen: outputs on
};

bool LooksLikeOsc(std::string_view data);

ParseResult ParseOscPacket(std::string_view data, const VmtMapping& mapping, const ParseLimits& limits = {},
                           VmtState* state = nullptr);

// Unity (left handed, +Z forward) <-> SteamVR driver space (right handed,
// -Z forward): mirror the Z axis.
Vec3 UnityToDriver(const Vec3& p);
Quat UnityToDriver(const Quat& q);

// OSC encoding for /VMT/Out/... messages.
using OscOutArg = std::variant<int32_t, float, std::string>;
std::string EncodeOscMessage(const std::string& address, const std::vector<OscOutArg>& args);

} // namespace vhmd
