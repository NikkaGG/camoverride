// CameraOverride (Zygisk) — makes the rear camera in selected apps open as another camera ID
// (default "2" = ultrawide on Poco X4 Pro), without LSPosed.
//
// How: hooks the JNI native android.os.BinderProxy.transactNative inside the target app and,
// for calls to cameraserver (ICameraService), rewrites the camera ID "0" -> "2" in the parcel.
// Works for Java Camera2 and NDK Camera2 alike, since both go through binder.
//
// Runtime control (no rebuild), as root, then reopen the camera:
//   setprop debug.camoverride.id 2     # try 2, 5, 6
//   setprop debug.camoverride.id off   # disable
// Logs: adb logcat -s CamOverride

#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/types.h>

#include "zygisk.hpp"
#include "patch.h"

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

// ---- libbinder_ndk (public NDK library), resolved with dlsym ----
struct AParcel;
static AParcel *(*p_fromJava)(JNIEnv *, jobject) = nullptr;
static void (*p_delete)(AParcel *) = nullptr;
static int32_t (*p_getPos)(const AParcel *) = nullptr;
static int32_t (*p_getSize)(const AParcel *) = nullptr;
static int32_t (*p_setPos)(const AParcel *, int32_t) = nullptr;
static int32_t (*p_readInt)(const AParcel *, int32_t *) = nullptr;
static int32_t (*p_writeInt)(AParcel *, int32_t) = nullptr;

static int32_t op_getPos(void *p) { return p_getPos((AParcel *) p); }
static int32_t op_getSize(void *p) { return p_getSize((AParcel *) p); }
static int op_setPos(void *p, int32_t v) { return p_setPos((AParcel *) p, v); }
static int op_readInt(void *p, int32_t *o) { return p_readInt((AParcel *) p, o); }
static int op_writeInt(void *p, int32_t v) { return p_writeInt((AParcel *) p, v); }
static const ParcelOps kOps = {op_getPos, op_getSize, op_setPos, op_readInt, op_writeInt};

// Replacement camera ID character ('2' by default); -1 = disabled via property.
static int32_t targetChar() {
    char v[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.camoverride.id", v);
    if (strcmp(v, "off") == 0) return -1;
    if (v[0] != '\0' && v[1] == '\0') return (unsigned char) v[0];  // single-char IDs only (in-place patch)
    return '2';
}

typedef jboolean (*transact_t)(JNIEnv *, jobject, jint, jobject, jobject, jint);
static transact_t orig_transact = nullptr;

// Diagnostics: log the first N ICameraService calls we did NOT patch, with the raw words after the
// interface token, so the real parcel layout of e.g. connectDevice can be read from logcat.
static int g_dumpBudget = 120;

static void dumpUnpatched(const AParcel *p, jint code) {
    int32_t words[24];
    int n = dumpCameraCall(kOps, (void *) p, words, 24);
    if (n < 0) return;
    g_dumpBudget--;
    char buf[24 * 9 + 1];
    int off = 0;
    for (int i = 0; i < n; i++) off += snprintf(buf + off, sizeof(buf) - off, "%x ", (unsigned) words[i]);
    LOGI("ICameraService code=%d size=%d unpatched words: %s", code, p_getSize(p), buf);
}

static jboolean my_transact(JNIEnv *env, jobject thiz, jint code, jobject data, jobject reply, jint flags) {
    if (data != nullptr) {
        AParcel *p = p_fromJava(env, data);
        if (p != nullptr) {
            int r = patchCameraId(kOps, p, '0', targetChar);
            if (r != 0) {
                LOGI("ICameraService call code=%d (layout %d): camera 0 -> %c", code, r, (char) targetChar());
            } else if (g_dumpBudget > 0) {
                dumpUnpatched(p, code);
            }
            p_delete(p);
        }
    }
    return orig_transact(env, thiz, code, data, reply, flags);
}

static bool loadBinderNdk() {
    void *h = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!h) {
        LOGI("dlopen libbinder_ndk.so failed: %s", dlerror());
        return false;
    }
    p_fromJava = (decltype(p_fromJava)) dlsym(h, "AParcel_fromJavaParcel");
    p_delete = (decltype(p_delete)) dlsym(h, "AParcel_delete");
    p_getPos = (decltype(p_getPos)) dlsym(h, "AParcel_getDataPosition");
    p_getSize = (decltype(p_getSize)) dlsym(h, "AParcel_getDataSize");
    p_setPos = (decltype(p_setPos)) dlsym(h, "AParcel_setDataPosition");
    p_readInt = (decltype(p_readInt)) dlsym(h, "AParcel_readInt32");
    p_writeInt = (decltype(p_writeInt)) dlsym(h, "AParcel_writeInt32");
    if (!p_fromJava || !p_delete || !p_getPos || !p_getSize || !p_setPos || !p_readInt || !p_writeInt) {
        LOGI("missing libbinder_ndk symbols (fromJava=%p)", (void *) p_fromJava);
        return false;
    }
    return true;
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
        if (!target) return;
        LOGI("target app detected, hooking BinderProxy.transactNative");
        if (!loadBinderNdk()) return;

        JNINativeMethod m[] = {
            {"transactNative", "(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z", (void *) my_transact},
        };
        api->hookJniNativeMethods(env, "android/os/BinderProxy", m, 1);
        orig_transact = (transact_t) m[0].fnPtr;
        if (orig_transact == nullptr) {
            LOGI("transactNative not found / hook failed");
        } else {
            LOGI("hook installed");
        }
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
    bool target = false;
};

REGISTER_ZYGISK_MODULE(CamOverride)
