// VHMD Link: a native OpenXR app for standalone headsets (Pico 4 Ultra first).
//
// Stage 1: reads the head and controller poses and inputs every frame and sends
// them to the PC as VHMD protocol messages (one JSON array per line) over TCP to
// 127.0.0.1:39580. With `adb reverse tcp:39580 tcp:39580` that port is the PC's,
// where `python -m vhmd_client.headset_bridge` forwards each line to the VHMD
// driver's UDP port. The headset shows a floor grid (green while connected to
// the PC, red otherwise) and an axis triad on each controller.
//
// Coordinates: OpenXR's STAGE space (meters, +Y up, -Z forward, origin on the
// floor) matches SteamVR's standing space, so poses go out unchanged.

#include <android/log.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define LOG_TAG "VHMDLink"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr uint16_t kPcPort = 39580;

bool Check(XrResult r, const char* what) {
    if (XR_FAILED(r)) {
        LOGE("%s failed: %d", what, static_cast<int>(r));
        return false;
    }
    return true;
}

// -- network ------------------------------------------------------------------
// The render loop hands over the newest message; a thread (re)connects and
// sends it, so a missing PC never stalls a frame. Older unsent messages are
// dropped: only the latest pose matters.
class Sender {
public:
    void Start() { thread_ = std::thread([this] { Run(); }); }
    void Stop() {
        {
            std::lock_guard<std::mutex> l(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    void Post(std::string line) {
        {
            std::lock_guard<std::mutex> l(mu_);
            pending_ = std::move(line);
            has_ = true;
        }
        cv_.notify_one();
    }
    bool Connected() const { return connected_.load(); }

private:
    void Run() {
        int fd = -1;
        while (true) {
            std::string line;
            {
                std::unique_lock<std::mutex> l(mu_);
                cv_.wait_for(l, std::chrono::milliseconds(500), [this] { return stop_ || has_; });
                if (stop_) break;
                if (!has_) continue;
                line.swap(pending_);
                has_ = false;
            }
            if (fd < 0) fd = Connect();
            if (fd < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            const char* p = line.data();
            size_t left = line.size();
            while (left > 0) {
                ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
                if (n <= 0) break;
                p += n;
                left -= static_cast<size_t>(n);
            }
            if (left > 0) {
                close(fd);
                fd = -1;
                connected_ = false;
                LOGI("PC connection lost");
            }
        }
        if (fd >= 0) close(fd);
    }

    int Connect() {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(kPcPort);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // adb reverse
        if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            close(fd);
            return -1;
        }
        connected_ = true;
        LOGI("connected to the PC");
        return fd;
    }

    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::string pending_;
    bool has_ = false;
    bool stop_ = false;
    std::atomic<bool> connected_{false};
};

// -- small math ---------------------------------------------------------------
struct Mat4 {
    float m[16];   // column-major
};

Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

Mat4 FromPose(const XrPosef& p) {
    const float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
    Mat4 r{};
    r.m[0] = 1 - 2 * (y * y + z * z);
    r.m[1] = 2 * (x * y + z * w);
    r.m[2] = 2 * (x * z - y * w);
    r.m[4] = 2 * (x * y - z * w);
    r.m[5] = 1 - 2 * (x * x + z * z);
    r.m[6] = 2 * (y * z + x * w);
    r.m[8] = 2 * (x * z + y * w);
    r.m[9] = 2 * (y * z - x * w);
    r.m[10] = 1 - 2 * (x * x + y * y);
    r.m[12] = p.position.x;
    r.m[13] = p.position.y;
    r.m[14] = p.position.z;
    r.m[15] = 1;
    return r;
}

Mat4 InvertRigid(const Mat4& a) {   // rotation + translation only
    Mat4 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[j * 4 + i] = a.m[i * 4 + j];
    for (int i = 0; i < 3; ++i)
        r.m[12 + i] = -(r.m[i] * a.m[12] + r.m[4 + i] * a.m[13] + r.m[8 + i] * a.m[14]);
    r.m[15] = 1;
    return r;
}

Mat4 Projection(const XrFovf& fov, float n, float f) {
    const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
    const float u = std::tan(fov.angleUp), d = std::tan(fov.angleDown);
    const float w = r - l, h = u - d;
    Mat4 m{};
    m.m[0] = 2 / w;
    m.m[5] = 2 / h;
    m.m[8] = (r + l) / w;
    m.m[9] = (u + d) / h;
    m.m[10] = -(f + n) / (f - n);
    m.m[11] = -1;
    m.m[14] = -(2 * f * n) / (f - n);
    return m;
}

// -- GL ---------------------------------------------------------------------
const char* kVs = R"(#version 300 es
uniform mat4 mvp;
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 col;
out vec3 vcol;
void main() { vcol = col; gl_Position = mvp * vec4(pos, 1.0); }
)";
const char* kFs = R"(#version 300 es
precision mediump float;
in vec3 vcol;
out vec4 o;
void main() { o = vec4(vcol, 1.0); }
)";

GLuint Compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("shader: %s", log);
    }
    return s;
}

