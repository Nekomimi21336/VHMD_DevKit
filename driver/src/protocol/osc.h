// Optional VMT (Virtual Motion Tracker) compatible OSC receiver.
// Only a subset of the VMT API is understood; see docs/protocol.md.
// OSC datagrams are told apart from JSON by their first byte ('/' or '#').
#pragma once

#include <string>
#include <string_view>
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

bool LooksLikeOsc(std::string_view data);

ParseResult ParseOscPacket(std::string_view data, const VmtMapping& mapping, const ParseLimits& limits = {});

// Unity (left handed, +Z forward) <-> SteamVR driver space (right handed,
// -Z forward): mirror the Z axis.
Vec3 UnityToDriver(const Vec3& p);
Quat UnityToDriver(const Quat& q);

} // namespace vhmd
