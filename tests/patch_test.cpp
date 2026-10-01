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
    printf("Passed %d parcel checks (protected Binder objects, offsets, front camera, disabled mode, diagnostics).\n", checks);
}