struct Vertex {
    float x, y, z, r, g, b;
};

void Line(std::vector<Vertex>& v, float x0, float y0, float z0, float x1, float y1, float z1, float r, float g,
          float b) {
    v.push_back({x0, y0, z0, r, g, b});
    v.push_back({x1, y1, z1, r, g, b});
}

void Triad(std::vector<Vertex>& v, const XrPosef& p, float len) {
    const Mat4 m = FromPose(p);
    const float o[3] = {m.m[12], m.m[13], m.m[14]};
    const float cols[3][3] = {{1, 0.2f, 0.2f}, {0.2f, 1, 0.2f}, {0.3f, 0.5f, 1}};
    for (int a = 0; a < 3; ++a)
        Line(v, o[0], o[1], o[2], o[0] + m.m[a * 4] * len, o[1] + m.m[a * 4 + 1] * len, o[2] + m.m[a * 4 + 2] * len,
             cols[a][0], cols[a][1], cols[a][2]);
}

// -- app ----------------------------------------------------------------------
struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t width = 0, height = 0;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    GLuint fbo = 0, depth = 0;
};

enum Hand { kLeft = 0, kRight = 1 };

struct App {
    android_app* android = nullptr;
    bool resumed = false;

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    bool exit = false;
    XrSpace base = XR_NULL_HANDLE;   // stage (floor) space
    XrSpace head = XR_NULL_HANDLE;   // view space
    std::vector<XrViewConfigurationView> view_configs;
    std::vector<Swapchain> swapchains;
    bool has_bd_controller = false;

    XrActionSet action_set = XR_NULL_HANDLE;
    XrPath hand_paths[2] = {};
    XrAction grip_pose = XR_NULL_HANDLE, trigger = XR_NULL_HANDLE, trigger_touch = XR_NULL_HANDLE,
             squeeze = XR_NULL_HANDLE, stick = XR_NULL_HANDLE, stick_click = XR_NULL_HANDLE,
             stick_touch = XR_NULL_HANDLE, btn_a = XR_NULL_HANDLE, btn_b = XR_NULL_HANDLE, btn_x = XR_NULL_HANDLE,
             btn_y = XR_NULL_HANDLE, menu = XR_NULL_HANDLE;
    XrSpace grip_spaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

    GLuint program = 0, vbo = 0, vao = 0;
    GLint mvp_loc = -1;

    Sender sender;
};

void OnAppCmd(android_app* a, int32_t cmd) {
    App* app = static_cast<App*>(a->userData);
    switch (cmd) {
    case APP_CMD_RESUME: app->resumed = true; break;
    case APP_CMD_PAUSE: app->resumed = false; break;
    case APP_CMD_DESTROY: app->exit = true; break;
    default: break;
    }
}

