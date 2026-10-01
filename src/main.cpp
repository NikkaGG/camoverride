// CameraOverride (Zygisk) — makes the rear camera in selected apps open as another camera ID
// (default "3" for the supplied Poco X4 Pro camera dump), without LSPosed.
//
// How: hooks the JNI native android.os.BinderProxy.transactNative inside the target app and,
// rewrites service IDs and each CaptureRequest's logical ID for the opened device.
// Covers Java Camera2 BinderProxy calls. Native C++ Binder calls need a separate hook.
//
// Runtime control (no rebuild), as root, then reopen the camera:
//   setprop debug.camoverride.id 3
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
#include <mutex>
#include <vector>

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
struct AStatus;
static AParcel *(*p_fromJava)(JNIEnv *, jobject) = nullptr;
static void (*p_delete)(AParcel *) = nullptr;
static int32_t (*p_getPos)(const AParcel *) = nullptr;
static int32_t (*p_getSize)(const AParcel *) = nullptr;
static int32_t (*p_setPos)(const AParcel *, int32_t) = nullptr;
static int32_t (*p_readInt)(const AParcel *, int32_t *) = nullptr;
static int32_t (*p_writeInt)(AParcel *, int32_t) = nullptr;
static int32_t (*p_readBinder)(const AParcel *, AIBinder **) = nullptr;
static void (*p_decStrong)(AIBinder *) = nullptr;
static int32_t (*p_readStatus)(const AParcel *, AStatus **) = nullptr;
static bool (*p_statusOk)(const AStatus *) = nullptr;
static int32_t (*p_exception)(const AStatus *) = nullptr;
static int32_t (*p_serviceError)(const AStatus *) = nullptr;
static int32_t (*p_transportStatus)(const AStatus *) = nullptr;
static const char *(*p_statusMessage)(const AStatus *) = nullptr;
static void (*p_deleteStatus)(AStatus *) = nullptr;
static bool g_replyDiagnostics = false;

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
static CameraMethod kDeviceMethods[] = {
    {"disconnect", CameraIdLayout::None},
    {"beginConfigure", CameraIdLayout::None},
    {"endConfigure", CameraIdLayout::None},
    {"createStream", CameraIdLayout::None},
    {"createDefaultRequest", CameraIdLayout::None},
    {"submitRequest", CameraIdLayout::None},
    {"submitRequestList", CameraIdLayout::None},
};

static jclass g_metadataClass = nullptr, g_surfaceClass = nullptr;
static jmethodID g_metadataCtor = nullptr, g_metadataRead = nullptr, g_metadataClose = nullptr;
static jmethodID g_parcelReadBinder = nullptr, g_parcelReadParcelable = nullptr, g_surfaceRelease = nullptr;
static bool g_requestTag = false;

static bool resolveCaptureDecoders(JNIEnv *env) {
    const auto find = [&](const char *name) -> jclass {
        jclass cls = env->FindClass(name);
        if (env->ExceptionCheck()) env->ExceptionClear();
        return cls;
    };
    const auto method = [&](jclass cls, const char *name, const char *signature) -> jmethodID {
        if (!cls) return nullptr;
        jmethodID id = env->GetMethodID(cls, name, signature);
        if (env->ExceptionCheck()) env->ExceptionClear();
        return id;
    };
    jclass metadata = find("android/hardware/camera2/impl/CameraMetadataNative");
    jclass parcel = find("android/os/Parcel");
    jclass surface = find("android/view/Surface");
    jclass request = find("android/hardware/camera2/CaptureRequest");
    g_metadataCtor = method(metadata, "<init>", "()V");
    g_metadataRead = method(metadata, "readFromParcel", "(Landroid/os/Parcel;)V");
    g_metadataClose = method(metadata, "close", "()V");
    g_parcelReadBinder = method(parcel, "readStrongBinder", "()Landroid/os/IBinder;");
    g_parcelReadParcelable = method(parcel, "readParcelable", "(Ljava/lang/ClassLoader;)Landroid/os/Parcelable;");
    g_surfaceRelease = method(surface, "release", "()V");
    const bool ready = request && g_metadataCtor && g_metadataRead &&
                       g_metadataClose && g_parcelReadBinder && g_parcelReadParcelable && g_surfaceRelease;
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (ready) {
        // This marker was introduced with the serialized user tag; also handles ROM backports.
        g_requestTag = env->GetStaticFieldID(request, "SET_TAG_STRING_PREFIX", "Ljava/lang/String;") != nullptr;
        if (env->ExceptionCheck()) env->ExceptionClear();
        g_metadataClass = (jclass) env->NewGlobalRef(metadata);
        if (env->ExceptionCheck()) env->ExceptionClear();
        g_surfaceClass = (jclass) env->NewGlobalRef(surface);
    }
    env->DeleteLocalRef(metadata);
    env->DeleteLocalRef(parcel);
    env->DeleteLocalRef(surface);
    env->DeleteLocalRef(request);
    const bool globalsReady = !env->ExceptionCheck() && g_metadataClass && g_surfaceClass;
    if (env->ExceptionCheck()) env->ExceptionClear();
    LOGI("capture decoders ready=%d user_tag=%d", ready && globalsReady, g_requestTag);
    return ready && globalsReady;
}

