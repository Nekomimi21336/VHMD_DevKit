// Virtual HMD: pose from the network plus a dummy desktop-window display
// (IVRDisplayComponent, same approach as openvr samples/drivers/simplehmd).
#pragma once

#include <memory>

#include "config/driver_config.h"
#include "device_base.h"

namespace vhmd {

struct DisplayConfig {
    int32_t window_x = 0;
    int32_t window_y = 0;
    uint32_t window_width = 1920;
    uint32_t window_height = 1080;
    uint32_t render_width = 1440;
    uint32_t render_height = 1600;
    float fov_deg = 100.0f;           // symmetric horizontal & vertical FOV per eye
    float display_frequency = 90.0f;
    float seconds_vsync_to_photons = 0.011f;
    float ipd_meters = 0.063f;

    // Reads the "vhmd_display" section of the SteamVR settings.
    static DisplayConfig FromSettings();
};

class HmdDisplayComponent : public vr::IVRDisplayComponent {
public:
    explicit HmdDisplayComponent(const DisplayConfig& cfg) : cfg_(cfg) {}

    void GetWindowBounds(int32_t* x, int32_t* y, uint32_t* width, uint32_t* height) override;
    bool IsDisplayOnDesktop() override { return on_desktop_; }
    void SetOnDesktop(bool v) { on_desktop_ = v; }
    bool IsDisplayRealDisplay() override { return false; }
    void GetRecommendedRenderTargetSize(uint32_t* width, uint32_t* height) override;
    void GetEyeOutputViewport(vr::EVREye eye, uint32_t* x, uint32_t* y, uint32_t* width, uint32_t* height) override;
    void GetProjectionRaw(vr::EVREye eye, float* left, float* right, float* top, float* bottom) override;
    vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye eye, float u, float v) override;
    bool ComputeInverseDistortion(vr::HmdVector2_t* result, vr::EVREye eye, uint32_t channel, float u, float v) override;

private:
    bool on_desktop_ = true;
    DisplayConfig cfg_;
};

class DirectModeComponent; // linux/direct_mode.h

class HmdDevice : public VirtualDevice {
public:
    // `direct_mode` (Linux only) makes the compositor hand the eye images to
    // the driver instead of opening a window; see linux/direct_mode.h.
    HmdDevice(std::shared_ptr<DeviceState> state, const DisplayConfig& display, bool direct_mode, const PreviewConfig& preview,
              bool preview_window);
    HmdDevice(std::shared_ptr<DeviceState> state, const DisplayConfig& display, bool direct_mode,
              const PreviewConfig& preview);
    ~HmdDevice() override;

    vr::ETrackedDeviceClass DeviceClass() const override { return vr::TrackedDeviceClass_HMD; }
    void* GetComponent(const char* name_and_version) override;
    bool DirectMode() const;
    // JSON object describing the display path (for config op "list").
    std::string DisplayStatusJson() const;
    // Starts / pauses the shared-memory preview (direct mode only). Returns false if unavailable.
    bool SetPreviewActive(bool active);
    // Changes the frame rate while running (direct mode). Returns false if unavailable.
    bool SetRefreshRate(double hz);
    double RefreshRate() const;
    // Shows / hides the preview window (Windows direct mode). Returns false if unavailable.
    bool SetPreviewWindow(bool visible);

protected:
    void OnActivate(vr::PropertyContainerHandle_t container) override;
    void OnDeactivate() override;
    void OnInput(const std::string& name, float value) override;
    void OnGaze(const GazeMessage& m) override;
    // Protocol positions are the head (HMD) position. Only when configured
    // (hmd.headModel) is pos treated as the neck and SteamVR's neck model applied.
    bool ApplyHeadModel() const override { return head_model_; }

private:
    DisplayConfig display_cfg_;
    bool head_model_;
    HmdDisplayComponent display_;
    std::unique_ptr<DirectModeComponent> direct_;
    vr::VRInputComponentHandle_t system_click_ = vr::k_ulInvalidInputComponentHandle;
    vr::VRInputComponentHandle_t eye_tracking_ = vr::k_ulInvalidInputComponentHandle;
};

} // namespace vhmd
