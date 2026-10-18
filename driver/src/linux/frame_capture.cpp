#include "linux/frame_capture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <vector>

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "linux/vk_loader.h"
#include "util/log.h"
#include "util/preview_shm.h"

namespace vhmd {
namespace {

bool IsSrgbOrFloat(uint32_t f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        return true;
    default:
        return false;
    }
}

// Exports the writers' fences of a dma-buf and waits for them, bounded.
// Returns false if the fence did not signal in time (the copy then races,
// which for a preview only means a torn frame).
bool WaitDmabufWrites(int fd, int timeout_ms) {
#if defined(DMA_BUF_IOCTL_EXPORT_SYNC_FILE)
    dma_buf_export_sync_file info{};
    info.flags = DMA_BUF_SYNC_READ;
    info.fd = -1;
    if (ioctl(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &info) != 0 || info.fd < 0) return true; // nothing to wait on
    pollfd p{};
    p.fd = info.fd;
    p.events = POLLIN;
    const int r = poll(&p, 1, timeout_ms);
    close(info.fd);
    return r > 0;
#else
    (void)fd;
    (void)timeout_ms;
    return true;
#endif
}

uint64_t RealtimeNs() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

} // namespace

struct FrameCapture::Impl {
    VkFns vk;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem_props{};

    struct Imported {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0, format = 0;
        bool layout_ready = false;
        int sync_fd = -1; // our own dup, used only to export write fences
    };
    std::map<vr::SharedTextureHandle_t, Imported> imported;

    VkImage staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    uint32_t staging_w = 0, staging_h = 0;
    VkFormat staging_format = VK_FORMAT_UNDEFINED;

    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory buffer_mem = VK_NULL_HANDLE;
    VkDeviceSize buffer_size = 0;
    void* mapped = nullptr;
    bool coherent = true;

    int shm_fd = -1;
    void* shm = nullptr;
    size_t shm_size = 0;
    uint64_t seq = 0;
    uint64_t frame_index = 0;
    std::string shm_path;

    ~Impl() { Destroy(); }

