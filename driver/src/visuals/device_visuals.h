// Per-device labels for SteamVR: status icons and render models that show
// which body part a device is ("L FOOT") and its protocol id ("TRACKER_1").
// Pure generation (PNG / OBJ / JSON bytes), no OpenVR dependency.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vhmd {

struct DeviceLabel {
    std::string icon_text; // 1-3 characters for the 32x32 status icon ("LF", "W", "L")
    std::string title;     // main line on the model ("L FOOT", "WAIST", "L")
    std::string subtitle;  // second line ("TRACKER_1", "LEFT_HAND")
};

// Labels for trackers (by short role name, see config/roles.h) and controllers.
DeviceLabel TrackerLabel(const std::string& role_short_name, const std::string& id);
DeviceLabel ControllerLabel(bool left, const std::string& id);

// Status icon states, matching the Prop_NamedIconPath*_String properties.
enum class IconState { Off, Searching, SearchingAlert, Ready, ReadyAlert, NotReady, Standby, AlertLow };
const char* IconStateName(IconState s);

// 32x32 RGBA PNG: a hexagon in the state's color with the label text.
std::vector<uint8_t> MakeStatusIcon(const std::string& text, IconState state);

// Axes: the device's own axes as colored bars (X red, Y green, Z blue, 10 cm)
// plus a white pointer along -Z (forward), for development ("renderModels": "axes").
enum class ModelShape { Tracker, Controller, Axes };

struct RenderModelFiles {
    std::string obj;              // body.obj
    std::string mtl;              // body.mtl
    std::vector<uint8_t> texture; // body.png
    std::string json;             // <name>.json
};

// A simple labelled mesh: trackers are a flat box with the label on the top
// and the sides; controllers are a capsule with a label plate on top. The
// controller model keeps the "tip" / "handgrip" components.
RenderModelFiles MakeRenderModel(ModelShape shape, const DeviceLabel& label);

// Minimal PNG encoder (RGBA8, stored deflate blocks). Exposed for tests.
std::vector<uint8_t> EncodePng(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba);

} // namespace vhmd
