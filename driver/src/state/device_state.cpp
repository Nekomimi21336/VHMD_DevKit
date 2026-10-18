#include "state/device_state.h"

#include <algorithm>

namespace vhmd {
namespace {

double Seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

double UnixNow() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

DeviceState::DeviceState(const DeviceConfig& cfg, const DriverConfig& global)
    : cfg_(cfg),
      timeout_s_(global.timeout_ms / 1000.0),
      policy_(cfg.timeout_policy.value_or(global.timeout_policy)),
      smoothing_(global.smoothing),
      created_(Clock::now()) {
    latest_.id = cfg.id;
    latest_.pos = cfg.default_pos;
    latest_.rot = cfg.default_rot;
}

void DeviceState::SubmitPose(const PoseMessage& m, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    if (last_update_) {
        const double dt = Seconds(now - *last_update_);
        if (dt > 0.0) {
            const double inst = 1.0 / dt;
            rate_hz_ = rate_hz_ == 0.0 ? inst : rate_hz_ * 0.9 + inst * 0.1;
        }
    }
    last_update_ = now;
    ++pose_packets_;
    if (m.ts) latency_ms_ = (UnixNow() - *m.ts) * 1000.0;

    explicit_invalid_ = !m.valid;
    if (!m.valid) return; // keep the previous pose for when tracking resumes
    latest_ = m;
}

void DeviceState::SubmitInput(const InputMessage& m, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    (void)now;
    ++input_packets_;
    // Every received value is re-applied (coalesced to the latest per name and
    // tick). No "unchanged" filtering here: several names can alias the same
    // component ("b", "secondary", "/input/b/click"), so a per-name cache
    // would drop real changes.
    for (const auto& [name, value] : m.values) pending_inputs_[name] = value;
}

void DeviceState::SubmitSkeleton(const SkeletonMessage& m) {
    std::lock_guard<std::mutex> lock(mu_);
    skeleton_ = m;
}

void DeviceState::SubmitGaze(const GazeMessage& m) {
    std::lock_guard<std::mutex> lock(mu_);
    gaze_ = m;
}

OutputPose DeviceState::ComputeOutput(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    OutputPose out;

    const double dt = last_output_ ? Seconds(now - *last_output_) : 0.0;
    last_output_ = now;

    // Before the first packet the device sits at its configured default pose;
    // the timeout clock starts at creation so policies apply uniformly.
    const Clock::time_point ref = last_update_.value_or(created_);
    out.timed_out = Seconds(now - ref) > timeout_s_;

    smoother_.Update(latest_.pos, latest_.rot, smoothing_, dt);
    out.pos = smoother_.Position();
    out.rot = smoother_.Rotation();

    if (!out.timed_out) {
        if (latest_.vel) out.vel = *latest_.vel;
        if (latest_.angvel) out.angvel = *latest_.angvel;
    }

    if (explicit_invalid_) out.pose_valid = false;
    if (out.timed_out) {
        switch (policy_) {
        case TimeoutPolicy::Hold:
            break; // keep last pose, zero velocity so prediction does not drift
        case TimeoutPolicy::Invalid:
            out.pose_valid = false;
            break;
        case TimeoutPolicy::Disconnect:
            out.pose_valid = false;
            out.connected = false;
            break;
        }
    }
    return out;
}

std::vector<std::pair<std::string, float>> DeviceState::TakeChangedInputs() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::pair<std::string, float>> out(pending_inputs_.begin(), pending_inputs_.end());
    pending_inputs_.clear();
    return out;
}

std::optional<SkeletonMessage> DeviceState::TakeSkeleton() {
    std::lock_guard<std::mutex> lock(mu_);
    auto s = std::move(skeleton_);
    skeleton_.reset();
    return s;
}

std::optional<GazeMessage> DeviceState::TakeGaze() {
    std::lock_guard<std::mutex> lock(mu_);
    auto g = std::move(gaze_);
    gaze_.reset();
    return g;
}

DeviceStats DeviceState::Stats(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mu_);
    DeviceStats s;
    s.pose_packets = pose_packets_;
    s.input_packets = input_packets_;
    if (last_update_) {
        s.seconds_since_update = Seconds(now - *last_update_);
        // Rate decays to 0 once packets stop arriving.
        s.rate_hz = s.seconds_since_update > 1.0 ? 0.0 : rate_hz_;
    }
    s.sender_latency_ms = latency_ms_;
    return s;
}

void DeviceState::Reset(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    latest_ = PoseMessage{};
    latest_.id = cfg_.id;
    latest_.pos = cfg_.default_pos;
    latest_.rot = cfg_.default_rot;
    explicit_invalid_ = false;
    last_update_ = now;
    smoother_ = PoseSmoother{};
}

} // namespace vhmd
