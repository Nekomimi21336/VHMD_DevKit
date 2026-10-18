// Linux self-test for the direct-mode frame capture, without SteamVR.
//
// Plays the compositor's role: creates two exportable Vulkan images with the
// same parameters the driver requests from SteamVR (NewSharedVulkanImage),
// fills them with known colours, exports opaque fds and hands them to
// FrameCapture exactly like DirectModeComponent does. Then reads the shared
// memory preview back and checks size, seqlock and pixel colours.
//
// Exit codes: 0 pass, 1 fail, 77 skipped (no usable Vulkan device).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "linux/frame_capture.h"
#include "linux/vk_loader.h"
#include "util/log.h"
#include "util/preview_shm.h"

using namespace vhmd;

namespace {

int g_fail = 0;
#define EXPECT(c)                                                        \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

int Skip(const char* why) {
    std::printf("SKIP: %s\n", why);
    return 77;
}

struct Exporter {
    VkFns vk;
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qf = 0;
    PFN_vkGetMemoryFdKHR GetMemoryFd = nullptr;
    PFN_vkCmdClearColorImage CmdClearColorImage = nullptr;
    VkPhysicalDeviceMemoryProperties mp{};
    uint32_t device_count = 0;

    bool Init(std::string& err, uint32_t index = 0) {
        if (!vk.LoadLibrary(err)) return false;
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;
        if (vk.CreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) return err = "vkCreateInstance", false;
        if (!vk.LoadInstance(inst, err)) return false;
        uint32_t n = 0;
        vk.EnumeratePhysicalDevices(inst, &n, nullptr);
        if (!n) return err = "no device", false;
        std::vector<VkPhysicalDevice> d(n);
        vk.EnumeratePhysicalDevices(inst, &n, d.data());
        device_count = n;
        if (index >= n) return err = "no such device", false;
        phys = d[index];
        uint32_t qn = 0;
        vk.GetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qp(qn);
        vk.GetPhysicalDeviceQueueFamilyProperties(phys, &qn, qp.data());
        for (uint32_t i = 0; i < qn; ++i) {
            if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                qf = i;
                break;
            }
        }
        const float prio = 1.0f;
        VkDeviceQueueCreateInfo q{};
        q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        q.queueFamilyIndex = qf;
        q.queueCount = 1;
        q.pQueuePriorities = &prio;
        const char* exts[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &q;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = exts;
        if (vk.CreateDevice(phys, &dci, nullptr, &dev) != VK_SUCCESS) return err = "vkCreateDevice", false;
        if (!vk.LoadDevice(dev, err)) return false;
        GetMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vk.GetDeviceProcAddr(dev, "vkGetMemoryFdKHR"));
        CmdClearColorImage = reinterpret_cast<PFN_vkCmdClearColorImage>(vk.GetDeviceProcAddr(dev, "vkCmdClearColorImage"));
        if (!GetMemoryFd || !CmdClearColorImage) return err = "missing vkGetMemoryFdKHR", false;
        vk.GetDeviceQueue(dev, qf, 0, &queue);
        vk.GetPhysicalDeviceMemoryProperties(phys, &mp);
        return true;
    }

    // Creates an image like DirectModeComponent asks SteamVR for, clears it to
    // `rgba` and returns an exported opaque fd (or -1).
    int MakeImage(uint32_t w, uint32_t h, VkFormat fmt, const float rgba[4]) {
        VkExternalMemoryImageCreateInfo ext{};
        ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.pNext = &ext;
        ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = fmt;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        // The importer omits TRANSFER_DST; the writer (here and in SteamVR)
        // needs some way to write. Drivers ignore usage for memory layout.
        ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage img;
        if (vk.CreateImage(dev, &ci, nullptr, &img) != VK_SUCCESS) return -1;
        VkMemoryRequirements req{};
        vk.GetImageMemoryRequirements(dev, img, &req);
        uint32_t type = 0;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if (req.memoryTypeBits & (1u << i)) {
                type = i;
                break;
            }
        }
        VkExportMemoryAllocateInfo exp{};
        exp.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryDedicatedAllocateInfo ded{};
        ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        ded.pNext = &exp;
        ded.image = img;
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.pNext = &ded;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        VkDeviceMemory mem;
        if (vk.AllocateMemory(dev, &ai, nullptr, &mem) != VK_SUCCESS) return -1;
        vk.BindImageMemory(dev, img, mem, 0);

