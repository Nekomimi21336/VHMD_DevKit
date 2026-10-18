#include "linux/direct_mode.h"

#include <chrono>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "linux/vk_loader.h" // Vulkan enums for the shared image parameters
#include "util/log.h"
#include "util/precise_sleep.h"

namespace vhmd {
namespace {

// Usage/flags requested for the shared eye images. FrameCapture imports the
// images with exactly these parameters (an opaque-fd import must match the
// creation parameters), so keep both in sync.
constexpr uint32_t kImageUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
constexpr uint32_t kImageFlags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

} // namespace

DirectModeComponent::DirectModeComponent(double refresh_hz, const CaptureSettings& capture)
    : refresh_hz_(refresh_hz > 1.0 ? refresh_hz : 90.0), capture_(std::make_unique<FrameCapture>(capture)) {}

DirectModeComponent::~DirectModeComponent() {
    Stop();
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<SwapSet*> sets;
    for (auto& [h, s] : by_handle_) {
        if (s->handles[0] == h) sets.push_back(s);
    }
    for (auto* s : sets) DestroySet(s);
}

void DirectModeComponent::Start() {
    if (vsync_running_.exchange(true)) return;
    vsync_thread_ = std::thread(&DirectModeComponent::VsyncLoop, this);
    Log(LogLevel::Info, "direct mode: vsync clock started at %.1f Hz", refresh_hz_);
}

void DirectModeComponent::Stop() {
    if (!vsync_running_.exchange(false)) return;
    vsync_cv_.notify_all();
    if (vsync_thread_.joinable()) vsync_thread_.join();
}

void DirectModeComponent::VsyncLoop() {
    using clock = std::chrono::steady_clock;
    const auto period =
        std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / refresh_hz_));
    PreciseSleeper sleeper;
    auto next = clock::now() + period;
    while (vsync_running_) {
        sleeper.SleepUntil(next);
        vr::VRServerDriverHost()->VsyncEvent(0.0);
        {
            std::lock_guard<std::mutex> lock(vsync_mu_);
            ++vsync_count_;
        }
        vsync_cv_.notify_all();
        next += period;
        const auto now = clock::now();
        if (now > next + period * 4) next = now + period; // fell far behind: resync, do not burst
    }
}

void DirectModeComponent::CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t* desc, SwapTextureSet_t* out) {
    if (!desc || !out) return;
    auto* ipc = vr::VRIPCResourceManager();
    if (!ipc) {
        Log(LogLevel::Error, "direct mode: VRIPCResourceManager unavailable");
        return;
    }
    auto set = std::make_unique<SwapSet>();
    set->pid = pid;
    set->desc = *desc;
    std::vector<int> fds;

    for (int i = 0; i < 3; ++i) {
        vr::SharedTextureHandle_t handle = 0;
        const bool ok = ipc->NewSharedVulkanImage(desc->nFormat, desc->nWidth, desc->nHeight, /*bRenderable=*/true,
                                                  /*bMappable=*/false, /*bComputeAccess=*/true, /*mips=*/1,
                                                  /*layers=*/1, kImageFlags, kImageUsage, &handle);
        uint64_t ipc_handle = 0;
        int fd = -1;
        const bool shared = ok && ipc->RefResource(handle, &ipc_handle) && ipc->ReceiveSharedFd(ipc_handle, &fd);
        if (!shared) {
            Log(LogLevel::Error, "direct mode: shared image %d of %ux%u fmt %u failed (%s)", i, desc->nWidth,
                desc->nHeight, desc->nFormat, ok ? "fd" : "NewSharedVulkanImage");
            if (handle) ipc->UnrefResource(handle);
            for (int j = 0; j < i; ++j) ipc->UnrefResource(set->handles[j]);
            for (int f : fds) close(f);
            std::lock_guard<std::mutex> lock(mu_);
            stats_.last_error = "CreateSwapTextureSet failed";
            return;
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC); // kept for the preview; never hand it to child processes
        set->handles[i] = handle;
        out->rSharedTextureHandles[i] = handle;
        fds.push_back(fd);
    }
    out->unTextureFlags = 0;

    std::lock_guard<std::mutex> lock(mu_);
    for (int i = 0; i < 3; ++i) {
        by_handle_[set->handles[i]] = set.get();
        capture_->RegisterImage(set->handles[i], fds[i], desc->nWidth, desc->nHeight, desc->nFormat);
    }
    ++stats_.swap_sets_created;
    ++stats_.swap_sets_alive;
    stats_.last_width = desc->nWidth;
    stats_.last_height = desc->nHeight;
    stats_.last_format = desc->nFormat;
    Log(LogLevel::Info, "direct mode: swap texture set for pid %u: %ux%u VkFormat %u, %u samples", pid, desc->nWidth,
        desc->nHeight, desc->nFormat, desc->nSampleCount);
    set.release(); // owned through by_handle_
}

