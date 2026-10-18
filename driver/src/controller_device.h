// Virtual left/right hand controller: buttons, axes, haptics and a hand
// skeleton. Registered with its own controller type ("vhmd_controller").
#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>

#include "device_base.h"

namespace vhmd {

struct HapticEvent {
    std::string id;
    float duration_s = 0.0f;
    float frequency = 0.0f;
    float amplitude = 0.0f;
};

class ControllerDevice : public VirtualDevice {
public:
    explicit ControllerDevice(std::shared_ptr<DeviceState> state);

    vr::ETrackedDeviceClass DeviceClass() const override { return vr::TrackedDeviceClass_Controller; }
    bool IsLeft() const { return left_; }

    // Returns the haptic request if `event` targets this controller.
    std::optional<HapticEvent> MatchHaptic(const vr::VREvent_t& event) const;

    // Canonical component name for a protocol input name (handles aliases
    // such as "primary", "thumbstick_x", raw "/input/..." paths). Empty if unknown.
    static std::string CanonicalInputName(const std::string& name, bool left);

protected:
    void OnActivate(vr::PropertyContainerHandle_t container) override;
    void OnDeactivate() override;
    void OnInput(const std::string& name, float value) override;
    void OnSkeleton(const SkeletonMessage& m) override;
    void OnTick(Clock::time_point now) override;

private:
    struct Component {
        vr::VRInputComponentHandle_t handle = vr::k_ulInvalidInputComponentHandle;
        bool boolean = false;
    };

    void SetComponent(const std::string& canonical, float value);
    void DeriveClickTouch(const std::string& axis, float value);
    void UpdateSkeletonFromCurls(const std::array<float, 5>& curl, const std::array<float, 5>& splay);
    void PushSkeleton(const vr::VRBoneTransform_t* bones);

    bool left_;
    std::map<std::string, Component> components_;
    std::map<std::string, float> values_;
    std::map<std::string, bool> explicit_; // click/touch driven explicitly by the client
    vr::VRInputComponentHandle_t haptic_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t skeleton_ = vr::k_ulInvalidInputComponentHandle;
    std::optional<Clock::time_point> last_explicit_skeleton_;
    std::array<vr::VRBoneTransform_t, 31> reference_bones_{}; // open hand
};

} // namespace vhmd
