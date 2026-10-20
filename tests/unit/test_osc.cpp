#include "test_framework.h"

#include <cstdint>
#include <cstring>
#include <variant>

#include "protocol/osc.h"

using namespace vhmd;

namespace {

// Minimal OSC encoder for tests.
class OscBuilder {
public:
    explicit OscBuilder(const std::string& address) { Str(address); }
    OscBuilder& I(int32_t v) {
        tags_ += 'i';
        U32(static_cast<uint32_t>(v));
        return *this;
    }
    OscBuilder& F(float v) {
        tags_ += 'f';
        uint32_t u;
        std::memcpy(&u, &v, 4);
        U32(u);
        return *this;
    }
    OscBuilder& S(const std::string& s) {
        tags_ += 's';
        Pad(args_, s);
        return *this;
    }
    std::string Build() const {
        std::string out = head_;
        Pad(out, tags_);
        return out + args_;
    }

private:
    static void Pad(std::string& out, const std::string& s) {
        out += s;
        out.push_back('\0');
        while (out.size() % 4) out.push_back('\0');
    }
    void Str(const std::string& s) { Pad(head_, s); }
    void U32(uint32_t u) {
        for (int i = 3; i >= 0; --i) args_.push_back(static_cast<char>((u >> (i * 8)) & 0xff));
    }
    std::string head_, tags_ = ",", args_;
};

std::string Bundle(const std::vector<std::string>& elems) {
    std::string out("#bundle\0", 8);
    out += std::string(8, '\0'); // timetag
    for (const auto& e : elems) {
        const uint32_t n = static_cast<uint32_t>(e.size());
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<char>((n >> (i * 8)) & 0xff));
        out += e;
    }
    return out;
}

} // namespace

TEST(osc_room_driver_pose) {
    VmtMapping map;
    auto pkt = OscBuilder("/VMT/Room/Driver").I(2).I(1).F(0).F(0.5f).F(1.0f).F(-0.25f).F(0).F(0).F(0).F(1).Build();
    auto r = ParseOscPacket(pkt, map);
    CHECK(r.errors.empty());
    const auto* p = std::get_if<PoseMessage>(&r.messages.at(0));
    CHECK(p && p->id == "tracker_2");
    if (!p) return;
    CHECK_NEAR(p->pos.x, 0.5, 1e-6);
    CHECK_NEAR(p->pos.z, -0.25, 1e-6);
    CHECK_NEAR(p->rot.w, 1.0, 1e-6);
}

TEST(osc_unity_pose_is_converted) {
    VmtMapping map;
    map.index_to_id = {"tracker_7"};
    auto pkt = OscBuilder("/VMT/Raw/Unity").I(0).I(1).F(0).F(0).F(1).F(2).F(0).F(0).F(0).F(1).Build();
    auto r = ParseOscPacket(pkt, map);
    const auto* p = std::get_if<PoseMessage>(&r.messages.at(0));
    CHECK(p && p->id == "tracker_7");
    if (p) CHECK_NEAR(p->pos.z, -2.0, 1e-6);
}

TEST(osc_enable_selects_hand_and_disable) {
    VmtMapping map;
    auto left = OscBuilder("/VMT/Room/Driver").I(5).I(2).F(0).F(0).F(1).F(0).F(0).F(0).F(0).F(1).Build();
    auto r = ParseOscPacket(left, map);
    CHECK(std::get<PoseMessage>(r.messages.at(0)).id == "left_hand");

    auto off = OscBuilder("/VMT/Room/Driver").I(1).I(0).F(0).F(0).F(1).F(0).F(0).F(0).F(0).F(1).Build();
    auto r2 = ParseOscPacket(off, map);
    CHECK(!std::get<PoseMessage>(r2.messages.at(0)).valid);
}

TEST(osc_inputs) {
    VmtMapping map;
    map.index_to_id = {"right_hand"};
    auto trig = OscBuilder("/VMT/Input/Trigger").I(0).I(0).F(0).F(0.7f).Build();
    auto r = ParseOscPacket(trig, map);
    const auto* in = std::get_if<InputMessage>(&r.messages.at(0));
    CHECK(in && in->id == "right_hand" && in->values.at(0).first == "trigger");

    auto btn = OscBuilder("/VMT/Input/Button/Touch").I(0).I(1).F(0).I(1).Build();
    auto r2 = ParseOscPacket(btn, map);
    CHECK(std::get<InputMessage>(r2.messages.at(0)).values.at(0).first == "a_touch");

    auto joy = OscBuilder("/VMT/Input/Joystick").I(0).I(0).F(0).F(-0.5f).F(0.25f).Build();
    auto r3 = ParseOscPacket(joy, map);
    CHECK(std::get<InputMessage>(r3.messages.at(0)).values.size() == 2);
}

