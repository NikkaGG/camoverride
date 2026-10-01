// CameraOverride (Zygisk) — makes the rear camera in selected apps open as another camera ID
// (default "2"; confirm the ultrawide ID on your firmware), without LSPosed.
//
// How: hooks the JNI native android.os.BinderProxy.transactNative inside the target app and,
// for calls to cameraserver (ICameraService), rewrites the camera ID "0" -> "2" in the parcel.
// Covers Java Camera2 BinderProxy calls. Native C++ Binder calls need a separate hook.
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
#include <atomic>

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
struct AIBinder;
static AParcel *(*p_fromJava)(JNIEnv *, jobject) = nullptr;
static void (*p_delete)(AParcel *) = nullptr;
static int32_t (*p_getPos)(const AParcel *) = nullptr;
static int32_t (*p_getSize)(const AParcel *) = nullptr;
static int32_t (*p_setPos)(const AParcel *, int32_t) = nullptr;
static int32_t (*p_readInt)(const AParcel *, int32_t *) = nullptr;
static int32_t (*p_writeInt)(AParcel *, int32_t) = nullptr;
static int32_t (*p_readBinder)(const AParcel *, AIBinder **) = nullptr;
static void (*p_decStrong)(AIBinder *) = nullptr;

static int32_t op_getPos(void *p) { return p_getPos((AParcel *) p); }
static int32_t op_getSize(void *p) { return p_getSize((AParcel *) p); }
static int op_setPos(void *p, int32_t v) { return p_setPos((AParcel *) p, v); }
static int op_readInt(void *p, int32_t *o) { return p_readInt((AParcel *) p, o); }
static int op_writeInt(void *p, int32_t v) { return p_writeInt((AParcel *) p, v); }
static int op_skipBinder(void *p) {
    AIBinder *binder = nullptr;
    const int status = p_readBinder((AParcel *) p, &binder);
    if (binder != nullptr) p_decStrong(binder);
    return status;
}
static const ParcelOps kOps = {op_getPos, op_getSize, op_setPos, op_readInt, op_writeInt, op_skipBinder};

struct CameraMethod {
    const char *name;
    CameraIdLayout layout;
    jint code = -1;
};
static CameraMethod kMethods[] = {
    {"connectDevice", CameraIdLayout::BinderThenString},
    {"getCameraCharacteristics", CameraIdLayout::FirstString},
    {"supportsCameraApi", CameraIdLayout::FirstString},
    {"isHiddenPhysicalCamera", CameraIdLayout::FirstString},
    {"setTorchMode", CameraIdLayout::FirstString},
    {"turnOnTorchWithStrengthLevel", CameraIdLayout::FirstString},
    {"getTorchStrengthLevel", CameraIdLayout::FirstString},
    {"createDefaultRequest", CameraIdLayout::FirstString},
    {"isSessionConfigurationWithParametersSupported", CameraIdLayout::FirstString},
    {"getSessionCharacteristics", CameraIdLayout::FirstString},
};

// Transaction numbers change across Android versions. Resolve the firmware's
// generated Stub constants before installing the hook; never guess the method.
static bool resolveCameraMethods(JNIEnv *env) {
    jclass stub = env->FindClass("android/hardware/ICameraService$Stub");
    if (stub == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGI("Cannot resolve ICameraService$Stub; hook disabled");
        return false;
    }
    for (auto &method : kMethods) {
        char fieldName[128] = {};
        snprintf(fieldName, sizeof(fieldName), "TRANSACTION_%s", method.name);
        jfieldID field = env->GetStaticFieldID(stub, fieldName, "I");
        if (field == nullptr || env->ExceptionCheck()) {
            env->ExceptionClear();  // optional method may not exist on this version
            continue;
        }
        method.code = env->GetStaticIntField(stub, field);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            method.code = -1;
        }
    }
    env->DeleteLocalRef(stub);
    const bool ready = kMethods[0].code > 0 && kMethods[1].code > 0;
    LOGI("v0.3 transactions: connectDevice=%d getCameraCharacteristics=%d ready=%d",
         kMethods[0].code, kMethods[1].code, ready);
    return ready;
}

static const CameraMethod *cameraMethod(jint code) {
    for (const auto &method : kMethods) if (method.code == code) return &method;
    return nullptr;
}

// Replacement camera ID character ('2' by default); -1 = disabled via property.
static int32_t targetChar() {
    char v[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.camoverride.id", v);
    if (strcmp(v, "off") == 0) return -1;
    if (v[0] >= '0' && v[0] <= '9' && v[1] == '\0') return v[0];  // single-digit IDs (in-place)
    return '2';
}

typedef jboolean (*transact_t)(JNIEnv *, jobject, jint, jobject, jobject, jint);
static transact_t orig_transact = nullptr;

// Diagnostics: log the first N ICameraService calls we did NOT patch, with the raw words after the
// interface token, so the real parcel layout of e.g. connectDevice can be read from logcat.
static std::atomic<int> g_dumpBudget{120};

static void dumpUnpatched(const AParcel *p, jint code, const CameraMethod *method) {
    int32_t words[24] = {};
    int32_t offset = 0;
    int status = 0;
    const auto layout = method ? method->layout : CameraIdLayout::None;
    int n = dumpCameraCall(kOps, (void *) p, layout, words, 24, &offset, &status);
    if (n < 0) return;
    if (g_dumpBudget.fetch_sub(1, std::memory_order_relaxed) <= 0) return;
    char buf[24 * 9 + 1] = {};
    size_t off = 0;
    for (int i = 0; i < n; i++) {
        int written = snprintf(buf + off, sizeof(buf) - off, "%x ", (unsigned) words[i]);
        if (written < 0 || (size_t) written >= sizeof(buf) - off) break;
        off += (size_t) written;
    }
    LOGI("ICameraService code=%d method=%s size=%d args_offset=%d read_status=%d unpatched words: %s",
         code, method ? method->name : "other", p_getSize(p), offset, status, n ? buf : "<empty>");
}

static jboolean my_transact(JNIEnv *env, jobject thiz, jint code, jobject data, jobject reply, jint flags) {
    if (data != nullptr) {
        AParcel *p = p_fromJava(env, data);
        if (p != nullptr) {
            const CameraMethod *method = cameraMethod(code);
            const int32_t to = targetChar();  // one consistent value per transaction
            const auto layout = method ? method->layout : CameraIdLayout::None;
            const auto r = patchCameraId(kOps, p, layout, '0', to);
            if (r.layout != 0) {
                LOGI("ICameraService code=%d method=%s (layout %d offset=%d): camera 0 -> %c",
                     code, method->name, r.layout, r.idOffset, (char) to);
            } else if (g_dumpBudget.load(std::memory_order_relaxed) > 0) {
                dumpUnpatched(p, code, method);
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
    p_readBinder = (decltype(p_readBinder)) dlsym(h, "AParcel_readStrongBinder");
    p_decStrong = (decltype(p_decStrong)) dlsym(h, "AIBinder_decStrong");
    if (!p_fromJava || !p_delete || !p_getPos || !p_getSize || !p_setPos || !p_readInt ||
        !p_writeInt || !p_readBinder || !p_decStrong) {
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
        if (target) methodsReady = resolveCameraMethods(env);
        // Non-target app: unload this module from the process entirely.
        if (!target) api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!target || !methodsReady) return;
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
    bool methodsReady = false;
};

REGISTER_ZYGISK_MODULE(CamOverride)
