#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../src/patch.h"

// Model Android's object table: primitive reads/writes overlapping a Binder fail.
// skipBinder consumes the actual registered object size, including stability.
struct Parcel {
    std::vector<int32_t> words;
    int32_t pos = 0;
    int32_t binderAt = -1;
    int32_t binderSize = 0;
    int binderStatus = 0;
    bool failWrite = false;
    int binderReads = 0;
};
static int32_t position(void *p) { return static_cast<Parcel *>(p)->pos; }
static int32_t size(void *p) { return static_cast<int32_t>(static_cast<Parcel *>(p)->words.size() * 4); }
static int setPosition(void *p, int32_t n) {
    if (n < 0 || n > size(p)) return -1;
    static_cast<Parcel *>(p)->pos = n;
    return 0;
}
static bool overlapsBinder(const Parcel &p) {
    return p.binderAt >= 0 && p.pos < p.binderAt + p.binderSize && p.pos + 4 > p.binderAt;
}
static int readInt(void *p, int32_t *out) {
    auto &parcel = *static_cast<Parcel *>(p);
    if (parcel.pos + 4 > size(p)) return -61;
    if (overlapsBinder(parcel)) { parcel.pos += 4; return -1; }
    *out = parcel.words[parcel.pos / 4];
    parcel.pos += 4;
    return 0;
}
static int writeInt(void *p, int32_t value) {
    auto &parcel = *static_cast<Parcel *>(p);
    if (parcel.failWrite || overlapsBinder(parcel) || parcel.pos + 4 > size(p)) return -1;
    parcel.words[parcel.pos / 4] = value;
    parcel.pos += 4;
    return 0;
}
static int skipBinder(void *p) {
    auto &parcel = *static_cast<Parcel *>(p);
    parcel.binderReads++;
    if (parcel.binderStatus) return parcel.binderStatus;
    if (parcel.pos != parcel.binderAt || parcel.pos + parcel.binderSize > size(p)) return -1;
    parcel.pos += parcel.binderSize;
    return 0;
}
static const ParcelOps ops{position, size, setPosition, readInt, writeInt, skipBinder};

