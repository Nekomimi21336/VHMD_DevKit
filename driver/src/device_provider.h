// IServerTrackedDeviceProvider: builds devices from the configuration, runs
// the UDP receiver thread and the fixed-rate pose update thread.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
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
    void OnDatagram(std::string_view data, const Endpoint& from);
    int DeviceIndexFor(const std::string& id) const;
    std::string SetRole(const std::string& id, const RoleInfo& role);

    DriverConfig config_;
    std::string install_path_;
    std::vector<std::shared_ptr<DeviceState>> states_;
    std::vector<std::unique_ptr<VirtualDevice>> devices_;
    std::vector<ControllerDevice*> controllers_;
    std::vector<TrackerDevice*> trackers_;
    std::unique_ptr<MessageRouter> router_;

    UdpReceiver receiver_;
    std::mutex client_mu_;
    Endpoint last_client_;

    std::thread update_thread_;
    std::atomic<bool> running_{false};
};

} // namespace vhmd
