#include "hmd_device.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "util/log.h"

namespace vhmd {
namespace {

constexpr const char* kDisplaySection = "vhmd_display";

int32_t GetInt(const char* key, int32_t fallback) {
    vr::EVRSettingsError err = vr::VRSettingsError_None;
    const int32_t v = vr::VRSettings()->GetInt32(kDisplaySection, key, &err);
    return err == vr::VRSettingsError_None ? v : fallback;
}

float GetFloat(const char* key, float fallback) {
    vr::EVRSettingsError err = vr::VRSettingsError_None;
    const float v = vr::VRSettings()->GetFloat(kDisplaySection, key, &err);
    return err == vr::VRSettingsError_None && std::isfinite(v) ? v : fallback;
}

} // namespace

DisplayConfig DisplayConfig::FromSettings() {
    DisplayConfig c;
    c.window_x = GetInt("windowX", c.window_x);
    c.window_y = GetInt("windowY", c.window_y);
    c.window_width = static_cast<uint32_t>(std::max(64, GetInt("windowWidth", static_cast<int32_t>(c.window_width))));
    c.window_height = static_cast<uint32_t>(std::max(64, GetInt("windowHeight", static_cast<int32_t>(c.window_height))));
    c.render_width = static_cast<uint32_t>(std::max(64, GetInt("renderWidth", static_cast<int32_t>(c.render_width))));
    c.render_height = static_cast<uint32_t>(std::max(64, GetInt("renderHeight", static_cast<int32_t>(c.render_height))));
    c.fov_deg = std::clamp(GetFloat("fovDegrees", c.fov_deg), 30.0f, 170.0f);
    c.display_frequency = std::clamp(GetFloat("displayFrequency", c.display_frequency), 30.0f, 240.0f);
    c.seconds_vsync_to_photons = GetFloat("secondsFromVsyncToPhotons", c.seconds_vsync_to_photons);
    c.ipd_meters = std::clamp(GetFloat("ipdMeters", c.ipd_meters), 0.04f, 0.09f);
    return c;
}

void HmdDisplayComponent::GetWindowBounds(int32_t* x, int32_t* y, uint32_t* width, uint32_t* height) {
    *x = cfg_.window_x;
    *y = cfg_.window_y;
    *width = cfg_.window_width;
    *height = cfg_.window_height;
}

void HmdDisplayComponent::GetRecommendedRenderTargetSize(uint32_t* width, uint32_t* height) {
    *width = cfg_.render_width;
    *height = cfg_.render_height;
}

void HmdDisplayComponent::GetEyeOutputViewport(vr::EVREye eye, uint32_t* x, uint32_t* y, uint32_t* width,
                                               uint32_t* height) {
    *y = 0;
    *width = cfg_.window_width / 2;
    *height = cfg_.window_height;
    *x = eye == vr::Eye_Left ? 0 : cfg_.window_width / 2;
}

void HmdDisplayComponent::GetProjectionRaw(vr::EVREye /*eye*/, float* left, float* right, float* top, float* bottom) {
    const float t = std::tan(cfg_.fov_deg * 0.5f * 3.14159265f / 180.0f);
    *left = -t;
    *right = t;
    *top = -t;
    *bottom = t;
}

vr::DistortionCoordinates_t HmdDisplayComponent::ComputeDistortion(vr::EVREye /*eye*/, float u, float v) {
    vr::DistortionCoordinates_t c{};
    c.rfRed[0] = c.rfGreen[0] = c.rfBlue[0] = u;
    c.rfRed[1] = c.rfGreen[1] = c.rfBlue[1] = v;
    return c;
}

bool HmdDisplayComponent::ComputeInverseDistortion(vr::HmdVector2_t*, vr::EVREye, uint32_t, float, float) {
    return false; // let SteamVR estimate from ComputeDistortion (identity)
}

HmdDevice::HmdDevice(std::shared_ptr<DeviceState> state, const DisplayConfig& display)
    : VirtualDevice(std::move(state)), display_cfg_(display), head_model_(State().Config().head_model), display_(display) {}

void* HmdDevice::GetComponent(const char* name_and_version) {
    if (std::strcmp(name_and_version, vr::IVRDisplayComponent_Version) == 0) {
        return static_cast<vr::IVRDisplayComponent*>(&display_);
    }
    return nullptr;
}

void HmdDevice::OnActivate(vr::PropertyContainerHandle_t c) {
    auto* p = vr::VRProperties();
    SetCommonProperties(c, "VHMD Virtual HMD", "generic_hmd");
    SetIcons(c, "headset");
    p->SetFloatProperty(c, vr::Prop_UserIpdMeters_Float, display_cfg_.ipd_meters);
    p->SetFloatProperty(c, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.0f);
    p->SetFloatProperty(c, vr::Prop_DisplayFrequency_Float, display_cfg_.display_frequency);
    p->SetFloatProperty(c, vr::Prop_SecondsFromVsyncToPhotons_Float, display_cfg_.seconds_vsync_to_photons);
    p->SetBoolProperty(c, vr::Prop_IsOnDesktop_Bool, false);
    p->SetBoolProperty(c, vr::Prop_DisplayDebugMode_Bool, true);
    p->SetStringProperty(c, vr::Prop_InputProfilePath_String, "{vhmd}/input/vhmd_hmd_profile.json");
    p->SetStringProperty(c, vr::Prop_ControllerType_String, "vhmd_hmd");

    vr::VRDriverInput()->CreateBooleanComponent(c, "/input/system/click", &system_click_);

    if (State().Config().eye_tracking) {
        const auto err = vr::VRDriverInput()->CreateEyeTrackingComponent(c, "/eyetracking", &eye_tracking_);
        if (err == vr::VRInputError_None) {
            p->SetBoolProperty(c, vr::Prop_SupportsXrEyeGazeInteraction_Bool, true);
        } else {
            Log(LogLevel::Warning, "CreateEyeTrackingComponent failed (%d); gaze messages will be ignored", err);
            eye_tracking_ = vr::k_ulInvalidInputComponentHandle;
        }
    }
}

void HmdDevice::OnInput(const std::string& name, float value) {
    if ((name == "system" || name == "/input/system/click") && system_click_ != vr::k_ulInvalidInputComponentHandle) {
        vr::VRDriverInput()->UpdateBooleanComponent(system_click_, value >= 0.5f, 0.0);
    }
}

void HmdDevice::OnGaze(const GazeMessage& m) {
    if (eye_tracking_ == vr::k_ulInvalidInputComponentHandle) return;
    vr::VREyeTrackingData_t d{};
    d.bActive = true;
    d.bValid = m.valid;
    d.bTracked = m.valid;
    d.vGazeOrigin = {{static_cast<float>(m.origin.x), static_cast<float>(m.origin.y), static_cast<float>(m.origin.z)}};
    d.vGazeTarget = {{static_cast<float>(m.target.x), static_cast<float>(m.target.y), static_cast<float>(m.target.z)}};
    vr::VRDriverInput()->UpdateEyeTrackingComponent(eye_tracking_, &d, 0.0);
}

} // namespace vhmd
