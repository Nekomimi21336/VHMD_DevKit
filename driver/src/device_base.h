// Common base for all virtual devices. Owns the per-device DeviceState link
// and the SteamVR device index; subclasses add properties and components.
#pragma once

#include <memory>
#include <mutex>
#include <string>

#include <openvr_driver.h>

#include "state/device_state.h"

namespace vhmd {

class VirtualDevice : public vr::ITrackedDeviceServerDriver {
public:
    explicit VirtualDevice(std::shared_ptr<DeviceState> state);
    // ITrackedDeviceServerDriver has no virtual destructor; devices are owned
    // (and deleted) through VirtualDevice pointers only.
    virtual ~VirtualDevice() = default;

    // ITrackedDeviceServerDriver
    vr::EVRInitError Activate(uint32_t object_id) final;
    void Deactivate() final;
    void EnterStandby() override {}
    void* GetComponent(const char* name_and_version) override;
    void DebugRequest(const char* request, char* response, uint32_t response_size) override;
    vr::DriverPose_t GetPose() override;

    // Called by the provider's update thread.
    void Update(Clock::time_point now);
    // Called from RunFrame for every polled event. Returns true if consumed.
    virtual bool HandleEvent(const vr::VREvent_t& /*event*/) { return false; }

    virtual vr::ETrackedDeviceClass DeviceClass() const = 0;
    const std::string& Id() const { return state_->Config().id; }
    const std::string& Serial() const { return state_->Config().serial; }
    // SteamVR object id, or vr::k_unTrackedDeviceIndexInvalid.
    uint32_t Index() const;

protected:
    // Subclass hooks (called with the device lock held, only while active).
    virtual void OnActivate(vr::PropertyContainerHandle_t container) = 0;
    virtual void OnDeactivate() {}
    virtual void OnInput(const std::string& /*name*/, float /*value*/) {}
    virtual void OnSkeleton(const SkeletonMessage& /*m*/) {}
    virtual void OnGaze(const GazeMessage& /*m*/) {}
    // Called every tick after inputs were applied (e.g. skeleton refresh).
    virtual void OnTick(Clock::time_point /*now*/) {}
    virtual bool ApplyHeadModel() const { return false; }

    void SetCommonProperties(vr::PropertyContainerHandle_t container, const char* model, const char* render_model);
    void SetIcons(vr::PropertyContainerHandle_t container, const std::string& prefix);

    DeviceState& State() { return *state_; }

private:
    std::shared_ptr<DeviceState> state_;
    mutable std::mutex mu_;
    uint32_t index_ = vr::k_unTrackedDeviceIndexInvalid;
    Clock::time_point activated_at_{};
    vr::DriverPose_t last_pose_{};
};

// Helpers shared by device implementations.
vr::DriverPose_t MakeDriverPose(const OutputPose& out, bool head_model);

} // namespace vhmd