    int FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags* got = nullptr) const {
        for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want) {
                if (got) *got = mem_props.memoryTypes[i].propertyFlags;
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    bool Init(int device_index, std::string& err) {
        if (!vk.LoadLibrary(err)) return false;
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "vhmd-preview";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;
        VkResult r = vk.CreateInstance(&ici, nullptr, &instance);
        if (r != VK_SUCCESS) {
            err = std::string("vkCreateInstance: ") + VkResultName(r);
            return false;
        }
        if (!vk.LoadInstance(instance, err)) return false;

        uint32_t count = 0;
        vk.EnumeratePhysicalDevices(instance, &count, nullptr);
        if (count == 0) {
            err = "no Vulkan physical device";
            return false;
        }
        std::vector<VkPhysicalDevice> devs(count);
        vk.EnumeratePhysicalDevices(instance, &count, devs.data());
        const uint32_t idx = device_index >= 0 && static_cast<uint32_t>(device_index) < count ? device_index : 0;
        phys = devs[idx];
        VkPhysicalDeviceProperties props{};
        vk.GetPhysicalDeviceProperties(phys, &props);
        Log(LogLevel::Info, "preview: Vulkan device %u/%u '%s'", idx, count, props.deviceName);

        uint32_t ext_count = 0;
        vk.EnumerateDeviceExtensionProperties(phys, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> exts(ext_count);
        vk.EnumerateDeviceExtensionProperties(phys, nullptr, &ext_count, exts.data());
        const bool has_fd = std::any_of(exts.begin(), exts.end(), [](const VkExtensionProperties& e) {
            return std::strcmp(e.extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0;
        });
        if (!has_fd) {
            err = "device lacks VK_KHR_external_memory_fd";
            return false;
        }

        uint32_t qcount = 0;
        vk.GetPhysicalDeviceQueueFamilyProperties(phys, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qcount);
        vk.GetPhysicalDeviceQueueFamilyProperties(phys, &qcount, qprops.data());
        bool found = false;
        for (uint32_t i = 0; i < qcount; ++i) {
            if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { // vkCmdBlitImage needs graphics
                queue_family = i;
                found = true;
                break;
            }
        }
        if (!found) {
            err = "no graphics queue";
            return false;
        }

        const float prio = 0.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = queue_family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        const char* dev_exts[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = dev_exts;
        r = vk.CreateDevice(phys, &dci, nullptr, &dev);
        if (r != VK_SUCCESS) {
            err = std::string("vkCreateDevice: ") + VkResultName(r);
            return false;
        }
        if (!vk.LoadDevice(dev, err)) return false;
        vk.GetDeviceQueue(dev, queue_family, 0, &queue);
        vk.GetPhysicalDeviceMemoryProperties(phys, &mem_props);

        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queue_family;
        if (vk.CreateCommandPool(dev, &pci, nullptr, &pool) != VK_SUCCESS) {
            err = "vkCreateCommandPool failed";
            return false;
        }
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (vk.AllocateCommandBuffers(dev, &cai, &cmd) != VK_SUCCESS) {
            err = "vkAllocateCommandBuffers failed";
            return false;
        }
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vk.CreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS) {
            err = "vkCreateFence failed";
            return false;
        }
        return true;
    }

    // Imports a shared image created by SteamVR's NewSharedVulkanImage. The
    // create info must match what the driver requested there (see
    // DirectModeComponent::CreateSwapTextureSet). Takes ownership of fd.
    bool Import(vr::SharedTextureHandle_t h, int fd, int sync_fd, uint32_t w, uint32_t hgt, uint32_t format,
                std::string& err) {
        Imported im;
        im.width = w;
        im.height = hgt;
        im.format = format;
        im.sync_fd = sync_fd;

        VkExternalMemoryImageCreateInfo ext{};
        ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.pNext = &ext;
        ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = static_cast<VkFormat>(format);
        ci.extent = {w, hgt, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = vk.CreateImage(dev, &ci, nullptr, &im.image);
        if (r != VK_SUCCESS) {
            close(fd);
            if (sync_fd >= 0) close(sync_fd);
            err = std::string("vkCreateImage(import): ") + VkResultName(r);
            return false;
        }
        VkMemoryRequirements req{};
        vk.GetImageMemoryRequirements(dev, im.image, &req);
        int type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type < 0) type = FindMemoryType(req.memoryTypeBits, 0);

        VkImportMemoryFdInfoKHR imp{};
        imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        imp.fd = fd;
        VkMemoryDedicatedAllocateInfo ded{};
        ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        ded.pNext = &imp;
        ded.image = im.image;
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.pNext = &ded;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = static_cast<uint32_t>(std::max(type, 0));
        r = vk.AllocateMemory(dev, &ai, nullptr, &im.memory);
        if (r != VK_SUCCESS) {
            close(fd); // not consumed on failure
            if (sync_fd >= 0) close(sync_fd);
            vk.DestroyImage(dev, im.image, nullptr);
            err = std::string("vkAllocateMemory(import fd): ") + VkResultName(r);
            return false;
        }
        vk.BindImageMemory(dev, im.image, im.memory, 0);
        imported[h] = im;
        return true;
    }

    void Drop(vr::SharedTextureHandle_t h) {
        auto it = imported.find(h);
        if (it == imported.end()) return;
        vk.DeviceWaitIdle(dev);
        vk.DestroyImage(dev, it->second.image, nullptr);
        vk.FreeMemory(dev, it->second.memory, nullptr);
        if (it->second.sync_fd >= 0) close(it->second.sync_fd);
        imported.erase(it);
    }

    bool EnsureTargets(uint32_t w, uint32_t h, VkFormat format, std::string& err) {
        if (staging != VK_NULL_HANDLE && staging_w == w && staging_h == h && staging_format == format) return true;
        vk.DeviceWaitIdle(dev);
        DestroyTargets();

        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vk.CreateImage(dev, &ci, nullptr, &staging) != VK_SUCCESS) {
            err = "vkCreateImage(staging) failed";
            return false;
        }
        VkMemoryRequirements req{};
        vk.GetImageMemoryRequirements(dev, staging, &req);
        int type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type < 0) type = FindMemoryType(req.memoryTypeBits, 0);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = static_cast<uint32_t>(std::max(type, 0));
        if (vk.AllocateMemory(dev, &ai, nullptr, &staging_mem) != VK_SUCCESS) {
            err = "vkAllocateMemory(staging) failed";
            return false;
        }
        vk.BindImageMemory(dev, staging, staging_mem, 0);

        buffer_size = static_cast<VkDeviceSize>(w) * h * 4;
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = buffer_size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vk.CreateBuffer(dev, &bci, nullptr, &buffer) != VK_SUCCESS) {
            err = "vkCreateBuffer failed";
            return false;
        }
        vk.GetBufferMemoryRequirements(dev, buffer, &req);
        VkMemoryPropertyFlags got = 0;
        type = FindMemoryType(req.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &got);
        if (type < 0) type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &got);
        if (type < 0) {
            err = "no host-visible memory";
            return false;
        }
        coherent = (got & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = static_cast<uint32_t>(type);
        if (vk.AllocateMemory(dev, &ai, nullptr, &buffer_mem) != VK_SUCCESS) {
            err = "vkAllocateMemory(buffer) failed";
            return false;
        }
        vk.BindBufferMemory(dev, buffer, buffer_mem, 0);
        if (vk.MapMemory(dev, buffer_mem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
            err = "vkMapMemory failed";
            return false;
        }
        staging_w = w;
        staging_h = h;
        staging_format = format;
        return true;
    }

    void DestroyTargets() {
        if (!dev) return;
        if (mapped) vk.UnmapMemory(dev, buffer_mem);
        mapped = nullptr;
        if (buffer) vk.DestroyBuffer(dev, buffer, nullptr);
        if (buffer_mem) vk.FreeMemory(dev, buffer_mem, nullptr);
        if (staging) vk.DestroyImage(dev, staging, nullptr);
        if (staging_mem) vk.FreeMemory(dev, staging_mem, nullptr);
        buffer = VK_NULL_HANDLE;
        buffer_mem = staging_mem = VK_NULL_HANDLE;
        staging = VK_NULL_HANDLE;
        staging_w = staging_h = 0;
    }

    bool EnsureShm(uint32_t w, uint32_t h, std::string& err) {
        const size_t want = sizeof(PreviewHeader) + static_cast<size_t>(w) * h * 4;
        if (shm && shm_size == want) return true;
        if (shm) munmap(shm, shm_size);
        shm = nullptr;
        if (shm_fd < 0) {
            shm_fd = open(shm_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
            if (shm_fd < 0) {
                err = "cannot open " + shm_path;
                return false;
            }
        }
        if (ftruncate(shm_fd, static_cast<off_t>(want)) != 0) {
            err = "ftruncate failed for " + shm_path;
            return false;
        }
        void* p = mmap(nullptr, want, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        if (p == MAP_FAILED) {
            err = "mmap failed for " + shm_path;
            return false;
        }
        shm = p;
        shm_size = want;
        return true;
    }

    void WriteShm(uint32_t w, uint32_t h, uint32_t source_format) {
        auto* hdr = static_cast<PreviewHeader*>(shm);
        auto* pixels = static_cast<uint8_t*>(shm) + sizeof(PreviewHeader);
        // Single writer: seq is even between frames, odd while writing.
        __atomic_store_n(&hdr->seq, ++seq, __ATOMIC_RELEASE); // odd: writing
        __atomic_thread_fence(__ATOMIC_RELEASE);
        std::memcpy(hdr->magic, kPreviewMagic, sizeof(kPreviewMagic));
        hdr->version = kPreviewVersion;
        hdr->header_size = sizeof(PreviewHeader);
        hdr->width = w;
        hdr->height = h;
        hdr->stride = w * 4;
        hdr->format = kPreviewFormatRgba8;
        hdr->frame_index = ++frame_index;
        hdr->timestamp_ns = RealtimeNs();
        hdr->eye_width = w / 2;
        hdr->source_format = source_format;
        std::memcpy(pixels, mapped, static_cast<size_t>(w) * h * 4);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&hdr->seq, ++seq, __ATOMIC_RELEASE); // even: stable
    }

    void Barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                              0, nullptr, 1, &b);
    }

    bool Capture(const CaptureEye& left, const CaptureEye& right, uint32_t max_eye_w, double frame_ms,
                 std::string& err) {
        auto li = imported.find(left.handle);
        auto ri = imported.find(right.handle);
        if (li == imported.end() || ri == imported.end()) return true; // not imported (yet)
        Imported* eyes[2] = {&li->second, &ri->second};
        const CaptureEye* src[2] = {&left, &right};

        // Source rectangles from the texture bounds (may be flipped).
        int32_t sx0[2], sx1[2], sy0[2], sy1[2];
        for (int e = 0; e < 2; ++e) {
            const auto& b = src[e]->bounds;
            sx0[e] = static_cast<int32_t>(std::lround(b.uMin * eyes[e]->width));
            sx1[e] = static_cast<int32_t>(std::lround(b.uMax * eyes[e]->width));
            sy0[e] = static_cast<int32_t>(std::lround(b.vMin * eyes[e]->height));
            sy1[e] = static_cast<int32_t>(std::lround(b.vMax * eyes[e]->height));
            if (sx0[e] == sx1[e] || sy0[e] == sy1[e]) return true; // empty region
        }
        const uint32_t src_w = static_cast<uint32_t>(std::abs(sx1[0] - sx0[0]));
        const uint32_t src_h = static_cast<uint32_t>(std::abs(sy1[0] - sy0[0]));
        const uint32_t out_w = std::max<uint32_t>(1, std::min(src_w, max_eye_w));
        const uint32_t out_h = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(double(src_h) * out_w / src_w)));
        const VkFormat dst_format = IsSrgbOrFloat(eyes[0]->format) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

        VkFormatProperties sp{}, dp{};
        vk.GetPhysicalDeviceFormatProperties(phys, static_cast<VkFormat>(eyes[0]->format), &sp);
        vk.GetPhysicalDeviceFormatProperties(phys, dst_format, &dp);
        if (!(sp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
            !(dp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
            err = "blit not supported for eye texture format " + std::to_string(eyes[0]->format);
            return false;
        }
        const VkFilter filter = (sp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                                    ? VK_FILTER_LINEAR
                                    : VK_FILTER_NEAREST;
        if (!EnsureTargets(out_w * 2, out_h, dst_format, err)) return false;
        if (!EnsureShm(out_w * 2, out_h, err)) return false;

        // Wait (bounded) until the compositor's writes to both eyes finished.
        const int wait_ms = std::max(1, static_cast<int>(frame_ms));
        for (auto* im : eyes) {
            if (im->sync_fd >= 0) WaitDmabufWrites(im->sync_fd, wait_ms);
        }

        vk.ResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk.BeginCommandBuffer(cmd, &bi);
        for (auto* im : eyes) {
            // The images live in the compositor's world; like ALVR we take
            // them from UNDEFINED once and read them as TRANSFER_SRC after.
            Barrier(im->image, im->layout_ready ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT);
            im->layout_ready = true;
        }
        Barrier(staging, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        for (int e = 0; e < 2; ++e) {
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[0] = {sx0[e], sy0[e], 0};
            blit.srcOffsets[1] = {sx1[e], sy1[e], 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {static_cast<int32_t>(out_w * e), 0, 0};
            blit.dstOffsets[1] = {static_cast<int32_t>(out_w * (e + 1)), static_cast<int32_t>(out_h), 1};
            vk.CmdBlitImage(cmd, eyes[e]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
        }
        Barrier(staging, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {out_w * 2, out_h, 1};
        vk.CmdCopyImageToBuffer(cmd, staging, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        VkBufferMemoryBarrier bb{};
        bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = buffer;
        bb.size = VK_WHOLE_SIZE;
        vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb,
                              0, nullptr);
        vk.EndCommandBuffer(cmd);

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vk.ResetFences(dev, 1, &fence);
        VkResult r = vk.QueueSubmit(queue, 1, &si, fence);
        if (r != VK_SUCCESS) {
            err = std::string("vkQueueSubmit: ") + VkResultName(r);
            return false;
        }
        r = vk.WaitForFences(dev, 1, &fence, VK_TRUE, 500ull * 1000 * 1000);
        if (r != VK_SUCCESS) {
            err = std::string("vkWaitForFences: ") + VkResultName(r);
            return false;
        }
        if (!coherent) {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = buffer_mem;
            range.size = VK_WHOLE_SIZE;
            vk.InvalidateMappedMemoryRanges(dev, 1, &range);
        }
        WriteShm(out_w * 2, out_h, eyes[0]->format);
        return true;
    }

    void Destroy() {
        if (dev) {
            vk.DeviceWaitIdle(dev);
            for (auto& [h, im] : imported) {
                vk.DestroyImage(dev, im.image, nullptr);
                vk.FreeMemory(dev, im.memory, nullptr);
                if (im.sync_fd >= 0) close(im.sync_fd);
            }
            imported.clear();
            DestroyTargets();
            if (fence) vk.DestroyFence(dev, fence, nullptr);
            if (pool) vk.DestroyCommandPool(dev, pool, nullptr);
            vk.DestroyDevice(dev, nullptr);
            dev = VK_NULL_HANDLE;
        }
        if (instance && vk.DestroyInstance) vk.DestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
        vk.Unload();
        if (shm) munmap(shm, shm_size);
        shm = nullptr;
        if (shm_fd >= 0) {
            close(shm_fd);
            unlink(shm_path.c_str()); // readers see the file vanish when SteamVR stops
        }
        shm_fd = -1;
    }
};

FrameCapture::FrameCapture(const CaptureSettings& settings) : settings_(settings) {
    stats_.state = settings_.enabled ? "waiting for first frame" : "disabled";
    if (settings_.enabled) thread_ = std::thread(&FrameCapture::Worker, this);
}

FrameCapture::~FrameCapture() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        exit_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
    delete impl_;
    impl_ = nullptr;
    for (auto& [h, info] : images_) {
        if (info.fd >= 0) close(info.fd);
    }
}

void FrameCapture::RegisterImage(vr::SharedTextureHandle_t handle, int fd, uint32_t width, uint32_t height,
                                 uint32_t vk_format) {
    std::lock_guard<std::mutex> lock(mu_);
    auto& info = images_[handle];
    if (info.fd >= 0) close(info.fd);
    info = ImageInfo{fd, width, height, vk_format};
    forgotten_.erase(handle);
}

void FrameCapture::ForgetImage(vr::SharedTextureHandle_t handle) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = images_.find(handle);
    if (it != images_.end()) {
        if (it->second.fd >= 0) close(it->second.fd);
        images_.erase(it);
    }
    forgotten_[handle] = true;
}

void FrameCapture::Submit(const CaptureEye& left, const CaptureEye& right) {
    if (!settings_.enabled) return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        job_left_ = left;
        job_right_ = right;
        pending_ = true;
        ++stats_.submitted;
    }
    cv_.notify_one();
}

CaptureStats FrameCapture::Stats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
}

void FrameCapture::Worker() {
    using clock = std::chrono::steady_clock;
    const auto min_interval = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(1.0 / std::clamp(settings_.fps, 1.0, 120.0)));
    const double frame_ms = 1000.0 / 90.0;
    bool failed = false;
    auto next = clock::now();

    while (true) {
        CaptureEye left, right;
        std::vector<vr::SharedTextureHandle_t> drop;
        struct ToImport {
            vr::SharedTextureHandle_t h;
            int fd, sync_fd;
            ImageInfo info;
        };
        std::vector<ToImport> to_import;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return pending_ || exit_; });
            if (exit_) break;
            left = job_left_;
            right = job_right_;
            pending_ = false;
            for (auto& [h, f] : forgotten_) drop.push_back(h);
            forgotten_.clear();
            if (!failed) {
                // dup under the lock: ForgetImage may close the registered fd.
                for (const auto h : {left.handle, right.handle}) {
                    auto it = images_.find(h);
                    if (it == images_.end() || it->second.fd < 0) continue;
                    if (impl_ && impl_->imported.count(h)) continue;
                    to_import.push_back({h, fcntl(it->second.fd, F_DUPFD_CLOEXEC, 0),
                                         fcntl(it->second.fd, F_DUPFD_CLOEXEC, 0), it->second});
                }
            }
        }
        if (failed) continue;

        std::string err;
        if (!impl_) {
            impl_ = new Impl();
            impl_->shm_path = settings_.path;
            if (!impl_->Init(settings_.vulkan_device, err)) {
                for (auto& t : to_import) {
                    close(t.fd);
                    close(t.sync_fd);
                }
                failed = true;
            }
        }
        if (!failed) {
            for (auto h : drop) impl_->Drop(h);
            for (auto& t : to_import) {
                if (!impl_->Import(t.h, t.fd, t.sync_fd, t.info.width, t.info.height, t.info.format, err)) {
                    failed = true;
                    break;
                }
            }
        }
        if (!failed && !impl_->Capture(left, right, settings_.max_eye_width, frame_ms, err)) failed = true;

        {
            std::lock_guard<std::mutex> lock(mu_);
            if (failed) {
                stats_.active = false;
                stats_.state = "error: " + err;
            } else if (impl_ && impl_->frame_index > 0) {
                stats_.active = true;
                stats_.written = impl_->frame_index;
                stats_.state = "running";
            }
        }
        if (failed) {
            Log(LogLevel::Warning, "preview disabled: %s (rendering to SteamVR is not affected)", err.c_str());
            delete impl_;
            impl_ = nullptr;
            continue;
        }
        next += min_interval;
        const auto now = clock::now();
        if (next < now) next = now;
        std::this_thread::sleep_until(next);
    }
}

} // namespace vhmd
