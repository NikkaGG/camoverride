// CameraOverride (Zygisk) — forces the rear camera of selected apps to a different
// camera ID (default: "2", the ultrawide on Poco X4 Pro) by hooking the NDK Camera2 API.
//
// Runtime control (no rebuild needed), run as root:
//   setprop debug.camoverride.id 2     # target ID (try 2, 5, 6)
//   setprop debug.camoverride.id off   # disable remap
// Logs: adb logcat -s CamOverride

#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <cstring>

#include "zygisk.hpp"
#include "dobby.h"

#define TAG "CamOverride"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static const char *kTargets[] = {
    "com.instagram.android",
    "com.zhiliaoapp.musically",   // TikTok (global)
    "com.ss.android.ugc.trill",   // TikTok (some regions)
    "org.telegram.messenger",
    "org.telegram.messenger.web",
};

static bool isTarget(const char *name) {
    if (!name) return false;
    for (const char *pkg : kTargets) {
        size_t n = strlen(pkg);
        // also match sub-processes like "com.instagram.android:camera"
        if (strncmp(name, pkg, n) == 0 && (name[n] == '\0' || name[n] == ':')) return true;
    }
    return false;
}

// Rear main camera "0" -> configured ID. Everything else (front "1", etc.) untouched.
static const char *mapId(const char *id) {
    if (!id || strcmp(id, "0") != 0) return id;
    static thread_local char buf[PROP_VALUE_MAX];
    char v[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.camoverride.id", v);
    if (strcmp(v, "off") == 0) return id;
    strncpy(buf, v[0] ? v : "2", sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

// camera_status_t ACameraManager_openCamera(ACameraManager*, const char*, ACameraDevice_StateCallbacks*, ACameraDevice**)
typedef int (*open_t)(void *, const char *, void *, void **);
static open_t orig_open = nullptr;
static int my_open(void *mgr, const char *id, void *cb, void **dev) {
    const char *nid = mapId(id);
    if (nid != id) LOGI("openCamera %s -> %s", id, nid);
    return orig_open(mgr, nid, cb, dev);
}

// camera_status_t ACameraManager_getCameraCharacteristics(ACameraManager*, const char*, ACameraMetadata**)
typedef int (*chars_t)(void *, const char *, void **);
static chars_t orig_chars = nullptr;
static int my_chars(void *mgr, const char *id, void **out) {
    const char *nid = mapId(id);
    if (nid != id) LOGI("getCameraCharacteristics %s -> %s", id, nid);
    return orig_chars(mgr, nid, out);
}

static void installHooks() {
    // Force-load the NDK camera lib so it is mapped now, even if the app loads it later.
    void *h = dlopen("libcamera2ndk.so", RTLD_NOW);
    if (!h) {
        LOGI("dlopen libcamera2ndk.so failed: %s", dlerror());
        return;
    }
    void *p_open = dlsym(h, "ACameraManager_openCamera");
    void *p_chars = dlsym(h, "ACameraManager_getCameraCharacteristics");
    if (p_open) {
        int r = DobbyHook(p_open, reinterpret_cast<dobby_dummy_func_t>(my_open),
                          reinterpret_cast<dobby_dummy_func_t *>(&orig_open));
        LOGI("hook openCamera: %d", r);
    }
    if (p_chars) {
        int r = DobbyHook(p_chars, reinterpret_cast<dobby_dummy_func_t>(my_chars),
                          reinterpret_cast<dobby_dummy_func_t *>(&orig_chars));
        LOGI("hook getCameraCharacteristics: %d", r);
    }
}

class CamOverride : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        const char *name = env->GetStringUTFChars(args->nice_name, nullptr);
        target = isTarget(name);
        env->ReleaseStringUTFChars(args->nice_name, name);
        // Non-target app: unload this module from the process entirely.
        if (!target) api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (target) {
            LOGI("target app detected, installing hooks");
            installHooks();
        }
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
    bool target = false;
};

REGISTER_ZYGISK_MODULE(CamOverride)