struct DecodeContext { JNIEnv *env; jobject parcel; };
static int decodeMetadata(void *context, void *) {
    const auto &ctx = *static_cast<DecodeContext *>(context);
    auto env = ctx.env;
    jobject metadata = env->NewObject(g_metadataClass, g_metadataCtor);
    if (env->ExceptionCheck() || !metadata) {
        env->ExceptionClear();
        return -1;
    }
    env->CallVoidMethod(metadata, g_metadataRead, ctx.parcel);
    bool failed = env->ExceptionCheck();
    if (failed) env->ExceptionClear(); // our temporary decoder's exception, never the app's transact
    env->CallVoidMethod(metadata, g_metadataClose); // deterministic native allocation cleanup
    if (env->ExceptionCheck()) { env->ExceptionClear(); failed = true; }
    env->DeleteLocalRef(metadata);
    return failed ? -1 : 0;
}
static int decodeSurface(void *context, void *) {
    const auto &ctx = *static_cast<DecodeContext *>(context);
    auto env = ctx.env;
    jobject surface = env->CallObjectMethod(ctx.parcel, g_parcelReadParcelable, nullptr);
    bool failed = env->ExceptionCheck();
    if (failed) env->ExceptionClear();
    if (surface) {
        if (env->IsInstanceOf(surface, g_surfaceClass)) {
            env->CallVoidMethod(surface, g_surfaceRelease);
            if (env->ExceptionCheck()) { env->ExceptionClear(); failed = true; }
        } else failed = true;
        env->DeleteLocalRef(surface);
    }
    return failed ? -1 : 0;
}

// BinderProxy caches one Java proxy per remote Binder. Weak refs do not keep a
// closed device alive. Bind the target to this successful open, not a later property.
struct DeviceSession { jweak binder; int32_t target; };
static std::mutex g_sessionsMutex;
static std::vector<DeviceSession> g_sessions;
static int32_t sessionTarget(JNIEnv *env, jobject binder, bool remove = false) {
    std::lock_guard<std::mutex> lock(g_sessionsMutex);
    int32_t target = -1;
    for (auto it = g_sessions.begin(); it != g_sessions.end();) {
        const bool dead = env->IsSameObject(it->binder, nullptr);
        const bool match = !dead && env->IsSameObject(it->binder, binder);
        if (match) target = it->target;
        if (dead || (match && remove)) {
            env->DeleteWeakGlobalRef(it->binder);
            it = g_sessions.erase(it);
        } else ++it;
    }
    return target;
}
static void rememberSession(JNIEnv *env, jobject reply, int32_t target) {
    jobject binder = env->CallObjectMethod(reply, g_parcelReadBinder);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGI("connectDevice: cannot decode returned device Binder");
        return;
    }
    if (!binder) { LOGI("connectDevice: returned device Binder is null"); return; }
    sessionTarget(env, binder, true);
    jweak weak = env->NewWeakGlobalRef(binder);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (weak) {
        std::lock_guard<std::mutex> lock(g_sessionsMutex);
        g_sessions.push_back({weak, target});
        LOGI("device session registered: logical camera 0 -> %c", (char) target);
    }
    env->DeleteLocalRef(binder);
}