TEST(osc_bundle_and_garbage) {
    VmtMapping map;
    auto a = OscBuilder("/VMT/Room/Driver").I(0).I(1).F(0).F(0).F(1).F(0).F(0).F(0).F(0).F(1).Build();
    auto b = OscBuilder("/VMT/Room/Driver").I(1).I(1).F(0).F(0).F(1).F(0).F(0).F(0).F(0).F(1).Build();
    auto r = ParseOscPacket(Bundle({a, b}), map);
    CHECK(r.messages.size() == 2);

    // Truncated / garbage packets must not crash and must be rejected.
    auto t = ParseOscPacket(a.substr(0, a.size() - 5), map);
    CHECK(t.messages.empty() && !t.errors.empty());
    auto g = ParseOscPacket(std::string("/VMT\0\0\0\0,ffz", 13), map);
    CHECK(g.messages.empty());
    auto u = ParseOscPacket(OscBuilder("/VMT/SetRoomMatrix").F(1).Build(), map);
    CHECK(u.messages.empty() && !u.errors.empty());
    // Wrong argument types
    auto w = ParseOscPacket(OscBuilder("/VMT/Room/Driver").S("x").Build(), map);
    CHECK(w.messages.empty());
}

TEST(osc_input_names_extended) {
    VmtMapping map;
    map.index_to_id = {"right_hand"};
    auto touch = ParseOscPacket(OscBuilder("/VMT/Input/Button/Touch").I(0).I(4).F(0).I(1).Build(), map);
    CHECK(std::get<InputMessage>(touch.messages.at(0)).values.at(0).first == "joystick_touch");
    auto click = ParseOscPacket(OscBuilder("/VMT/Input/Button").I(0).I(5).F(0).I(1).Build(), map);
    CHECK(std::get<InputMessage>(click.messages.at(0)).values.at(0).first == "trigger_click");
    auto pad = ParseOscPacket(OscBuilder("/VMT/Input/Joystick").I(0).I(1).F(0).F(0.5f).F(-0.5f).Build(), map);
    CHECK(std::get<InputMessage>(pad.messages.at(0)).values.at(0).first == "trackpad_x");
    auto force = ParseOscPacket(OscBuilder("/VMT/Input/Trigger").I(0).I(2).F(0).F(0.3f).Build(), map);
    CHECK(std::get<InputMessage>(force.messages.at(0)).values.at(0).first == "grip_force");
}

TEST(osc_joint_and_follow) {
    VmtMapping map;
    VmtState state;
    auto j = ParseOscPacket(OscBuilder("/VMT/Joint/Driver").I(3).I(1).F(0).F(0).F(0.1f).F(0).F(0).F(0).F(0).F(1).S("HMD").Build(),
                            map, {}, &state);
    const auto* jp = std::get_if<PoseMessage>(&j.messages.at(0));
    CHECK(jp && jp->parent == "HMD" && !jp->follow && jp->id == "tracker_3");
    auto f = ParseOscPacket(OscBuilder("/VMT/Follow/Unity").I(3).I(1).F(0).F(0).F(0).F(1).F(0).F(0).F(0).F(1).S("VHMD-CTL-L").Build(),
                            map, {}, &state);
    const auto* fp = std::get_if<PoseMessage>(&f.messages.at(0));
    CHECK(fp && fp->follow && fp->parent == "VHMD-CTL-L");
    if (fp) CHECK_NEAR(fp->pos.z, -1.0, 1e-6); // Unity +Z forward -> driver -Z
    auto missing = ParseOscPacket(OscBuilder("/VMT/Joint/Driver").I(3).I(1).F(0).F(0).F(0).F(0).F(0).F(0).F(0).F(1).Build(),
                                  map, {}, &state);
    CHECK(missing.messages.empty() && !missing.errors.empty());
}

