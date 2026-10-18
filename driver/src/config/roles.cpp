#include "config/roles.h"

#include <cctype>

namespace vhmd {
namespace {

struct RoleEntry {
    const char* short_name;
    const char* steamvr_role;
};

// Role list as found in SteamVR's own tracker profile (drivers/htc/resources/
// input/vive_tracker_profile.json, SteamVR 2.17.10).
constexpr RoleEntry kRoles[] = {
    {"none", "TrackerRole_None"},
    {"handed", "TrackerRole_Handed"},
    {"left_foot", "TrackerRole_LeftFoot"},
    {"right_foot", "TrackerRole_RightFoot"},
    {"left_shoulder", "TrackerRole_LeftShoulder"},
    {"right_shoulder", "TrackerRole_RightShoulder"},
    {"left_elbow", "TrackerRole_LeftElbow"},
    {"right_elbow", "TrackerRole_RightElbow"},
    {"left_knee", "TrackerRole_LeftKnee"},
    {"right_knee", "TrackerRole_RightKnee"},
    {"left_wrist", "TrackerRole_LeftWrist"},
    {"right_wrist", "TrackerRole_RightWrist"},
    {"left_ankle", "TrackerRole_LeftAnkle"},
    {"right_ankle", "TrackerRole_RightAnkle"},
    {"waist", "TrackerRole_Waist"},
    {"chest", "TrackerRole_Chest"},
    {"camera", "TrackerRole_Camera"},
    {"keyboard", "TrackerRole_Keyboard"},
};

// Lowercase and drop '_', '-' and spaces: "TrackerRole_LeftFoot" -> "trackerroleleftfoot".
std::string Squash(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (c == '_' || c == '-' || c == ' ') continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

} // namespace

std::optional<RoleInfo> NormalizeRole(std::string_view name) {
    std::string key = Squash(name);
    for (const char* prefix : {"trackerrole", "vivetracker"}) {
        const std::string p(prefix);
        if (key.rfind(p, 0) == 0) {
            key = key.substr(p.size());
            break;
        }
    }
    for (const auto& r : kRoles) {
        if (Squash(r.short_name) == key) return RoleInfo{r.short_name, r.steamvr_role};
    }
    return std::nullopt;
}

std::vector<RoleInfo> AllRoles() {
    std::vector<RoleInfo> out;
    for (const auto& r : kRoles) out.push_back({r.short_name, r.steamvr_role});
    return out;
}

} // namespace vhmd
