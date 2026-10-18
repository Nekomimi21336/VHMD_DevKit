// Thread-safe latest-value buffer for one virtual device. The network thread
// writes into it, the update thread reads from it. No OpenVR dependency so
// timeout/smoothing behavior can be unit tested.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "config/driver_config.h"
#include "math/pose_math.h"
#include "protocol/messages.h"

namespace vhmd {

using Clock = std::chrono::steady_clock;

struct OutputPose {
    Vec3 pos{};
    Quat rot{};
    Vec3 vel{};
    Vec3 angvel{};
    bool pose_valid = true;
    bool connected = true;
    bool timed_out = false;
};

struct DeviceStats {
    uint64_t pose_packets = 0;
    uint64_t input_packets = 0;
    double seconds_since_update = -1.0; // -1 = never received
    double rate_hz = 0.0;               // pose packets per second (smoothed)
    double sender_latency_ms = -1.0;    // now - ts of last pose (if ts given), unix clock
};

class DeviceState {
public:
    DeviceState(const DeviceConfig& cfg, const DriverConfig& global);

    // --- network thread -----------------------------------------------------
    void SubmitPose(const PoseMessage& m, Clock::time_point now);
    void SubmitInput(const InputMessage& m, Clock::time_point now);
    void SubmitSkeleton(const SkeletonMessage& m);
    void SubmitGaze(const GazeMessage& m);

    // --- update thread ------------------------------------------------------
    // Computes the pose to report at `now`, applying timeout policy and smoothing.
    OutputPose ComputeOutput(Clock::time_point now);

    // Returns input values received since the last call (latest per name).
    std::vector<std::pair<std::string, float>> TakeChangedInputs();
    std::optional<SkeletonMessage> TakeSkeleton();
    std::optional<GazeMessage> TakeGaze();

    DeviceStats Stats(Clock::time_point now) const;

    // Reset to the configured default pose (config op "reset").
    void Reset(Clock::time_point now);

    const DeviceConfig& Config() const { return cfg_; }

private:
    DeviceConfig cfg_;
    double timeout_s_;
    TimeoutPolicy policy_;
    double smoothing_;

    mutable std::mutex mu_;
    PoseMessage latest_{};
    bool explicit_invalid_ = false;
    std::optional<Clock::time_point> last_update_;
    Clock::time_point created_;
    std::optional<Clock::time_point> last_output_;
    PoseSmoother smoother_;

    std::map<std::string, float> pending_inputs_;
    std::optional<SkeletonMessage> skeleton_;
    std::optional<GazeMessage> gaze_;

    uint64_t pose_packets_ = 0;
    uint64_t input_packets_ = 0;
    double rate_hz_ = 0.0;
    double latency_ms_ = -1.0;
};

} // namespace vhmd
