#include "test_framework.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "protocol/parser.h"

using namespace vhmd;

TEST(parser_pose_basic) {
    auto r = ParseJsonPacket(R"({"type":"pose","id":"hmd","pos":[0,1.7,0],"rot":[1,0,0,0],"vel":[0.1,0,0],"ts":1760000000.5})");
    CHECK(r.errors.empty());
    CHECK(r.messages.size() == 1);
    const auto* p = std::get_if<PoseMessage>(&r.messages[0]);
    CHECK(p != nullptr);
    if (!p) return;
    CHECK(p->id == "hmd");
    CHECK_NEAR(p->pos.y, 1.7, 1e-12);
    CHECK(p->vel.has_value());
    CHECK(!p->angvel.has_value());
    CHECK(p->ts.has_value());
}

TEST(parser_skeleton_packed_bones) {
    // 31 bones of 7 int16; bone 3 turned half way round X, at 1 cm on X. Base64 by hand.
    std::vector<uint8_t> bytes(31 * 14, 0);
    for (size_t i = 0; i < 31; ++i) {
        bytes[i * 14] = 0xFF;   // w = 32767
        bytes[i * 14 + 1] = 0x7F;
    }
    bytes[3 * 14 + 8] = 100;    // x = 100 * 0.1 mm
    static const char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    for (size_t i = 0; i + 2 < bytes.size(); i += 3) {
        const uint32_t v = uint32_t(bytes[i]) << 16 | uint32_t(bytes[i + 1]) << 8 | bytes[i + 2];
        for (int s = 18; s >= 0; s -= 6) text += kDigits[(v >> s) & 63];
    }
    auto good = [&](const std::string& pack) {
        return ParseJsonPacket(R"({"type":"skeleton","id":"left_hand","pack":")" + pack + R"("})");
    };
    // 434 bytes end in 2 extra: padded.
    const uint32_t v = uint32_t(bytes[432]) << 16 | uint32_t(bytes[433]) << 8;
    text += kDigits[(v >> 18) & 63];
    text += kDigits[(v >> 12) & 63];
    text += kDigits[(v >> 6) & 63];
    text += '=';

    auto r = good(text);
    CHECK(r.errors.empty());
    const auto* s = std::get_if<SkeletonMessage>(&r.messages.at(0));
    CHECK(s != nullptr);
    if (!s) return;
    CHECK(s->rotations.size() == 31 && s->positions.size() == 31);
    CHECK_NEAR(s->rotations[3].w, 1.0, 1e-6);
    CHECK_NEAR(s->positions[3].x, 0.01, 1e-6);
    CHECK(good("AAAA").messages.empty());
}

TEST(parser_mic) {
    // Samples 1, -1, 256 as little-endian 16 bit.
    auto r = ParseJsonPacket(R"([{"type":"mic","seq":7,"pcm":"AQD//wAB"}])");
    CHECK(r.errors.empty());
    const auto* m = std::get_if<MicMessage>(&r.messages.at(0));
    CHECK(m != nullptr);
    if (!m) return;
    CHECK(m->seq == 7);
    CHECK(m->pcm == std::vector<int16_t>({1, -1, 256}));
    for (const char* bad : {R"({"type":"mic"})", R"({"type":"mic","pcm":"A$=="})", R"({"type":"mic","pcm":"AQ=="})"}) {
        CHECK(ParseJsonPacket(bad).messages.empty());
    }
}

TEST(parser_pose_normalizes_quaternion) {
    auto r = ParseJsonPacket(R"({"type":"pose","id":"hmd","pos":[0,0,0],"rot":[2,0,0,0]})");
    const auto* p = std::get_if<PoseMessage>(&r.messages.at(0));
    CHECK_NEAR(p->rot.w, 1.0, 1e-12);
}

TEST(parser_rejects_invalid_values) {
    const char* bad[] = {
        R"({"type":"pose","id":"hmd","pos":[0,1],"rot":[1,0,0,0]})",           // short pos
        R"({"type":"pose","id":"hmd","pos":[0,1,0],"rot":[0,0,0,0]})",         // zero quat
        R"({"type":"pose","id":"hmd","pos":[0,1e9,0],"rot":[1,0,0,0]})",       // out of range
        R"({"type":"pose","id":"hmd","pos":["a",1,0],"rot":[1,0,0,0]})",       // wrong type
        R"({"type":"pose","pos":[0,1,0],"rot":[1,0,0,0]})",                    // no id
        R"({"type":"pose","id":"hmd","pos":[0,1,0],"rot":[1,0,0,0],"vel":[1000,0,0]})",
        R"({"type":"input","id":"right_hand","name":"trigger","value":5})",
        R"({"type":"input","id":"right_hand","name":"trigger"})",
        R"({"type":"skeleton","id":"left_hand","bones":[[1,0,0,0]]})",
        R"({"type":"warp","id":"hmd"})",
        R"({"id":"hmd"})",
        R"([1,2,3])",
        R"({"type":"pose","id":"hmd","pos":[0,1,0],"rot":[1,0,0,0])",          // truncated JSON
        "",
    };
    for (const char* s : bad) {
        auto r = ParseJsonPacket(s);
        CHECK(r.messages.empty());
        CHECK(!r.errors.empty());
    }
}

TEST(parser_nan_literal_is_rejected) {
    // NaN is not valid JSON; make sure it does not slip through as a number.
    auto r = ParseJsonPacket(R"({"type":"pose","id":"hmd","pos":[NaN,0,0],"rot":[1,0,0,0]})");
    CHECK(r.messages.empty());
}

TEST(parser_batch_keeps_valid_entries) {
    auto r = ParseJsonPacket(R"([
        {"type":"pose","id":"hmd","pos":[0,1.7,0],"rot":[1,0,0,0]},
        {"type":"pose","id":"tracker_0","pos":[0,1,0]},
        {"type":"input","id":"right_hand","values":{"trigger":0.5,"a":true}}
    ])");
    CHECK(r.messages.size() == 2);
    CHECK(r.errors.size() == 1);
    const auto* in = std::get_if<InputMessage>(&r.messages.at(1));
    CHECK(in && in->values.size() == 2);
}

