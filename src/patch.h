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
#include <vector>

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
template <size_t N>
inline int32_t afterInterfaceToken(const ParcelOps &ops, void *p, const char16_t (&descriptor)[N]) {
    constexpr size_t wordCount = (sizeof(descriptor) + 3) / 4;
    if (ops.getSize(p) < 16 + (int32_t) (wordCount * 4)) return -1;
    if (ops.setPos(p, 0) != 0) return -1;
    int32_t v = 0;
    if (ops.readInt(p, &v) != 0) return -1;                         // strict mode policy
    if (ops.readInt(p, &v) != 0) return -1;                         // work source uid
    if (ops.readInt(p, &v) != 0 || v != kParcelHeader) return -1;   // 'SYST'
    if (ops.readInt(p, &v) != 0 || v != (int32_t) N - 1) return -1;
    int32_t w[wordCount];
    for (auto &x : w) {
        if (ops.readInt(p, &x) != 0) return -1;
    }
    if (memcmp(w, descriptor, sizeof(descriptor)) != 0) return -1;
    return ops.getPos(p);
}

inline int32_t afterCameraServiceToken(const ParcelOps &ops, void *p) {
    return afterInterfaceToken(ops, p, kCameraServiceDescriptor);
}

static const char16_t kCameraDeviceDescriptor[] = u"android.hardware.camera2.ICameraDeviceUser";

// CaptureRequest parcels contain opaque CameraMetadata blobs and optionally Surface
// objects. Let the firmware's own decoders consume those, never scan their contents.
struct CaptureDecoders {
    void *context;
    int (*metadata)(void *, void *);
    int (*surface)(void *, void *);
    bool userTag;
};
struct CapturePatchResult {
    int requests = 0;
    int patched = 0;
    int status = 0;
};
struct MetadataRange { int32_t start; int32_t end; };

inline int skipString16(const ParcelOps &ops, void *p, int32_t *single = nullptr,
                        int32_t *wordPosition = nullptr) {
    int32_t len = 0;
    if (ops.readInt(p, &len) != 0 || len < -1) return -1;
    if (single) *single = -1;
    if (wordPosition) *wordPosition = ops.getPos(p);
    if (len == -1) return 0;
    const int64_t bytes = ((static_cast<int64_t>(len) + 1) * 2 + 3) & ~int64_t(3);
    if (bytes > ops.getSize(p) - ops.getPos(p)) return -1;
    if (len == 1 && single) return ops.readInt(p, single);
    return ops.setPos(p, ops.getPos(p) + static_cast<int32_t>(bytes));
}

// AIDL typed object presence, settings (logical first, then physical), reprocess,
// Parcelable Surface array, cached stream/surface pairs, optional string tag.
// Validate the entire array before changing any ID; preserve physical IDs and blobs.
inline CapturePatchResult patchCaptureRequests(const ParcelOps &ops, void *p,
        const CaptureDecoders &decoders, bool list, int32_t from, int32_t to,
        std::vector<MetadataRange> *logicalMetadata = nullptr) {
    CapturePatchResult result;
    if (logicalMetadata) logicalMetadata->clear();
    if (to < 0 || to == from) return result;
    const int32_t saved = ops.getPos(p);
    std::vector<int32_t> positions;
    const auto parse = [&]() -> int {
        if (afterInterfaceToken(ops, p, kCameraDeviceDescriptor) < 0) return -1;
        int32_t count = 1;
        if (list && ops.readInt(p, &count) != 0) return -2;
        if (count < 1 || count > 128) return -2;
        result.requests = count;
        for (int32_t request = 0; request < count; ++request) {
            int32_t present = 0, settings = 0;
            if (ops.readInt(p, &present) != 0 || present != 1 ||
                ops.readInt(p, &settings) != 0 || settings < 1 || settings > 32) return -3;
            for (int32_t setting = 0; setting < settings; ++setting) {
                int32_t id = -1, word = -1;
                if (skipString16(ops, p, &id, &word) != 0) return -4;
                if (setting == 0 && id == from) positions.push_back(word);
                const int32_t start = ops.getPos(p);
                if (decoders.metadata(decoders.context, p) != 0 ||
                    ops.getPos(p) <= start || ops.getPos(p) > ops.getSize(p)) return -5;
                if (logicalMetadata && setting == 0 && id == from)
                    logicalMetadata->push_back({start, ops.getPos(p)});
            }
            int32_t reprocess = 0, surfaces = 0, streams = 0;
            if (ops.readInt(p, &reprocess) != 0 || (reprocess != 0 && reprocess != 1) ||
                ops.readInt(p, &surfaces) != 0 || surfaces < -1 || surfaces > 64) return -6;
            for (int32_t surface = 0; surface < surfaces; ++surface) {
                const int32_t start = ops.getPos(p);
                if (decoders.surface(decoders.context, p) != 0 ||
                    ops.getPos(p) <= start || ops.getPos(p) > ops.getSize(p)) return -7;
            }
            if (ops.readInt(p, &streams) != 0 || streams < 0 ||
                streams > (ops.getSize(p) - ops.getPos(p)) / 8) return -8;
            for (int32_t stream = 0; stream < streams; ++stream) {
                int32_t index = -1;
                if (ops.readInt(p, &index) != 0 || index < 0 ||
                    ops.readInt(p, &index) != 0 || index < 0) return -8;
            }
            if (decoders.userTag) {
                int32_t tag = 0;
                if (ops.readInt(p, &tag) != 0 || (tag != 0 && tag != 1) ||
                    (tag == 1 && skipString16(ops, p) != 0)) return -9;
            }
        }
        int32_t streaming = 0;
        if (ops.readInt(p, &streaming) != 0 || (streaming != 0 && streaming != 1) ||
            ops.getPos(p) != ops.getSize(p)) return -10;
        return 0;
    };
    result.status = parse();
    if (result.status == 0) {
        for (auto word : positions) {
            if (ops.setPos(p, word) != 0 || ops.writeInt(p, to) != 0) {
                result.status = -11;
                // Roll back a failed write so a burst never mixes logical IDs.
                for (auto restore : positions) {
                    if (ops.setPos(p, restore) == 0) ops.writeInt(p, from);
                }
                result.patched = 0;
                break;
            }
            ++result.patched;
        }
    }
    if (result.status != 0 && logicalMetadata) logicalMetadata->clear();
    ops.setPos(p, saved);
    return result;
}