bool InitLoader(App& app) {
    PFN_xrInitializeLoaderKHR init = nullptr;
    if (!Check(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                     reinterpret_cast<PFN_xrVoidFunction*>(&init)),
               "xrGetInstanceProcAddr(xrInitializeLoaderKHR)"))
        return false;
    XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    li.applicationVM = app.android->activity->vm;
    li.applicationContext = app.android->activity->clazz;
    return Check(init(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&li)), "xrInitializeLoaderKHR");
}

bool CreateInstance(App& app) {
    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
    std::vector<const char*> exts = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                                     XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME};
    for (auto& p : props) {
        if (strcmp(p.extensionName, XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME) == 0) {
            exts.push_back(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);
            app.has_bd_controller = true;
        }
    }
    LOGI("%u runtime extensions; Pico controller profile: %s", n, app.has_bd_controller ? "yes" : "no");

    XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android_info.applicationVM = app.android->activity->vm;
    android_info.applicationActivity = app.android->activity->clazz;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &android_info;
    strncpy(ci.applicationInfo.applicationName, "VHMD Link", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    strncpy(ci.applicationInfo.engineName, "none", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.enabledExtensionNames = exts.data();
    if (!Check(xrCreateInstance(&ci, &app.instance), "xrCreateInstance")) return false;

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(app.instance, &ip);
    LOGI("runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
         XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    return Check(xrGetSystem(app.instance, &si, &app.system), "xrGetSystem");
}

bool InitEgl(App& app) {
    app.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(app.display, nullptr, nullptr);
    const EGLint attrs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                            EGL_RENDERABLE_TYPE, 0x0040 /* EGL_OPENGL_ES3_BIT */, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLint count = 0;
    if (!eglChooseConfig(app.display, attrs, &app.config, 1, &count) || count == 0) {
        LOGE("eglChooseConfig failed");
        return false;
    }
    const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    app.context = eglCreateContext(app.display, app.config, EGL_NO_CONTEXT, ctx_attrs);
    const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    app.surface = eglCreatePbufferSurface(app.display, app.config, pb);
    if (app.context == EGL_NO_CONTEXT || !eglMakeCurrent(app.display, app.surface, app.surface, app.context)) {
        LOGE("EGL context failed");
        return false;
    }
    return true;
}

bool CreateSession(App& app) {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR reqs_fn = nullptr;
    xrGetInstanceProcAddr(app.instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&reqs_fn));
    XrGraphicsRequirementsOpenGLESKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (!reqs_fn || !Check(reqs_fn(app.instance, app.system, &reqs), "xrGetOpenGLESGraphicsRequirementsKHR"))
        return false;

    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = app.display;
    binding.config = app.config;
    binding.context = app.context;
    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
    ci.next = &binding;
    ci.systemId = app.system;
    if (!Check(xrCreateSession(app.instance, &ci, &app.session), "xrCreateSession")) return false;

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    if (XR_FAILED(xrCreateReferenceSpace(app.session, &rs, &app.base))) {
        LOGI("no STAGE space; using LOCAL");
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        if (!Check(xrCreateReferenceSpace(app.session, &rs, &app.base), "xrCreateReferenceSpace(LOCAL)")) return false;
    }
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    return Check(xrCreateReferenceSpace(app.session, &rs, &app.head), "xrCreateReferenceSpace(VIEW)");
}

bool CreateSwapchains(App& app) {
    uint32_t n = 0;
    xrEnumerateViewConfigurationViews(app.instance, app.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n,
                                      nullptr);
    app.view_configs.assign(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(app.instance, app.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n,
                                      app.view_configs.data());

    uint32_t fc = 0;
    xrEnumerateSwapchainFormats(app.session, 0, &fc, nullptr);
    std::vector<int64_t> formats(fc);
    xrEnumerateSwapchainFormats(app.session, fc, &fc, formats.data());
    int64_t format = formats.empty() ? GL_RGBA8 : formats[0];
    for (int64_t f : formats)
        if (f == GL_SRGB8_ALPHA8) format = f;

    for (auto& vc : app.view_configs) {
        Swapchain sc;
        sc.width = static_cast<int32_t>(vc.recommendedImageRectWidth);
        sc.height = static_cast<int32_t>(vc.recommendedImageRectHeight);
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        ci.format = format;
        ci.sampleCount = 1;
        ci.width = vc.recommendedImageRectWidth;
        ci.height = vc.recommendedImageRectHeight;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        if (!Check(xrCreateSwapchain(app.session, &ci, &sc.handle), "xrCreateSwapchain")) return false;
        uint32_t ic = 0;
        xrEnumerateSwapchainImages(sc.handle, 0, &ic, nullptr);
        sc.images.assign(ic, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateSwapchainImages(sc.handle, ic, &ic, reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data()));
        glGenFramebuffers(1, &sc.fbo);
        glGenRenderbuffers(1, &sc.depth);
        glBindRenderbuffer(GL_RENDERBUFFER, sc.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, sc.width, sc.height);
        app.swapchains.push_back(sc);
        LOGI("eye swapchain %dx%d, %u images", sc.width, sc.height, ic);
    }
    return true;
}

XrPath Path(App& app, const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(app.instance, s, &p);
    return p;
}

XrAction MakeAction(App& app, const char* name, XrActionType type, bool both_hands) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    ci.actionType = type;
    strncpy(ci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(ci.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (both_hands) {
        ci.countSubactionPaths = 2;
        ci.subactionPaths = app.hand_paths;
    }
    XrAction a = XR_NULL_HANDLE;
    Check(xrCreateAction(app.action_set, &ci, &a), name);
    return a;
}

void SuggestBindings(App& app, const char* profile, bool has_menu_left, bool has_ab_xy) {
    struct B {
        XrAction a;
        const char* path;
    };
    std::vector<B> b = {
        {app.grip_pose, "/user/hand/left/input/grip/pose"},   {app.grip_pose, "/user/hand/right/input/grip/pose"},
        {app.trigger, "/user/hand/left/input/trigger/value"}, {app.trigger, "/user/hand/right/input/trigger/value"},
        {app.trigger_touch, "/user/hand/left/input/trigger/touch"},
        {app.trigger_touch, "/user/hand/right/input/trigger/touch"},
        {app.squeeze, "/user/hand/left/input/squeeze/value"}, {app.squeeze, "/user/hand/right/input/squeeze/value"},
        {app.stick, "/user/hand/left/input/thumbstick"},      {app.stick, "/user/hand/right/input/thumbstick"},
        {app.stick_click, "/user/hand/left/input/thumbstick/click"},
        {app.stick_click, "/user/hand/right/input/thumbstick/click"},
        {app.stick_touch, "/user/hand/left/input/thumbstick/touch"},
        {app.stick_touch, "/user/hand/right/input/thumbstick/touch"},
    };
    if (has_ab_xy) {
        b.push_back({app.btn_x, "/user/hand/left/input/x/click"});
        b.push_back({app.btn_y, "/user/hand/left/input/y/click"});
        b.push_back({app.btn_a, "/user/hand/right/input/a/click"});
        b.push_back({app.btn_b, "/user/hand/right/input/b/click"});
    }
    if (has_menu_left) b.push_back({app.menu, "/user/hand/left/input/menu/click"});
    std::vector<XrActionSuggestedBinding> sb;
    for (auto& x : b) sb.push_back({x.a, Path(app, x.path)});
    XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    s.interactionProfile = Path(app, profile);
    s.suggestedBindings = sb.data();
    s.countSuggestedBindings = static_cast<uint32_t>(sb.size());
    XrResult r = xrSuggestInteractionProfileBindings(app.instance, &s);
    LOGI("bindings for %s: %s", profile, XR_SUCCEEDED(r) ? "ok" : "not accepted");
}

bool CreateActions(App& app) {
    XrActionSetCreateInfo ci{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(ci.actionSetName, "vhmd");
    strcpy(ci.localizedActionSetName, "VHMD");
    if (!Check(xrCreateActionSet(app.instance, &ci, &app.action_set), "xrCreateActionSet")) return false;
    app.hand_paths[kLeft] = Path(app, "/user/hand/left");
    app.hand_paths[kRight] = Path(app, "/user/hand/right");

    app.grip_pose = MakeAction(app, "grip_pose", XR_ACTION_TYPE_POSE_INPUT, true);
    app.trigger = MakeAction(app, "trigger", XR_ACTION_TYPE_FLOAT_INPUT, true);
    app.trigger_touch = MakeAction(app, "trigger_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    app.squeeze = MakeAction(app, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT, true);
    app.stick = MakeAction(app, "thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, true);
    app.stick_click = MakeAction(app, "thumbstick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    app.stick_touch = MakeAction(app, "thumbstick_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    app.btn_a = MakeAction(app, "a", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    app.btn_b = MakeAction(app, "b", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    app.btn_x = MakeAction(app, "x", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    app.btn_y = MakeAction(app, "y", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    app.menu = MakeAction(app, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false);

    if (app.has_bd_controller) SuggestBindings(app, "/interaction_profiles/bytedance/pico4_controller", true, true);
    SuggestBindings(app, "/interaction_profiles/oculus/touch_controller", true, true);

    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo sc{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sc.action = app.grip_pose;
        sc.subactionPath = app.hand_paths[h];
        sc.poseInActionSpace.orientation.w = 1;
        Check(xrCreateActionSpace(app.session, &sc, &app.grip_spaces[h]), "xrCreateActionSpace");
    }
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &app.action_set;
    return Check(xrAttachSessionActionSets(app.session, &ai), "xrAttachSessionActionSets");
}

void InitGl(App& app) {
    app.program = glCreateProgram();
    glAttachShader(app.program, Compile(GL_VERTEX_SHADER, kVs));
    glAttachShader(app.program, Compile(GL_FRAGMENT_SHADER, kFs));
    glLinkProgram(app.program);
    app.mvp_loc = glGetUniformLocation(app.program, "mvp");
    glGenVertexArrays(1, &app.vao);
    glGenBuffers(1, &app.vbo);
    glBindVertexArray(app.vao);
    glBindBuffer(GL_ARRAY_BUFFER, app.vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(3 * sizeof(float)));
    glBindVertexArray(0);
}

// -- per frame ----------------------------------------------------------------
float GetFloat(App& app, XrAction a, XrPath sub) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = sub;
    XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
    xrGetActionStateFloat(app.session, &gi, &s);
    return s.isActive ? s.currentState : 0.0f;
}

bool GetBool(App& app, XrAction a, XrPath sub) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = sub;
    XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
    xrGetActionStateBoolean(app.session, &gi, &s);
    return s.isActive && s.currentState;
}

XrVector2f GetVec2(App& app, XrAction a, XrPath sub) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = sub;
    XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
    xrGetActionStateVector2f(app.session, &gi, &s);
    return s.isActive ? s.currentState : XrVector2f{0, 0};
}

void AppendPose(std::string& out, const char* id, const XrSpaceLocation& loc) {
    char buf[256];
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((loc.locationFlags & need) != need) {
        snprintf(buf, sizeof(buf), "{\"type\":\"pose\",\"id\":\"%s\",\"valid\":false}", id);
    } else {
        const XrPosef& p = loc.pose;
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"pose\",\"id\":\"%s\",\"pos\":[%.5f,%.5f,%.5f],\"rot\":[%.6f,%.6f,%.6f,%.6f]}", id,
                 p.position.x, p.position.y, p.position.z, p.orientation.w, p.orientation.x, p.orientation.y,
                 p.orientation.z);
    }
    if (out.size() > 1) out += ',';
    out += buf;
}

void AppendInputs(App& app, std::string& out, int hand) {
    const XrPath sub = app.hand_paths[hand];
    const float trig = GetFloat(app, app.trigger, sub);
    const float grip = GetFloat(app, app.squeeze, sub);
    const XrVector2f st = GetVec2(app, app.stick, sub);
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "{\"type\":\"input\",\"id\":\"%s\",\"values\":{\"trigger\":%.3f,\"trigger_click\":%s,"
                     "\"trigger_touch\":%s,\"grip\":%.3f,\"grip_click\":%s,\"joystick_x\":%.3f,\"joystick_y\":%.3f,"
                     "\"joystick_click\":%s,\"joystick_touch\":%s",
                     hand == kLeft ? "left_hand" : "right_hand", trig, trig >= 0.95f ? "true" : "false",
                     GetBool(app, app.trigger_touch, sub) || trig > 0.05f ? "true" : "false", grip,
                     grip >= 0.9f ? "true" : "false", st.x, st.y, GetBool(app, app.stick_click, sub) ? "true" : "false",
                     GetBool(app, app.stick_touch, sub) ? "true" : "false");
    if (hand == kLeft)
        snprintf(buf + n, sizeof(buf) - n, ",\"x\":%s,\"y\":%s,\"menu\":%s}}",
                 GetBool(app, app.btn_x, XR_NULL_PATH) ? "true" : "false",
                 GetBool(app, app.btn_y, XR_NULL_PATH) ? "true" : "false",
                 GetBool(app, app.menu, XR_NULL_PATH) ? "true" : "false");
    else
        snprintf(buf + n, sizeof(buf) - n, ",\"a\":%s,\"b\":%s}}", GetBool(app, app.btn_a, XR_NULL_PATH) ? "true" : "false",
                 GetBool(app, app.btn_b, XR_NULL_PATH) ? "true" : "false");
    out += ',';
    out += buf;
}

void RenderEye(App& app, Swapchain& sc, uint32_t image, const XrView& view, const std::vector<Vertex>& verts) {
    glBindFramebuffer(GL_FRAMEBUFFER, sc.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc.images[image].image, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sc.depth);
    glViewport(0, 0, sc.width, sc.height);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.03f, 0.035f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const Mat4 mvp = Multiply(Projection(view.fov, 0.05f, 100.0f), InvertRigid(FromPose(view.pose)));
    glUseProgram(app.program);
    glUniformMatrix4fv(app.mvp_loc, 1, GL_FALSE, mvp.m);
    glBindVertexArray(app.vao);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(verts.size()));
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Frame(App& app) {
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (!Check(xrWaitFrame(app.session, nullptr, &fs), "xrWaitFrame")) return;
    xrBeginFrame(app.session, nullptr);

    XrActiveActionSet active{app.action_set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    xrSyncActions(app.session, &sync);

    // Poses for the PC.
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION}, hands[2] = {{XR_TYPE_SPACE_LOCATION}, {XR_TYPE_SPACE_LOCATION}};
    xrLocateSpace(app.head, app.base, fs.predictedDisplayTime, &head);
    for (int h = 0; h < 2; ++h) xrLocateSpace(app.grip_spaces[h], app.base, fs.predictedDisplayTime, &hands[h]);
    std::string msg = "[";
    AppendPose(msg, "hmd", head);
    AppendPose(msg, "left_hand", hands[kLeft]);
    AppendPose(msg, "right_hand", hands[kRight]);
    AppendInputs(app, msg, kLeft);
    AppendInputs(app, msg, kRight);
    msg += "]\n";
    app.sender.Post(std::move(msg));

    std::vector<XrCompositionLayerProjectionView> proj_views;
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<XrCompositionLayerBaseHeader*> layers;
    if (fs.shouldRender) {
        XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = app.base;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        uint32_t vc = 0;
        std::vector<XrView> views(app.swapchains.size(), {XR_TYPE_VIEW});
        xrLocateViews(app.session, &vli, &vs, static_cast<uint32_t>(views.size()), &vc, views.data());

        // Scene: floor grid (green when the PC is connected, red when not) and
        // an axis triad on each controller.
        std::vector<Vertex> verts;
        const bool ok = app.sender.Connected();
        const float gr = ok ? 0.15f : 0.45f, gg = ok ? 0.45f : 0.12f, gb = ok ? 0.25f : 0.12f;
        for (int i = -10; i <= 10; ++i) {
            const float t = i * 0.5f;
            Line(verts, t, 0, -5, t, 0, 5, gr, gg, gb);
            Line(verts, -5, 0, t, 5, 0, t, gr, gg, gb);
        }
        Line(verts, 0, 0.001f, 0, 1, 0.001f, 0, 1, 0.2f, 0.2f);    // +X
        Line(verts, 0, 0.001f, 0, 0, 0.001f, -1, 0.3f, 0.5f, 1);   // -Z (forward)
        for (int h = 0; h < 2; ++h)
            if (hands[h].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) Triad(verts, hands[h].pose, 0.1f);
        glBindBuffer(GL_ARRAY_BUFFER, app.vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(Vertex)), verts.data(),
                     GL_STREAM_DRAW);

        proj_views.resize(vc, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
        for (uint32_t i = 0; i < vc; ++i) {
            Swapchain& sc = app.swapchains[i];
            uint32_t image = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            xrAcquireSwapchainImage(sc.handle, &ai, &image);
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(sc.handle, &wi);
            RenderEye(app, sc, image, views[i], verts);
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(sc.handle, &ri);
            proj_views[i].pose = views[i].pose;
            proj_views[i].fov = views[i].fov;
            proj_views[i].subImage.swapchain = sc.handle;
            proj_views[i].subImage.imageRect = {{0, 0}, {sc.width, sc.height}};
        }
        layer.space = app.base;
        layer.viewCount = vc;
        layer.views = proj_views.data();
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer));
    }

    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = static_cast<uint32_t>(layers.size());
    ei.layers = layers.data();
    xrEndFrame(app.session, &ei);
}

void PollEvents(App& app) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(app.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            app.state = e->state;
            LOGI("session state %d", static_cast<int>(app.state));
            if (app.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                app.running = Check(xrBeginSession(app.session, &bi), "xrBeginSession");
            } else if (app.state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(app.session);
                app.running = false;
            } else if (app.state == XR_SESSION_STATE_EXITING || app.state == XR_SESSION_STATE_LOSS_PENDING) {
                app.exit = true;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            app.exit = true;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

}  // namespace

void android_main(android_app* android) {
    App app;
    app.android = android;
    android->userData = &app;
    android->onAppCmd = OnAppCmd;

    if (!InitLoader(app) || !CreateInstance(app) || !InitEgl(app) || !CreateSession(app) ||
        !CreateSwapchains(app) || !CreateActions(app)) {
        LOGE("startup failed");
        ANativeActivity_finish(android->activity);
    } else {
        InitGl(app);
        app.sender.Start();
        LOGI("running; sending to 127.0.0.1:%u (adb reverse)", kPcPort);
    }

    while (!android->destroyRequested) {
        // Block on Android events only while the XR session is idle.
        for (;;) {
            int events = 0;
            android_poll_source* source = nullptr;
            const int timeout = (!app.running && !app.resumed && app.session == XR_NULL_HANDLE) ? -1 : 0;
            if (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) < 0) break;
            if (source) source->process(android, source);
            if (android->destroyRequested) break;
        }
        if (app.instance == XR_NULL_HANDLE || app.session == XR_NULL_HANDLE) continue;
        PollEvents(app);
        if (app.exit) {
            ANativeActivity_finish(android->activity);
            app.exit = false;
        }
        if (!app.running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        Frame(app);
    }

    app.sender.Stop();
    if (app.session) xrDestroySession(app.session);
    if (app.instance) xrDestroyInstance(app.instance);
    if (app.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(app.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (app.context != EGL_NO_CONTEXT) eglDestroyContext(app.display, app.context);
        if (app.surface != EGL_NO_SURFACE) eglDestroySurface(app.display, app.surface);
        eglTerminate(app.display);
    }
}
