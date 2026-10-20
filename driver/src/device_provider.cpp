#include "device_provider.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "controller_device.h"
#include "hmd_device.h"
#include "tracker_device.h"
#include "util/log.h"
#include "util/precise_sleep.h"
#include "util/thread_cpu.h"

namespace vhmd {
namespace {

constexpr const char* kDriverSection = "driver_vhmd";

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

std::string DeviceProvider::ResolveConfigPath() const {
    char buf[1024] = {};
    vr::EVRSettingsError err = vr::VRSettingsError_None;
    vr::VRSettings()->GetString(kDriverSection, "configFile", buf, sizeof(buf), &err);
    if (err == vr::VRSettingsError_None && buf[0] != '\0') return buf;
    return install_path_ + "/resources/settings/vhmd_config.json";
}

void DeviceProvider::LoadConfig() {
    const std::string path = ResolveConfigPath();
    const std::string text = ReadFile(std::filesystem::u8path(path));
    if (text.empty()) {
        Log(LogLevel::Warning, "config file '%s' not found or empty; using built-in defaults", path.c_str());
        config_ = DefaultConfig();
        return;
    }
    ConfigLoadResult r = LoadConfigFromJson(text);
    for (const auto& w : r.warnings) Log(LogLevel::Warning, "config: %s", w.c_str());
    config_ = std::move(r.config);
    Log(LogLevel::Info, "config loaded from '%s'", path.c_str());
}

vr::EVRInitError DeviceProvider::Init(vr::IVRDriverContext* driver_context) {
    VR_INIT_SERVER_DRIVER_CONTEXT(driver_context);
    SetLogSink([](LogLevel, const std::string& line) {
        if (auto* log = vr::VRDriverLog()) log->Log(line.c_str());
    });
    Log(LogLevel::Info, "VirtualHMD driver %s starting (OpenVR headers %u.%u.%u)", kDriverVersion, vr::k_nSteamVRVersionMajor,
        vr::k_nSteamVRVersionMinor, vr::k_nSteamVRVersionBuild);

    install_path_ = vr::VRProperties()->GetStringProperty(vr::VRDriverHandle(), vr::Prop_InstallPath_String);
    VirtualDevice::SetInstallPath(install_path_);
    LoadConfig();
    SetVerbose(config_.verbose);
    CreateDevices();

    RouterHooks hooks;
    hooks.device_index = [this](const std::string& id) { return DeviceIndexFor(id); };
    hooks.set_role = [this](const std::string& id, const RoleInfo& role) { return SetRole(id, role); };
    hooks.request_add = [this](const std::string& id) { RequestAdd(id); };
    hooks.set_refresh_rate = [this](double hz) -> std::string {
        if (!hmd_ || !hmd_->SetRefreshRate(hz)) return "changing the frame rate needs direct mode (display.mode = direct)";
        Log(LogLevel::Info, "refresh rate set to %.1f Hz", hz);
        return "";
    };
    hooks.refresh_rate = [this]() { return hmd_ ? hmd_->RefreshRate() : 0.0; };
    hooks.set_preview_window = [this](bool on) -> std::string {
        if (!hmd_ || !hmd_->SetPreviewWindow(on)) return "the preview window needs display.mode = direct on Windows";
        Log(LogLevel::Info, "preview window %s", on ? "shown" : "hidden");
        return "";
    };
    hooks.set_preview = [this](bool on) -> std::string {
        if (!hmd_ || !hmd_->SetPreviewActive(on)) return "preview needs direct mode (Linux, or display.mode = direct on Windows)";
        Log(LogLevel::Info, "preview %s", on ? "started" : "paused");
        return "";
    };
    hooks.perf_status = [this]() {
        nlohmann::json j = {{"update_thread_cpu_pct", perf_.update_cpu_pct.load()},
                            {"receive_thread_cpu_pct", perf_.receive_cpu_pct.load()},
                            {"update_tick_us_avg", perf_.tick_avg_us.load()},
                            {"update_tick_us_max", perf_.tick_max_us.load()}};
        return j.dump();
    };
    hooks.set_hand_mode = [this](const std::string& mode) { return SetHandMode(mode); };
    hooks.hand_mode = [this]() {
        std::lock_guard<std::mutex> lock(hand_mode_mu_);
        return hand_mode_;
    };
    hooks.display_status = [this]() { return hmd_ ? hmd_->DisplayStatusJson() : std::string("{}"); };
    router_ = std::make_unique<MessageRouter>(config_, states_, hooks);

    std::string err;
    if (!receiver_.Start(config_.bind_address, config_.port,
                         [this](std::string_view d, const Endpoint& from) { OnDatagram(d, from); }, err)) {
        // Keep the devices alive (they hold their default pose) so SteamVR still
        // starts; the problem is visible in vrserver.txt.
        Log(LogLevel::Error, "UDP receiver failed: %s", err.c_str());
    } else {
        Log(LogLevel::Info, "listening on udp://%s:%u%s", config_.bind_address.c_str(), receiver_.Port(),
            config_.vmt_enabled ? " (JSON + VMT OSC)" : " (JSON)");
        if (config_.bind_address != "127.0.0.1" && config_.bind_address != "localhost") {
            Log(LogLevel::Warning, "network.allowRemote is enabled: the driver accepts packets from other hosts");
        }
    }

    VirtualDevice::SetParentLookup([this](const std::string& serial, Vec3& pos, Quat& rot) {
        return RawPoseOf(serial, pos, rot);
    });
    running_ = true;
    update_thread_ = std::thread(&DeviceProvider::UpdateLoop, this);
    return vr::VRInitError_None;
}

void DeviceProvider::CreateDevices() {
    DisplayConfig display = DisplayConfig::FromSettings();
    if (config_.display_refresh_hz > 0.0) display.display_frequency = static_cast<float>(config_.display_refresh_hz);
    for (const auto& dc : config_.devices) {
        auto state = std::make_shared<DeviceState>(dc, config_);
        states_.push_back(state);
        if (!dc.enabled) continue;

        std::unique_ptr<VirtualDevice> dev;
        switch (dc.kind) {
        case DeviceKind::Hmd: {
#if defined(__linux__)
            const bool direct = config_.display_mode != "window";
#else
            const bool direct = config_.display_mode == "direct";
#endif
            auto h = std::make_unique<HmdDevice>(state, display, direct, config_.preview, config_.preview_window);
            hmd_ = h.get();
            dev = std::move(h);
            break;
        }
        case DeviceKind::Controller: {
            auto c = std::make_unique<ControllerDevice>(state);
            controllers_.push_back(c.get());
            // Hand-tracking twin: same data, registered the first time "hands"
            // mode is selected, connected only in that mode.
            auto twin = ControllerDevice::MakeHandTwin(state);
            twin->SetActive(false);
            c->AddFollower(twin.get());
            controllers_.push_back(twin.get());
            hand_twins_.push_back(twin.get());
            devices_.push_back(std::move(twin));
            dev = std::move(c);
            break;
        }
        case DeviceKind::Tracker: {
            auto t = std::make_unique<TrackerDevice>(state, config_.apply_tracker_roles);
            trackers_.push_back(t.get());
            dev = std::move(t);
            break;
        }
        }
        if (!dc.on_demand) RegisterDevice(*dev);
        devices_.push_back(std::move(dev));
    }
    size_t on_demand = 0;
    for (const auto& d : config_.devices) on_demand += d.enabled && d.on_demand;
    Log(LogLevel::Info, "registered %zu devices, %zu more on demand", registered_.size(), on_demand);
    if (config_.hand_mode == "hands") SetHandMode("hands");
}

std::string DeviceProvider::SetHandMode(const std::string& mode) {
    if (mode != "controllers" && mode != "hands") return "mode must be \"controllers\" or \"hands\"";
    if (hand_twins_.empty()) return "no controllers";
    const bool hands = mode == "hands";
    {
        std::lock_guard<std::mutex> lock(hand_mode_mu_);
        if (hand_mode_ == mode) return "";
        hand_mode_ = mode;
    }
    for (auto* c : controllers_) c->SetActive(c->IsHandTwin() == hands);
    // The twins are registered with SteamVR on first use (from RunFrame).
    if (hands) {
        for (auto* t : hand_twins_) RequestAdd(t->Key());
    }
    Log(LogLevel::Info, "hand mode: %s", mode.c_str());
    return "";
}

void DeviceProvider::RegisterDevice(VirtualDevice& dev) {
    if (!registered_.insert(dev.Key()).second) return;
    if (!vr::VRServerDriverHost()->TrackedDeviceAdded(dev.Serial().c_str(), dev.DeviceClass(), &dev)) {
        Log(LogLevel::Error, "TrackedDeviceAdded failed for %s (%s)", dev.Id().c_str(), dev.Serial().c_str());
    }
}

void DeviceProvider::RequestAdd(const std::string& key) {
    std::lock_guard<std::mutex> lock(add_mu_);
    if (!registered_.count(key)) pending_adds_.insert(key);
}

void DeviceProvider::ProcessPendingAdds() {
    std::set<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(add_mu_);
        ids.swap(pending_adds_);
    }
    for (const auto& id : ids) {
        for (auto& d : devices_) {
            if (d->Key() != id) continue;
            std::lock_guard<std::mutex> lock(add_mu_); // guards registered_
            if (registered_.count(id)) break;
            Log(LogLevel::Info, "adding on-demand device %s (%s)", id.c_str(), d->Serial().c_str());
            RegisterDevice(*d);
            break;
        }
    }
}

void DeviceProvider::UpdateLoop() {
    using namespace std::chrono;
    PreciseSleeper sleeper;
    const auto period = duration_cast<Clock::duration>(duration<double>(1.0 / config_.update_rate_hz));
    auto next = Clock::now();
    RateLimiter jitter_limiter(1.0 / 30.0, 1.0); // at most one warning per 30 s
    double worst_late_ms = 0.0;
    uint64_t late_ticks = 0;
    // Load accounting (reported in status as "perf").
    auto perf_since = Clock::now();
    double perf_cpu0 = CurrentThreadCpuSeconds(), perf_recv0 = receiver_.CpuSeconds();
    double work_sum_us = 0.0, work_max_us = 0.0;
    uint64_t work_ticks = 0;

    while (running_) {
        const auto now = Clock::now();
        const double late_ms = duration<double, std::milli>(now - next).count();
        if (late_ms > 4.0) {
            ++late_ticks;
            worst_late_ms = std::max(worst_late_ms, late_ms);
            uint64_t suppressed = 0;
            if (late_ticks >= 10 && jitter_limiter.Allow(suppressed)) {
                Log(LogLevel::Warning, "update thread jitter: %llu late ticks (worst %.1f ms)",
                    static_cast<unsigned long long>(late_ticks), worst_late_ms);
                late_ticks = 0;
                worst_late_ms = 0.0;
            }
        }
        const auto work0 = Clock::now();
        if (config_.vmt_enabled) RefreshRawPoses(now);
        for (auto& d : devices_) {
            if (!d->IsFollower()) d->Update(now); // followers are updated by their leader
        }
        if (config_.vmt_enabled) SendVmtOutputs(now);
        const double work_us = duration<double, std::micro>(Clock::now() - work0).count();
        work_sum_us += work_us;
        work_max_us = std::max(work_max_us, work_us);
        ++work_ticks;
        if (const double wall = duration<double>(Clock::now() - perf_since).count(); wall >= 1.0) {
            const double cpu = CurrentThreadCpuSeconds(), recv = receiver_.CpuSeconds();
            perf_.update_cpu_pct = 100.0 * (cpu - perf_cpu0) / wall;
            perf_.receive_cpu_pct = 100.0 * (recv - perf_recv0) / wall;
            perf_.tick_avg_us = work_sum_us / static_cast<double>(work_ticks);
            perf_.tick_max_us = work_max_us;
            perf_since = Clock::now();
            perf_cpu0 = cpu;
            perf_recv0 = recv;
            work_sum_us = work_max_us = 0.0;
            work_ticks = 0;
        }

        next += period;
        if (Clock::now() > next + period * 4) next = Clock::now(); // fell far behind: resync
        sleeper.SleepUntil(next);
    }
}

namespace {
Quat MatrixRotation(const vr::HmdMatrix34_t& m) {
    const double t = m.m[0][0] + m.m[1][1] + m.m[2][2];
    Quat q;
    if (t > 0) {
        const double s = std::sqrt(t + 1.0) * 2;
        q = {0.25 * s, (m.m[2][1] - m.m[1][2]) / s, (m.m[0][2] - m.m[2][0]) / s, (m.m[1][0] - m.m[0][1]) / s};
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        const double s = std::sqrt(1.0 + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2;
        q = {(m.m[2][1] - m.m[1][2]) / s, 0.25 * s, (m.m[0][1] + m.m[1][0]) / s, (m.m[0][2] + m.m[2][0]) / s};
    } else if (m.m[1][1] > m.m[2][2]) {
        const double s = std::sqrt(1.0 + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2;
        q = {(m.m[0][2] - m.m[2][0]) / s, (m.m[0][1] + m.m[1][0]) / s, 0.25 * s, (m.m[1][2] + m.m[2][1]) / s};
    } else {
        const double s = std::sqrt(1.0 + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2;
        q = {(m.m[1][0] - m.m[0][1]) / s, (m.m[0][2] + m.m[2][0]) / s, (m.m[1][2] + m.m[2][1]) / s, 0.25 * s};
    }
    if (!Normalize(q)) q = {};
    return q;
}
} // namespace

void DeviceProvider::RefreshRawPoses(Clock::time_point now) {
    vr::VRServerDriverHost()->GetRawTrackedDevicePoses(0.0f, raw_poses_.data(), static_cast<uint32_t>(raw_poses_.size()));
    if (now - serials_read_ < std::chrono::seconds(1)) return;
    serials_read_ = now;
    auto* props = vr::VRProperties();
    for (uint32_t i = 0; i < serials_.size(); ++i) {
        vr::ETrackedPropertyError err = vr::TrackedProp_Success;
        std::string serial = props->GetStringProperty(props->TrackedDeviceToPropertyContainer(i),
                                                      vr::Prop_SerialNumber_String, &err);
        serials_[i] = err == vr::TrackedProp_Success ? std::move(serial) : std::string();
    }
}

bool DeviceProvider::RawPoseOf(const std::string& serial, Vec3& pos, Quat& rot) const {
    for (uint32_t i = 0; i < serials_.size(); ++i) {
        const bool match = serial == "HMD" ? i == vr::k_unTrackedDeviceIndex_Hmd : (!serials_[i].empty() && serials_[i] == serial);
        if (!match) continue;
        const auto& p = raw_poses_[i];
        if (!p.bPoseIsValid) return false;
        const auto& m = p.mDeviceToAbsoluteTracking;
        pos = {m.m[0][3], m.m[1][3], m.m[2][3]};
        rot = MatrixRotation(m);
        return true;
    }
    return false;
}

std::string DeviceProvider::DevicesList() const {
    std::string out;
    for (uint32_t i = 0; i < serials_.size(); ++i) {
        if (!serials_[i].empty()) out += std::to_string(i) + ":" + serials_[i] + "\n";
    }
    return out;
}

void DeviceProvider::SendVmtOutputs(Clock::time_point now) {
    auto out = router_->VmtOutputs(
        now, [this](const std::string& serial, Vec3& p, Quat& q) { return RawPoseOf(serial, p, q); },
        [this]() { return DevicesList(); });
    if (out.datagrams.empty() || out.port == 0) return;
    Endpoint to;
    to.ipv4_host_order = out.ip;
    to.port = out.port;
    for (const auto& d : out.datagrams) receiver_.SendTo(d, to);
}

void DeviceProvider::OnDatagram(std::string_view data, const Endpoint& from) {
    {
        std::lock_guard<std::mutex> lock(client_mu_);
        last_client_ = from;
    }
    if (auto reply = router_->HandleDatagram(data, Clock::now())) receiver_.SendTo(*reply, from);
}

int DeviceProvider::DeviceIndexFor(const std::string& id) const {
    // A hand has two devices (controller and hand twin): report the active one.
    int fallback = -1;
    for (const auto& d : devices_) {
        if (d->Id() != id) continue;
        const uint32_t idx = d->Index();
        const int index = idx == vr::k_unTrackedDeviceIndexInvalid ? -1 : static_cast<int>(idx);
        if (d->IsActive()) return index;
        if (fallback < 0) fallback = index;
    }
    return fallback;
}

std::string DeviceProvider::SetRole(const std::string& id, const RoleInfo& role) {
    for (auto* t : trackers_) {
        if (t->Id() == id) return t->ApplyRole(role, /*overwrite=*/true);
    }
    return "tracker not registered";
}

void DeviceProvider::RunFrame() {
    ProcessPendingAdds();
    vr::VREvent_t event{};
    while (vr::VRServerDriverHost()->PollNextEvent(&event, sizeof(event))) {
        for (auto* c : controllers_) {
            if (auto h = c->MatchHaptic(event)) {
                Endpoint to;
                {
                    std::lock_guard<std::mutex> lock(client_mu_);
                    to = last_client_;
                }
                if (to.valid()) {
                    nlohmann::json j = {{"type", "haptic"},     {"id", h->id},
                                        {"duration", h->duration_s}, {"frequency", h->frequency},
                                        {"amplitude", h->amplitude}};
                    receiver_.SendTo(j.dump(), to);
                }
                if (auto vmt = router_->VmtHapticTarget(h->id)) {
                    Endpoint dest;
                    dest.ipv4_host_order = vmt->ip;
                    dest.port = vmt->port;
                    receiver_.SendTo(EncodeOscMessage("/VMT/Out/Haptic", {int32_t(vmt->index), float(h->frequency),
                                                                          float(h->amplitude), float(h->duration_s)}),
                                     dest);
                }
            }
        }
        for (auto& d : devices_) d->HandleEvent(event);
    }
}

void DeviceProvider::Cleanup() {
    Log(LogLevel::Info, "shutting down");
    VirtualDevice::SetParentLookup(nullptr);
    receiver_.Stop();
    running_ = false;
    if (update_thread_.joinable()) update_thread_.join();
    router_.reset();
    controllers_.clear();
    hand_twins_.clear();
    trackers_.clear();
    hmd_ = nullptr;
    devices_.clear();
    pending_adds_.clear();
    registered_.clear();
    states_.clear();
    SetLogSink(nullptr);
    VR_CLEANUP_SERVER_DRIVER_CONTEXT();
}

} // namespace vhmd
