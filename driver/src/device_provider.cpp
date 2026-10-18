#include "device_provider.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "controller_device.h"
#include "hmd_device.h"
#include "tracker_device.h"
#include "util/log.h"
#include "util/precise_sleep.h"

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
    LoadConfig();
    SetVerbose(config_.verbose);
    CreateDevices();

    RouterHooks hooks;
    hooks.device_index = [this](const std::string& id) { return DeviceIndexFor(id); };
    hooks.set_role = [this](const std::string& id, const RoleInfo& role) { return SetRole(id, role); };
    hooks.request_add = [this](const std::string& id) { RequestAdd(id); };
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

    running_ = true;
    update_thread_ = std::thread(&DeviceProvider::UpdateLoop, this);
    return vr::VRInitError_None;
}

void DeviceProvider::CreateDevices() {
    const DisplayConfig display = DisplayConfig::FromSettings();
    for (const auto& dc : config_.devices) {
        auto state = std::make_shared<DeviceState>(dc, config_);
        states_.push_back(state);
        if (!dc.enabled) continue;

        std::unique_ptr<VirtualDevice> dev;
        switch (dc.kind) {
        case DeviceKind::Hmd:
            dev = std::make_unique<HmdDevice>(state, display);
            break;
        case DeviceKind::Controller: {
            auto c = std::make_unique<ControllerDevice>(state);
            controllers_.push_back(c.get());
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
}

void DeviceProvider::RegisterDevice(VirtualDevice& dev) {
    if (!registered_.insert(dev.Id()).second) return;
    if (!vr::VRServerDriverHost()->TrackedDeviceAdded(dev.Serial().c_str(), dev.DeviceClass(), &dev)) {
        Log(LogLevel::Error, "TrackedDeviceAdded failed for %s (%s)", dev.Id().c_str(), dev.Serial().c_str());
    }
}

void DeviceProvider::RequestAdd(const std::string& id) {
    std::lock_guard<std::mutex> lock(add_mu_);
    if (!registered_.count(id)) pending_adds_.insert(id);
}

void DeviceProvider::ProcessPendingAdds() {
    std::set<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(add_mu_);
        ids.swap(pending_adds_);
    }
    for (const auto& id : ids) {
        for (auto& d : devices_) {
            if (d->Id() != id) continue;
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
        for (auto& d : devices_) d->Update(now);

        next += period;
        if (Clock::now() > next + period * 4) next = Clock::now(); // fell far behind: resync
        sleeper.SleepUntil(next);
    }
}

void DeviceProvider::OnDatagram(std::string_view data, const Endpoint& from) {
    {
        std::lock_guard<std::mutex> lock(client_mu_);
        last_client_ = from;
    }
    if (auto reply = router_->HandleDatagram(data, Clock::now())) receiver_.SendTo(*reply, from);
}

int DeviceProvider::DeviceIndexFor(const std::string& id) const {
    for (const auto& d : devices_) {
        if (d->Id() == id) {
            const uint32_t idx = d->Index();
            return idx == vr::k_unTrackedDeviceIndexInvalid ? -1 : static_cast<int>(idx);
        }
    }
    return -1;
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
            }
        }
        for (auto& d : devices_) d->HandleEvent(event);
    }
}

void DeviceProvider::Cleanup() {
    Log(LogLevel::Info, "shutting down");
    receiver_.Stop();
    running_ = false;
    if (update_thread_.joinable()) update_thread_.join();
    router_.reset();
    controllers_.clear();
    trackers_.clear();
    devices_.clear();
    pending_adds_.clear();
    registered_.clear();
    states_.clear();
    SetLogSink(nullptr);
    VR_CLEANUP_SERVER_DRIVER_CONTEXT();
}

} // namespace vhmd
