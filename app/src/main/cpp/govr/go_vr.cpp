#include <jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <VrApi.h>
#include <VrApi_Helpers.h>
#include <VrApi_Input.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <dlfcn.h>
#include <signal.h>
#include <unwind.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include "arm_model.h"
#include "go_home.h"
#include "xr_frame_channel.h"
#include "xr_frame_math.h"
#include "xr_tracking.h"

#define LOG_TAG "DroidDeckGoVR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

/* The Go keeps no tombstones a user can read, so steps and native crashes go to the app's
 * external files folder, which is reachable over USB. */
char g_stepsPath[512], g_crashPath[512];
const char *volatile g_step = "start";
struct sigaction g_previous[NSIG];

void step(const char *name) {
    g_step = name;
    LOGI("step: %s", name);
    if (!g_stepsPath[0]) return;
    if (FILE *file = std::fopen(g_stepsPath, "a")) {
        std::fprintf(file, "%s\n", name);
        std::fclose(file);
    }
}

_Unwind_Reason_Code crashFrame(_Unwind_Context *context, void *argument) {
    const int fd = *static_cast<int *>(argument);
    const uintptr_t pc = _Unwind_GetIP(context);
    if (!pc) return _URC_END_OF_STACK;
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(pc), &info) && info.dli_fname)
        dprintf(fd, "  %s+0x%zx %s\n", info.dli_fname, (size_t)(pc - (uintptr_t)info.dli_fbase), info.dli_sname ? info.dli_sname : "");
    else dprintf(fd, "  0x%zx\n", (size_t)pc);
    return _URC_NO_REASON;
}

void onCrash(int signal, siginfo_t *info, void *) {
    int fd = open(g_crashPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        dprintf(fd, "signal %d code %d address %p during step: %s\n", signal, info ? info->si_code : 0,
                info ? info->si_addr : nullptr, g_step);
        _Unwind_Backtrace(crashFrame, &fd);
        close(fd);
    }
    sigaction(signal, &g_previous[signal], nullptr);
    raise(signal);
}

void installCrashReport(const char *directory) {
    if (!directory) return;
    std::snprintf(g_stepsPath, sizeof(g_stepsPath), "%s/go-vr-steps.txt", directory);
    std::snprintf(g_crashPath, sizeof(g_crashPath), "%s/go-vr-crash.txt", directory);
    unlink(g_stepsPath);
    static char stack[64 * 1024];
    stack_t alternate{};
    alternate.ss_sp = stack;
    alternate.ss_size = sizeof(stack);
    sigaltstack(&alternate, nullptr);
    struct sigaction action{};
    action.sa_sigaction = onCrash;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int signal : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE}) sigaction(signal, &action, &g_previous[signal]);
}

constexpr uint32_t kTrackedFlags = 15u | DROIDDECK_XR_POSE_3DOF;
constexpr float kStandingHeight = 1.6f;
constexpr int64_t kStaleFrameNs = 500000000LL;
constexpr int kRequestedVrApiMinor = 25;
constexpr int kImmersive = 1, kShowScreen = 2, kShowKeyboard = 4;
constexpr int kTargetUi = 1, kTargetScreen = 2, kTargetKeyboard = 3;
constexpr int kUiWidth = 1024, kUiHeight = 512, kKeyboardWidth = 1024, kKeyboardHeight = 384;

struct Settings {
    float refreshHz = 72.f;
    int guestDivisor = 2;
    float renderScale = 0.75f;
    int cpuLevel = 4;
    int gpuLevel = 4;
};

int64_t monotonicNs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

DroidDeckXrPose posePacket(const ovrPosef &pose, float floorOffset, uint32_t flags) {
    return {{pose.Orientation.x, pose.Orientation.y, pose.Orientation.z, pose.Orientation.w},
            {pose.Position.x, pose.Position.y + floorOffset, pose.Position.z}, flags};
}

void fovFromProjection(const ovrMatrix4f &projection, float fov[4]) {
    fov[0] = std::atan((projection.M[0][2] - 1.f) / projection.M[0][0]);
    fov[1] = std::atan((projection.M[0][2] + 1.f) / projection.M[0][0]);
    fov[2] = std::atan((projection.M[1][2] + 1.f) / projection.M[1][1]);
    fov[3] = std::atan((projection.M[1][2] - 1.f) / projection.M[1][1]);
}

/* Eye images arrive side by side with their first row at the top; VrApi samples eye buffers with
 * (0,0) at the bottom left. Both are folded into the tan-angle matrix so the frame is uploaded
 * once, unchanged, and never redrawn. Row 2 of the matrix is the homogeneous divisor. */
