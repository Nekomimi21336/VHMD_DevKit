// Virtual generic tracker (waist, feet, knees, elbows, chest, ...).
#pragma once

#include <mutex>
#include <string>

#include "config/roles.h"
#include "device_base.h"

namespace vhmd {

class TrackerDevice : public VirtualDevice {
public:
    TrackerDevice(std::shared_ptr<DeviceState> state, bool apply_role);

    vr::ETrackedDeviceClass DeviceClass() const override { return vr::TrackedDeviceClass_GenericTracker; }

    // Key used by SteamVR's "trackers" settings section for this device.
    std::string RoleSettingsKey() const;
    // Writes the role into SteamVR's "trackers" settings. `overwrite` = false
    // keeps a role the user already picked in "Manage Trackers".
    std::string ApplyRole(const RoleInfo& role, bool overwrite);
    // Follows role changes made in SteamVR's "Manage Trackers".
    bool HandleEvent(const vr::VREvent_t& event) override;

protected:
    void OnActivate(vr::PropertyContainerHandle_t container) override;

private:
    std::string CurrentRole();
    // Shows the current role on the render model, status icon and model name.
    void RefreshLabel();

    bool apply_role_;
    vr::PropertyContainerHandle_t container_ = vr::k_ulInvalidPropertyContainer;
    std::mutex label_mu_;
    std::string shown_role_;
};

} // namespace vhmd
