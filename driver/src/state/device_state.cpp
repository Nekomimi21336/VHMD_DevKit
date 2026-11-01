#include "state/device_state.h"

#include <algorithm>

namespace vhmd {
namespace {

double Seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

double UnixNow() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

std::optional<TrackingMode> ParseTrackingMode(std::string_view s) {
    if (s == "active") return TrackingMode::Active;
    if (s == "paused") return TrackingMode::Paused;
    if (s == "disconnected") return TrackingMode::Disconnected;
    return std::nullopt;
}

const char* ToString(TrackingMode mode) {
    switch (mode) {
    case TrackingMode::Paused: return "paused";
    case TrackingMode::Disconnected: return "disconnected";
    default: return "active";
    }
}

DeviceState::DeviceState(const DeviceConfig& cfg, const DriverConfig& global)
    : cfg_(cfg),
      timeout_s_(global.timeout_ms / 1000.0),
      policy_(global.EffectivePolicy(cfg)),
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
    if (tracking_ != TrackingMode::Active) return;

    explicit_invalid_ = !m.valid;
    if (!m.valid) return; // keep the previous pose for when tracking resumes
    latest_ = m;
}

void DeviceState::SubmitInput(const InputMessage& m, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    (void)now;
    ++input_packets_;
    if (tracking_ == TrackingMode::Disconnected) return;
    // Every received value is re-applied (coalesced to the latest per name and
    // tick). No "unchanged" filtering here: several names can alias the same
    // component ("b", "secondary", "/input/b/click"), so a per-name cache
    // would drop real changes.
    for (const auto& [name, value] : m.values) {
        pending_inputs_[name] = value;
        seen_inputs_.insert(name);
    }
}

void DeviceState::SubmitSkeleton(const SkeletonMessage& m) {
    std::lock_guard<std::mutex> lock(mu_);
    if (tracking_ != TrackingMode::Disconnected) skeleton_ = m;
}

void DeviceState::SubmitGaze(const GazeMessage& m) {
    std::lock_guard<std::mutex> lock(mu_);
    gaze_ = m;
}

bool DeviceOffset::IsZero() const {
    return pos.x == 0 && pos.y == 0 && pos.z == 0 && ypr.x == 0 && ypr.y == 0 && ypr.z == 0;
}

void ApplyOffset(OutputPose& out, const DeviceOffset& offset) {
    if (offset.IsZero()) return;
    if (offset.room) {
        out.pos = out.pos + offset.pos;
    } else {
        const Vec3 arm = Rotate(out.rot, offset.pos);
        out.pos = out.pos + arm;
        out.vel = out.vel + Cross(out.angvel, arm);   // the corrected point swings with the device
    }
    out.rot = Multiply(out.rot, offset.rot);
}

void DeviceState::SetOffset(const Vec3& pos, const Vec3& ypr, bool room) {
    constexpr double kRadians = 3.14159265358979323846 / 180.0;
    std::lock_guard<std::mutex> lock(mu_);
    offset_.pos = pos;
    offset_.ypr = ypr;
    offset_.room = room;
    offset_.rot = FromYawPitchRoll(ypr.x * kRadians, ypr.y * kRadians, ypr.z * kRadians);
}

DeviceOffset DeviceState::Offset() const {
    std::lock_guard<std::mutex> lock(mu_);
    return offset_;
}

void DeviceState::SetTracking(TrackingMode mode, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    if (mode == tracking_) return;
    tracking_ = mode;
    StopMotion(now);
    if (mode == TrackingMode::Disconnected) {
        ReleaseInputs();
        skeleton_.reset();
    }
}

TrackingMode DeviceState::Tracking() const {
    std::lock_guard<std::mutex> lock(mu_);
    return tracking_;
}

OutputPose DeviceState::ComputeOutput(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    OutputPose out;

    const double dt = last_output_ ? Seconds(now - *last_output_) : 0.0;
    last_output_ = now;

    // Before the first packet the device sits at its configured default pose;
    // the timeout clock starts at creation so policies apply uniformly.
    const Clock::time_point ref = last_update_.value_or(created_);
    // A pose relative to a parent (VMT Joint / Follow) keeps following it without
    // new packets, like VMT; the parent's own validity decides.
    out.timed_out = tracking_ != TrackingMode::Paused && latest_.parent.empty() && Seconds(now - ref) > timeout_s_;

    smoother_.Update(latest_.pos, latest_.rot, smoothing_, dt);
    out.pos = smoother_.Position();
    out.rot = smoother_.Rotation();

    if (!out.timed_out) {
        if (latest_.vel) out.vel = *latest_.vel;
        if (latest_.angvel) out.angvel = *latest_.angvel;
        if (latest_.acc) out.acc = *latest_.acc;
        if (latest_.angacc) out.angacc = *latest_.angacc;
        // Sample age when sent + time since it arrived.
        if (latest_.age && last_update_) out.time_offset = -(*latest_.age + Seconds(now - *last_update_));
    }

    out.parent = latest_.parent;
    out.follow = latest_.follow;
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
    if (tracking_ == TrackingMode::Disconnected) {
        out.pose_valid = false;
        out.connected = false;
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

void DeviceState::Handover(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    ReleaseInputs();
    StopMotion(now);
}

void DeviceState::ReleaseInputs() {
    for (const auto& name : seen_inputs_) {
        if (name == "proximity" || name == "battery" || name == "charging") continue;
        pending_inputs_[name] = 0.0f;
    }
}

void DeviceState::StopMotion(Clock::time_point now) {
    latest_.vel.reset();
    latest_.angvel.reset();
    latest_.acc.reset();
    latest_.angacc.reset();
    latest_.age.reset();
    if (last_update_) last_update_ = now;
}

bool DeviceState::LatestPose(Vec3& pos, Quat& rot) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!last_update_ || pose_packets_ == 0) return false;
    pos = latest_.pos;
    rot = latest_.rot;
    return true;
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