ovrMatrix4f sideBySideTexCoords(const float projection[4][4], int eye) {
    ovrMatrix4f source{};
    for (int row = 0; row < 4; row++)
        for (int column = 0; column < 4; column++) source.M[row][column] = projection[row][column];
    ovrMatrix4f matrix = ovrMatrix4f_TanAngleMatrixFromProjection(&source);
    for (int column = 0; column < 4; column++) {
        matrix.M[1][column] = matrix.M[2][column] - matrix.M[1][column];
        matrix.M[0][column] = 0.5f * matrix.M[0][column] + 0.5f * eye * matrix.M[2][column];
    }
    return matrix;
}

class GoVR {
public:
    explicit GoVR(android_app *app) : app_(app) {}
    ~GoVR() { cleanup(); }

    void run() {
        initialize();
        while (!app_->destroyRequested) {
            android_poll_source *source = nullptr;
            int events = 0;
            int timeout = ovr_ ? 0 : 100;
            for (int count = 0; count < 32; count++) {
                if (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void **>(&source)) < 0) break;
                if (source) source->process(app_, source);
                if (app_->destroyRequested) break;
                timeout = 0;
            }
            updateVrMode();
            if (ovr_ && !app_->destroyRequested) frame();
        }
    }

    void health(const char *error = "") {
        if (!app_->activity->externalDataPath) return;
        int steamvrState = -1;
        if (env_ && statusCallback_) {
            steamvrState = env_->CallIntMethod(app_->activity->clazz, statusCallback_);
            if (env_->ExceptionCheck()) { env_->ExceptionClear(); steamvrState = -1; }
        }
        std::string path = std::string(app_->activity->externalDataPath) + "/go-vr-health.json";
        std::string temporary = path + ".tmp";
        FILE *file = std::fopen(temporary.c_str(), "w");
        if (!file) return;
        std::fprintf(file,
            "{\"frames\":%llu,\"uploaded\":%llu,\"vrMode\":%s,\"refreshHz\":%.1f,\"guestDivisor\":%d,"
            "\"eye\":[%d,%d],\"controller\":%d,\"receivedFrames\":%llu,\"rejectedFrames\":%llu,"
            "\"sourceFrameId\":%llu,\"steamvrState\":%d,\"floorOffset\":%.2f,\"error\":\"%s\"}\n",
            (unsigned long long)frames_, (unsigned long long)uploaded_, ovr_ ? "true" : "false",
            refreshHz_, settings_.guestDivisor, eyeWidth_, eyeHeight_, remoteHand_,
            (unsigned long long)receiver_.Received(), (unsigned long long)receiver_.Rejected(),
            (unsigned long long)sourceHeader_.frame_id, steamvrState, floorOffset_, error);
        std::fclose(file);
        std::rename(temporary.c_str(), path.c_str());
    }