TEST(parser_batch_limit) {
    std::string s = "[";
    for (int i = 0; i < 300; ++i) s += std::string(i ? "," : "") + R"({"type":"config","op":"ping"})";
    s += "]";
    auto r = ParseJsonPacket(s);
    CHECK(r.messages.empty());
    CHECK(!r.errors.empty());
}

TEST(parser_skeleton_bones_and_curl) {
    std::string bones = "[";
    for (int i = 0; i < 31; ++i) bones += std::string(i ? "," : "") + "[1,0,0,0]";
    bones += "]";
    auto r = ParseJsonPacket(R"({"type":"skeleton","id":"left_hand","bones":)" + bones + "}");
    CHECK(r.errors.empty());
    const auto* s = std::get_if<SkeletonMessage>(&r.messages.at(0));
    CHECK(s && s->rotations.size() == 31 && s->positions.empty());

    auto c = ParseJsonPacket(R"({"type":"skeleton","id":"left_hand","curl":[0,1,1,1,1],"splay":[0,0,0,0,0]})");
    CHECK(c.errors.empty());
    const auto* sc = std::get_if<SkeletonMessage>(&c.messages.at(0));
    CHECK(sc && sc->curl.has_value() && sc->splay.has_value());
}

TEST(parser_gaze_dir_and_config) {
    auto g = ParseJsonPacket(R"({"type":"gaze","id":"hmd","origin":[0,0,0],"dir":[0,0,-2]})");
    const auto* gm = std::get_if<GazeMessage>(&g.messages.at(0));
    CHECK(gm != nullptr);
    if (gm) CHECK_NEAR(gm->target.z, -1.0, 1e-12);

    auto c = ParseJsonPacket(R"({"type":"config","op":"set_role","id":"tracker_0","role":"waist","seq":7})");
    const auto* cm = std::get_if<ConfigMessage>(&c.messages.at(0));
    CHECK(cm && cm->op == "set_role" && cm->role == "waist" && cm->seq && *cm->seq == 7);
}

TEST(parser_pose_invalid_flag_without_pose) {
    auto r = ParseJsonPacket(R"({"type":"pose","id":"tracker_0","valid":false})");
    CHECK(r.errors.empty());
    const auto* p = std::get_if<PoseMessage>(&r.messages.at(0));
    CHECK(p && !p->valid);
}

TEST(parser_looks_like_json) {
    CHECK(LooksLikeJson("  {\"a\":1}"));
    CHECK(LooksLikeJson("[]"));
    CHECK(!LooksLikeJson("/VMT/Room/Unity"));
    CHECK(!LooksLikeJson(""));
}

TEST(parser_pose_acceleration_and_age) {
    auto r = ParseJsonPacket(R"({"type":"pose","id":"tracker_0","pos":[0,1,0],"rot":[1,0,0,0],"acc":[0,-9.8,0],"angacc":[0,1,0],"age":0.02})");
    CHECK(r.errors.empty());
    const auto* p = std::get_if<PoseMessage>(&r.messages.at(0));
    CHECK(p && p->acc && p->angacc && p->age);
    if (p && p->age) CHECK_NEAR(*p->age, 0.02, 1e-12);
    auto old = ParseJsonPacket(R"({"type":"pose","id":"tracker_0","pos":[0,1,0],"rot":[1,0,0,0],"age":2})");
    CHECK(old.messages.empty() && !old.errors.empty());
    auto neg = ParseJsonPacket(R"({"type":"pose","id":"tracker_0","pos":[0,1,0],"rot":[1,0,0,0],"age":-0.1})");
    CHECK(neg.messages.empty());
}
