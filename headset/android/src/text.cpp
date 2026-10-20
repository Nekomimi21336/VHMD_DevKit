#include "text.h"

#include <android/bitmap.h>
#include <android/log.h>

#include <cstring>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "VHMDLink", __VA_ARGS__)

namespace vhmd_link {

namespace {
jclass GlobalClass(JNIEnv* env, const char* name) {
    jclass local = env->FindClass(name);
    if (!local) {
        env->ExceptionClear();
        return nullptr;
    }
    auto* global = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return global;
}

// JNI's NewStringUTF takes "modified UTF-8", which differs from UTF-8 only for
// NUL and characters outside the BMP; build the string from UTF-16 instead.
jstring MakeString(JNIEnv* env, const std::string& s) {
    std::u16string u;
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0;
        int n = 0;
        if (c < 0x80) cp = c, n = 1;
        else if ((c >> 5) == 6) cp = c & 0x1F, n = 2;
        else if ((c >> 4) == 14) cp = c & 0x0F, n = 3;
        else if ((c >> 3) == 30) cp = c & 0x07, n = 4;
        else { ++i; continue; }
        if (i + n > s.size()) break;
        for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            u.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            u.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            u.push_back(static_cast<char16_t>(cp));
        }
    }
    return env->NewString(reinterpret_cast<const jchar*>(u.data()), static_cast<jsize>(u.size()));
}
}  // namespace

TextPainter::~TextPainter() {
    Release();
    if (vm_ && env_) vm_->DetachCurrentThread();
}

void TextPainter::Release() {
    if (!env_) return;
    for (jobject* o : {&bitmap_, &canvas_, &paint_, &argb8888_}) {
        if (*o) env_->DeleteGlobalRef(*o);
        *o = nullptr;
    }
    for (jclass* c : {&bitmap_cls_, &canvas_cls_, &paint_cls_, &rectf_cls_}) {
        if (*c) env_->DeleteGlobalRef(*c);
        *c = nullptr;
    }
}

