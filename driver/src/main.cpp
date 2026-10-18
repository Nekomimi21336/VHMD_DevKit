// Driver entry point. HmdDriverFactory is the only exported symbol.
#include <cstring>

#include <openvr_driver.h>

#include "device_provider.h"
#include "util/platform.h"

static vhmd::DeviceProvider g_provider;

VHMD_EXPORT void* HmdDriverFactory(const char* interface_name, int* return_code) {
    if (std::strcmp(vr::IServerTrackedDeviceProvider_Version, interface_name) == 0) {
        return static_cast<vr::IServerTrackedDeviceProvider*>(&g_provider);
    }
    if (return_code) *return_code = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
