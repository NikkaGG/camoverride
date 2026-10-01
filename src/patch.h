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
};

static const char16_t kCameraServiceDescriptor[] = u"android.hardware.ICameraService";
static const int32_t kDescriptorLen =
        (int32_t) (sizeof(kCameraServiceDescriptor) / sizeof(char16_t)) - 1;  // 31
static const int32_t kParcelHeader = 0x53595354;  // 'SYST'
// How far after the descriptor (in int32 units) to look for [len=1][id].
static const int kScanInts = 16;

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

// fromChar: camera ID character to replace (e.g. '0').
// toFn: returns the replacement character, or -1 to leave the parcel untouched
//       (only called once the parcel really is an ICameraService call carrying fromChar).
// Returns 0 = untouched, otherwise 1 + (int32 offset of the ID after the descriptor):
//   1 = ID is the first argument, 8 = after a binder object.
inline int patchCameraId(const ParcelOps &ops, void *p, int32_t fromChar, int32_t (*toFn)()) {
    const int32_t size = ops.getSize(p);
    const int32_t saved = ops.getPos(p);
    int result = 0;
    const int32_t base = afterCameraServiceToken(ops, p);
    if (base >= 0) {
        for (int k = 0; k < kScanInts; k++) {
            const int32_t c = base + 4 * k;
            if (c + 8 > size) break;
            int32_t len = 0, id = 0;
            if (ops.setPos(p, c) != 0 || ops.readInt(p, &len) != 0 || ops.readInt(p, &id) != 0) break;
            // UTF-16LE "X" + NUL packs into one int32 whose value is X
            if (len == 1 && id == fromChar) {
                const int32_t to = toFn();
                if (to >= 0 && ops.setPos(p, c + 4) == 0 && ops.writeInt(p, to) == 0) result = k + 1;
                break;
            }
        }
    }
    ops.setPos(p, saved);
    return result;
}

// Diagnostics: copies up to maxInts int32 words that follow the descriptor into out.
// Returns the number copied, or -1 if the parcel is not an ICameraService call. Never modifies the parcel.
inline int dumpCameraCall(const ParcelOps &ops, void *p, int32_t *out, int maxInts) {
    const int32_t size = ops.getSize(p);
    const int32_t saved = ops.getPos(p);
    int n = -1;
    const int32_t base = afterCameraServiceToken(ops, p);
    if (base >= 0) {
        n = 0;
        if (ops.setPos(p, base) == 0) {
            while (n < maxInts && base + 4 * (n + 1) <= size && ops.readInt(p, &out[n]) == 0) n++;
        }
    }
    ops.setPos(p, saved);
    return n;
}
