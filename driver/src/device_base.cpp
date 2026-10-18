#include "device_base.h"

#include <cstring>

#include "util/log.h"

namespace vhmd {

namespace {
constexpr auto kPublishDelayAfterActivate = std::chrono::milliseconds(100);
} // namespace

vr::DriverPose_t MakeDriverPose(const OutputPose& out, bool head_model) {
    vr::DriverPose_t pose{};
    pose.qWorldFromDriverRotation.w = 1.0;
    pose.qDriverFromHeadRotation.w = 1.0;
    pose.vecPosition[0] = out.pos.x;
    pose.vecPosition[1] = out.pos.y;
    pose.vecPosition[2] = out.pos.z;
    pose.qRotation.w = out.rot.w;
    pose.qRotation.x = out.rot.x;
    pose.qRotation.y = out.rot.y;
    pose.qRotation.z = out.rot.z;
    pose.vecVelocity[0] = out.vel.x;
    pose.vecVelocity[1] = out.vel.y;
    pose.vecVelocity[2] = out.vel.z;
    pose.vecAngularVelocity[0] = out.angvel.x;
    pose.vecAngularVelocity[1] = out.angvel.y;
    pose.vecAngularVelocity[2] = out.angvel.z;
    pose.poseIsValid = out.pose_valid;
    pose.deviceIsConnected = out.connected;
    pose.result = out.pose_valid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
    pose.shouldApplyHeadModel = head_model;
    pose.willDriftInYaw = false;
    return pose;
}

VirtualDevice::VirtualDevice(std::shared_ptr<DeviceState> state) : state_(std::move(state)) {
    last_pose_ = MakeDriverPose(OutputPose{state_->Config().default_pos, state_->Config().default_rot}, false);
}

vr::EVRInitError VirtualDevice::Activate(uint32_t object_id) {
    std::lock_guard<std::mutex> lock(mu_);
    index_ = object_id;
    activated_at_ = Clock::now();
    const auto container = vr::VRProperties()->TrackedDeviceToPropertyContainer(object_id);
    OnActivate(container);
    Log(LogLevel::Info, "activated %s (serial %s) as device index %u", Id().c_str(), Serial().c_str(), object_id);
    return vr::VRInitError_None;
}

void VirtualDevice::Deactivate() {
    std::lock_guard<std::mutex> lock(mu_);
    OnDeactivate();
    index_ = vr::k_unTrackedDeviceIndexInvalid;
}

uint32_t VirtualDevice::Index() const {
    std::lock_guard<std::mutex> lock(mu_);
    return index_;
}

void* VirtualDevice::GetComponent(const char* /*name_and_version*/) { return nullptr; }

void VirtualDevice::DebugRequest(const char* /*request*/, char* response, uint32_t response_size) {
    if (response_size >= 1) response[0] = 0;
}

vr::DriverPose_t VirtualDevice::GetPose() {
    std::lock_guard<std::mutex> lock(mu_);
    return last_pose_;
}

void VirtualDevice::Update(Clock::time_point now) {
    // Read the shared buffers first (DeviceState has its own lock).
    const OutputPose out = state_->ComputeOutput(now);
    auto inputs = state_->TakeChangedInputs();
    auto skeleton = state_->TakeSkeleton();
    auto gaze = state_->TakeGaze();

    std::lock_guard<std::mutex> lock(mu_);
    if (index_ == vr::k_unTrackedDeviceIndexInvalid) return;
    // vrserver finishes taking ownership of the device shortly after Activate()
    // returns; pose updates sent in that window are rejected with
    // "PoseUpdated ... owned by driver (-1)". Give it a moment.
    if (now - activated_at_ < kPublishDelayAfterActivate) return;
    last_pose_ = MakeDriverPose(out, ApplyHeadModel());
    vr::VRServerDriverHost()->TrackedDevicePoseUpdated(index_, last_pose_, sizeof(vr::DriverPose_t));
    for (const auto& [name, value] : inputs) OnInput(name, value);
    if (skeleton) OnSkeleton(*skeleton);
    if (gaze) OnGaze(*gaze);
    OnTick(now);
}

void VirtualDevice::SetCommonProperties(vr::PropertyContainerHandle_t c, const char* model, const char* render_model) {
    auto* p = vr::VRProperties();
    p->SetStringProperty(c, vr::Prop_ModelNumber_String, model);
    p->SetStringProperty(c, vr::Prop_ManufacturerName_String, "VirtualHMD (development)");
    p->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "vhmd");
    p->SetStringProperty(c, vr::Prop_SerialNumber_String, Serial().c_str());
    p->SetStringProperty(c, vr::Prop_RenderModelName_String, render_model);
    p->SetStringProperty(c, vr::Prop_RegisteredDeviceType_String, ("vhmd/" + Serial()).c_str());
    p->SetStringProperty(c, vr::Prop_ResourceRoot_String, "vhmd");
    p->SetBoolProperty(c, vr::Prop_WillDriftInYaw_Bool, false);
    p->SetBoolProperty(c, vr::Prop_DeviceIsWireless_Bool, false);
    p->SetBoolProperty(c, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);
}

void VirtualDevice::SetIcons(vr::PropertyContainerHandle_t c, const std::string& prefix) {
    auto* p = vr::VRProperties();
    auto path = [&](const char* state) { return "{vhmd}/icons/" + prefix + "_status_" + state; };
    p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceOff_String, (path("off") + ".png").c_str());
    p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceSearching_String, (path("searching") + ".gif").c_str());
    p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceSearchingAlert_String, (path("searching_alert") + ".gif").c_str());
    p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceReady_String, (path("ready") + ".png").c_str());
    p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceReadyAlert_String, (path("ready_alert") + ".png").c_str());
    p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceNotReady_String, (path("error") + ".png").c_str());
    p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceStandby_String, (path("standby") + ".png").c_str());
    p->SetStringProperty(c, vr::Prop_NamedIconPathDeviceAlertLow_String, (path("ready_low") + ".png").c_str());
}

} // namespace vhmd
