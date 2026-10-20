#include "test_framework.h"

#include <cstring>
#include <string>

#include "visuals/device_visuals.h"

using namespace vhmd;

namespace {
uint32_t BE(const std::vector<uint8_t>& v, size_t at) {
    return (uint32_t(v[at]) << 24) | (uint32_t(v[at + 1]) << 16) | (uint32_t(v[at + 2]) << 8) | v[at + 3];
}
} // namespace

TEST(visuals_labels_for_roles_and_hands) {
    auto f = TrackerLabel("left_foot", "tracker_1");
    CHECK(f.icon_text == "LF" && f.title == "L FOOT" && f.subtitle == "tracker_1");
    auto w = TrackerLabel("waist", "tracker_0");
    CHECK(w.icon_text == "W" && w.title == "WAIST");
    auto none = TrackerLabel("none", "tracker_3");
    CHECK(none.icon_text == "T3" && none.title == "TRACKER");
    auto r = ControllerLabel(false, "right_hand");
    CHECK(r.icon_text == "R" && r.title == "R" && r.subtitle == "right_hand");
}

TEST(visuals_png_header_and_size) {
    const auto png = MakeStatusIcon("LF", IconState::Ready);
    CHECK(png.size() > 32 * 32 * 4);
    CHECK(std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) == 0);
    CHECK(std::memcmp(png.data() + 12, "IHDR", 4) == 0);
    CHECK(BE(png, 16) == 32 && BE(png, 20) == 32);
}

TEST(visuals_render_models) {
    const auto t = MakeRenderModel(ModelShape::Tracker, TrackerLabel("right_knee", "tracker_4"));
    CHECK(t.obj.find("mtllib body.mtl") == 0);
    CHECK(t.obj.find("\nf ") != std::string::npos);
    CHECK(t.json.find("\"body.obj\"") != std::string::npos);
    CHECK(BE(t.texture, 16) == 256);
    const auto c = MakeRenderModel(ModelShape::Controller, ControllerLabel(true, "left_hand"));
    CHECK(c.json.find("\"tip\"") != std::string::npos);
    CHECK(c.json.find("\"handgrip\"") != std::string::npos);
    // Axes: 5 boxes (X, Y, Z bars, center, forward pointer) = 30 quads = 60 triangles.
    const auto a = MakeRenderModel(ModelShape::Axes, ControllerLabel(false, "right_hand"));
    size_t faces = 0;
    for (size_t p = a.obj.find("\nf "); p != std::string::npos; p = a.obj.find("\nf ", p + 1)) ++faces;
    CHECK(faces == 60);
    CHECK(a.json.find("\"openxr_aim\"") != std::string::npos);
    CHECK(BE(a.texture, 16) == 256);
}