        VkCommandPool pool;
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = qf;
        vk.CreateCommandPool(dev, &pci, nullptr, &pool);
        VkCommandBuffer cmd;
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vk.AllocateCommandBuffers(dev, &cai, &cmd);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vk.BeginCommandBuffer(cmd, &bi);
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                              nullptr, 1, &b);
        VkClearColorValue color{};
        std::memcpy(color.float32, rgba, sizeof(float) * 4);
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        vk.EndCommandBuffer(cmd);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vk.QueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vk.DeviceWaitIdle(dev);

        VkMemoryGetFdInfoKHR gi{};
        gi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        gi.memory = mem;
        gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        int fd = -1;
        if (GetMemoryFd(dev, &gi, &fd) != VK_SUCCESS) return -1;
        return fd; // image/memory intentionally kept alive until exit
    }
};

struct Frame {
    PreviewHeader hdr{};
    std::vector<uint8_t> pixels;
};

bool ReadPreview(const std::string& path, Frame& out) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st {};
    fstat(fd, &st);
    if (st.st_size < static_cast<off_t>(sizeof(PreviewHeader))) {
        close(fd);
        return false;
    }
    void* p = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return false;
    const auto* hdr = static_cast<const PreviewHeader*>(p);
    bool ok = false;
    for (int attempt = 0; attempt < 100 && !ok; ++attempt) {
        const uint64_t s1 = __atomic_load_n(&hdr->seq, __ATOMIC_ACQUIRE);
        if (s1 & 1) continue;
        out.hdr = *hdr;
        const size_t n = static_cast<size_t>(out.hdr.stride) * out.hdr.height;
        if (sizeof(PreviewHeader) + n > static_cast<size_t>(st.st_size)) break;
        out.pixels.assign(static_cast<const uint8_t*>(p) + sizeof(PreviewHeader),
                          static_cast<const uint8_t*>(p) + sizeof(PreviewHeader) + n);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        ok = __atomic_load_n(&hdr->seq, __ATOMIC_ACQUIRE) == s1;
    }
    munmap(p, st.st_size);
    return ok;
}

} // namespace

// True if FrameCapture can import images exported on device `index`. Some
// Vulkan implementations cannot (e.g. Mesa's dzn on WSL); the driver then
// just disables the preview, so the test moves on to the next device.
static bool DeviceCanImport(uint32_t index) {
    Exporter ex;
    std::string err;
    if (!ex.Init(err, index)) return false;
    const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    const int fd_l = ex.MakeImage(16, 16, VK_FORMAT_R8G8B8A8_UNORM, grey);
    const int fd_r = ex.MakeImage(16, 16, VK_FORMAT_R8G8B8A8_UNORM, grey);
    if (fd_l < 0 || fd_r < 0) return false;
    CaptureSettings cs;
    cs.path = "/tmp/vhmd_capture_probe";
    cs.fps = 60;
    cs.start_active = true;
    cs.vulkan_device = static_cast<int>(index);
    FrameCapture cap(cs);
    cap.RegisterImage(1, fd_l, 16, 16, VK_FORMAT_R8G8B8A8_UNORM);
    cap.RegisterImage(2, fd_r, 16, 16, VK_FORMAT_R8G8B8A8_UNORM);
    CaptureEye l, r;
    l.handle = 1;
    r.handle = 2;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        cap.Submit(l, r);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const CaptureStats st = cap.Stats();
        if (st.written >= 1) return true;
        if (st.state.rfind("error", 0) == 0) break;
    }
    return false;
}

