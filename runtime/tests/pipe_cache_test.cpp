// Game-independent test of the pipeline cache files (src/renderer/pipe_cache.h): the cache-file
// wrapper and the Vulkan header check (vendor, device, pipelineCacheUUID), the key list, and the
// atomic write.
#include "../src/renderer/pipe_cache.h"
#include <cstdio>
#include <cstdlib>
#include <string>

static int fails;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (!(c)) { printf("FAILED %s:%d %s\n", __FILE__, __LINE__, #c); fails++; }      \
    } while (0)

static const uint32_t VENDOR = 0x10de, DEVICE = 0x1f08;
static const uint8_t UUID[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

// A driver blob: VkPipelineCacheHeaderVersionOne (32 bytes) then `extra` bytes of data.
static std::vector<uint8_t> driver_blob(uint32_t vendor, uint32_t device, const uint8_t *uuid, size_t extra) {
    std::vector<uint8_t> b(32 + extra);
    uint32_t w[4] = {32, 1, vendor, device};
    memcpy(b.data(), w, 16);
    memcpy(b.data() + 16, uuid, 16);
    for (size_t i = 0; i < extra; i++) b[32 + i] = (uint8_t)(i * 7 + 3);
    return b;
}

static int check(const std::vector<uint8_t> &file, size_t *off = nullptr, size_t *len = nullptr) {
    size_t o = 0, l = 0;
    int r = pc_cache_check(file.data(), file.size(), VENDOR, DEVICE, UUID, &o, &l);
    if (off) *off = o;
    if (len) *len = l;
    return r;
}

static void test_cache_file() {
    std::vector<uint8_t> blob = driver_blob(VENDOR, DEVICE, UUID, 200), file;
    pc_cache_wrap(blob.data(), blob.size(), file);
    CHECK(file.size() == PC_CACHE_HEAD + blob.size());
    size_t off = 0, len = 0;
    CHECK(check(file, &off, &len) == PC_OK);
    CHECK(off == PC_CACHE_HEAD && len == blob.size() && !memcmp(file.data() + off, blob.data(), len));

    CHECK(check({}) == PC_TRUNCATED);   // empty file
    std::vector<uint8_t> f = file;
    f.resize(10);
    CHECK(check(f) == PC_TRUNCATED);   // shorter than the wrapper
    f = file;
    f.pop_back();
    CHECK(check(f) == PC_TRUNCATED);   // payload cut short
    f = file;
    f[0] ^= 1;
    CHECK(check(f) == PC_BAD_MAGIC);
    f = file;
    f[4] = 9;   // wrapper version
    CHECK(check(f) == PC_BAD_MAGIC);
    f = file;
    f[PC_CACHE_HEAD + 100] ^= 0x40;   // one data bit
    CHECK(check(f) == PC_CHECKSUM);

    // Header fields, each rewrapped so only the field is wrong.
    uint8_t other[16];
    memcpy(other, UUID, 16);
    other[15] ^= 0x80;
    struct { std::vector<uint8_t> b; int want; } cases[] = {
        {driver_blob(0x1002, DEVICE, UUID, 64), PC_DEVICE_MISMATCH},   // vendor
        {driver_blob(VENDOR, 0x1f09, UUID, 64), PC_DEVICE_MISMATCH},   // device
        {driver_blob(VENDOR, DEVICE, other, 64), PC_DEVICE_MISMATCH},  // driver build (UUID)
        {driver_blob(VENDOR, DEVICE, UUID, 0), PC_OK},                 // header only
    };
    for (auto &c : cases) {
        pc_cache_wrap(c.b.data(), c.b.size(), f);
        CHECK(check(f) == c.want);
    }
    std::vector<uint8_t> b = driver_blob(VENDOR, DEVICE, UUID, 64);
    b[4] = 2;   // headerVersion
    pc_cache_wrap(b.data(), b.size(), f);
    CHECK(check(f) == PC_BAD_HEADER);
    b = driver_blob(VENDOR, DEVICE, UUID, 64);
    b[0] = 16;   // headerSize below the 32 bytes of version one
    pc_cache_wrap(b.data(), b.size(), f);
    CHECK(check(f) == PC_BAD_HEADER);
    b = driver_blob(VENDOR, DEVICE, UUID, 64);
    b[0] = 200;   // headerSize past the data
    pc_cache_wrap(b.data(), b.size(), f);
    CHECK(check(f) == PC_BAD_HEADER);
    b.resize(20);   // shorter than the Vulkan header
    pc_cache_wrap(b.data(), b.size(), f);
    CHECK(check(f) == PC_BAD_HEADER);
    CHECK(pc_cache_reason(PC_DEVICE_MISMATCH) && strlen(pc_cache_reason(PC_DEVICE_MISMATCH)) > 3);
}

static void test_keys() {
    const uint32_t stride = 5;
    const uint64_t build = 0x1234567890abcdefull;
    std::vector<uint32_t> recs;
    for (uint32_t i = 0; i < 3 * stride; i++) recs.push_back(i * 0x01010101u + 7);
    std::vector<uint8_t> file;
    pc_keys_write(recs, stride, build, file);
    std::vector<uint32_t> back;
    CHECK(pc_keys_read(file.data(), file.size(), stride, build, back) == 3 && back == recs);
    CHECK(pc_keys_read(file.data(), file.size(), stride + 1, build, back) == 0 && back.empty());   // record layout changed
    CHECK(pc_keys_read(file.data(), file.size(), stride, build + 1, back) == 0 && back.empty());   // other shaders
    std::vector<uint8_t> f = file;
    f[f.size() / 2] ^= 1;
    CHECK(pc_keys_read(f.data(), f.size(), stride, build, back) == 0 && back.empty());   // corrupt
    f = file;
    f.resize(f.size() - 3);
    CHECK(pc_keys_read(f.data(), f.size(), stride, build, back) == 0 && back.empty());   // truncated
    CHECK(pc_keys_read(nullptr, 0, stride, build, back) == 0);
    pc_keys_write({}, stride, build, file);
    CHECK(pc_keys_read(file.data(), file.size(), stride, build, back) == 0 && back.empty());
    // The cap keeps the newest records (the end of the list).
    std::vector<uint32_t> many;
    for (uint32_t i = 0; i < (PC_KEYS_MAX + 10) * 2; i++) many.push_back(i);
    pc_keys_write(many, 2, build, file);
    CHECK(pc_keys_read(file.data(), file.size(), 2, build, back) == PC_KEYS_MAX && back.size() == 2 * PC_KEYS_MAX && back[0] == 20);
}

static void test_files(const char *dir) {
    std::string path = std::string(dir) + "/pipe_cache_test.bin";
    remove(path.c_str());
    std::vector<uint8_t> data(1000, 0x5a), back;
    CHECK(!pc_read_file(path.c_str(), back) && back.empty());   // missing
    CHECK(pc_write_atomic(path.c_str(), data.data(), data.size()));
    CHECK(pc_read_file(path.c_str(), back) && back == data);
    data.assign(10, 0x33);   // replaces a larger file
    CHECK(pc_write_atomic(path.c_str(), data.data(), data.size()));
    CHECK(pc_read_file(path.c_str(), back) && back == data);
    FILE *t = fopen((path + ".tmp").c_str(), "rb");
    CHECK(!t);   // no temporary left behind
    if (t) fclose(t);
    remove(path.c_str());
    CHECK(!pc_write_atomic((std::string(dir) + "/no/such/dir/x.bin").c_str(), data.data(), data.size()));
}

int main(int argc, char **argv) {
    CHECK(pc_hash64("", 0) == 0xcbf29ce484222325ull);   // FNV-1a offset basis
    CHECK(pc_hash64("a", 1) == 0xaf63dc4c8601ec8cull);
    test_cache_file();
    test_keys();
    test_files(argc > 1 ? argv[1] : ".");
    if (fails) printf("%d FAILED\n", fails);
    else printf("ALL OK\n");
    return fails ? 1 : 0;
}
