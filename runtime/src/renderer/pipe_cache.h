/* Pipeline cache files of the renderer (src/renderer/vk_renderer.cpp).
 * Header-only and Vulkan-free so tests/pipe_cache_test.cpp checks it without a device.
 *
 * pipeline-cache.bin: a 24-byte wrapper (magic "XBPC", version 1, payload size, FNV-1a 64 of the
 * payload, little-endian) then the driver's vkGetPipelineCacheData blob, which starts with
 * VkPipelineCacheHeaderVersionOne (headerSize, headerVersion 1, vendorID, deviceID,
 * pipelineCacheUUID[16]). pc_cache_check accepts it only for the same device and driver build; the
 * checksum keeps a torn or corrupt file from ever reaching the driver.
 *
 * pipeline-keys.bin: the geometry pipelines a run used, to build again ahead of time on the next
 * start. Magic "XBPK", version 1, words per record, record count, a build hash (the geometry
 * shaders: another build's records are dropped), the records, FNV-1a 64 of everything before it. */
#ifndef PIPE_CACHE_H
#define PIPE_CACHE_H
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall MoveFileExA(const char *from, const char *to, unsigned long flags);
#else
#include <unistd.h>
#endif

enum { PC_OK, PC_TRUNCATED, PC_BAD_MAGIC, PC_CHECKSUM, PC_BAD_HEADER, PC_DEVICE_MISMATCH };
const size_t PC_CACHE_HEAD = 24;      // the wrapper in front of the driver's blob
const uint32_t PC_KEYS_MAX = 4096;    // records kept in the key list (the newest)
const uint32_t PC_MAGIC_CACHE = 0x43504258u, PC_MAGIC_KEYS = 0x4b504258u;   // "XBPC", "XBPK"

static inline uint64_t pc_hash64(const void *data, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

static inline uint32_t pc_rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t pc_rd64(const uint8_t *p) { return pc_rd32(p) | (uint64_t)pc_rd32(p + 4) << 32; }
static inline void pc_put32(std::vector<uint8_t> &v, uint32_t x) {
    for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> 8 * i));
}
static inline void pc_put64(std::vector<uint8_t> &v, uint64_t x) {
    pc_put32(v, (uint32_t)x);
    pc_put32(v, (uint32_t)(x >> 32));
}

static inline const char *pc_cache_reason(int r) {
    switch (r) {
    case PC_OK: return "ok";
    case PC_TRUNCATED: return "truncated";
    case PC_BAD_MAGIC: return "not a pipeline cache file";
    case PC_CHECKSUM: return "corrupt (checksum)";
    case PC_BAD_HEADER: return "bad Vulkan cache header";
    case PC_DEVICE_MISMATCH: return "made by another GPU or driver";
    }
    return "?";
}

static inline void pc_cache_wrap(const uint8_t *blob, size_t n, std::vector<uint8_t> &out) {
    out.clear();
    out.reserve(PC_CACHE_HEAD + n);
    pc_put32(out, PC_MAGIC_CACHE);
    pc_put32(out, 1);
    pc_put64(out, n);
    pc_put64(out, pc_hash64(blob, n));
    if (n) out.insert(out.end(), blob, blob + n);
}

// The driver blob of a pipeline-cache.bin file (*off, *len) if the file is whole and was made
// for this device and driver (vendorID, deviceID, pipelineCacheUUID); else why not.
static inline int pc_cache_check(const uint8_t *file, size_t n, uint32_t vendor, uint32_t device, const uint8_t uuid[16], size_t *off,
                                 size_t *len) {
    *off = *len = 0;
    if (!file || n < PC_CACHE_HEAD) return PC_TRUNCATED;
    if (pc_rd32(file) != PC_MAGIC_CACHE || pc_rd32(file + 4) != 1) return PC_BAD_MAGIC;
    const uint64_t size = pc_rd64(file + 8);
    if (size != n - PC_CACHE_HEAD) return PC_TRUNCATED;
    const uint8_t *b = file + PC_CACHE_HEAD;
    if (pc_hash64(b, (size_t)size) != pc_rd64(file + 16)) return PC_CHECKSUM;
    if (size < 32) return PC_BAD_HEADER;
    const uint32_t head = pc_rd32(b);
    if (head < 32 || head > size || pc_rd32(b + 4) != 1) return PC_BAD_HEADER;   // VK_PIPELINE_CACHE_HEADER_VERSION_ONE
    if (pc_rd32(b + 8) != vendor || pc_rd32(b + 12) != device || memcmp(b + 16, uuid, 16)) return PC_DEVICE_MISMATCH;
    *off = PC_CACHE_HEAD;
    *len = (size_t)size;
    return PC_OK;
}

// recs: `stride` words per record; only the newest PC_KEYS_MAX records are written.
static inline void pc_keys_write(const std::vector<uint32_t> &recs, uint32_t stride, uint64_t build, std::vector<uint8_t> &out) {
    size_t count = stride ? recs.size() / stride : 0, first = count > PC_KEYS_MAX ? count - PC_KEYS_MAX : 0;
    count -= first;
    out.clear();
    out.reserve(24 + count * stride * 4 + 8);
    pc_put32(out, PC_MAGIC_KEYS);
    pc_put32(out, 1);
    pc_put32(out, stride);
    pc_put32(out, (uint32_t)count);
    pc_put64(out, build);
    for (size_t i = first * stride; i < (first + count) * stride; i++) pc_put32(out, recs[i]);
    pc_put64(out, pc_hash64(out.data(), out.size()));
}

// The records of a pipeline-keys.bin file (their count; 0 and none when the file is missing,
// damaged or made for another record layout or build).
static inline uint32_t pc_keys_read(const uint8_t *file, size_t n, uint32_t stride, uint64_t build, std::vector<uint32_t> &out) {
    out.clear();
    if (!file || n < 32 || !stride) return 0;
    if (pc_rd32(file) != PC_MAGIC_KEYS || pc_rd32(file + 4) != 1 || pc_rd32(file + 8) != stride || pc_rd64(file + 16) != build) return 0;
    const uint32_t count = pc_rd32(file + 12);
    if (count > PC_KEYS_MAX || n != 24 + (size_t)count * stride * 4 + 8) return 0;
    if (pc_hash64(file, n - 8) != pc_rd64(file + n - 8)) return 0;
    out.resize((size_t)count * stride);
    for (size_t i = 0; i < out.size(); i++) out[i] = pc_rd32(file + 24 + i * 4);
    return count;
}

static inline bool pc_read_file(const char *path, std::vector<uint8_t> &out) {
    out.clear();
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + got);
    const bool ok = !ferror(f);
    fclose(f);
    if (!ok) out.clear();
    return ok;
}

// Writes path.tmp, flushes it to disk and renames it over path: a crash leaves the old file or
// the new one, never half of one.
static inline bool pc_write_atomic(const char *path, const void *data, size_t n) {
    char tmp[1024];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return false;
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = (!n || fwrite(data, 1, n, f) == n) && fflush(f) == 0;
#ifndef _WIN32
    ok = ok && fsync(fileno(f)) == 0;
#endif
    ok = fclose(f) == 0 && ok;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp, path, 1 | 8);   // MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
#else
    ok = ok && rename(tmp, path) == 0;
#endif
    if (!ok) remove(tmp);
    return ok;
}
#endif