TEST(osc_room_matrix_applies_to_room_only) {
    VmtMapping map;
    VmtState state;
    // Room -> driver: translate by (1, 0, 0) and turn 90 degrees about +Y.
    auto set = ParseOscPacket(OscBuilder("/VMT/SetRoomMatrix").F(0).F(0).F(1).F(1).F(0).F(1).F(0).F(0).F(-1).F(0).F(0).F(0).Build(),
                              map, {}, &state);
    CHECK(set.errors.empty());
    auto room = ParseOscPacket(OscBuilder("/VMT/Room/Driver").I(0).I(1).F(0).F(0).F(0).F(-1).F(0).F(0).F(0).F(1).Build(),
                               map, {}, &state);
    const auto* rp = std::get_if<PoseMessage>(&room.messages.at(0));
    CHECK(rp != nullptr);
    if (rp) {
        CHECK_NEAR(rp->pos.x, 0.0, 1e-6); // (0,0,-1) rotated -> (-1,0,0), + (1,0,0)
        CHECK_NEAR(rp->pos.z, 0.0, 1e-6);
    }
    auto raw = ParseOscPacket(OscBuilder("/VMT/Raw/Driver").I(0).I(1).F(0).F(0).F(0).F(-1).F(0).F(0).F(0).F(1).Build(),
                              map, {}, &state);
    CHECK_NEAR(std::get<PoseMessage>(raw.messages.at(0)).pos.z, -1.0, 1e-6);
}

TEST(osc_skeleton_buffer_scalar_and_reset) {
    VmtMapping map;
    VmtState state;
    // Index 0 registered as the right hand (enable 3).
    ParseOscPacket(OscBuilder("/VMT/Room/Driver").I(0).I(3).F(0).F(0).F(1).F(0).F(0).F(0).F(0).F(1).Build(), map, {}, &state);
    for (int b = 0; b < 31; ++b) {
        ParseOscPacket(OscBuilder("/VMT/Skeleton/Driver").I(0).I(b).F(0).F(0.01f).F(0).F(0).F(0).F(0).F(1).Build(), map, {}, &state);
    }
    auto apply = ParseOscPacket(OscBuilder("/VMT/Skeleton/Apply").I(0).F(0).Build(), map, {}, &state);
    const auto* s = std::get_if<SkeletonMessage>(&apply.messages.at(0));
    CHECK(s && s->id == "right_hand" && s->rotations.size() == 31 && s->positions.size() == 31);
    auto scalar = ParseOscPacket(OscBuilder("/VMT/Skeleton/Scalar").I(0).I(2).F(0.25f).I(0).I(0).Build(), map, {}, &state);
    const auto* c = std::get_if<SkeletonMessage>(&scalar.messages.at(0));
    CHECK(c && c->curl.has_value());
    if (c && c->curl) CHECK_NEAR((*c->curl)[1], 0.75, 1e-6); // index finger, VMT 0 = fist
    auto reset = ParseOscPacket(OscBuilder("/VMT/Reset").Build(), map, {}, &state);
    CHECK(reset.messages.size() == 1 && !std::get<PoseMessage>(reset.messages.at(0)).valid);
}

TEST(osc_control_and_encoder) {
    VmtMapping map;
    VmtState state;
    CHECK(ParseOscPacket(OscBuilder("/VMT/Subscribe/Device").S("HMD").Build(), map, {}, &state).errors.empty());
    CHECK(state.subscriptions.count("HMD") == 1);
    CHECK(ParseOscPacket(OscBuilder("/VMT/Set/Destination").S("127.0.0.1").I(40000).Build(), map, {}, &state).errors.empty());
    CHECK(state.dest_port == 40000 && state.dest_ip == 0x7F000001);
    CHECK(!ParseOscPacket(OscBuilder("/VMT/Set/Destination").S("1.2.3").I(1).Build(), map, {}, &state).errors.empty());
    CHECK(ParseOscPacket(OscBuilder("/VMT/SetAutoPoseUpdate").I(1).Build(), map, {}, &state).errors.empty());
    // Encoder output matches what an OSC sender would build.
    CHECK(EncodeOscMessage("/VMT/Out/Log", {int32_t{0}, std::string("ok")}) == OscBuilder("/VMT/Out/Log").I(0).S("ok").Build());
}
