#include "tracker_device.h"

#include "util/log.h"

namespace vhmd {

TrackerDevice::TrackerDevice(std::shared_ptr<DeviceState> state, bool apply_role)
    : VirtualDevice(std::move(state)), apply_role_(apply_role) {}

std::string TrackerDevice::RoleSettingsKey() const {
    // Matches Prop_RegisteredDeviceType_String set in SetCommonProperties().
    return "/devices/vhmd/" + Serial();
}

std::string TrackerDevice::ApplyRole(const RoleInfo& role, bool overwrite) {
    const std::string key = RoleSettingsKey();
    char current[128] = {};
    vr::EVRSettingsError err = vr::VRSettingsError_None;
    vr::VRSettings()->GetString(vr::k_pch_Trackers_Section, key.c_str(), current, sizeof(current), &err);
    if (!overwrite && err == vr::VRSettingsError_None && current[0] != '\0') {
        Log(LogLevel::Info, "%s: keeping existing role %s", Id().c_str(), current);
        return {};
    }
    vr::VRSettings()->SetString(vr::k_pch_Trackers_Section, key.c_str(), role.steamvr_role.c_str(), &err);
    if (err != vr::VRSettingsError_None) return "VRSettings SetString failed (" + std::to_string(err) + ")";
    Log(LogLevel::Info, "%s: role set to %s (%s)", Id().c_str(), role.steamvr_role.c_str(), key.c_str());
    return {};
}

void TrackerDevice::OnActivate(vr::PropertyContainerHandle_t c) {
    auto* p = vr::VRProperties();
    SetCommonProperties(c, "VHMD Virtual Tracker", "generic_tracker");
    SetIcons(c, "tracker");
    p->SetStringProperty(c, vr::Prop_ControllerType_String, "vhmd_tracker");
    p->SetStringProperty(c, vr::Prop_InputProfilePath_String, "{vhmd}/input/vhmd_tracker_profile.json");
    p->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32, vr::TrackedControllerRole_OptOut);

    if (apply_role_) {
        if (auto role = NormalizeRole(State().Config().role); role && role->short_name != "none") {
            const std::string err = ApplyRole(*role, /*overwrite=*/false);
            if (!err.empty()) Log(LogLevel::Warning, "%s: %s", Id().c_str(), err.c_str());
        }
    }
}

} // namespace vhmd
