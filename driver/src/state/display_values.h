// The shape of the HMD's display as SteamVR knows it (render size, field of
// view, IPD), and changing it while SteamVR runs (config op "display").
//
// The values SteamVR started with are kept: every reply shows them next to the
// current ones, and {"reset": true} goes back to them. Changes are never written
// to the settings, so a SteamVR restart also returns to the startup values.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "protocol/messages.h"

namespace vhmd {

inline constexpr uint32_t kMinRenderSize = 64, kMaxRenderSize = 8192;
inline constexpr double kMinIpdMeters = 0.04, kMaxIpdMeters = 0.09;
inline constexpr double kMaxEyeTan = 10.0;

struct DisplayValues {
    uint32_t render_width = 1440, render_height = 1600;   // per eye
    float ipd_meters = 0.063f;
    // Per-eye field of view as tangents [left, right, up, down] with OpenXR
    // signs (left and down negative).
    float eye_tan[2][4] = {{-1, 1, 1, -1}, {-1, 1, 1, -1}};

    bool operator==(const DisplayValues& o) const {
        if (render_width != o.render_width || render_height != o.render_height || ipd_meters != o.ipd_meters) return false;
        for (int e = 0; e < 2; ++e)
            for (int i = 0; i < 4; ++i)
                if (eye_tan[e][i] != o.eye_tan[e][i]) return false;
        return true;
    }
    bool operator!=(const DisplayValues& o) const { return !(*this == o); }
};

// Error text, or "" when the change is acceptable. The limits are those of the
// "vhmd_display" settings (DisplayConfig::FromSettings).
inline std::string ValidateDisplayChange(const DisplayChange& c) {
    if (c.render) {
        for (double v : *c.render) {
            if (!(v >= kMinRenderSize) || v > kMaxRenderSize || v != std::floor(v))
                return "render: width and height must be whole numbers from " + std::to_string(kMinRenderSize) + " to " +
                       std::to_string(kMaxRenderSize);
        }
    }
    if (c.ipd && (!(*c.ipd >= kMinIpdMeters) || *c.ipd > kMaxIpdMeters)) return "ipd: must be from 0.04 to 0.09 (meters)";
    if (c.tan) {
        for (int e = 0; e < 2; ++e) {
            const double* t = c.tan->data() + e * 4;   // left, right, up, down
            for (int i = 0; i < 4; ++i) {
                if (!std::isfinite(t[i]) || std::abs(t[i]) > kMaxEyeTan || t[i] == 0.0)
                    return "tan: each value must be a non-zero tangent within -10..10";
            }
            if (!(t[0] < t[1]) || !(t[3] < t[2])) return "tan: needs left < right and down < up for each eye";
        }
    }
    return "";
}

// The values after `c`; fields it does not name stay as they are. A reset takes
// `startup`, then applies whatever else `c` names.
inline DisplayValues ApplyDisplayChange(const DisplayValues& now, const DisplayValues& startup, const DisplayChange& c) {
    DisplayValues v = c.reset ? startup : now;
    if (c.render) {
        v.render_width = static_cast<uint32_t>((*c.render)[0]);
        v.render_height = static_cast<uint32_t>((*c.render)[1]);
    }
    if (c.ipd) v.ipd_meters = static_cast<float>(*c.ipd);
    if (c.tan) {
        for (int e = 0; e < 2; ++e)
            for (int i = 0; i < 4; ++i) v.eye_tan[e][i] = static_cast<float>((*c.tan)[e * 4 + i]);
    }
    return v;
}

inline nlohmann::json DisplayValuesJson(const DisplayValues& v) {
    return {{"render", {v.render_width, v.render_height}},
            {"ipd", v.ipd_meters},
            {"tan",
             {{v.eye_tan[0][0], v.eye_tan[0][1], v.eye_tan[0][2], v.eye_tan[0][3]},
              {v.eye_tan[1][0], v.eye_tan[1][1], v.eye_tan[1][2], v.eye_tan[1][3]}}}};
}

} // namespace vhmd