struct EdgePatchResult {
    bool changed = false;
    int oldMode = -1;
    int status = 0; // 1 = unsupported/FD blob, 2 = missing key, negative = malformed/write error
};

// CameraMetadata::writeToParcel: size, blob type, padded blob, alignment offset.
// The stable camera_metadata_t v1 header has 32-bit sizes/offsets and 16-byte
// entries. EDGE_MODE is a BYTE[1] value stored inside its typed entry, not a
// strength slider. Modify only that byte of an existing inline metadata blob.
// FD-backed blobs and unknown versions are deliberately left untouched.
inline EdgePatchResult patchEdgeMode(const ParcelOps &ops, void *p, MetadataRange range, int mode) {
    EdgePatchResult result;
    const int32_t saved = ops.getPos(p);
    const auto parse = [&]() -> int {
        if (mode < 0 || mode > 3 || range.start < 0 || range.end < range.start || range.end > ops.getSize(p) ||
            range.end - range.start < 12 || (range.start & 3) != 0 ||
            ops.setPos(p, range.start) != 0) return -1;
        int32_t blobSize = 0, blobType = -1;
        if (ops.readInt(p, &blobSize) != 0 || ops.readInt(p, &blobType) != 0) return -1;
        if (blobType != 0) return 1;
        if (blobSize < 56 || blobSize > 64 * 1024 || (blobSize & 3) != 0 ||
            static_cast<int64_t>(range.start) + 12 + blobSize != range.end) return -1;
        const int32_t blobStart = ops.getPos(p);
        std::vector<int32_t> words(blobSize / 4);
        for (auto &word : words) if (ops.readInt(p, &word) != 0) return -1;
        int32_t offset = -1;
        if (ops.readInt(p, &offset) != 0 || offset < 0 || offset >= 8) return -1;
        const auto *bytes = reinterpret_cast<const uint8_t *>(words.data());
        const auto u32 = [&](uint32_t relative) -> uint32_t {
            uint32_t value = 0;
            memcpy(&value, bytes + offset + relative, sizeof(value));
            return value;
        };
        if (blobSize - offset < 48) return -1;
        const uint32_t packetSize = u32(0), version = u32(4), count = u32(12), capacity = u32(16);
        const uint32_t entries = u32(20), dataCount = u32(24), dataCapacity = u32(28), data = u32(32);
        if (version != 1) return 1;
        if (packetSize < 48 || packetSize > static_cast<uint32_t>(blobSize - offset) ||
            count > capacity || entries < 48 || (entries & 3) != 0 ||
            static_cast<uint64_t>(entries) + static_cast<uint64_t>(capacity) * 16 > packetSize ||
            data < static_cast<uint64_t>(entries) + static_cast<uint64_t>(capacity) * 16 ||
            dataCount > dataCapacity || static_cast<uint64_t>(data) + dataCapacity > packetSize) return -1;
        int32_t valueByte = -1;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t entry = entries + i * 16;
            if (u32(entry) != 0x00030000) continue; // ANDROID_EDGE_MODE
            if (valueByte >= 0 || u32(entry + 4) != 1 || bytes[offset + entry + 12] != 0) return -1;
            valueByte = offset + entry + 8;
            result.oldMode = bytes[valueByte];
            if (result.oldMode > 3) return -1;
        }
        if (valueByte < 0) return 2;
        if (result.oldMode == mode) return 0;
        const uint32_t shift = (valueByte & 3) * 8;
        const uint32_t original = static_cast<uint32_t>(words[valueByte / 4]);
        const uint32_t replacement = (original & ~(uint32_t(0xff) << shift)) | (uint32_t(mode) << shift);
        if (ops.setPos(p, blobStart + (valueByte & ~3)) != 0 ||
            ops.writeInt(p, static_cast<int32_t>(replacement)) != 0) return -2;
        result.changed = true;
        return 0;
    };
    result.status = parse();
    ops.setPos(p, saved);
    return result;
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
