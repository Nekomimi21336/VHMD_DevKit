// Layout of the eye-image preview the Linux direct-mode HMD publishes in
// shared memory (default /dev/shm/vhmd_preview). Readers: see
// clients/python/vhmd_client/preview.py. All fields little-endian.
//
// Writers bump `seq` to an odd value before touching the pixels and to the
// next even value afterwards (seqlock). A reader copies the pixels, then
// accepts the frame only if `seq` was even and unchanged across the copy.
// If width/height change the file is resized; readers re-map when the file
// size no longer matches header_size + stride * height.
//
// Fields after `reserved` were added later; pixels always start at
// header_size, so readers of the 72-byte layout keep working.
#pragma once

#include <cstddef>
#include <cstdint>

namespace vhmd {

inline constexpr char kPreviewMagic[8] = {'V', 'H', 'M', 'D', 'P', 'R', 'V', '1'};
inline constexpr uint32_t kPreviewVersion = 1;
inline constexpr uint32_t kPreviewFormatRgba8 = 1; // 8-bit RGBA, sRGB encoded

#pragma pack(push, 1)
struct PreviewHeader {
    char magic[8];          // "VHMDPRV1"
    uint32_t version;       // kPreviewVersion
    uint32_t header_size;   // sizeof(PreviewHeader); pixels start here
    uint32_t width;         // whole image: left eye | right eye, side by side
    uint32_t height;
    uint32_t stride;        // bytes per row
    uint32_t format;        // kPreviewFormatRgba8
    uint64_t seq;           // seqlock counter (even = stable)
    uint64_t frame_index;   // presents captured so far
    uint64_t timestamp_ns;  // CLOCK_REALTIME of the capture
    uint32_t eye_width;     // width / 2
    uint32_t source_format; // VkFormat (Linux) / DXGI_FORMAT (Windows) of the compositor's eye textures
    uint32_t reserved[2];
    // HMD pose the frame was rendered for (the compositor's SubmitLayer pose), in
    // the HMD driver's tracking space: x y z, qw qx qy qz. Valid when bit 0 of
    // pose_flags is set (Windows direct mode).
    uint32_t pose_flags;
    float pose[7];
};
#pragma pack(pop)

static_assert(sizeof(PreviewHeader) == 104, "PreviewHeader layout is part of the reader contract");
static_assert(offsetof(PreviewHeader, pose_flags) == 72, "new fields go after the original 72 bytes");

} // namespace vhmd
