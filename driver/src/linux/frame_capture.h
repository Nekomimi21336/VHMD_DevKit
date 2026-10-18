// Linux only: copies the compositor's eye textures (shared Vulkan images,
// received as fds from IVRIPCResourceManagerClient) into a shared-memory
// preview (util/preview_shm.h). Runs on its own thread and never blocks the
// compositor: Submit() only replaces a single-slot "latest frame" mailbox.
// Any Vulkan failure disables the preview for the session, it never takes
// vrserver down.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <openvr_driver.h>

namespace vhmd {

struct CaptureSettings {
    bool enabled = true;
    std::string path = "/dev/shm/vhmd_preview";
    double fps = 15.0;            // capture rate cap
    uint32_t max_eye_width = 960; // downscale each eye to at most this width
    int vulkan_device = -1;       // physical device index, -1 = first
};

struct CaptureEye {
    vr::SharedTextureHandle_t handle = 0;
    vr::VRTextureBounds_t bounds{0.0f, 0.0f, 1.0f, 1.0f};
};

struct CaptureStats {
    uint64_t submitted = 0;
    uint64_t written = 0;
    bool active = false;     // Vulkan initialised and writing
    std::string state;       // "disabled", "waiting", "running", "error: ..."
};

class FrameCapture {
public:
    explicit FrameCapture(const CaptureSettings& settings);
    ~FrameCapture();
    FrameCapture(const FrameCapture&) = delete;
    FrameCapture& operator=(const FrameCapture&) = delete;

    // Remembers a shared image. Takes ownership of `fd` (a dma-buf / opaque fd).
    void RegisterImage(vr::SharedTextureHandle_t handle, int fd, uint32_t width, uint32_t height, uint32_t vk_format);
    void ForgetImage(vr::SharedTextureHandle_t handle);
    // Queues the latest presented frame (latest wins).
    void Submit(const CaptureEye& left, const CaptureEye& right);

    CaptureStats Stats() const;

private:
    struct Impl;
    void Worker();

    CaptureSettings settings_;
    Impl* impl_ = nullptr; // Vulkan state, worker thread only

    mutable std::mutex mu_;
    std::condition_variable cv_;
    struct ImageInfo {
        int fd = -1;
        uint32_t width = 0, height = 0, format = 0;
    };
    std::map<vr::SharedTextureHandle_t, ImageInfo> images_;   // guarded by mu_
    std::map<vr::SharedTextureHandle_t, bool> forgotten_;     // handles to drop from the Vulkan cache
    bool pending_ = false;
    CaptureEye job_left_, job_right_;
    bool exit_ = false;
    CaptureStats stats_;

    std::thread thread_;
};

} // namespace vhmd
