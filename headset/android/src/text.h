// Text for VHMD Link's panels, drawn by Android itself (android.graphics.Canvas
// through JNI), so any language the system has fonts for works, Japanese
// included. The result is an RGBA image (premultiplied alpha, row 0 at the
// bottom, ready for glTexSubImage2D).
#pragma once

#include <jni.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vhmd_link {

struct TextLine {
    std::string text;       // UTF-8
    float size = 32;        // pixels
    uint32_t color = 0xFFFFFFFF;   // ARGB
    bool bold = false;
    float gap_before = 0;   // extra pixels above this line
};

class TextPainter {
public:
    ~TextPainter();
    // On the thread that will paint (attaches it to the VM).
    bool Init(JavaVM* vm);
    // A w x h panel: a rounded rectangle in `background` (ARGB; 0 = none) with
    // the lines from the top-left corner, `margin` pixels in. Returns false if
    // drawing is not available.
    bool Paint(int w, int h, uint32_t background, float margin, const std::vector<TextLine>& lines,
               std::vector<uint8_t>* rgba);

private:
    bool EnsureBitmap(int w, int h);
    void Release();

    JavaVM* vm_ = nullptr;
    JNIEnv* env_ = nullptr;
    jclass bitmap_cls_ = nullptr, canvas_cls_ = nullptr, paint_cls_ = nullptr, rectf_cls_ = nullptr;
    jobject argb8888_ = nullptr;
    jobject bitmap_ = nullptr, canvas_ = nullptr, paint_ = nullptr;
    int w_ = 0, h_ = 0;
    jmethodID create_bitmap_ = nullptr, erase_color_ = nullptr, canvas_init_ = nullptr, draw_text_ = nullptr,
              draw_round_rect_ = nullptr, rectf_init_ = nullptr, set_color_ = nullptr, set_text_size_ = nullptr,
              set_fake_bold_ = nullptr, measure_text_ = nullptr;
};

}  // namespace vhmd_link
