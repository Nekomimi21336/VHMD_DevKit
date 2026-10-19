// Linux only: IVRDriverDirectModeComponent for the virtual HMD.
//
// Linux SteamVR's compositor refuses to run without a display it can take
// with direct mode (DRM lease); the desktop-window path of
// IVRDisplayComponent that works on Windows fails with
// VRInitError_Compositor_CannotDRMLeaseDisplay (also for SteamVR's own null
// driver, ValveSoftware/SteamVR-for-Linux#889). With this component the
// driver owns the final output instead: the compositor renders the eye
// images into shared Vulkan images the driver allocates through
// IVRIPCResourceManagerClient (Linux-only API) and hands them over in
// SubmitLayer/Present. Same design as ALVR's Linux driver.
//
// The driver also announces vsyncs itself (Prop_DriverDirectModeSendsVsyncEvents)
// from a fixed-rate clock, and PostPresent paces the compositor against it.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <openvr_driver.h>

#include "linux/frame_capture.h"

namespace vhmd {

struct DirectModeStats {
    uint64_t swap_sets_created = 0;
    uint64_t swap_sets_alive = 0;
    uint64_t layers_submitted = 0;
    uint64_t presents = 0;
    uint64_t vsyncs = 0;
    uint32_t last_width = 0, last_height = 0, last_format = 0;
    std::string last_error;
    CaptureStats capture;
};

class DirectModeComponent : public vr::IVRDriverDirectModeComponent {
public:
    DirectModeComponent(double refresh_hz, const CaptureSettings& capture);
    virtual ~DirectModeComponent();

    // Start/stop the vsync clock (called from HMD Activate/Deactivate).
    void Start();
    void Stop();

    void CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t* desc, SwapTextureSet_t* out) override;
    void DestroySwapTextureSet(vr::SharedTextureHandle_t handle) override;
    void DestroyAllSwapTextureSets(uint32_t pid) override;
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*indices)[2]) override;
    void SubmitLayer(const SubmitLayerPerEye_t (&per_eye)[2]) override;
    void Present(vr::SharedTextureHandle_t sync_texture) override;
    void PostPresent(const Throttling_t* throttling) override;
    void GetFrameTiming(vr::DriverDirectMode_FrameTiming* timing) override;

    DirectModeStats Stats() const;
    // Changes the vsync clock (frame rate) while running.
    void SetRefreshRate(double hz) { refresh_hz_ = hz; }
    double RefreshRate() const { return refresh_hz_; }
    void SetPreviewActive(bool active) { capture_->SetActive(active); }

private:
    struct SwapSet {
        uint32_t pid = 0;
        SwapTextureSetDesc_t desc{};
        vr::SharedTextureHandle_t handles[3] = {0, 0, 0};
        uint32_t index = 0;
    };

    void DestroySet(SwapSet* set); // mu_ held
    void VsyncLoop();

    std::atomic<double> refresh_hz_;
    std::unique_ptr<FrameCapture> capture_;

    mutable std::mutex mu_;
    std::map<vr::SharedTextureHandle_t, SwapSet*> by_handle_;
    SubmitLayerPerEye_t layer0_[2]{};
    uint32_t layer_count_ = 0;
    DirectModeStats stats_;

    // Vsync clock
    std::thread vsync_thread_;
    std::atomic<bool> vsync_running_{false};
    mutable std::mutex vsync_mu_;
    std::condition_variable vsync_cv_;
    uint64_t vsync_count_ = 0;
};

} // namespace vhmd
