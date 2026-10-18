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
