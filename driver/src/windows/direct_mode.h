// Windows: IVRDriverDirectModeComponent for the virtual HMD (display.mode = "direct").
//
// Instead of the compositor opening a desktop window, the driver owns the
// output: it creates the eye textures (D3D11, shared handles) the compositor
// renders into, receives them in SubmitLayer/Present and paces the compositor
// with its own vsync clock, so the frame rate is exactly display.refreshHz.
// Nothing is shown unless asked: an optional preview window (like a camera
// library's preview) and the same shared-memory preview as on Linux
// (named mapping "vhmd_preview", see util/preview_shm.h) for tools that grab
// frames. Design follows ALVR's Windows driver; no code taken from it.
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

namespace vhmd {

struct CaptureSettings {
    bool enabled = true;
    std::string path = "vhmd_preview"; // name of the shared-memory mapping ("/dev/shm/" prefix ignored)
    double fps = 15.0;                 // shared-memory capture rate cap
    uint32_t max_eye_width = 960;
    int vulkan_device = -1;            // here: DXGI adapter index, -1 = default adapter
    bool start_active = false;
};

struct CaptureStats {
    uint64_t submitted = 0;
    uint64_t written = 0;
    bool active = false;
    std::string state;
};

struct DirectModeStats {
    uint64_t swap_sets_created = 0;
    uint64_t swap_sets_alive = 0;
    uint64_t layers_submitted = 0;
    uint64_t presents = 0;
    uint64_t vsyncs = 0;
    uint32_t last_width = 0, last_height = 0, last_format = 0;
    std::string last_error;
    CaptureStats capture;
    bool window_visible = false;
    uint64_t window_frames = 0;
};

class DirectModeComponent : public vr::IVRDriverDirectModeComponent {
public:
    DirectModeComponent(double refresh_hz, const CaptureSettings& capture, bool window_visible, double window_fps);
    virtual ~DirectModeComponent();

    // Creates the D3D11 device. On failure the HMD falls back to window mode.
    bool Init(std::string& error);
    // LUID of the adapter the eye textures live on (Prop_GraphicsAdapterLuid_Uint64).
    uint64_t AdapterLuid() const { return adapter_luid_; }

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
    void SetPreviewActive(bool active);
    void SetWindowVisible(bool visible);

private:
    struct Impl;
    void CopyPreview(const SubmitLayerPerEye_t (&layer)[2]);
    void VsyncLoop();
    void WindowLoop();
    void ReadbackLoop();

    std::atomic<double> refresh_hz_;
    CaptureSettings capture_;
    double window_fps_;
    uint64_t adapter_luid_ = 0;
    std::unique_ptr<Impl> impl_;

    mutable std::mutex mu_;
    DirectModeStats stats_;
    SubmitLayerPerEye_t layer0_[2]{};
    uint32_t layer_count_ = 0;
    std::atomic<bool> preview_active_{false};
    std::atomic<bool> window_visible_{false};

    std::atomic<bool> running_{false};
    std::thread vsync_thread_, window_thread_, readback_thread_;
    mutable std::mutex vsync_mu_;
    std::condition_variable vsync_cv_;
    uint64_t vsync_count_ = 0;
};

} // namespace vhmd