bool TextPainter::Init(JavaVM* vm) {
    vm_ = vm;
    if (vm->AttachCurrentThread(&env_, nullptr) != JNI_OK || !env_) {
        env_ = nullptr;
        return false;
    }
    JNIEnv* e = env_;
    bitmap_cls_ = GlobalClass(e, "android/graphics/Bitmap");
    canvas_cls_ = GlobalClass(e, "android/graphics/Canvas");
    paint_cls_ = GlobalClass(e, "android/graphics/Paint");
    rectf_cls_ = GlobalClass(e, "android/graphics/RectF");
    jclass config_cls = e->FindClass("android/graphics/Bitmap$Config");
    if (!bitmap_cls_ || !canvas_cls_ || !paint_cls_ || !rectf_cls_ || !config_cls) {
        e->ExceptionClear();
        LOGE("text: android.graphics classes not found");
        return false;
    }
    jfieldID f = e->GetStaticFieldID(config_cls, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
    argb8888_ = e->NewGlobalRef(e->GetStaticObjectField(config_cls, f));
    e->DeleteLocalRef(config_cls);

    create_bitmap_ = e->GetStaticMethodID(bitmap_cls_, "createBitmap",
                                          "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    erase_color_ = e->GetMethodID(bitmap_cls_, "eraseColor", "(I)V");
    canvas_init_ = e->GetMethodID(canvas_cls_, "<init>", "(Landroid/graphics/Bitmap;)V");
    draw_text_ = e->GetMethodID(canvas_cls_, "drawText", "(Ljava/lang/String;FFLandroid/graphics/Paint;)V");
    draw_round_rect_ = e->GetMethodID(canvas_cls_, "drawRoundRect", "(Landroid/graphics/RectF;FFLandroid/graphics/Paint;)V");
    rectf_init_ = e->GetMethodID(rectf_cls_, "<init>", "(FFFF)V");
    jmethodID paint_init = e->GetMethodID(paint_cls_, "<init>", "(I)V");
    set_color_ = e->GetMethodID(paint_cls_, "setColor", "(I)V");
    set_text_size_ = e->GetMethodID(paint_cls_, "setTextSize", "(F)V");
    set_fake_bold_ = e->GetMethodID(paint_cls_, "setFakeBoldText", "(Z)V");
    measure_text_ = e->GetMethodID(paint_cls_, "measureText", "(Ljava/lang/String;)F");
    if (e->ExceptionCheck() || !create_bitmap_ || !draw_text_ || !paint_init) {
        e->ExceptionClear();
        LOGE("text: android.graphics methods not found");
        return false;
    }
    jobject paint = e->NewObject(paint_cls_, paint_init, 1 /* ANTI_ALIAS_FLAG */);
    paint_ = e->NewGlobalRef(paint);
    e->DeleteLocalRef(paint);
    return true;
}

bool TextPainter::EnsureBitmap(int w, int h) {
    if (bitmap_ && w == w_ && h == h_) return true;
    JNIEnv* e = env_;
    if (bitmap_) e->DeleteGlobalRef(bitmap_), bitmap_ = nullptr;
    if (canvas_) e->DeleteGlobalRef(canvas_), canvas_ = nullptr;
    jobject bmp = e->CallStaticObjectMethod(bitmap_cls_, create_bitmap_, w, h, argb8888_);
    if (e->ExceptionCheck() || !bmp) {
        e->ExceptionClear();
        return false;
    }
    jobject canvas = e->NewObject(canvas_cls_, canvas_init_, bmp);
    bitmap_ = e->NewGlobalRef(bmp);
    canvas_ = e->NewGlobalRef(canvas);
    e->DeleteLocalRef(bmp);
    e->DeleteLocalRef(canvas);
    w_ = w;
    h_ = h;
    return true;
}

bool TextPainter::Paint(int w, int h, uint32_t background, float margin, const std::vector<TextLine>& lines,
                        std::vector<uint8_t>* rgba) {
    if (!env_ || !paint_ || !EnsureBitmap(w, h)) return false;
    JNIEnv* e = env_;
    e->CallVoidMethod(bitmap_, erase_color_, 0);
    if (background) {
        jobject r = e->NewObject(rectf_cls_, rectf_init_, 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
        e->CallVoidMethod(paint_, set_color_, static_cast<jint>(background));
        const float radius = margin * 0.8f;
        e->CallVoidMethod(canvas_, draw_round_rect_, r, radius, radius, paint_);
        e->DeleteLocalRef(r);
    }
    float y = margin;
    for (const TextLine& l : lines) {
        y += l.gap_before + l.size;
        e->CallVoidMethod(paint_, set_text_size_, l.size);
        e->CallVoidMethod(paint_, set_fake_bold_, static_cast<jboolean>(l.bold));
        e->CallVoidMethod(paint_, set_color_, static_cast<jint>(l.color));
        jstring s = MakeString(e, l.text);
        e->CallVoidMethod(canvas_, draw_text_, s, margin, y, paint_);
        e->DeleteLocalRef(s);
        y += l.size * 0.35f;
    }
    if (e->ExceptionCheck()) {
        e->ExceptionClear();
        return false;
    }
    AndroidBitmapInfo info{};
    void* pixels = nullptr;
    if (AndroidBitmap_getInfo(e, bitmap_, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
        AndroidBitmap_lockPixels(e, bitmap_, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS)
        return false;
    rgba->resize(static_cast<size_t>(w) * h * 4);
    // Bitmap row 0 is the top; GL's row 0 is the bottom.
    for (int row = 0; row < h; ++row)
        memcpy(rgba->data() + static_cast<size_t>(h - 1 - row) * w * 4,
               static_cast<const uint8_t*>(pixels) + static_cast<size_t>(row) * info.stride, static_cast<size_t>(w) * 4);
    AndroidBitmap_unlockPixels(e, bitmap_);
    return true;
}

}  // namespace vhmd_link
