// IServerTrackedDeviceProvider: builds devices from the configuration, runs
// the UDP receiver thread and the fixed-rate pose update thread.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <openvr_driver.h>

#include "config/driver_config.h"
#include "device_base.h"
#include "net/udp_receiver.h"
#include "state/router.h"

namespace vhmd {

class ControllerDevice;
class TrackerDevice;
class HmdDevice;

class DeviceProvider : public vr::IServerTrackedDeviceProvider {
public:
    vr::EVRInitError Init(vr::IVRDriverContext* driver_context) override;
    void Cleanup() override;
    const char* const* GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
    void RunFrame() override;
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

private:
    std::string ResolveConfigPath() const;
    void LoadConfig();
    void CreateDevices();
    void UpdateLoop();
    // Raw poses of all SteamVR devices (any driver), refreshed each update tick;
    // serials re-read once a second. Used for VMT Joint/Follow and subscriptions.
    void RefreshRawPoses(Clock::time_point now);
    bool RawPoseOf(const std::string& serial, Vec3& pos, Quat& rot) const;
    std::string DevicesList() const;
    void SendVmtOutputs(Clock::time_point now);
    void OnDatagram(std::string_view data, const Endpoint& from);
    int DeviceIndexFor(const std::string& id) const;
    std::string SetRole(const std::string& id, const RoleInfo& role);
    void RegisterDevice(VirtualDevice& dev);
    void RequestAdd(const std::string& key);
    void ProcessPendingAdds();
    std::string SetHandMode(const std::string& mode);

    DriverConfig config_;
    std::string install_path_;
    std::vector<std::shared_ptr<DeviceState>> states_;
    std::vector<std::unique_ptr<VirtualDevice>> devices_;
    std::vector<ControllerDevice*> controllers_; // controllers and their hand twins
    std::vector<ControllerDevice*> hand_twins_;
    std::mutex hand_mode_mu_;
    std::string hand_mode_ = "controllers";
    std::vector<TrackerDevice*> trackers_;
    HmdDevice* hmd_ = nullptr;
    std::unique_ptr<MessageRouter> router_;

    UdpReceiver receiver_;
    std::mutex client_mu_;
    Endpoint last_client_;
    std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> raw_poses_{};
    std::array<std::string, vr::k_unMaxTrackedDeviceCount> serials_{};
    Clock::time_point serials_read_{};

    // On-demand registration: queued by the receive thread, performed in RunFrame.
    std::mutex add_mu_;
    std::set<std::string> pending_adds_;
    std::set<std::string> registered_;

    // Self-measured load, refreshed once per second by the update thread.
    struct Perf {
        std::atomic<double> update_cpu_pct{0}, receive_cpu_pct{0}, tick_avg_us{0}, tick_max_us{0};
    } perf_;

    std::thread update_thread_;
    std::atomic<bool> running_{false};
};

} // namespace vhmd