private:
    android_app *app_;
    JNIEnv *env_ = nullptr;
    ovrJava java_{};
    ovrMobile *ovr_ = nullptr;
    bool resumed_ = false, guestAttached_ = false, initialized_ = false;
    ANativeWindow *window_ = nullptr;
    Settings settings_;
    float refreshHz_ = 72.f;
    float floorOffset_ = 0.f;
    int eyeWidth_ = 0, eyeHeight_ = 0;
    uint64_t frameIndex_ = 0, frames_ = 0, uploaded_ = 0;
    jmethodID frameCallback_ = nullptr, activeCallback_ = nullptr, statusCallback_ = nullptr;
    jmethodID homeCallback_ = nullptr, uiCallback_ = nullptr, screenCallback_ = nullptr, pointerCallback_ = nullptr;
    jmethodID keyboardCallback_ = nullptr;
    govr::Home home_;
    float homeY_ = NAN;
    int lastTarget_ = 0;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    ovrTextureSwapChain *swapchain_ = nullptr;
    int swapchainLength_ = 0, swapchainIndex_ = 0;
    uint32_t swapchainWidth_ = 0, swapchainHeight_ = 0, swapchainColor_ = 0;
    XrFrameReceiver receiver_;
    DroidDeckXrFrameHeader sourceHeader_{};
    DroidDeckXrTracking *tracking_ = nullptr;
    DroidDeckXrHaptics *haptics_ = nullptr;
    ovrDeviceID remote_ = ovrDeviceIdType_Invalid;
    int remoteHand_ = -1;
    float trackpadMax_[2] = {1.f, 1.f};
    DroidDeckXrHand hand_{};
    droiddeck::ArmModel arms_[2] = {droiddeck::ArmModel(false), droiddeck::ArmModel(true)};
    double lastArmTime_ = 0;
    uint8_t recenterCount_ = 0;

    bool intExtra(jobject intent, jmethodID getInt, const char *name, int *value) {
        jstring key = env_->NewStringUTF(name);
        jint result = env_->CallIntMethod(intent, getInt, key, (jint)*value);
        env_->DeleteLocalRef(key);
        if (env_->ExceptionCheck()) { env_->ExceptionClear(); return false; }
        *value = result;
        return true;
    }

    void readSettings() {
        jclass activityClass = env_->GetObjectClass(app_->activity->clazz);
        jmethodID getIntent = env_->GetMethodID(activityClass, "getIntent", "()Landroid/content/Intent;");
        jobject intent = env_->CallObjectMethod(app_->activity->clazz, getIntent);
        jclass intentClass = env_->GetObjectClass(intent);
        jmethodID getInt = env_->GetMethodID(intentClass, "getIntExtra", "(Ljava/lang/String;I)I");
        int refresh = (int)settings_.refreshHz, scale = (int)(settings_.renderScale * 100.f);
        intExtra(intent, getInt, "refresh_hz", &refresh);
        intExtra(intent, getInt, "guest_divisor", &settings_.guestDivisor);
        intExtra(intent, getInt, "render_scale_percent", &scale);
        intExtra(intent, getInt, "cpu_level", &settings_.cpuLevel);
        intExtra(intent, getInt, "gpu_level", &settings_.gpuLevel);
        settings_.refreshHz = refresh == 60 ? 60.f : 72.f;
        settings_.guestDivisor = std::clamp(settings_.guestDivisor, 1, 4);
        settings_.renderScale = std::clamp(scale, 40, 100) / 100.f;
        settings_.cpuLevel = std::clamp(settings_.cpuLevel, 0, 4);
        settings_.gpuLevel = std::clamp(settings_.gpuLevel, 0, 4);
        env_->DeleteLocalRef(intentClass);
        env_->DeleteLocalRef(intent);
        env_->DeleteLocalRef(activityClass);
    }

    void initialize() {
        step("initialize");
        app_->userData = this;
        app_->onAppCmd = [](android_app *app, int32_t command) {
            auto *runtime = static_cast<GoVR *>(app->userData);
            switch (command) {
                case APP_CMD_RESUME: runtime->resumed_ = true; break;
                case APP_CMD_PAUSE: runtime->resumed_ = false; break;
                case APP_CMD_INIT_WINDOW: runtime->window_ = app->window; break;
                case APP_CMD_TERM_WINDOW: runtime->window_ = nullptr; break;
                default: break;
            }
        };
        // The Go remote's Back button is the SteamVR system button; leaving VR goes through the Oculus button.
        app_->onInputEvent = [](android_app *, AInputEvent *event) -> int32_t {
            return AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY && AKeyEvent_getKeyCode(event) == AKEYCODE_BACK;
        };
        app_->activity->vm->AttachCurrentThread(&env_, nullptr);
        jclass activityClass = env_->GetObjectClass(app_->activity->clazz);
        activeCallback_ = env_->GetMethodID(activityClass, "onXrActive", "(Z)V");
        frameCallback_ = env_->GetMethodID(activityClass, "onXrFrame", "(J)V");
        statusCallback_ = env_->GetMethodID(activityClass, "onXrStatus", "()I");
        homeCallback_ = env_->GetMethodID(activityClass, "onHomeFrame", "()I");
        uiCallback_ = env_->GetMethodID(activityClass, "onUiPixels", "()Ljava/nio/ByteBuffer;");
        screenCallback_ = env_->GetMethodID(activityClass, "onScreenTexture", "(I)[F");
        pointerCallback_ = env_->GetMethodID(activityClass, "onPointer", "(IFFZZZ)V");
        keyboardCallback_ = env_->GetMethodID(activityClass, "onKeyboardPixels", "()Ljava/nio/ByteBuffer;");
        env_->DeleteLocalRef(activityClass);
        if (env_->ExceptionCheck()) { env_->ExceptionClear(); throw std::runtime_error("VR activity bridge methods unavailable"); }
        readSettings();
        step("vrapi_Initialize");

        java_.Vm = app_->activity->vm;
        java_.Env = env_;
        java_.ActivityObject = app_->activity->clazz;
        ovrInitParms initParms = vrapi_DefaultInitParms(&java_);
        // The Go's last system driver refuses apps that request a newer VrApi than it has (it shows
        // "update required" with no update to install). Everything used here exists, with the same
        // structure layouts, from VrApi 1.1.25 on.
        initParms.MinorVersion = kRequestedVrApiMinor;
        initParms.PatchVersion = 0;
        if (vrapi_Initialize(&initParms) != VRAPI_INITIALIZE_SUCCESS)
            throw std::runtime_error("vrapi_Initialize failed: the Oculus VR runtime is unavailable");
        initialized_ = true;
        LOGI("VrApi %s", vrapi_GetVersionString());
        step(vrapi_GetVersionString());
        step("egl");

        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (!eglInitialize(display_, nullptr, nullptr)) throw std::runtime_error("eglInitialize failed");
        const EGLint attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_NONE};
        EGLConfig config{};
        EGLint found = 0;
        if (!eglChooseConfig(display_, attributes, &config, 1, &found) || found != 1)
            throw std::runtime_error("EGL config unavailable");
        const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, contextAttributes);
        if (context_ == EGL_NO_CONTEXT) throw std::runtime_error("eglCreateContext failed");
        const EGLint surfaceAttributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        surface_ = eglCreatePbufferSurface(display_, config, surfaceAttributes);
        if (!eglMakeCurrent(display_, surface_, surface_, context_)) throw std::runtime_error("eglMakeCurrent failed");

        int suggestedWidth = vrapi_GetSystemPropertyInt(&java_, VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_WIDTH);
        int suggestedHeight = vrapi_GetSystemPropertyInt(&java_, VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_HEIGHT);
        if (suggestedWidth <= 0 || suggestedHeight <= 0) suggestedWidth = suggestedHeight = 1024;
        eyeWidth_ = std::max(256, (int)(suggestedWidth * settings_.renderScale) & ~7);
        eyeHeight_ = std::max(256, (int)(suggestedHeight * settings_.renderScale) & ~7);

        std::string directory = std::string(app_->activity->internalDataPath) + "/.wayland-rt";
        if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) throw std::runtime_error("VR IPC directory unavailable");
        tracking_ = static_cast<DroidDeckXrTracking *>(mapFile(directory + "/" DROIDDECK_XR_FILE, sizeof(DroidDeckXrTracking)));
        haptics_ = static_cast<DroidDeckXrHaptics *>(mapFile(directory + "/" DROIDDECK_XR_HAPTIC_FILE, sizeof(DroidDeckXrHaptics)));
        haptics_->magic = DROIDDECK_XR_HAPTIC_MAGIC;
        haptics_->version = DROIDDECK_XR_VERSION;
        if (!receiver_.Start(directory + "/" DROIDDECK_XR_FRAME_SOCKET)) throw std::runtime_error("VR frame receiver unavailable");
        step("room resources");
        home_.Create(suggestedWidth, suggestedHeight);
        step("ipc files");
        LOGI("eye %dx%d (suggested %dx%d), %.0f Hz display, guest every %d refresh(es)", eyeWidth_, eyeHeight_,
             suggestedWidth, suggestedHeight, settings_.refreshHz, settings_.guestDivisor);
        health();
    }

    void *mapFile(const std::string &filename, size_t size) {
        int descriptor = open(filename.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (descriptor < 0) throw std::runtime_error("VR IPC file unavailable");
        if (ftruncate(descriptor, size) != 0) { close(descriptor); throw std::runtime_error("VR IPC file size failed"); }
        void *mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
        close(descriptor);
        if (mapping == MAP_FAILED) throw std::runtime_error("VR IPC map failed");
        return mapping;
    }

    void updateVrMode() {
        if (resumed_ && window_ && !ovr_) enterVr();
        else if ((!resumed_ || !window_) && ovr_) leaveVr();
    }

    void enterVr() {
        step("vrapi_EnterVrMode");
        ovrModeParms parms = vrapi_DefaultModeParms(&java_);
        parms.Flags |= VRAPI_MODE_FLAG_NATIVE_WINDOW | VRAPI_MODE_FLAG_FRONT_BUFFER_SRGB;
        parms.Flags &= ~VRAPI_MODE_FLAG_RESET_WINDOW_FULLSCREEN;
        parms.Display = (size_t)display_;
        parms.WindowSurface = (size_t)window_;
        parms.ShareContext = (size_t)context_;
        ovr_ = vrapi_EnterVrMode(&parms);
        if (!ovr_) throw std::runtime_error("vrapi_EnterVrMode failed");
        step("vr mode settings");
        if (vrapi_SetTrackingSpace(ovr_, VRAPI_TRACKING_SPACE_LOCAL_FLOOR) == ovrSuccess) floorOffset_ = 0.f;
        else {
            vrapi_SetTrackingSpace(ovr_, VRAPI_TRACKING_SPACE_LOCAL);
            floorOffset_ = kStandingHeight;
        }
        vrapi_SetClockLevels(ovr_, settings_.cpuLevel, settings_.gpuLevel);
        vrapi_SetPerfThread(ovr_, VRAPI_PERF_THREAD_TYPE_MAIN, (uint32_t)syscall(SYS_gettid));
        vrapi_SetExtraLatencyMode(ovr_, VRAPI_EXTRA_LATENCY_MODE_ON);
        refreshHz_ = vrapi_SetDisplayRefreshRate(ovr_, settings_.refreshHz) == ovrSuccess
            ? settings_.refreshHz : vrapi_GetSystemPropertyFloat(&java_, VRAPI_SYS_PROP_DISPLAY_REFRESH_RATE);
        if (!(refreshHz_ >= 30.f && refreshHz_ <= 120.f)) refreshHz_ = 60.f;
        for (auto &arm : arms_) arm.Reset();
        remote_ = ovrDeviceIdType_Invalid;
        step("in VR");
        LOGI("entered VR: %.0f Hz, CPU/GPU level %d/%d, floor offset %.2f m", refreshHz_,
             settings_.cpuLevel, settings_.gpuLevel, floorOffset_);
        health();
    }

    void leaveVr() {
        attachGuest(false);
        if (tracking_) __atomic_store_n(&tracking_->active, 0u, __ATOMIC_RELEASE);
        vrapi_LeaveVrMode(ovr_);
        ovr_ = nullptr;
        LOGI("left VR");
        health();
    }

    void attachGuest(bool attach) {
        if (attach == guestAttached_) return;
        env_->CallVoidMethod(app_->activity->clazz, activeCallback_, static_cast<jboolean>(attach));
        if (env_->ExceptionCheck()) {
            env_->ExceptionDescribe();
            env_->ExceptionClear();
            throw std::runtime_error("Guest VR handshake failed");
        }
        guestAttached_ = attach;
    }

    void findRemote() {
        remote_ = ovrDeviceIdType_Invalid;
        remoteHand_ = -1;
        for (uint32_t index = 0;; index++) {
            ovrInputCapabilityHeader header{};
            if (vrapi_EnumerateInputDevices(ovr_, index, &header) < 0) break;
            if (header.Type != ovrControllerType_TrackedRemote) continue;
            ovrInputTrackedRemoteCapabilities capabilities{};
            capabilities.Header = header;
            if (vrapi_GetInputDeviceCapabilities(ovr_, &capabilities.Header) != ovrSuccess) continue;
            remote_ = header.DeviceID;
            remoteHand_ = (capabilities.ControllerCapabilities & ovrControllerCaps_LeftHand) ? 0 : 1;
            trackpadMax_[0] = std::max<float>(1.f, capabilities.TrackpadMaxX);
            trackpadMax_[1] = std::max<float>(1.f, capabilities.TrackpadMaxY);
            LOGI("remote %u in the %s hand, trackpad %ux%u", (unsigned)remote_, remoteHand_ ? "right" : "left",
                 (unsigned)capabilities.TrackpadMaxX, (unsigned)capabilities.TrackpadMaxY);
            return;
        }
    }

    void updateRemote(const ovrTracking2 &head, double displayTime) {
        hand_ = {};
        if (remote_ == ovrDeviceIdType_Invalid || frames_ % 72 == 0) findRemote();
        if (remote_ == ovrDeviceIdType_Invalid) return;
        ovrInputStateTrackedRemote state{};
        state.Header.ControllerType = ovrControllerType_TrackedRemote;
        if (vrapi_GetCurrentInputState(ovr_, remote_, &state.Header) != ovrSuccess) {
            remote_ = ovrDeviceIdType_Invalid;
            return;
        }
        ovrTracking tracking{};
        if (vrapi_GetInputTrackingState(ovr_, remote_, displayTime, &tracking) != ovrSuccess
            || !(tracking.Status & VRAPI_TRACKING_STATUS_ORIENTATION_VALID)) return;
        if (state.RecenterCount != recenterCount_) {
            recenterCount_ = state.RecenterCount;
            for (auto &arm : arms_) arm.Reset();
        }
        const ovrQuatf &controller = tracking.HeadPose.Pose.Orientation;
        ovrVector3f position = tracking.HeadPose.Pose.Position;
        if (!(tracking.Status & VRAPI_TRACKING_STATUS_POSITION_VALID)) {
            const ovrPosef &headPose = head.HeadPose.Pose;
            const ovrVector3f &spin = tracking.HeadPose.AngularVelocity;
            const double now = displayTime;
            const double dt = lastArmTime_ > 0 ? std::clamp(now - lastArmTime_, 0.0, 0.1) : 0.0;
            lastArmTime_ = now;
            droiddeck::Vec3 arm = arms_[remoteHand_].Update(
                {headPose.Position.x, headPose.Position.y, headPose.Position.z},
                {headPose.Orientation.w, headPose.Orientation.x, headPose.Orientation.y, headPose.Orientation.z},
                {controller.w, controller.x, controller.y, controller.z},
                std::sqrt(spin.x * spin.x + spin.y * spin.y + spin.z * spin.z), dt);
            position = {(float)arm.x, (float)arm.y, (float)arm.z};
        }
        ovrPosef pose{};
        pose.Orientation = controller;
        pose.Position = position;
        hand_.pose = posePacket(pose, floorOffset_, kTrackedFlags);
        hand_.active = 1;
        const bool trigger = state.Buttons & (ovrButton_A | ovrButton_Trigger);
        const bool touching = state.TrackpadStatus != 0;
        hand_.trigger = trigger ? 1.f : 0.f;
        if (touching) {
            hand_.stick[0] = std::clamp(state.TrackpadPosition.x / trackpadMax_[0] * 2.f - 1.f, -1.f, 1.f);
            hand_.stick[1] = std::clamp(1.f - state.TrackpadPosition.y / trackpadMax_[1] * 2.f, -1.f, 1.f);
        }
        if (trigger) hand_.buttons |= DROIDDECK_XR_TRIGGER_TOUCH;
        if (touching) hand_.buttons |= DROIDDECK_XR_STICK_TOUCH | DROIDDECK_XR_PRIMARY_TOUCH;
        if (state.Buttons & ovrButton_Enter) hand_.buttons |= DROIDDECK_XR_PRIMARY_CLICK;
        if (state.Buttons & ovrButton_Back) hand_.buttons |= DROIDDECK_XR_MENU_CLICK;
    }

    void publishTracking(const ovrTracking2 &tracking, double displayTime) {
        if (!tracking_) return;
        DroidDeckXrTracking packet{};
        packet.magic = DROIDDECK_XR_MAGIC;
        packet.version = DROIDDECK_XR_VERSION;
        packet.active = (tracking.Status & VRAPI_TRACKING_STATUS_ORIENTATION_VALID) != 0;
        packet.sampled_ns = monotonicNs();
        packet.predicted_ns = packet.sampled_ns + (int64_t)((displayTime - vrapi_GetTimeInSeconds()) * 1e9);
        packet.period_ns = (int64_t)(1e9 / refreshHz_) * settings_.guestDivisor;
        packet.head = posePacket(tracking.HeadPose.Pose, floorOffset_, kTrackedFlags);
        for (int eye = 0; eye < 2; eye++) {
            const ovrMatrix4f eyePose = ovrMatrix4f_Inverse(&tracking.Eye[eye].ViewMatrix);
            ovrPosef pose = tracking.HeadPose.Pose;
            pose.Position = {eyePose.M[0][3], eyePose.M[1][3], eyePose.M[2][3]};
            packet.eyes[eye].pose = posePacket(pose, floorOffset_, kTrackedFlags);
            fovFromProjection(tracking.Eye[eye].ProjectionMatrix, packet.eyes[eye].fov);
            packet.eyes[eye].width = eyeWidth_;
            packet.eyes[eye].height = eyeHeight_;
        }
        if (remoteHand_ >= 0) packet.hands[remoteHand_] = hand_;
        uint32_t sequence = __atomic_load_n(&tracking_->seq, __ATOMIC_RELAXED);
        __atomic_store_n(&tracking_->seq, sequence | 1u, __ATOMIC_SEQ_CST);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        std::memcpy(tracking_, &packet, offsetof(DroidDeckXrTracking, seq));
        std::memcpy(reinterpret_cast<char *>(tracking_) + offsetof(DroidDeckXrTracking, active),
            reinterpret_cast<const char *>(&packet) + offsetof(DroidDeckXrTracking, active),
            sizeof(packet) - offsetof(DroidDeckXrTracking, active));
        __atomic_store_n(&tracking_->seq, (sequence | 1u) + 1u, __ATOMIC_RELEASE);
    }

    void ensureSwapchain(uint32_t width, uint32_t height, uint32_t color) {
        if (swapchain_ && width == swapchainWidth_ && height == swapchainHeight_ && color == swapchainColor_) return;
        if (swapchain_) vrapi_DestroyTextureSwapChain(swapchain_);
        const int64_t format = color == DROIDDECK_XR_COLOR_SRGB_RGBA8 ? GL_SRGB8_ALPHA8 : GL_RGBA8;
        swapchain_ = vrapi_CreateTextureSwapChain3(VRAPI_TEXTURE_TYPE_2D, format, (int)width, (int)height, 1, 3);
        if (!swapchain_) throw std::runtime_error("vrapi_CreateTextureSwapChain3 failed");
        swapchainLength_ = vrapi_GetTextureSwapChainLength(swapchain_);
        swapchainIndex_ = 0;
        swapchainWidth_ = width;
        swapchainHeight_ = height;
        swapchainColor_ = color;
        for (int index = 0; index < swapchainLength_; index++) {
            glBindTexture(GL_TEXTURE_2D, vrapi_GetTextureSwapChainHandle(swapchain_, index));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        LOGI("frame swapchain %ux%u %s, %d images", width, height,
             color == DROIDDECK_XR_COLOR_SRGB_RGBA8 ? "sRGB" : "linear", swapchainLength_);
    }

    bool receiveFrame() {
        DroidDeckXrPixels frame;
        bool fresh = false;
        while (receiver_.Take(&frame)) {
            if (fresh) receiver_.Recycle(std::move(pending_.pixels));
            pending_ = std::move(frame);
            fresh = true;
        }
        if (!fresh) return false;
        ensureSwapchain(pending_.header.width, pending_.header.height, pending_.header.color_space);
        swapchainIndex_ = (swapchainIndex_ + 1) % swapchainLength_;
        glBindTexture(GL_TEXTURE_2D, vrapi_GetTextureSwapChainHandle(swapchain_, swapchainIndex_));
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)pending_.header.width, (GLsizei)pending_.header.height,
                        GL_RGBA, GL_UNSIGNED_BYTE, pending_.pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        if (glGetError() != GL_NO_ERROR) throw std::runtime_error("Frame upload failed");
        sourceHeader_ = pending_.header;
        receiver_.Recycle(std::move(pending_.pixels));
        uploaded_++;
        return true;
    }
    DroidDeckXrPixels pending_;

    template <typename T>
    T checked(T value, const char *what) {
        if (env_->ExceptionCheck()) {
            env_->ExceptionDescribe();
            env_->ExceptionClear();
            throw std::runtime_error(what);
        }
        return value;
    }

    void frame() {
        frameIndex_++;
        const double displayTime = vrapi_GetPredictedDisplayTime(ovr_, frameIndex_);
        const ovrTracking2 tracking = vrapi_GetPredictedTracking2(ovr_, displayTime);
        updateRemote(tracking, displayTime);
        const int flags = checked(env_->CallIntMethod(app_->activity->clazz, homeCallback_), "Home state callback failed");
        if (flags & kImmersive) steamVrFrame(tracking, displayTime);
        else homeFrame(tracking, displayTime, flags & kShowScreen, flags & kShowKeyboard);
        frames_++;
        if (frames_ % 360 == 0) {
            health();
            LOGI("frames=%llu uploaded=%llu received=%llu rejected=%llu remote=%d immersive=%d", (unsigned long long)frames_,
                 (unsigned long long)uploaded_, (unsigned long long)receiver_.Received(),
                 (unsigned long long)receiver_.Rejected(), remoteHand_, flags & kImmersive);
        }
    }

    void submit(const ovrLayerHeader2 *layer, double displayTime) {
        const ovrLayerHeader2 *layers[] = {layer};
        ovrSubmitFrameDescription2 description{};
        description.SwapInterval = 1;
        description.FrameIndex = frameIndex_;
        description.DisplayTime = displayTime;
        description.LayerCount = 1;
        description.Layers = layers;
        vrapi_SubmitFrame2(ovr_, &description);
    }

    void uploadPanel(jmethodID callback, int width, int height, bool keyboard) {
        jobject pixels = checked(env_->CallObjectMethod(app_->activity->clazz, callback), "Panel callback failed");
        if (!pixels) return;
        void *address = env_->GetDirectBufferAddress(pixels);
        if (address && env_->GetDirectBufferCapacity(pixels) == (jlong)width * height * 4) {
            if (keyboard) home_.UploadKeyboard(address, width, height);
            else home_.UploadUi(address, width, height);
        }
        env_->DeleteLocalRef(pixels);
    }

    void homeFrame(const ovrTracking2 &tracking, double displayTime, bool showScreen, bool showKeyboard) {
        if (frames_ == 0) step("first room frame");
        attachGuest(false);
        if (tracking_) __atomic_store_n(&tracking_->active, 0u, __ATOMIC_RELEASE);
        if (std::isnan(homeY_) && (tracking.Status & VRAPI_TRACKING_STATUS_ORIENTATION_VALID))
            homeY_ = tracking.HeadPose.Pose.Position.y;
        const float eyeY = std::isnan(homeY_) ? kStandingHeight - floorOffset_ : homeY_;
        govr::Home::Scene scene;
        scene.floorY = -floorOffset_;
        if (showScreen) {
            scene.screen = {0.f, eyeY + 0.25f, -2.4f, 2.56f, 1.44f, true};
            if (showKeyboard) scene.keyboard = {0.f, eyeY - 0.68f, -1.5f, 1.2f, 0.45f, true};
            else scene.ui = {0.f, eyeY - 0.78f, -1.7f, 0.8f, 0.4f, true};
        } else {
            scene.ui = {0.f, eyeY - 0.1f, -1.6f, 1.0f, 0.5f, true};
        }
        uploadPanel(uiCallback_, kUiWidth, kUiHeight, false);
        uploadPanel(keyboardCallback_, kKeyboardWidth, kKeyboardHeight, true);
        if (showScreen) {
            auto matrix = static_cast<jfloatArray>(checked(env_->CallObjectMethod(app_->activity->clazz, screenCallback_,
                (jint)home_.ScreenTexture()), "Steam screen callback failed"));
            if (matrix) {
                if (env_->GetArrayLength(matrix) == 16) {
                    env_->GetFloatArrayRegion(matrix, 0, 16, scene.screenTransform);
                    scene.screenLive = true;
                }
                env_->DeleteLocalRef(matrix);
            }
        }
        int target = 0;
        float u = 0, v = 0;
        if (hand_.active) {
            const auto &pose = hand_.pose;
            const ovrVector3f origin{pose.position[0], pose.position[1] - floorOffset_, pose.position[2]};
            const droiddeck::Vec3 forward = droiddeck::Rotate(
                {pose.orientation[3], pose.orientation[0], pose.orientation[1], pose.orientation[2]}, {0.0, 0.0, -1.0});
            const ovrVector3f direction{(float)forward.x, (float)forward.y, (float)forward.z};
            float distance = 3.f, hitDistance = 0, hu = 0, hv = 0;
            if (scene.ui.Hit(origin, direction, &hitDistance, &hu, &hv) && hitDistance < distance) {
                distance = hitDistance; target = kTargetUi; u = hu; v = hv;
            }
            if (scene.screen.Hit(origin, direction, &hitDistance, &hu, &hv) && hitDistance < distance) {
                distance = hitDistance; target = kTargetScreen; u = hu; v = hv;
            }
            if (scene.keyboard.Hit(origin, direction, &hitDistance, &hu, &hv) && hitDistance < distance) {
                distance = hitDistance; target = kTargetKeyboard; u = hu; v = hv;
            }
            scene.laser = true;
            scene.laserFrom = origin;
            scene.laserTo = {origin.x + direction.x * distance, origin.y + direction.y * distance, origin.z + direction.z * distance};
            if (target) {
                scene.cursor = true;
                scene.cursorAt = scene.laserTo;
            }
        }
        if (target || lastTarget_) {
            env_->CallVoidMethod(app_->activity->clazz, pointerCallback_, (jint)target, u, v,
                (jboolean)(hand_.trigger > 0.5f), (jboolean)((hand_.buttons & DROIDDECK_XR_PRIMARY_CLICK) != 0),
                (jboolean)((hand_.buttons & DROIDDECK_XR_MENU_CLICK) != 0));
            checked(0, "Pointer callback failed");
        }
        lastTarget_ = target;
        if (frames_ == 0) step("first room render");
        ovrLayerProjection2 layer = home_.Render(tracking, scene);
        if (frames_ == 0) step("first room submit");
        layer.HeadPose.TimeInSeconds = displayTime;
        layer.Header.Flags |= VRAPI_FRAME_LAYER_FLAG_CHROMATIC_ABERRATION_CORRECTION;
        submit(&layer.Header, displayTime);
    }

    void steamVrFrame(const ovrTracking2 &tracking, double displayTime) {
        publishTracking(tracking, displayTime);
        attachGuest(true);
        if (frameIndex_ % (uint64_t)settings_.guestDivisor == 0) {
            env_->CallVoidMethod(app_->activity->clazz, frameCallback_, (jlong)monotonicNs());
            if (env_->ExceptionCheck()) { env_->ExceptionClear(); throw std::runtime_error("Guest frame callback failed"); }
        }
        receiveFrame();

        ovrLayerProjection2 layer = vrapi_DefaultLayerProjection2();
        const bool valid = swapchain_ && sourceHeader_.frame_id && monotonicNs() - sourceHeader_.captured_ns < kStaleFrameNs;
        if (valid) {
            const DroidDeckXrRenderView left = xr_frame_view(sourceHeader_, 0), right = xr_frame_view(sourceHeader_, 1);
            layer.HeadPose.Pose.Orientation = {left.orientation[0], left.orientation[1], left.orientation[2], left.orientation[3]};
            layer.HeadPose.Pose.Position = {(left.position[0] + right.position[0]) * 0.5f,
                                            (left.position[1] + right.position[1]) * 0.5f - floorOffset_,
                                            (left.position[2] + right.position[2]) * 0.5f};
            layer.HeadPose.TimeInSeconds = displayTime;
            for (int eye = 0; eye < VRAPI_FRAME_LAYER_EYE_MAX; eye++) {
                layer.Textures[eye].ColorSwapChain = swapchain_;
                layer.Textures[eye].SwapChainIndex = swapchainIndex_;
                layer.Textures[eye].TexCoordsFromTanAngles = sideBySideTexCoords(sourceHeader_.projection[eye], eye);
            }
        } else layer = vrapi_DefaultLayerBlackProjection2();
        layer.Header.Flags |= VRAPI_FRAME_LAYER_FLAG_CHROMATIC_ABERRATION_CORRECTION;
        submit(&layer.Header, displayTime);
    }

    void cleanup() {
        receiver_.Stop();
        if (ovr_) {
            try { attachGuest(false); } catch (const std::exception &error) { LOGE("cleanup: %s", error.what()); }
            vrapi_LeaveVrMode(ovr_);
            ovr_ = nullptr;
        }
        if (tracking_) {
            __atomic_store_n(&tracking_->active, 0u, __ATOMIC_RELEASE);
            munmap(tracking_, sizeof(DroidDeckXrTracking));
        }
        if (haptics_) munmap(haptics_, sizeof(DroidDeckXrHaptics));
        if (swapchain_) vrapi_DestroyTextureSwapChain(swapchain_);
        home_.Destroy();
        if (display_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
            if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
            eglTerminate(display_);
        }
        if (initialized_) vrapi_Shutdown();
        if (env_) app_->activity->vm->DetachCurrentThread();
    }
};

}

void android_main(android_app *app) {
    installCrashReport(app->activity->externalDataPath);
    GoVR runtime(app);
    try { runtime.run(); }
    catch (const std::exception &error) {
        LOGE("%s", error.what());
        step(error.what());
        runtime.health(error.what());
        ANativeActivity_finish(app->activity);
    }
}