static Parcel token() {
    Parcel p;
    p.words = {0, -1, kParcelHeader, kDescriptorLen};
    auto start = p.words.size();
    p.words.resize(start + sizeof(kCameraServiceDescriptor) / 4);
    memcpy(p.words.data() + start, kCameraServiceDescriptor, sizeof(kCameraServiceDescriptor));
    return p;
}
static Parcel call(int32_t id, int binderBytes = 0) {
    Parcel p = token();
    if (binderBytes) {
        p.binderAt = size(&p);
        p.binderSize = binderBytes;
        p.words.resize(p.words.size() + binderBytes / 4, 0x73622a85);
    }
    p.words.insert(p.words.end(), {1, id, 35, 0, 1, '0'}); // misleading later argument
    p.pos = size(&p); // outgoing Java Parcel commonly points to its end
    return p;
}
static void unchanged(Parcel p, CameraIdLayout layout, int to = '2') {
    const auto before = p.words;
    const auto saved = p.pos;
    assert(patchCameraId(ops, &p, layout, '0', to).layout == 0);
    assert(p.words == before && p.pos == saved);
}
static Parcel deviceToken() {
    Parcel p;
    constexpr auto bytes = sizeof(kCameraDeviceDescriptor);
    p.words = {0, -1, kParcelHeader, (int32_t) (bytes / 2 - 1)};
    auto start = p.words.size();
    p.words.resize(start + (bytes + 3) / 4, 0);
    memcpy(p.words.data() + start, kCameraDeviceDescriptor, bytes);
    return p;
}
static int decodeBlob(void *, void *p) {
    int32_t count = 0, word = 0;
    if (readInt(p, &count) != 0 || count < 0 || count > 100) return -1;
    for (int i = 0; i < count; ++i) if (readInt(p, &word) != 0) return -1;
    return 0;
}
static const CaptureDecoders decoders{nullptr, decodeBlob, decodeBlob, true};
static void appendRequest(Parcel &p, int id = '0', bool tag = false, bool physical = false, bool surface = false) {
    p.words.insert(p.words.end(), {1, physical ? 2 : 1, 1, id, 4, 1, '0', 19, 20});
    if (physical) p.words.insert(p.words.end(), {1, '0', 2, 1, '0'}); // physical ID stays 0
    p.words.insert(p.words.end(), {0, surface ? 1 : 0});
    if (surface) p.words.insert(p.words.end(), {2, 1, '0'});
    p.words.insert(p.words.end(), {1, 0, 0, tag ? 1 : 0});
    if (tag) p.words.insert(p.words.end(), {1, '0'}); // tag stays 0
}
static Parcel capture(bool list, int count = 1, bool tag = false, bool physical = false, bool surface = false,
                      std::vector<size_t> *ids = nullptr) {
    auto p = deviceToken();
    if (list) p.words.push_back(count);
    for (int i = 0; i < count; ++i) {
        if (ids) ids->push_back(p.words.size() + 3);
        appendRequest(p, '0', tag, physical, surface);
    }
    p.words.push_back(1); // repeating
    p.pos = size(&p);
    return p;
}
static Parcel edgeMetadata(int mode = 1, int offset = 0) {
    // 48-byte v1 header + two typed entries + 8 bytes of alignment padding.
    std::vector<uint8_t> blob(88, 0);
    const auto put = [&](int at, uint32_t value) { memcpy(blob.data() + offset + at, &value, 4); };
    put(0, 80); put(4, 1); put(12, 2); put(16, 2); put(20, 48); put(32, 80);
    put(40, 0xffffffff); put(44, 0xffffffff); // vendor ID unchanged
    put(48, 0x30000); put(52, 1); put(56, uint32_t(mode) | 0x98765400); // inline BYTE[1]
    put(64, 0x10000); put(68, 1); put(72, 0x11223301); // another typed value remains identical
    Parcel p; p.words = {0xabcdef, 88, 0};
    const auto begin = p.words.size();
    p.words.resize(begin + blob.size() / 4);
    memcpy(p.words.data() + begin, blob.data(), blob.size());
    p.words.push_back(offset);
    p.words.insert(p.words.end(), {1, '0', 99}); // following arguments unchanged
    p.pos = size(&p);
    return p;
}
int main() {
    int checks = 0;
    {
        auto p = call('0');
        auto before = p.words;
        auto r = patchCameraId(ops, &p, CameraIdLayout::FirstString, '0', '2');
        assert(r.layout == 1 && r.idOffset == 0 && r.status == 0);
        before[21] = '2';
        assert(p.words == before && p.pos == size(&p));
        checks++;
    }
    for (int bytes : {16, 20, 24, 28}) {
        auto p = call('0', bytes);
        auto before = p.words;
        int32_t primitive = 0;
        p.pos = p.binderAt;
        assert(readInt(&p, &primitive) != 0); // reproduce the old scanner's failure
        p.pos = size(&p);
        auto r = patchCameraId(ops, &p, CameraIdLayout::BinderThenString, '0', '2');
        assert(r.layout == bytes / 4 + 1 && r.idOffset == bytes && r.status == 0);
        before[(80 + bytes) / 4 + 1] = '2';
        assert(p.words == before && p.pos == size(&p) && p.binderReads == 1);
        checks++;
    }
    unchanged(call('1', 28), CameraIdLayout::BinderThenString); checks++; // front stays front
    unchanged(call('0', 28), CameraIdLayout::BinderThenString, -1); checks++; // off
    unchanged(call('0', 28), CameraIdLayout::BinderThenString, '0'); checks++;
    unchanged(call('0'), CameraIdLayout::None); checks++; // unknown transaction
    {
        auto p = call('0');
        p.words[20] = 2; // multi-character ID is not an in-place one-character replacement
        unchanged(p, CameraIdLayout::FirstString); checks++;
    }
    {
        auto p = call('0', 28); p.binderStatus = -22;
        auto before = p.words;
        auto r = patchCameraId(ops, &p, CameraIdLayout::BinderThenString, '0', '2');
        assert(r.layout == 0 && r.status == -22 && p.words == before && p.pos == size(&p));
        checks++;
    }
    {
        auto p = call('0'); p.words[4] ^= 1;
        unchanged(p, CameraIdLayout::FirstString); checks++;
        p = token(); p.words.push_back(1); p.pos = size(&p);
        unchanged(p, CameraIdLayout::FirstString); checks++;
    }
    {
        auto p = call('0'); p.failWrite = true;
        unchanged(p, CameraIdLayout::FirstString); checks++;
    }
    {
        auto p = call('0', 28); auto before = p.words;
        int32_t words[24] = {}, offset = -1; int status = -1;
        assert(dumpCameraCall(ops, &p, CameraIdLayout::BinderThenString, words, 24, &offset, &status) == 6);
        assert(offset == 28 && status == 0 && words[0] == 1 && words[1] == '0');
        assert(p.words == before && p.pos == size(&p)); checks++;
        assert(dumpCameraCall(ops, &p, CameraIdLayout::None, words, 24, &offset, &status) == 0);
        assert(status != 0 && p.words == before && p.pos == size(&p)); checks++;
    }
    {
        Parcel p;
        constexpr auto bytes = sizeof(kCameraDeviceDescriptor);
        p.words = {0, -1, kParcelHeader, (int32_t) (bytes / 2 - 1)};
        auto start = p.words.size();
        p.words.resize(start + (bytes + 3) / 4, 0);
        memcpy(p.words.data() + start, kCameraDeviceDescriptor, bytes);
        const auto tokenEnd = size(&p);
        p.words.push_back(0);
        auto before = p.words;
        assert(afterInterfaceToken(ops, &p, kCameraDeviceDescriptor) == tokenEnd);
        assert(p.words == before); checks++;
        assert(afterCameraServiceToken(ops, &p) == -1); checks++;
    }
    for (bool list : {false, true}) for (bool tag : {false, true}) {
        std::vector<size_t> ids;
        auto p = capture(list, list ? 3 : 1, tag, true, true, &ids);
        auto expected = p.words;
        for (auto id : ids) expected[id] = '3';
        const auto r = patchCaptureRequests(ops, &p, decoders, list, '0', '3');
        assert(r.status == 0 && r.requests == (list ? 3 : 1) && r.patched == r.requests);
        assert(p.words == expected && p.pos == size(&p)); checks++;
    }
    {
        auto p = deviceToken(); p.words.push_back(2);
        appendRequest(p, '1'); appendRequest(p, '0'); p.words.push_back(0); p.pos = size(&p);
        const auto r = patchCaptureRequests(ops, &p, decoders, true, '0', '3');
        assert(r.status == 0 && r.patched == 1); checks++; // front request and metadata preserved
    }
    {
        auto p = capture(true, 2); auto before = p.words;
        auto r = patchCaptureRequests(ops, &p, decoders, true, '0', -1);
        assert(r.patched == 0 && p.words == before && p.pos == size(&p)); checks++;
        p.failWrite = true;
        r = patchCaptureRequests(ops, &p, decoders, true, '0', '3');
        assert(r.status == -11 && r.patched == 0 && p.words == before && p.pos == size(&p)); checks++;
    }
    {
        const auto complete = capture(true, 2, true, true, true);
        // Every possible truncation, including after a valid first request, leaves all bytes untouched.
        for (size_t length = 0; length < complete.words.size(); ++length) {
            auto p = complete; p.words.resize(length); p.pos = size(&p); auto before = p.words;
            const auto r = patchCaptureRequests(ops, &p, decoders, true, '0', '3');
            assert(r.status != 0 && r.patched == 0 && p.words == before && p.pos == size(&p));
        }
        checks++;
    }
    {
        auto p = capture(false); p.words.erase(p.words.end() - 2); p.pos = size(&p);
        auto old = decoders; old.userTag = false;
        assert(patchCaptureRequests(ops, &p, old, false, '0', '3').patched == 1); checks++;
    }
    {
        auto p = capture(true, 2); p.words[4] ^= 1; auto before = p.words;
        assert(patchCaptureRequests(ops, &p, decoders, true, '0', '3').status != 0);
        assert(p.words == before && p.pos == size(&p)); checks++;
    }
    for (int offset = 0; offset < 8; ++offset) {
        auto p = edgeMetadata(1, offset); auto expected = p.words;
        const int byteAt = 12 + offset + 56;
        auto *bytes = reinterpret_cast<uint8_t *>(expected.data()); bytes[byteAt] = 2;
        const auto r = patchEdgeMode(ops, &p, {4, 104}, 2);
        assert(r.changed && r.oldMode == 1 && r.status == 0 && p.words == expected && p.pos == size(&p));
        checks++;
    }
    {
        auto p = edgeMetadata(2); auto before = p.words;
        auto r = patchEdgeMode(ops, &p, {4, 104}, 2);
        assert(!r.changed && r.oldMode == 2 && r.status == 0 && p.words == before); checks++;
        p.failWrite = true; r = patchEdgeMode(ops, &p, {4, 104}, 1);
        assert(!r.changed && r.status == -2 && p.words == before && p.pos == size(&p)); checks++;
    }
    {
        auto p = edgeMetadata(); p.words[2] = 1; p.binderAt = 12; p.binderSize = 28;
        auto before = p.words;
        const auto r = patchEdgeMode(ops, &p, {4, 104}, 2);
        assert(!r.changed && r.status == 1 && p.words == before && p.pos == size(&p)); checks++;
    }
    // Every truncation, unknown metadata version, wrong type/count, missing or duplicate key is safe.
    {
        const auto original = edgeMetadata();
        for (int end = 4; end < 104; end += 4) {
            auto p = original; auto before = p.words;
            assert(!patchEdgeMode(ops, &p, {4, end}, 2).changed);
            assert(p.words == before && p.pos == size(&p));
        }
        checks++;
        for (auto field : std::vector<std::pair<int, int32_t>>{
                {3 + 1, 2}, {3 + 3, 10000}, {3 + 5, 10000},
                {3 + 12, 0x30001}, {3 + 13, 2}, {3 + 15, 1}, {3 + 16, 0x30000}}) {
            auto p = original; p.words[field.first] = field.second; auto before = p.words;
            assert(!patchEdgeMode(ops, &p, {4, 104}, 2).changed);
            assert(p.words == before && p.pos == size(&p)); checks++;
        }
    }
    {
        auto p = capture(true, 2, true, true);
        std::vector<MetadataRange> ranges;
        assert(patchCaptureRequests(ops, &p, decoders, true, '0', '3', &ranges).status == 0);
        assert(ranges.size() == 2); // physical settings excluded
        for (auto range : ranges) assert(range.end - range.start == 20);
        checks++;
        p.words.pop_back(); p.pos = size(&p);
        assert(patchCaptureRequests(ops, &p, decoders, true, '0', '3', &ranges).status != 0);
        assert(ranges.empty()); checks++;
    }
    printf("Passed %d parcel checks (Binder objects, capture arrays, bounded edge metadata, truncation, front camera).\n", checks);
}
