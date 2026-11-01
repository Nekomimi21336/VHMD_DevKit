#pragma once
// Finger curls of a hand on a controller, from what the controller senses.

#include <algorithm>
#include <array>

namespace vhmd {

struct HandSense {
    bool thumb_down = false;    // on the stick or a face button
    bool index_touch = false;   // on the trigger
    float trigger = 0.0f;
    float grip = 0.0f;
};

// Thumb to pinky, 0 open .. 1 closed. VRChat takes its hand gestures from these
// curls and the grip closes three fingers at once, so two of them need a rule of
// their own (the inputs of VRChat's Touch chart). With the thumb down and the grip
// released, a finger off the trigger is the peace sign and the trigger pulled is
// the horns. A finger resting on the trigger stays neutral: that is how the
// controller is held while walking.
inline std::array<float, 5> FingerCurls(const HandSense& s) {
    const float index = std::max(s.trigger, s.index_touch ? 0.25f : 0.0f);
    if (!s.thumb_down) return {0.0f, index, s.grip, s.grip, s.grip};
    const float horns = std::clamp((s.trigger - 0.5f) / 0.3f, 0.0f, 1.0f) * (1.0f - s.grip);
    const float peace = s.index_touch ? 0.0f : 1.0f;
    const float middle = s.grip + (1.0f - s.grip) * horns;
    return {0.7f, index * (1.0f - horns), middle, std::max(middle, peace), std::max(s.grip, peace)};
}

}  // namespace vhmd
