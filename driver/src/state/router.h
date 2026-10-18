// Routes decoded datagrams (JSON or VMT-OSC) to DeviceState buffers and
// answers "config" requests. Transport and OpenVR agnostic.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/driver_config.h"
#include "config/roles.h"
#include "state/device_state.h"
#include "util/log.h"

namespace vhmd {

inline constexpr const char* kDriverVersion = "0.3.0";

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
};

class MessageRouter {
public:
    MessageRouter(const DriverConfig& config, std::vector<std::shared_ptr<DeviceState>> devices, RouterHooks hooks = {});

    // Handles one datagram. Returns a reply to send back to the sender, if any.
    std::optional<std::string> HandleDatagram(std::string_view data, Clock::time_point now);

    // Looks up a device by id, or by tracker role short name ("waist").
    DeviceState* Find(std::string_view id) const;

    std::string StatusJson(Clock::time_point now, std::optional<double> seq) const;

    uint64_t TotalPackets() const { return total_packets_; }
    uint64_t RejectedMessages() const { return rejected_; }

private:
    std::optional<std::string> HandleConfig(const ConfigMessage& m, Clock::time_point now);
    void ReportErrors(const std::vector<std::string>& errors);

    DriverConfig config_;
    std::vector<std::shared_ptr<DeviceState>> devices_;
    std::map<std::string, DeviceState*, std::less<>> by_id_;
    RouterHooks hooks_;
    RateLimiter error_limiter_;
    std::atomic<uint64_t> total_packets_{0};
    std::atomic<uint64_t> rejected_{0};
};

} // namespace vhmd