int main() {
    SetLogSink([](LogLevel, const std::string& line) { std::printf("  log: %s\n", line.c_str()); });
    Exporter probe;
    std::string err;
    if (!probe.Init(err)) return Skip(err.c_str());
    uint32_t device = 0;
    while (device < probe.device_count && !DeviceCanImport(device)) {
        std::printf("device %u cannot import exported images, trying the next one\n", device);
        ++device;
    }
    if (device == probe.device_count) return Skip("no Vulkan device can import its own exported images");
    std::printf("using Vulkan device %u\n", device);

    Exporter ex;
    if (!ex.Init(err, device)) return Skip(err.c_str());

    const uint32_t W = 64, H = 48;
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    const int fd_left = ex.MakeImage(W, H, VK_FORMAT_R8G8B8A8_UNORM, red);
    const int fd_right = ex.MakeImage(W, H, VK_FORMAT_R8G8B8A8_UNORM, blue);
    if (fd_left < 0 || fd_right < 0) return Skip("could not export images");

    const std::string path = "/tmp/vhmd_capture_selftest";
    unlink(path.c_str());
    {
        CaptureSettings cs;
        cs.path = path;
        cs.fps = 60;
        cs.start_active = true;
        cs.max_eye_width = 32; // forces a 2x downscale
        cs.vulkan_device = static_cast<int>(device);
        FrameCapture cap(cs);
        cap.RegisterImage(1001, fd_left, W, H, VK_FORMAT_R8G8B8A8_UNORM);
        cap.RegisterImage(1002, fd_right, W, H, VK_FORMAT_R8G8B8A8_UNORM);
        CaptureEye l, r;
        l.handle = 1001;
        r.handle = 1002;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (int i = 0; std::chrono::steady_clock::now() < deadline; ++i) {
            cap.Submit(l, r);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (cap.Stats().written >= 3 || cap.Stats().state.rfind("error", 0) == 0) break;
        }
        const CaptureStats st = cap.Stats();
        std::printf("capture state: %s, submitted %llu, written %llu\n", st.state.c_str(),
                    static_cast<unsigned long long>(st.submitted), static_cast<unsigned long long>(st.written));
        EXPECT(st.state == "running");
        EXPECT(st.written >= 3);

        Frame f;
        EXPECT(ReadPreview(path, f));
        EXPECT(std::memcmp(f.hdr.magic, kPreviewMagic, 8) == 0);
        EXPECT(f.hdr.version == kPreviewVersion);
        EXPECT(f.hdr.width == 64 && f.hdr.height == 24 && f.hdr.eye_width == 32);
        EXPECT(f.hdr.stride == 64 * 4);
        EXPECT(f.hdr.format == kPreviewFormatRgba8);
        EXPECT((f.hdr.seq & 1) == 0);
        EXPECT(f.hdr.source_format == VK_FORMAT_R8G8B8A8_UNORM);
        if (f.pixels.size() == 64u * 24 * 4) {
            const uint8_t* lp = &f.pixels[(12 * 64 + 16) * 4]; // centre of left eye
            const uint8_t* rp = &f.pixels[(12 * 64 + 48) * 4]; // centre of right eye
            std::printf("left px %u,%u,%u,%u  right px %u,%u,%u,%u\n", lp[0], lp[1], lp[2], lp[3], rp[0], rp[1], rp[2],
                        rp[3]);
            EXPECT(lp[0] > 240 && lp[1] < 16 && lp[2] < 16);
            EXPECT(rp[0] < 16 && rp[1] < 16 && rp[2] > 240);
        } else {
            EXPECT(false);
        }

        // Texture bounds: only the left half of each source -> still solid colours.
        l.bounds = {0.0f, 0.0f, 0.5f, 1.0f};
        cap.Submit(l, r);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        EXPECT(cap.Stats().state == "running");

        // Paused: submits are ignored, nothing more is written.
        cap.SetActive(false);
        const uint64_t written = cap.Stats().written;
        for (int i = 0; i < 5; ++i) {
            cap.Submit(l, r);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        EXPECT(cap.Stats().state == "paused");
        EXPECT(cap.Stats().written == written);
        cap.SetActive(true);

        cap.ForgetImage(1001); // compositor destroyed the set
        cap.Submit(l, r);      // unknown handle now: must be ignored, not crash
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT(cap.Stats().state == "running");
    }
    struct stat st {};
    EXPECT(stat(path.c_str(), &st) != 0); // file removed on shutdown

    std::printf("%s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
