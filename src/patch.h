// Pure C++ (no Android dependencies) so the parcel logic can be unit-tested on a PC.
//
// Idea: every app talks to cameraserver through binder (interface "android.hardware.ICameraService").
// The camera ID travels as a string16 in the parcel:
//   getCameraCharacteristics(cameraId, ...) etc. -> [descriptor][len=1]["0"]
//   connectDevice(callbacks, cameraId, ...)      -> [descriptor][binder object + stability][len=1]["0"]
// "0" and "2" are both 1 UTF-16 char, so the ID can be overwritten in place (no resizing).
#pragma once
#include <cstdint>
#include <cstring>

struct ParcelOps {
    int32_t (*getPos)(void *);
    int32_t (*getSize)(void *);
    int (*setPos)(void *, int32_t);       // 0 = ok
    int (*readInt)(void *, int32_t *);    // 0 = ok
    int (*writeInt)(void *, int32_t);     // 0 = ok
    int (*skipBinder)(void *);            // reads/releases a strong Binder, including stability
};

static const char16_t kCameraServiceDescriptor[] = u"android.hardware.ICameraService";
static const int32_t kDescriptorLen =
        (int32_t) (sizeof(kCameraServiceDescriptor) / sizeof(char16_t)) - 1;  // 31
static const int32_t kParcelHeader = 0x53595354;  // 'SYST'
enum class CameraIdLayout { None, FirstString, BinderThenString };

struct CameraPatchResult {
    int layout = 0;
    int32_t idOffset = -1;
    int status = 0;
};

// Reads the interface token from position 0. Returns the position right after the descriptor,
// or -1 if this parcel is not an ICameraService call. Leaves the read position undefined.
inline int32_t afterCameraServiceToken(const ParcelOps &ops, void *p) {
    // strict mode(4) + work source(4) + header(4) + descriptor len(4) + descriptor(64)
    if (ops.getSize(p) < 12 + 4 + (int32_t) sizeof(kCameraServiceDescriptor)) return -1;
    if (ops.setPos(p, 0) != 0) return -1;
    int32_t v = 0;
    if (ops.readInt(p, &v) != 0) return -1;                         // strict mode policy
    if (ops.readInt(p, &v) != 0) return -1;                         // work source uid
    if (ops.readInt(p, &v) != 0 || v != kParcelHeader) return -1;   // 'SYST'
    if (ops.readInt(p, &v) != 0 || v != kDescriptorLen) return -1;  // descriptor length (chars)
    int32_t w[sizeof(kCameraServiceDescriptor) / sizeof(int32_t)];
    for (auto &x : w) {
        if (ops.readInt(p, &x) != 0) return -1;
    }
    if (memcmp(w, kCameraServiceDescriptor, sizeof(kCameraServiceDescriptor)) != 0) return -1;
    return ops.getPos(p);
}

// Parse only the known cameraId argument of a resolved ICameraService transaction.
// Never scan arbitrary words: other arguments can coincidentally contain [1, '0'].
// The Binder API, not a guessed 24/28-byte offset, consumes callbacks and stability.
inline CameraPatchResult patchCameraId(const ParcelOps &ops, void *p, CameraIdLayout layout,
                                      int32_t fromChar, int32_t toChar) {
    const int32_t size = ops.getSize(p);
    const int32_t saved = ops.getPos(p);
    CameraPatchResult result;
    if (layout == CameraIdLayout::None || toChar < 0 || toChar == fromChar) return result;
    const int32_t base = afterCameraServiceToken(ops, p);
    if (base >= 0) {
        bool readable = true;
        if (layout == CameraIdLayout::BinderThenString) {
            result.status = ops.skipBinder(p);
            readable = result.status == 0;
        }
        const int32_t c = ops.getPos(p);
        if (readable && c >= base && c <= size - 8) {
            result.idOffset = c - base;
            int32_t len = 0, id = 0;
            result.status = ops.readInt(p, &len);
            const int32_t idPos = ops.getPos(p);
            if (result.status == 0) result.status = ops.readInt(p, &id);
            // UTF-16LE "X" + NUL packs into one int32 whose value is X
            if (result.status == 0 && len == 1 && id == fromChar) {
                result.status = ops.setPos(p, idPos);
                if (result.status == 0) result.status = ops.writeInt(p, toChar);
                if (result.status == 0) result.layout = (c - base) / 4 + 1;
            }
        }
    }
    ops.setPos(p, saved);
    return result;
}

// Diagnostics: copies up to maxInts int32 words that follow the descriptor into out.
// Returns the number copied, or -1 if the parcel is not an ICameraService call. Never modifies the parcel.
inline int dumpCameraCall(const ParcelOps &ops, void *p, CameraIdLayout layout,
                          int32_t *out, int maxInts, int32_t *argsOffset, int *readStatus) {
    const int32_t size = ops.getSize(p);
    const int32_t saved = ops.getPos(p);
    int n = -1;
    const int32_t base = afterCameraServiceToken(ops, p);
    if (base >= 0) {
        n = 0;
        *readStatus = 0;
        if (layout == CameraIdLayout::BinderThenString) *readStatus = ops.skipBinder(p);
        const int32_t start = ops.getPos(p);
        *argsOffset = start - base;
        if (*readStatus == 0) {
            while (n < maxInts && start + 4 * (n + 1) <= size) {
                *readStatus = ops.readInt(p, &out[n]);
                if (*readStatus != 0) break;
                n++;
            }
        }
    }
    ops.setPos(p, saved);
    return n;
}