// Transaction numbers change across Android versions. Resolve the firmware's
// generated Stub constants before installing the hook; never guess the method.
template <size_t N>
static bool resolveMethods(JNIEnv *env, const char *className, CameraMethod (&methods)[N]) {
    jclass stub = env->FindClass(className);
    if (stub == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGI("Cannot resolve %s", className);
        return false;
    }
    for (auto &method : methods) {
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
    return true;
}

static bool resolveCameraMethods(JNIEnv *env) {
    if (!resolveMethods(env, "android/hardware/ICameraService$Stub", kMethods)) return false;
    resolveMethods(env, "android/hardware/camera2/ICameraDeviceUser$Stub", kDeviceMethods);
    const bool ready = kMethods[0].code > 0 && kMethods[1].code > 0 &&
                       kDeviceMethods[5].code > 0 && kDeviceMethods[6].code > 0 && resolveCaptureDecoders(env);
    LOGI("v0.5 transactions: connectDevice=%d getCameraCharacteristics=%d ready=%d",
         kMethods[0].code, kMethods[1].code, ready);
    return ready;
}

template <size_t N>
static const CameraMethod *cameraMethod(jint code, const CameraMethod (&methods)[N]) {
    for (const auto &method : methods) if (method.code == code) return &method;
    return nullptr;
}

// Read the service's actual reply, preserving it for the application's own decoder.
// Never turn a rejected open into success or consume/clear the app's exception.
static void logCameraReply(JNIEnv *env, jobject reply, const char *iface,
                           const CameraMethod *method, int32_t redirectedTo) {
    if (!g_replyDiagnostics || reply == nullptr) return;
    AParcel *p = p_fromJava(env, reply);
    if (p == nullptr) return;
    const int32_t saved = p_getPos(p);
    AStatus *status = nullptr;
    int readStatus = p_setPos(p, 0);
    if (readStatus == 0) readStatus = p_readStatus(p, &status);
    if (readStatus == 0 && status != nullptr) {
        const bool ok = p_statusOk(status);
        if (ok && redirectedTo >= 0 && strcmp(iface, "ICameraService") == 0 &&
            strcmp(method->name, "connectDevice") == 0) {
            rememberSession(env, reply, redirectedTo);
        }
        // Repeating capture submissions can be frequent: print failures plus open/configuration.
        const bool repetitive = strncmp(method->name, "submitRequest", 13) == 0;
        if (!ok || !repetitive) {
            const char *message = p_statusMessage(status);
            LOGI("%s reply method=%s redirected_to=%d ok=%d exception=%d service_error=%d transport=%d message=%s",
                 iface, method->name, redirectedTo >= 0 ? redirectedTo - '0' : -1, ok,
                 p_exception(status), p_serviceError(status), p_transportStatus(status),
                 message && message[0] ? message : "<none>");
        }
    } else {
        LOGI("%s reply method=%s status_read_error=%d", iface, method->name, readStatus);
    }
    if (status != nullptr) p_deleteStatus(status);
    p_setPos(p, saved);
    p_delete(p);
}

// Replacement camera ID character ('3' for this Poco dump); -1 = disabled via property.
static int32_t targetChar() {
    char v[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.camoverride.id", v);
    if (strcmp(v, "off") == 0) return -1;
    if (v[0] >= '0' && v[0] <= '9' && v[1] == '\0') return v[0];  // single-digit IDs (in-place)
    return '3';
}

typedef jboolean (*transact_t)(JNIEnv *, jobject, jint, jobject, jobject, jint);
static transact_t orig_transact = nullptr;

// Diagnostics: log the first N ICameraService calls we did NOT patch, with the raw words after the
// interface token, so the real parcel layout of e.g. connectDevice can be read from logcat.
static std::atomic<int> g_dumpBudget{120};
static std::atomic<int> g_requestLogBudget{12};

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
    const CameraMethod *observed = nullptr;
    const char *observedInterface = nullptr;
    int32_t redirectedTo = -1;
    bool disconnect = false;
    if (data != nullptr && !env->ExceptionCheck()) {
        AParcel *p = p_fromJava(env, data);
        if (p != nullptr) {
            const CameraMethod *method = cameraMethod(code, kMethods);
            const int32_t saved = p_getPos(p);
            if (method != nullptr && afterCameraServiceToken(kOps, p) >= 0) {
                observed = method;
                observedInterface = "ICameraService";
            }
            p_setPos(p, saved);
            if (observed == nullptr) {
                const CameraMethod *deviceMethod = cameraMethod(code, kDeviceMethods);
                if (deviceMethod != nullptr && afterInterfaceToken(kOps, p, kCameraDeviceDescriptor) >= 0) {
                    observed = deviceMethod;
                    observedInterface = "ICameraDeviceUser";
                    disconnect = strcmp(observed->name, "disconnect") == 0;
                }
                p_setPos(p, saved);
            }
            if (observedInterface && strcmp(observedInterface, "ICameraDeviceUser") == 0) {
                redirectedTo = sessionTarget(env, thiz);
                if (redirectedTo >= 0 && strncmp(observed->name, "submitRequest", 13) == 0) {
                    DecodeContext ctx{env, data};
                    CaptureDecoders decoders{&ctx, decodeMetadata, decodeSurface, g_requestTag};
                    const auto r = patchCaptureRequests(kOps, p, decoders,
                            strcmp(observed->name, "submitRequestList") == 0, '0', redirectedTo);
                    if (r.status != 0 || g_requestLogBudget.fetch_sub(1, std::memory_order_relaxed) > 0) {
                        LOGI("ICameraDeviceUser method=%s requests=%d patched=%d camera=0->%c parse_status=%d",
                             observed->name, r.requests, r.patched, (char) redirectedTo, r.status);
                    }
                }
            } else {
                const int32_t to = targetChar();  // one consistent value per transaction
                const auto layout = method ? method->layout : CameraIdLayout::None;
                const auto r = patchCameraId(kOps, p, layout, '0', to);
                if (r.layout != 0) {
                    redirectedTo = to;
                    LOGI("ICameraService code=%d method=%s (layout %d offset=%d): camera 0 -> %c",
                         code, method->name, r.layout, r.idOffset, (char) to);
                } else if (g_dumpBudget.load(std::memory_order_relaxed) > 0) {
                    dumpUnpatched(p, code, method);
                }
            }
            p_delete(p);
        }
    }
    const jboolean result = orig_transact(env, thiz, code, data, reply, flags);
    if (disconnect && !env->ExceptionCheck()) sessionTarget(env, thiz, true);
    if (observed != nullptr) {
        // A pending Java transport exception belongs to the app; do not clear it.
        if (env->ExceptionCheck()) {
            LOGI("%s method=%s JNI transport exception pending", observedInterface, observed->name);
        } else if (result == JNI_FALSE) {
            LOGI("%s method=%s transact returned false", observedInterface, observed->name);
        } else if ((flags & 1) == 0) {
            logCameraReply(env, reply, observedInterface, observed, redirectedTo);
        }
    }
    return result;
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
    p_readStatus = (decltype(p_readStatus)) dlsym(h, "AParcel_readStatusHeader");
    p_statusOk = (decltype(p_statusOk)) dlsym(h, "AStatus_isOk");
    p_exception = (decltype(p_exception)) dlsym(h, "AStatus_getExceptionCode");
    p_serviceError = (decltype(p_serviceError)) dlsym(h, "AStatus_getServiceSpecificError");
    p_transportStatus = (decltype(p_transportStatus)) dlsym(h, "AStatus_getStatus");
    p_statusMessage = (decltype(p_statusMessage)) dlsym(h, "AStatus_getMessage");
    p_deleteStatus = (decltype(p_deleteStatus)) dlsym(h, "AStatus_delete");
    g_replyDiagnostics = p_readStatus && p_statusOk && p_exception && p_serviceError &&
                         p_transportStatus && p_statusMessage && p_deleteStatus;
    LOGI("service reply diagnostics available=%d", g_replyDiagnostics);
    if (!g_replyDiagnostics || !p_fromJava || !p_delete || !p_getPos || !p_getSize || !p_setPos || !p_readInt ||
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
