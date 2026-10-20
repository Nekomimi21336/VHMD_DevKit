#include "controller_device.h"

#include <algorithm>
#include <cctype>

// Hand pose generator from the OpenVR samples (BSD-3-Clause, see
// THIRD_PARTY_NOTICES.md), compiled from third_party/openvr directly.
#include "hand_simulation.h"
#include "util/log.h"

namespace vhmd {
namespace {

enum class Kind { Bool, Scalar1, Scalar2 };

struct InputDef {
    const char* name;
    const char* path;
    Kind kind;
};

// Shared by both hands.
constexpr InputDef kCommonInputs[] = {
    {"system", "/input/system/click", Kind::Bool},
    {"system_touch", "/input/system/touch", Kind::Bool},
    {"menu", "/input/menu/click", Kind::Bool},
    {"trigger", "/input/trigger/value", Kind::Scalar1},
    {"trigger_click", "/input/trigger/click", Kind::Bool},
    {"trigger_touch", "/input/trigger/touch", Kind::Bool},
    {"grip", "/input/grip/value", Kind::Scalar1},
    {"grip_click", "/input/grip/click", Kind::Bool},
    {"grip_touch", "/input/grip/touch", Kind::Bool},
    {"joystick_x", "/input/joystick/x", Kind::Scalar2},
    {"joystick_y", "/input/joystick/y", Kind::Scalar2},
    {"joystick_click", "/input/joystick/click", Kind::Bool},
    {"joystick_touch", "/input/joystick/touch", Kind::Bool},
    {"trackpad_x", "/input/trackpad/x", Kind::Scalar2},
    {"trackpad_y", "/input/trackpad/y", Kind::Scalar2},
    {"trackpad_click", "/input/trackpad/click", Kind::Bool},
    {"trackpad_touch", "/input/trackpad/touch", Kind::Bool},
    {"trackpad_force", "/input/trackpad/force", Kind::Scalar1},
    {"grip_force", "/input/grip/force", Kind::Scalar1},
    {"thumbrest_touch", "/input/thumbrest/touch", Kind::Bool},
};
constexpr InputDef kLeftInputs[] = {
    {"x", "/input/x/click", Kind::Bool},
    {"x_touch", "/input/x/touch", Kind::Bool},
    {"y", "/input/y/click", Kind::Bool},
    {"y_touch", "/input/y/touch", Kind::Bool},
};
constexpr InputDef kRightInputs[] = {
    {"a", "/input/a/click", Kind::Bool},
    {"a_touch", "/input/a/touch", Kind::Bool},
    {"b", "/input/b/click", Kind::Bool},
    {"b_touch", "/input/b/touch", Kind::Bool},
};

template <typename F>
void ForEachDef(bool left, F&& f) {
    for (const auto& d : kCommonInputs) f(d);
    if (left) {
        for (const auto& d : kLeftInputs) f(d);
    } else {
        for (const auto& d : kRightInputs) f(d);
    }
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ReplacePrefix(std::string& s, const std::string& from, const std::string& to) {
    if (s.rfind(from, 0) != 0) return false;
    s = to + s.substr(from.size());
    return true;
}

constexpr float kClickOn = 0.75f, kClickOff = 0.65f, kTouchOn = 0.05f, kTouchOff = 0.02f;

// The sample hand generator leaves the five aux bones (26..30) zeroed. Never
// hand SteamVR a zero-length quaternion: replace it with identity.
void SanitizeBones(vr::VRBoneTransform_t* bones, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        auto& q = bones[i].orientation;
        if (q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z < 1e-12f) q = {1.0f, 0.0f, 0.0f, 0.0f};
        bones[i].position.v[3] = 1.0f;
    }
}

} // namespace

ControllerDevice::ControllerDevice(std::shared_ptr<DeviceState> state)
    : VirtualDevice(std::move(state)), left_(State().Config().role != "right") {
    InitReferenceBones();
}

ControllerDevice::ControllerDevice(std::shared_ptr<DeviceState> state, std::string serial)
    : VirtualDevice(std::move(state), std::move(serial), "#hand"), left_(State().Config().role != "right"),
      hand_twin_(true) {
    InitReferenceBones();
}

std::unique_ptr<ControllerDevice> ControllerDevice::MakeHandTwin(std::shared_ptr<DeviceState> state) {
    std::string serial = state->Config().serial;
    if (const auto pos = serial.find("CTL"); pos != std::string::npos) {
        serial.replace(pos, 3, "HAND");
    } else {
        serial += "-HAND";
    }
    return std::unique_ptr<ControllerDevice>(new ControllerDevice(std::move(state), std::move(serial)));
}

void ControllerDevice::InitReferenceBones() {
    // Open-hand reference pose: positions for messages that only carry rotations.
    MyHandSimulation sim;
    sim.ComputeSkeletonTransforms(left_ ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand,
                                  MyFingerCurls{}, MyFingerSplays{}, reference_bones_.data());
    SanitizeBones(reference_bones_.data(), reference_bones_.size());
}

std::string ControllerDevice::CanonicalInputName(const std::string& raw, bool left) {
    if (raw.rfind("/input/", 0) == 0) {
        std::string found;
        ForEachDef(left, [&](const InputDef& d) {
            if (raw == d.path) found = d.name;
        });
        return found;
    }
    std::string n = Lower(raw);
    // Aliases
    ReplacePrefix(n, "thumbstick_", "joystick_") || ReplacePrefix(n, "stick_", "joystick_");
    ReplacePrefix(n, "squeeze", "grip");
    ReplacePrefix(n, "touchpad", "trackpad");
    ReplacePrefix(n, "application_menu", "menu");
    if (n == "trigger_value") n = "trigger";
    if (n == "grip_value") n = "grip";
    // primary/secondary face buttons follow the hand: A/B on the right, X/Y on the left.
    const std::string primary = left ? "x" : "a";
    const std::string secondary = left ? "y" : "b";
    if (ReplacePrefix(n, "primary", primary) || ReplacePrefix(n, "secondary", secondary)) {
    } else if (left) {
        ReplacePrefix(n, "a", "x") || ReplacePrefix(n, "b", "y");
    } else {
        ReplacePrefix(n, "x", "a") || ReplacePrefix(n, "y", "b");
    }
    std::string found;
    ForEachDef(left, [&](const InputDef& d) {
        if (n == d.name) found = d.name;
    });
    return found;
}

void ControllerDevice::RefreshRenderModel() {
    const auto c = Container();
    if (c != vr::k_ulInvalidPropertyContainer) SetLabel(c, ModelShape::Controller, ControllerLabel(left_, Id()));
}

void ControllerDevice::OnActivate(vr::PropertyContainerHandle_t c) {
    auto* p = vr::VRProperties();
    auto* in = vr::VRDriverInput();
    const char* model = hand_twin_ ? (left_ ? "VHMD Virtual Hand (Left)" : "VHMD Virtual Hand (Right)")
                                   : (left_ ? "VHMD Virtual Controller (Left)" : "VHMD Virtual Controller (Right)");
    SetCommonProperties(c, model, "{vhmd}vhmd_controller"); // has "tip"/"handgrip" components (tools/gen_rendermodel.py)
    SetIcons(c, "controller");
    SetLabel(c, ModelShape::Controller, ControllerLabel(left_, Id()));
    p->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32,
                        left_ ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
    // Only one of a controller / hand twin is connected at a time; if both
    // ever are, SteamVR gives the hand role to the controller.
    p->SetInt32Property(c, vr::Prop_ControllerHandSelectionPriority_Int32, hand_twin_ ? -1 : 0);
    p->SetStringProperty(c, vr::Prop_ControllerType_String, "vhmd_controller");
    p->SetStringProperty(c, vr::Prop_InputProfilePath_String, "{vhmd}/input/vhmd_controller_profile.json");
    p->SetInt32Property(c, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_Controller);

    components_.clear();
    ForEachDef(left_, [&](const InputDef& d) {
        Component comp;
        comp.boolean = d.kind == Kind::Bool;
        vr::EVRInputError err;
        if (comp.boolean) {
            err = in->CreateBooleanComponent(c, d.path, &comp.handle);
        } else {
            err = in->CreateScalarComponent(c, d.path, &comp.handle, vr::VRScalarType_Absolute,
                                            d.kind == Kind::Scalar2 ? vr::VRScalarUnits_NormalizedTwoSided
                                                                    : vr::VRScalarUnits_NormalizedOneSided);
        }
        if (err != vr::VRInputError_None) Log(LogLevel::Warning, "%s: failed to create %s (%d)", Id().c_str(), d.path, err);
        components_[d.name] = comp;
    });
    in->CreateHapticComponent(c, "/output/haptic", &haptic_);

    if (hand_twin_ || State().Config().skeleton) {
        const auto err = in->CreateSkeletonComponent(
            c, left_ ? "/input/skeleton/left" : "/input/skeleton/right",
            left_ ? "/skeleton/hand/left" : "/skeleton/hand/right", "/pose/raw",
            // Partial lets SteamVR re-synthesize finger bones (explicit bone
            // rotations are then not passed through as sent); Full passes them.
            hand_twin_ || State().Config().skeleton_full ? vr::VRSkeletalTracking_Full : vr::VRSkeletalTracking_Partial,
            nullptr, 0, &skeleton_);
        if (err != vr::VRInputError_None) {
            Log(LogLevel::Warning, "%s: CreateSkeletonComponent failed (%d)", Id().c_str(), err);
            skeleton_ = vr::k_ulInvalidInputComponentHandle;
        } else {
            PushSkeleton(reference_bones_.data()); // SteamVR expects data right away
        }
    }
}

void ControllerDevice::OnDeactivate() {
    components_.clear();
    haptic_ = vr::k_ulInvalidInputComponentHandle;
    skeleton_ = vr::k_ulInvalidInputComponentHandle;
}

void ControllerDevice::SetComponent(const std::string& canonical, float value) {
    values_[canonical] = value;
    auto it = components_.find(canonical);
    if (it == components_.end() || it->second.handle == vr::k_ulInvalidInputComponentHandle) return;
    if (it->second.boolean) {
        vr::VRDriverInput()->UpdateBooleanComponent(it->second.handle, value >= 0.5f, 0.0);
    } else {
        vr::VRDriverInput()->UpdateScalarComponent(it->second.handle, value, 0.0);
    }
    // The profile declares force for the trackpad and grip, and SteamVR then
    // derives their click from the force value. Until a client sends force
    // itself, mirror the click into it so a plain click still clicks.
    for (const char* axis : {"trackpad", "grip"}) {
        const std::string a = axis;
        if (canonical == a + "_click" && !explicit_[a + "_force"]) SetComponent(a + "_force", value >= 0.5f ? 1.0f : 0.0f);
    }
}

void ControllerDevice::DeriveClickTouch(const std::string& axis, float value) {
    // Derive click/touch from an analog value unless the client drives them.
    const std::string click = axis + "_click", touch = axis + "_touch";
    if (!explicit_[click]) {
        const bool was = values_[click] >= 0.5f;
        const bool now = was ? value > kClickOff : value >= kClickOn;
        if (now != was) SetComponent(click, now ? 1.0f : 0.0f);
    }
    if (!explicit_[touch]) {
        const bool was = values_[touch] >= 0.5f;
        const bool now = was ? value > kTouchOff : value >= kTouchOn;
        if (now != was) SetComponent(touch, now ? 1.0f : 0.0f);
    }
}

void ControllerDevice::OnInput(const std::string& name, float value) {
    const std::string canonical = CanonicalInputName(name, left_);
    if (canonical.empty()) {
        Log(LogLevel::Debug, "%s: unknown input '%s'", Id().c_str(), name.c_str());
        return;
    }
    const auto& kinds = components_;
    auto it = kinds.find(canonical);
    if (it != kinds.end() && !it->second.boolean) {
        // Scalar: clamp to the component's range.
        const bool two_sided = canonical == "joystick_x" || canonical == "joystick_y" || canonical == "trackpad_x" ||
                               canonical == "trackpad_y";
        value = std::clamp(value, two_sided ? -1.0f : 0.0f, 1.0f);
    }
    if (canonical.size() > 6 && (canonical.rfind("_click") == canonical.size() - 6 ||
                                 canonical.rfind("_touch") == canonical.size() - 6 ||
                                 canonical.rfind("_force") == canonical.size() - 6)) {
        explicit_[canonical] = true;
    }
    SetComponent(canonical, value);
    if (canonical == "trigger" || canonical == "grip") DeriveClickTouch(canonical, value);
}

void ControllerDevice::PushSkeleton(const vr::VRBoneTransform_t* bones) {
    if (skeleton_ == vr::k_ulInvalidInputComponentHandle) return;
    auto* in = vr::VRDriverInput();
    in->UpdateSkeletonComponent(skeleton_, vr::VRSkeletalMotionRange_WithController, bones, 31);
    in->UpdateSkeletonComponent(skeleton_, vr::VRSkeletalMotionRange_WithoutController, bones, 31);
}

void ControllerDevice::UpdateSkeletonFromCurls(const std::array<float, 5>& curl, const std::array<float, 5>& splay) {
    MyHandSimulation sim;
    std::array<vr::VRBoneTransform_t, 31> bones{};
    sim.ComputeSkeletonTransforms(left_ ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand,
                                  MyFingerCurls{curl[0], curl[1], curl[2], curl[3], curl[4]},
                                  MyFingerSplays{splay[0], splay[1], splay[2], splay[3], splay[4]}, bones.data());
    SanitizeBones(bones.data(), bones.size());
    PushSkeleton(bones.data());
}

void ControllerDevice::OnSkeleton(const SkeletonMessage& m) {
    if (skeleton_ == vr::k_ulInvalidInputComponentHandle) return;
    last_explicit_skeleton_ = Clock::now();
    derived_pushed_at_.reset(); // re-derive as soon as explicit data stops
    if (m.rotations.size() == 31) {
        std::array<vr::VRBoneTransform_t, 31> bones = reference_bones_;
        for (size_t i = 0; i < 31; ++i) {
            const Quat& q = m.rotations[i];
            bones[i].orientation = {static_cast<float>(q.w), static_cast<float>(q.x), static_cast<float>(q.y),
                                    static_cast<float>(q.z)};
            if (m.positions.size() == 31) {
                const Vec3& p = m.positions[i];
                bones[i].position = {{static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z), 1.0f}};
            }
        }
        PushSkeleton(bones.data());
        return;
    }
    if (m.curl) UpdateSkeletonFromCurls(*m.curl, m.splay.value_or(std::array<float, 5>{}));
}

