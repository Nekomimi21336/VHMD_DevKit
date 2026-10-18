// Driver configuration (device layout, network, timeouts). Loaded from the
// JSON file resources/settings/vhmd_config.json (path overridable through
// the SteamVR setting driver_vhmd/configFile). Display parameters live in
// default.vrsettings like the OpenVR samples. No OpenVR dependency.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "math/pose_math.h"
#include "protocol/messages.h"
#include "protocol/osc.h"

namespace vhmd {

inline constexpr size_t kMaxTrackers = 16;

enum class TimeoutPolicy { Hold, Invalid, Disconnect };

std::optional<TimeoutPolicy> ParseTimeoutPolicy(std::string_view s);
const char* ToString(TimeoutPolicy p);

enum class DeviceKind { Hmd, Controller, Tracker };

struct DeviceConfig {
    DeviceKind kind = DeviceKind::Tracker;
    std::string id;       // protocol id: "hmd", "left_hand", "tracker_0", ...
    std::string serial;   // SteamVR serial number (unique)
    bool enabled = true;
    std::string role;     // trackers: short role name ("waist"); controllers: "left"/"right"
    Vec3 default_pos{};
    Quat default_rot{};
    std::optional<TimeoutPolicy> timeout_policy; // per-device override
    // Not registered with SteamVR at startup; added the first time a client
    // sends a message for it (or config op "add"). Default for trackers.
    bool on_demand = false;
    bool skeleton = true;                         // controllers only
    bool skeleton_full = false;                   // controllers: VRSkeletalTracking_Full (else Partial)
    bool eye_tracking = true;                     // hmd only
    bool head_model = false;                      // hmd only: true = pos is the neck pivot
};

struct PreviewConfig {
    bool enabled = true;
    std::string path = "/dev/shm/vhmd_preview";
    double fps = 15.0;
    uint32_t max_eye_width = 960;
    int vulkan_device = -1;
};

struct DriverConfig {
    std::string bind_address = "127.0.0.1";
    uint16_t port = 39570;
    bool allow_remote = false; // must be true to bind anything other than loopback

    double update_rate_hz = 120.0;
    double timeout_ms = 500.0;
    TimeoutPolicy timeout_policy = TimeoutPolicy::Hold;
    double smoothing = 0.0; // 0 = off, see PoseSmoother

    bool vmt_enabled = false;
    VmtMapping vmt_mapping;

    bool apply_tracker_roles = true; // write roles to SteamVR "trackers" settings if unset
    bool trackers_on_demand = true;  // default for trackers without "onDemand"
    // "controllers" or "hands": which twin of each hand is connected at start
    // (switchable at runtime with config op "hand_mode").
    std::string hand_mode = "controllers";

    // HMD output. "window": IVRDisplayComponent desktop window (Windows).
    // "direct": IVRDriverDirectModeComponent (Linux; the compositor hands the
    // eye images to the driver). "auto": direct on Linux, window elsewhere.
    std::string display_mode = "auto";
    PreviewConfig preview; // direct mode only: eye images to shared memory
    int log_errors_per_second = 5;
    bool verbose = false;

    ParseLimits limits;
    std::vector<DeviceConfig> devices;

    const DeviceConfig* Find(std::string_view id) const;
    // Timeout policy a device actually uses: its own override, else
    // "disconnect" for on-demand trackers (they grey out when the sender
    // stops), else the global policy.
    TimeoutPolicy EffectivePolicy(const DeviceConfig& d) const;
};

struct ConfigLoadResult {
    DriverConfig config;
    std::vector<std::string> warnings;
    bool ok = true; // false only if the text could not be parsed at all (defaults used)
};

// Built-in defaults: 1 HMD, 2 controllers, 8 trackers.
DriverConfig DefaultConfig();

// Parses config JSON text, starting from DefaultConfig(). Unknown keys are
// ignored, bad values produce warnings and fall back to defaults.
ConfigLoadResult LoadConfigFromJson(std::string_view text);

} // namespace vhmd
