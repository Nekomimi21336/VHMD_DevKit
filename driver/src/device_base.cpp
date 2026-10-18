#include "device_base.h"

#include <cstring>
#include <filesystem>
#include <fstream>

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
    serial_ = state_->Config().serial;
    key_ = state_->Config().id;
    last_pose_ = MakeDriverPose(OutputPose{state_->Config().default_pos, state_->Config().default_rot}, false);
}

VirtualDevice::VirtualDevice(std::shared_ptr<DeviceState> state, std::string serial, std::string key_suffix)
    : VirtualDevice(std::move(state)) {
    serial_ = std::move(serial);
    key_ += key_suffix;
}

void VirtualDevice::AddFollower(VirtualDevice* follower) {
    follower->is_follower_ = true;
    followers_.push_back(follower);
}

vr::EVRInitError VirtualDevice::Activate(uint32_t object_id) {
    std::lock_guard<std::mutex> lock(mu_);
    index_ = object_id;
    activated_at_ = Clock::now();
    const auto container = vr::VRProperties()->TrackedDeviceToPropertyContainer(object_id);
    OnActivate(container);
    Log(LogLevel::Info, "activated %s (serial %s) as device index %u", Key().c_str(), Serial().c_str(), object_id);
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
    Frame f;
    f.out = state_->ComputeOutput(now);
    f.inputs = state_->TakeChangedInputs();
    f.skeleton = state_->TakeSkeleton();
    f.gaze = state_->TakeGaze();
    Publish(now, f);
    for (auto* follower : followers_) follower->Publish(now, f);
}

void VirtualDevice::Publish(Clock::time_point now, const Frame& f) {
    std::lock_guard<std::mutex> lock(mu_);
    if (index_ == vr::k_unTrackedDeviceIndexInvalid) return;
    // vrserver finishes taking ownership of the device shortly after Activate()
    // returns; pose updates sent in that window are rejected with
    // "PoseUpdated ... owned by driver (-1)". Give it a moment.
    if (now - activated_at_ < kPublishDelayAfterActivate) return;
    OutputPose out = f.out;
    if (!active_) {
        out.connected = false;
        out.pose_valid = false;
    }
    last_pose_ = MakeDriverPose(out, ApplyHeadModel());
    vr::VRServerDriverHost()->TrackedDevicePoseUpdated(index_, last_pose_, sizeof(vr::DriverPose_t));
    for (const auto& [name, value] : f.inputs) OnInput(name, value);
    if (f.skeleton) OnSkeleton(*f.skeleton);
    if (f.gaze) OnGaze(*f.gaze);
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

namespace {
std::string& InstallPath() {
    static std::string path;
    return path;
}

std::string Slug(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        out += (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ? c : '_';
    }
    return out;
}

bool WriteFile(const std::filesystem::path& p, const void* data, size_t size) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(data), std::streamsize(size));
    return bool(f);
}
} // namespace

void VirtualDevice::SetInstallPath(const std::string& path) { InstallPath() = path; }

void VirtualDevice::SetLabel(vr::PropertyContainerHandle_t c, ModelShape shape, const DeviceLabel& label) {
    if (InstallPath().empty()) return;
    namespace fs = std::filesystem;
    const fs::path resources = fs::u8path(InstallPath()) / "resources";
    std::error_code ec;

    // Render model: one folder per device and label, so SteamVR (which caches
    // models by name) shows the new one when the role changes.
    const std::string name = "vhmd_" + Slug(Serial()) + "_" + Slug(label.title);
    const fs::path dir = resources / "rendermodels" / name;
    if (!fs::exists(dir / (name + ".json"), ec)) {
        const RenderModelFiles files = MakeRenderModel(shape, label);
        fs::create_directories(dir, ec);
        const bool ok = !ec && WriteFile(dir / "body.obj", files.obj.data(), files.obj.size()) &&
                        WriteFile(dir / "body.mtl", files.mtl.data(), files.mtl.size()) &&
                        WriteFile(dir / "body.png", files.texture.data(), files.texture.size()) &&
                        WriteFile(dir / (name + ".json"), files.json.data(), files.json.size());
        if (!ok) {
            Log(LogLevel::Warning, "%s: could not write render model %s; keeping the default model", Key().c_str(),
                dir.u8string().c_str());
            return;
        }
    }
    auto* p = vr::VRProperties();
    p->SetStringProperty(c, vr::Prop_RenderModelName_String, ("{vhmd}" + name).c_str());

    // Status icons, shared by all devices with the same short text.
    const fs::path icons = resources / "icons" / "labels";
    fs::create_directories(icons, ec);
    const std::pair<IconState, vr::ETrackedDeviceProperty> props[] = {
        {IconState::Off, vr::Prop_NamedIconPathDeviceOff_String},
        {IconState::Ready, vr::Prop_NamedIconPathDeviceReady_String},
        {IconState::ReadyAlert, vr::Prop_NamedIconPathDeviceReadyAlert_String},
        {IconState::NotReady, vr::Prop_NamedIconPathDeviceNotReady_String},
        {IconState::Standby, vr::Prop_NamedIconPathDeviceStandby_String},
        {IconState::AlertLow, vr::Prop_NamedIconPathDeviceAlertLow_String},
        {IconState::Searching, vr::Prop_NamedIconPathDeviceSearching_String},
        {IconState::SearchingAlert, vr::Prop_NamedIconPathDeviceSearchingAlert_String},
    };
    for (const auto& [state, prop] : props) {
        const std::string file = Slug(label.icon_text) + "_" + IconStateName(state) + ".png";
        if (!fs::exists(icons / file, ec)) {
            const auto png = MakeStatusIcon(label.icon_text, state);
            if (!WriteFile(icons / file, png.data(), png.size())) continue; // keep the generic icon
        }
        p->SetStringProperty(c, prop, ("{vhmd}/icons/labels/" + file).c_str());
    }
    Log(LogLevel::Info, "%s: label \"%s\" / %s (icon %s)", Key().c_str(), label.title.c_str(), label.subtitle.c_str(),
        label.icon_text.c_str());
}

} // namespace vhmd