void ControllerDevice::OnTick(Clock::time_point now) {
    if (skeleton_ == vr::k_ulInvalidInputComponentHandle) return;
    // While the client streams skeleton data, leave it alone; otherwise derive
    // a plausible hand pose from the button/axis state.
    if (last_explicit_skeleton_ && now - *last_explicit_skeleton_ < std::chrono::milliseconds(500)) return;
    auto v = [&](const char* k) {
        auto it = values_.find(k);
        return it == values_.end() ? 0.0f : it->second;
    };
    const bool thumb_down = v("a") + v("b") + v("x") + v("y") + v("a_touch") + v("b_touch") + v("x_touch") +
                                v("y_touch") + v("joystick_touch") + v("joystick_click") >
                            0.0f;
    const float index = std::max(v("trigger"), v("trigger_touch") * 0.25f);
    const float grip = v("grip");
    const std::array<float, 5> curl{thumb_down ? 0.7f : 0.0f, index, grip, grip, grip};
    // Unchanged: SteamVR keeps the last skeleton; re-send only once a second.
    if (derived_pushed_at_ && curl == derived_curl_ && now - *derived_pushed_at_ < std::chrono::seconds(1)) return;
    derived_curl_ = curl;
    derived_pushed_at_ = now;
    UpdateSkeletonFromCurls(curl, {});
}

std::optional<HapticEvent> ControllerDevice::MatchHaptic(const vr::VREvent_t& event) const {
    if (event.eventType != vr::VREvent_Input_HapticVibration) return std::nullopt;
    if (haptic_ == vr::k_ulInvalidInputComponentHandle || event.data.hapticVibration.componentHandle != haptic_) {
        return std::nullopt;
    }
    HapticEvent h;
    h.id = Id();
    h.duration_s = event.data.hapticVibration.fDurationSeconds;
    h.frequency = event.data.hapticVibration.fFrequency;
    h.amplitude = event.data.hapticVibration.fAmplitude;
    return h;
}

} // namespace vhmd
