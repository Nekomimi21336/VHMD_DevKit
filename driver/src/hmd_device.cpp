#include "hmd_device.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <nlohmann/json.hpp>

#include "util/log.h"
#include "util/platform.h"
#if defined(VHMD_PLATFORM_POSIX) && defined(__linux__)
#include "linux/direct_mode.h"
#define VHMD_HAS_DIRECT_MODE 1
#endif

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

#if !defined(VHMD_HAS_DIRECT_MODE)
// Incomplete elsewhere; only ever null there.
class DirectModeComponent {};
#endif

HmdDevice::HmdDevice(std::shared_ptr<DeviceState> state, const DisplayConfig& display, bool direct_mode,
                     const PreviewConfig& preview)
    : VirtualDevice(std::move(state)), display_cfg_(display), head_model_(State().Config().head_model), display_(display) {
#if defined(VHMD_HAS_DIRECT_MODE)
    if (direct_mode) {
        CaptureSettings cs;
        cs.enabled = preview.enabled;
        cs.path = preview.path;
        cs.fps = preview.fps;
        cs.max_eye_width = preview.max_eye_width;
        cs.vulkan_device = preview.vulkan_device;
        cs.start_active = preview.auto_start;
        direct_ = std::make_unique<DirectModeComponent>(display.display_frequency, cs);
    }
#else
    if (direct_mode) Log(LogLevel::Warning, "display.mode=direct is only implemented on Linux; using window mode");
    (void)preview;
#endif
}

HmdDevice::~HmdDevice() = default;

bool HmdDevice::DirectMode() const { return direct_ != nullptr; }

bool HmdDevice::SetPreviewActive(bool active) {
#if defined(__linux__)
    if (direct_) {
        direct_->SetPreviewActive(active);
        return true;
    }
#endif
    (void)active;
    return false;
}

std::string HmdDevice::DisplayStatusJson() const {
    nlohmann::json j;
    j["mode"] = direct_ ? "direct" : "window";
    j["width"] = display_cfg_.render_width;
    j["height"] = display_cfg_.render_height;
    j["refresh_hz"] = display_cfg_.display_frequency;
#if defined(VHMD_HAS_DIRECT_MODE)
    if (direct_) {
        const DirectModeStats s = direct_->Stats();
        j["swap_sets_created"] = s.swap_sets_created;
        j["swap_sets_alive"] = s.swap_sets_alive;
        j["layers_submitted"] = s.layers_submitted;
        j["presents"] = s.presents;
        j["vsyncs"] = s.vsyncs;
        j["eye_texture"] = {{"width", s.last_width}, {"height", s.last_height}, {"vk_format", s.last_format}};
        if (!s.last_error.empty()) j["error"] = s.last_error;
        j["preview"] = {{"state", s.capture.state},
                        {"submitted", s.capture.submitted},
                        {"written", s.capture.written},
                        {"active", s.capture.active}};
    }
#endif
    return j.dump();
}

void* HmdDevice::GetComponent(const char* name_and_version) {
    if (std::strcmp(name_and_version, vr::IVRDisplayComponent_Version) == 0) {
        return static_cast<vr::IVRDisplayComponent*>(&display_);
    }
#if defined(VHMD_HAS_DIRECT_MODE)
    if (direct_ && std::strcmp(name_and_version, vr::IVRDriverDirectModeComponent_Version) == 0) {
        return static_cast<vr::IVRDriverDirectModeComponent*>(direct_.get());
    }
#endif
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
#if defined(VHMD_HAS_DIRECT_MODE)
    if (direct_) {
        // The driver owns the output: it allocates the eye images, receives
        // them in Present and announces vsyncs itself. Debug (window) mode
        // must stay off or the compositor goes looking for a window again.
        p->SetBoolProperty(c, vr::Prop_DriverDirectModeSendsVsyncEvents_Bool, true);
        p->SetBoolProperty(c, vr::Prop_Hmd_AllowsClientToControlTextureIndex, true);
        p->SetBoolProperty(c, vr::Prop_SupportsXrTextureSets_Bool, true);
        p->SetBoolProperty(c, vr::Prop_HasDriverDirectModeComponent_Bool, true);
        direct_->Start();
        Log(LogLevel::Info, "HMD display: direct mode (eye images handed to the driver)");
    } else
#endif
    {
        p->SetBoolProperty(c, vr::Prop_DisplayDebugMode_Bool, true);
        Log(LogLevel::Info, "HMD display: desktop window (extended mode)");
    }
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

void HmdDevice::OnDeactivate() {
#if defined(VHMD_HAS_DIRECT_MODE)
    if (direct_) direct_->Stop();
#endif
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
