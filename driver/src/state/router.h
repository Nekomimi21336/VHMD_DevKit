// Routes decoded datagrams (JSON or VMT-OSC) to DeviceState buffers and
// answers "config" requests. Transport and OpenVR agnostic.
#pragma once

#include <atomic>
#include <functional>
#include <map>

#include <nlohmann/json.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/driver_config.h"
#include "config/roles.h"
#include "protocol/osc.h"
#include "state/device_state.h"
#include "util/log.h"

namespace vhmd {

#ifndef VHMD_VERSION
#define VHMD_VERSION "dev"
#endif
inline constexpr const char* kDriverVersion = VHMD_VERSION;

struct RouterHooks {
    // Returns the SteamVR device index for a protocol id, or -1 if not added.
    std::function<int(const std::string& id)> device_index;
    // Asks the provider to register an on-demand device with SteamVR. Called
    // from the receive thread; must be cheap and idempotent.
    std::function<void(const std::string& id)> request_add;
    // JSON object describing the HMD display path ("display" in status).
    std::function<std::string()> display_status;
    // Applies a tracker role. Returns an error string (empty = success).
    std::function<std::string(const std::string& id, const RoleInfo& role)> set_role;
    // Switches both hands between "controllers" and "hands" (hand tracking).
    // Returns an error string (empty = success).
    std::function<std::string(const std::string& mode)> set_hand_mode;
    // Current hand mode ("controllers" / "hands"), for status.
    std::function<std::string()> hand_mode;
    // Render models: "labeled" | "axes".
    std::function<std::string(const std::string& mode)> set_render_models;
    std::function<std::string()> render_models;
    // Display sync (HmdDevice::DisplaySync): error or "", shift applied (ms).
    std::function<std::string(double slack_ms, double period_ms, double target_ms, double weight, double* shift_ms)>
        display_sync;
    // Changes the virtual display's frame rate (direct mode). Returns an error string.
    std::function<std::string(double hz)> set_refresh_rate;
    // Current frame rate, for replies.
    std::function<double()> refresh_rate;
    // Shows / hides the preview window (Windows direct mode). Returns an error string.
    std::function<std::string(bool on)> set_preview_window;
    // Starts / pauses the direct-mode preview. Returns an error string (empty = success).
    std::function<std::string(bool on)> set_preview;
    // JSON object with the driver's own CPU use ("perf" in status).
    std::function<std::string()> perf_status;
};

class MessageRouter {
public:
    MessageRouter(const DriverConfig& config, std::vector<std::shared_ptr<DeviceState>> devices, RouterHooks hooks = {});

    // Handles one datagram. Returns a reply to send back to the sender, if any.
    std::optional<std::string> HandleDatagram(std::string_view data, Clock::time_point now);

    // Looks up a device by id, or by tracker role short name ("waist").
    DeviceState* Find(std::string_view id) const;

    std::string StatusJson(Clock::time_point now, std::optional<double> seq) const;

    // VMT outputs (/VMT/Out/...) due now, for the update thread to send to
    // ip:port. `pose_of` finds a device's raw pose by serial ("HMD" = HMD).
    struct VmtOutput {
        uint32_t ip = 0;
        uint16_t port = 0;
        std::vector<std::string> datagrams;
    };
    using PoseLookup = std::function<bool(const std::string& serial, Vec3& pos, Quat& rot)>;
    VmtOutput VmtOutputs(Clock::time_point now, const PoseLookup& pose_of, const std::function<std::string()>& devices_list);
    // Where to send /VMT/Out/Haptic for a device VMT drives (none if VMT is not in use).
    struct VmtHaptic {
        int index = 0;
        uint32_t ip = 0;
        uint16_t port = 0;
    };
    std::optional<VmtHaptic> VmtHapticTarget(const std::string& id);

    uint64_t TotalPackets() const { return total_packets_; }
    uint64_t RejectedMessages() const { return rejected_; }

private:
    std::optional<std::string> HandleConfig(const ConfigMessage& m, Clock::time_point now);
    // Control ownership: true if `src` may drive device `id` (counts senders).
    bool Accept(const std::string& id, const std::string& src, Clock::time_point now);
    std::string OwnerOf(const std::string& id) const;   // control_mu_ held
    nlohmann::json ControlJson(Clock::time_point now) const;
    void ReportErrors(const std::vector<std::string>& errors);

    DriverConfig config_;
    std::vector<std::shared_ptr<DeviceState>> devices_;
    std::map<std::string, DeviceState*, std::less<>> by_id_;
    RouterHooks hooks_;
    RateLimiter error_limiter_;
    std::atomic<uint64_t> total_packets_{0};

    // Who drives the devices: per-device owners over a default; "" = anyone.
    mutable std::mutex control_mu_;
    std::string default_owner_;
    std::map<std::string, std::string> owners_;
    struct SourceSeen {
        Clock::time_point last{};
        uint64_t accepted = 0, ignored = 0;
    };
    std::map<std::string, SourceSeen> sources_;
    std::atomic<uint64_t> rejected_{0};
    std::unique_ptr<VmtState> vmt_ = std::make_unique<VmtState>();
    Clock::time_point vmt_last_alive_{};
};

} // namespace vhmd