void DirectModeComponent::DestroySet(SwapSet* set) {
    auto* ipc = vr::VRIPCResourceManager();
    for (auto h : set->handles) {
        if (!h) continue;
        by_handle_.erase(h);
        capture_->ForgetImage(h);
        if (ipc) ipc->UnrefResource(h);
    }
    if (stats_.swap_sets_alive > 0) --stats_.swap_sets_alive;
    delete set;
}

void DirectModeComponent::DestroySwapTextureSet(vr::SharedTextureHandle_t handle) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = by_handle_.find(handle);
    if (it != by_handle_.end()) DestroySet(it->second);
}

void DirectModeComponent::DestroyAllSwapTextureSets(uint32_t pid) {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<SwapSet*> doomed;
    for (auto& [h, s] : by_handle_) {
        if (s->pid == pid && s->handles[0] == h) doomed.push_back(s);
    }
    for (auto* s : doomed) DestroySet(s);
    if (!doomed.empty()) Log(LogLevel::Info, "direct mode: released %zu swap set(s) of pid %u", doomed.size(), pid);
}

void DirectModeComponent::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*indices)[2]) {
    std::lock_guard<std::mutex> lock(mu_);
    for (int eye = 0; eye < 2; ++eye) {
        auto it = by_handle_.find(handles[eye]);
        if (it == by_handle_.end()) continue;
        SwapSet* s = it->second;
        // Both eyes may share one set; advance it once per call.
        if (eye == 1 && handles[1] != 0 && by_handle_.count(handles[0]) && by_handle_[handles[0]] == s) {
            if (indices) (*indices)[1] = s->index;
            continue;
        }
        s->index = (s->index + 1) % 3;
        if (indices) (*indices)[eye] = s->index;
    }
}

void DirectModeComponent::SubmitLayer(const SubmitLayerPerEye_t (&per_eye)[2]) {
    std::lock_guard<std::mutex> lock(mu_);
    if (layer_count_ == 0) {
        layer0_[0] = per_eye[0];
        layer0_[1] = per_eye[1];
    }
    ++layer_count_;
    ++stats_.layers_submitted;
}

void DirectModeComponent::Present(vr::SharedTextureHandle_t /*sync_texture*/) {
    CaptureEye left, right;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        ++stats_.presents;
        if (layer_count_ > 0) {
            left.handle = layer0_[0].hTexture;
            left.bounds = layer0_[0].bounds;
            right.handle = layer0_[1].hTexture;
            right.bounds = layer0_[1].bounds;
            have = left.handle != 0 && right.handle != 0;
        }
        layer_count_ = 0;
        if (stats_.presents == 1) Log(LogLevel::Info, "direct mode: first frame presented by the compositor");
    }
    // Only the bottom layer is captured; overlays the compositor already
    // flattened into it are included, separate layers are not (preview only).
    if (have) capture_->Submit(left, right);
}

void DirectModeComponent::PostPresent(const Throttling_t* /*throttling*/) {
    // Hold the compositor until our next vsync so it runs at the refresh rate.
    std::unique_lock<std::mutex> lock(vsync_mu_);
    const uint64_t start = vsync_count_;
    const auto timeout = std::chrono::duration<double>(2.0 / refresh_hz_);
    vsync_cv_.wait_for(lock, timeout, [&] { return vsync_count_ != start || !vsync_running_; });
}

void DirectModeComponent::GetFrameTiming(vr::DriverDirectMode_FrameTiming* timing) {
    if (!timing || timing->m_nSize < sizeof(vr::DriverDirectMode_FrameTiming)) return;
    // No real display: report every frame presented once, nothing dropped.
    // Reporting drops would make the compositor throttle (as ALVR found).
    timing->m_nNumFramePresents = 1;
    timing->m_nNumMisPresented = 0;
    timing->m_nNumDroppedFrames = 0;
    timing->m_nReprojectionFlags = 0; // must be cleared (see openvr_driver.h)
}

DirectModeStats DirectModeComponent::Stats() const {
    DirectModeStats s;
    {
        std::lock_guard<std::mutex> lock(mu_);
        s = stats_;
    }
    {
        std::lock_guard<std::mutex> lock(vsync_mu_);
        s.vsyncs = vsync_count_;
    }
    s.capture = capture_->Stats();
    return s;
}

} // namespace vhmd
