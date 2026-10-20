#include "tracker_device.h"

#include <algorithm>

#include "util/log.h"

namespace vhmd {
namespace {

struct TrackerInput {
    const char* name;
    const char* path;
    bool boolean;
    bool two_sided;
};

constexpr TrackerInput kTrackerInputs[] = {
    {"system", "/input/system/click", true, false},
    {"menu", "/input/menu/click", true, false},
    {"grip", "/input/grip/click", true, false},
    {"trigger", "/input/trigger/value", false, false},
    {"trigger_click", "/input/trigger/click", true, false},
    {"trackpad_x", "/input/trackpad/x", false, true},
    {"trackpad_y", "/input/trackpad/y", false, true},
    {"trackpad_click", "/input/trackpad/click", true, false},
    {"trackpad_touch", "/input/trackpad/touch", true, false},
};

} // namespace

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
    RefreshLabel();
    return {};
}

void TrackerDevice::OnActivate(vr::PropertyContainerHandle_t c) {
    auto* p = vr::VRProperties();
    SetCommonProperties(c, "VHMD Virtual Tracker", "generic_tracker");
    SetIcons(c, "tracker");
    container_ = c;
    p->SetStringProperty(c, vr::Prop_ControllerType_String, "vhmd_tracker");
    p->SetStringProperty(c, vr::Prop_InputProfilePath_String, "{vhmd}/input/vhmd_tracker_profile.json");
    p->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32, vr::TrackedControllerRole_OptOut);

    if (apply_role_) {
        if (auto role = NormalizeRole(State().Config().role); role && role->short_name != "none") {
            const std::string err = ApplyRole(*role, /*overwrite=*/false);
            if (!err.empty()) Log(LogLevel::Warning, "%s: %s", Id().c_str(), err.c_str());
        }
    }
    RefreshLabel();

    auto* in = vr::VRDriverInput();
    components_.clear();
    for (const auto& d : kTrackerInputs) {
        Component comp{vr::k_ulInvalidInputComponentHandle, d.boolean, d.two_sided};
        const auto err = d.boolean ? in->CreateBooleanComponent(c, d.path, &comp.handle)
                                   : in->CreateScalarComponent(c, d.path, &comp.handle, vr::VRScalarType_Absolute,
                                                               d.two_sided ? vr::VRScalarUnits_NormalizedTwoSided
                                                                           : vr::VRScalarUnits_NormalizedOneSided);
        if (err != vr::VRInputError_None) Log(LogLevel::Warning, "%s: failed to create %s (%d)", Id().c_str(), d.path, err);
        components_[d.name] = comp;
    }
}

void TrackerDevice::OnInput(const std::string& name, float value) {
    std::string key = name;
    for (const auto& d : kTrackerInputs) {
        if (name == d.path) key = d.name;
    }
    if (key == "grip_click") key = "grip";
    const auto it = components_.find(key);
    if (it == components_.end() || it->second.handle == vr::k_ulInvalidInputComponentHandle) {
        Log(LogLevel::Debug, "%s: unknown input '%s'", Id().c_str(), name.c_str());
        return;
    }
    auto* in = vr::VRDriverInput();
    if (it->second.boolean) {
        in->UpdateBooleanComponent(it->second.handle, value >= 0.5f, 0.0);
    } else {
        in->UpdateScalarComponent(it->second.handle, std::clamp(value, it->second.two_sided ? -1.0f : 0.0f, 1.0f), 0.0);
    }
}

std::string TrackerDevice::CurrentRole() {
    // The role SteamVR uses (the user may have changed it in "Manage Trackers").
    char current[128] = {};
    vr::EVRSettingsError err = vr::VRSettingsError_None;
    vr::VRSettings()->GetString(vr::k_pch_Trackers_Section, RoleSettingsKey().c_str(), current, sizeof(current), &err);
    if (err == vr::VRSettingsError_None && current[0] != 0) {
        if (auto role = NormalizeRole(current)) return role->short_name;
    }
    auto role = NormalizeRole(State().Config().role);
    return role ? role->short_name : "none";
}

void TrackerDevice::RefreshRenderModel() {
    {
        std::lock_guard<std::mutex> lock(label_mu_);
        shown_role_.clear();
    }
    RefreshLabel();
}

void TrackerDevice::RefreshLabel() {
    std::lock_guard<std::mutex> lock(label_mu_);
    const std::string role = CurrentRole();
    if (role == shown_role_ || container_ == vr::k_ulInvalidPropertyContainer) return;
    shown_role_ = role;
    SetLabel(container_, ModelShape::Tracker, TrackerLabel(role, Id()));
    const std::string model = "VHMD Tracker " + TrackerLabel(role, Id()).title + " (" + Id() + ")";
    vr::VRProperties()->SetStringProperty(container_, vr::Prop_ModelNumber_String, model.c_str());
}

bool TrackerDevice::HandleEvent(const vr::VREvent_t& event) {
    if (event.eventType == vr::VREvent_TrackersSectionSettingChanged && container_ != vr::k_ulInvalidPropertyContainer) {
        RefreshLabel();
    }
    return false;
}

} // namespace vhmd
