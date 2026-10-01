// Pure C++ (no Android dependencies) so the parcel logic can be unit-tested on a PC.
//
// Idea: every app talks to cameraserver through binder (interface "android.hardware.ICameraService").
// Both the Java Camera2 API and the NDK one end up sending the camera ID as a string in the parcel:
//   connectDevice(callbacks, cameraId, ...)         -> [binder object + stability word][len=1]["0"]
//   getCameraCharacteristics(cameraId, ...) etc.    ->                                [len=1]["0"]
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
// How far after the descriptor (in int32 units) to look for [len=1][id]. A binder object in a
// parcel is 24 bytes + a 4-byte stability word, so connectDevice's ID sits 7 ints in; scan a bit wider.
static const int kScanInts = 16;

// fromChar: camera ID character to replace (e.g. '0').
// toFn: returns the replacement character, or -1 to leave the parcel untouched
//       (only called once the parcel really is an ICameraService call carrying fromChar).
// Returns 0 = untouched, otherwise 1 + (int32 offset of the ID after the descriptor):
//   1 = ID is the first argument (getCameraCharacteristics...), 8 = after a binder object (connectDevice).
inline int patchCameraId(const ParcelOps &ops, void *p, int32_t fromChar, int32_t (*toFn)()) {
    const int32_t size = ops.getSize(p);
    // strict mode(4) + work source(4) + header(4) + descriptor len(4) + descriptor(64) + id len(4) + id(4)
    if (size < 12 + 4 + (int32_t) sizeof(kCameraServiceDescriptor) + 8) return 0;
    const int32_t saved = ops.getPos(p);
    if (ops.setPos(p, 0) != 0) return 0;

    int result = 0;
    do {
        int32_t v = 0;
        if (ops.readInt(p, &v) != 0) break;                              // strict mode policy
        if (ops.readInt(p, &v) != 0) break;                              // work source uid
        if (ops.readInt(p, &v) != 0 || v != kParcelHeader) break;        // 'SYST'
        if (ops.readInt(p, &v) != 0 || v != kDescriptorLen) break;       // descriptor length (chars)

        int32_t w[sizeof(kCameraServiceDescriptor) / sizeof(int32_t)];
        bool ok = true;
        for (auto &x : w) {
            if (ops.readInt(p, &x) != 0) { ok = false; break; }
        }
        if (!ok || memcmp(w, kCameraServiceDescriptor, sizeof(kCameraServiceDescriptor)) != 0) break;

        const int32_t base = ops.getPos(p);
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
    } while (false);

    ops.setPos(p, saved);
    return result;
}
