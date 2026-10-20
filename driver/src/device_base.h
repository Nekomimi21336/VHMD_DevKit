// Common base for all virtual devices. Owns the per-device DeviceState link
// and the SteamVR device index; subclasses add properties and components.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <openvr_driver.h>

#include "state/device_state.h"
#include "visuals/device_visuals.h"

namespace vhmd {

// Tracking universe shared by all vhmd devices ("VHMD" in ASCII). Without one,
// SteamVR has no seated/standing calibration for them: the seated zero pose
// comes back as an all-zero matrix and OpenXR's LOCAL space is upside down.
inline constexpr uint64_t kUniverseId = 0x56484D44;

class VirtualDevice : public vr::ITrackedDeviceServerDriver {
public:
    explicit VirtualDevice(std::shared_ptr<DeviceState> state);
    // A second device driven by the same state (e.g. the hand-tracking twin of
    // a controller): its own serial, registered under Id() + key_suffix.
    VirtualDevice(std::shared_ptr<DeviceState> state, std::string serial, std::string key_suffix);
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

    // Called by the provider's update thread (not for followers: a leader
    // reads its state once per tick and hands the same data to its followers,
    // because inputs and skeleton messages are consumed when read).
    void Update(Clock::time_point now);
    void AddFollower(VirtualDevice* follower);
    bool IsFollower() const { return is_follower_; }

    // An inactive device reports itself disconnected (it keeps receiving input
    // and skeleton updates so it is current the moment it becomes active).
    void SetActive(bool active) { active_ = active; }
    bool IsActive() const { return active_; }
    // Called from RunFrame for every polled event. Returns true if consumed.
    virtual bool HandleEvent(const vr::VREvent_t& /*event*/) { return false; }

    virtual vr::ETrackedDeviceClass DeviceClass() const = 0;
    const std::string& Id() const { return state_->Config().id; }
    // Unique per device (Id() unless this is a twin sharing another device's state).
    const std::string& Key() const { return key_; }
    const std::string& Serial() const { return serial_; }
    // SteamVR object id, or vr::k_unTrackedDeviceIndexInvalid.
    uint32_t Index() const;

protected:
    // Subclass hooks (called with the device lock held, only while active).
    virtual void OnActivate(vr::PropertyContainerHandle_t container) = 0;
    virtual void OnDeactivate() {}
    virtual void OnInput(const std::string& /*name*/, float /*value*/) {}
    // "battery" (0..1) and "charging" for every device kind; true if handled.
    bool HandleCommonInput(const std::string& name, float value);
    virtual void OnSkeleton(const SkeletonMessage& /*m*/) {}
    virtual void OnGaze(const GazeMessage& /*m*/) {}
    // Called every tick after inputs were applied (e.g. skeleton refresh).
    virtual void OnTick(Clock::time_point /*now*/) {}
    // Called when SetActive() switched this device on (refresh derived state).
    virtual void OnBecameActive() {}
    virtual bool ApplyHeadModel() const { return false; }

    void SetCommonProperties(vr::PropertyContainerHandle_t container, const char* model, const char* render_model);
    void SetIcons(vr::PropertyContainerHandle_t container, const std::string& prefix);
    // Labelled render model + status icons generated for this device (under the
    // driver's resources folder). Keeps the given fallbacks if writing fails.
    void SetLabel(vr::PropertyContainerHandle_t container, ModelShape shape, const DeviceLabel& label);

public:
    // Driver install folder (set once by the provider before devices activate).
    static void SetInstallPath(const std::string& path);
    // Raw pose of another device by serial ("HMD" = the active HMD), for poses
    // sent relative to a parent (VMT Joint / Follow). Set by the provider; called
    // on the update thread.
    using ParentLookup = std::function<bool(const std::string& serial, Vec3& pos, Quat& rot)>;
    static void SetParentLookup(ParentLookup lookup);

protected:
    DeviceState& State() { return *state_; }

private:
    struct Frame {
        OutputPose out;
        std::vector<std::pair<std::string, float>> inputs;
        std::optional<SkeletonMessage> skeleton;
        std::optional<GazeMessage> gaze;
    };
    void Publish(Clock::time_point now, const Frame& f);

    std::shared_ptr<DeviceState> state_;
    std::string serial_;
    std::string key_;
    std::vector<VirtualDevice*> followers_;
    bool is_follower_ = false;
    std::atomic<bool> active_{true};
    mutable std::mutex mu_;
    uint32_t index_ = vr::k_unTrackedDeviceIndexInvalid;
    Clock::time_point activated_at_{};
    vr::DriverPose_t last_pose_{};
    Clock::time_point last_publish_{};
    bool published_active_ = true;
};

// Helpers shared by device implementations.
vr::DriverPose_t MakeDriverPose(const OutputPose& out, bool head_model);

} // namespace vhmd
