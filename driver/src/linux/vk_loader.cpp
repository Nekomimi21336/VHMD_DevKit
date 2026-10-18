#include "linux/vk_loader.h"

#include <dlfcn.h>

namespace vhmd {

#define VHMD_LOAD(scope, handle, name)                                                  \
    name = reinterpret_cast<PFN_vk##name>(scope(handle, "vk" #name));                  \
    if (!name) {                                                                        \
        error = "missing Vulkan entry point vk" #name;                                  \
        return false;                                                                   \
    }

bool VkFns::LoadLibrary(std::string& error) {
    if (lib_) return true;
    for (const char* so : {"libvulkan.so.1", "libvulkan.so"}) {
        lib_ = dlopen(so, RTLD_NOW | RTLD_LOCAL);
        if (lib_) break;
    }
    if (!lib_) {
        error = "libvulkan.so.1 not found";
        return false;
    }
    GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib_, "vkGetInstanceProcAddr"));
    if (!GetInstanceProcAddr) {
        error = "vkGetInstanceProcAddr not exported by libvulkan";
        return false;
    }
    VHMD_LOAD(GetInstanceProcAddr, VK_NULL_HANDLE, CreateInstance)
    return true;
}

bool VkFns::LoadInstance(VkInstance instance, std::string& error) {
    VHMD_LOAD(GetInstanceProcAddr, instance, DestroyInstance)
    VHMD_LOAD(GetInstanceProcAddr, instance, EnumeratePhysicalDevices)
    VHMD_LOAD(GetInstanceProcAddr, instance, GetPhysicalDeviceProperties)
    VHMD_LOAD(GetInstanceProcAddr, instance, GetPhysicalDeviceQueueFamilyProperties)
    VHMD_LOAD(GetInstanceProcAddr, instance, GetPhysicalDeviceMemoryProperties)
    VHMD_LOAD(GetInstanceProcAddr, instance, GetPhysicalDeviceFormatProperties)
    VHMD_LOAD(GetInstanceProcAddr, instance, EnumerateDeviceExtensionProperties)
    VHMD_LOAD(GetInstanceProcAddr, instance, CreateDevice)
    VHMD_LOAD(GetInstanceProcAddr, instance, GetDeviceProcAddr)
    return true;
}

bool VkFns::LoadDevice(VkDevice device, std::string& error) {
    VHMD_LOAD(GetDeviceProcAddr, device, DestroyDevice)
    VHMD_LOAD(GetDeviceProcAddr, device, GetDeviceQueue)
    VHMD_LOAD(GetDeviceProcAddr, device, DeviceWaitIdle)
    VHMD_LOAD(GetDeviceProcAddr, device, CreateImage)
    VHMD_LOAD(GetDeviceProcAddr, device, DestroyImage)
    VHMD_LOAD(GetDeviceProcAddr, device, GetImageMemoryRequirements)
    VHMD_LOAD(GetDeviceProcAddr, device, BindImageMemory)
    VHMD_LOAD(GetDeviceProcAddr, device, CreateBuffer)
    VHMD_LOAD(GetDeviceProcAddr, device, DestroyBuffer)
    VHMD_LOAD(GetDeviceProcAddr, device, GetBufferMemoryRequirements)
    VHMD_LOAD(GetDeviceProcAddr, device, BindBufferMemory)
    VHMD_LOAD(GetDeviceProcAddr, device, AllocateMemory)
    VHMD_LOAD(GetDeviceProcAddr, device, FreeMemory)
    VHMD_LOAD(GetDeviceProcAddr, device, MapMemory)
    VHMD_LOAD(GetDeviceProcAddr, device, UnmapMemory)
    VHMD_LOAD(GetDeviceProcAddr, device, InvalidateMappedMemoryRanges)
    VHMD_LOAD(GetDeviceProcAddr, device, CreateCommandPool)
    VHMD_LOAD(GetDeviceProcAddr, device, DestroyCommandPool)
    VHMD_LOAD(GetDeviceProcAddr, device, AllocateCommandBuffers)
    VHMD_LOAD(GetDeviceProcAddr, device, ResetCommandBuffer)
    VHMD_LOAD(GetDeviceProcAddr, device, BeginCommandBuffer)
    VHMD_LOAD(GetDeviceProcAddr, device, EndCommandBuffer)
    VHMD_LOAD(GetDeviceProcAddr, device, CmdPipelineBarrier)
    VHMD_LOAD(GetDeviceProcAddr, device, CmdBlitImage)
    VHMD_LOAD(GetDeviceProcAddr, device, CmdCopyImageToBuffer)
    VHMD_LOAD(GetDeviceProcAddr, device, QueueSubmit)
    VHMD_LOAD(GetDeviceProcAddr, device, CreateFence)
    VHMD_LOAD(GetDeviceProcAddr, device, DestroyFence)
    VHMD_LOAD(GetDeviceProcAddr, device, WaitForFences)
    VHMD_LOAD(GetDeviceProcAddr, device, ResetFences)
    return true;
}

#undef VHMD_LOAD

void VkFns::Unload() {
    if (lib_) {
        dlclose(lib_);
        lib_ = nullptr;
    }
    *this = VkFns{};
}

const char* VkResultName(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    default: return "VkResult(other)";
    }
}

} // namespace vhmd
