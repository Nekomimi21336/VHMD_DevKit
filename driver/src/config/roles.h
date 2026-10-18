// Tracker role names. Accepts the short form ("waist"), the SteamVR
// controller-type form ("vive_tracker_waist") and the SteamVR settings form
// ("TrackerRole_Waist"); all normalize to the same RoleInfo.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vhmd {

struct RoleInfo {
    std::string short_name;   // "waist", "left_foot", ...
    std::string steamvr_role; // "TrackerRole_Waist", ...
};

std::optional<RoleInfo> NormalizeRole(std::string_view name);
std::vector<RoleInfo> AllRoles();

} // namespace vhmd
