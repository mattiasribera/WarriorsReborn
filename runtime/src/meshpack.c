/* --mesh-pack: see meshpack.h. */
#include "meshpack.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FNV_OFF 0xcbf29ce484222325ull
#define FNV_PRIME 0x100000001b3ull

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * FNV_PRIME;
    return h;
}

static void le32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

uint64_t meshpack_hash(const uint16_t *idx, uint32_t ni, uint32_t lo, uint32_t hi, const uint8_t *s0, uint32_t stride0,
                       const uint8_t *s1, uint32_t stride1) {
    uint8_t hdr[16] = {0};
    le32(hdr, ni);
    le32(hdr + 4, stride0);
    le32(hdr + 8, stride1);
    uint64_t h = fnv(FNV_OFF, hdr, sizeof hdr);
    h = fnv(h, (const uint8_t *)idx, (size_t)ni * 2);   /* little-endian host and guest */
    h = fnv(h, s0 + (size_t)lo * stride0, (size_t)(hi - lo + 1) * stride0);
    return fnv(h, s1 + (size_t)lo * stride1, (size_t)(hi - lo + 1) * stride1);
}

int meshpack_parse(const uint8_t *data, size_t len, uint64_t hash, MeshPackMesh *out) {
    if (len < 20 || memcmp(data, "WMP1", 4)) return -1;
    uint32_t nv = rd32(data + 4), ni = rd32(data + 8);
    if (rd32(data + 12) != MESHPACK_STRIDE0 || rd32(data + 16) != MESHPACK_STRIDE1) return -1;
    if (!nv || nv > MESHPACK_MAX_VERTS || !ni || ni % 3 || ni > 3u * 0x100000) return -1;
    size_t s0 = (size_t)nv * MESHPACK_STRIDE0, s1 = (size_t)nv * MESHPACK_STRIDE1, ib = (size_t)ni * 2;
    if (len != 20 + s0 + s1 + ib) return -1;
    uint8_t *copy = malloc(s0 + s1 + ib);
    if (!copy) return -1;
    memcpy(copy, data + 20, s0 + s1 + ib);
    /* The index block is 2-byte aligned: 20 + s0 (multiple of 32) + s1 (nv * 18) is even. */
    const uint16_t *ix = (const uint16_t *)(copy + s0 + s1);
    for (uint32_t i = 0; i < ni; i++)
        if (ix[i] >= nv) {
            free(copy);
            return -1;
        }
    *out = (MeshPackMesh){hash, nv, ni, copy, copy + s0, ix};
    return 0;
}

static struct {
    MeshPackMesh *m;
    int n;
    struct {
        uint64_t key, hash;
    } memo[256];
} mp;

static int parse_name(const char *s, uint64_t *h) {
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return -1;
        v = v << 4 | (uint64_t)d;
    }
    if (strcmp(s + 16, ".wmesh")) return -1;
    *h = v;
    return 0;
}

int meshpack_open(const char *dir) {
    meshpack_close();
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        uint64_t h;
        if (parse_name(e->d_name, &h) || meshpack_find(h)) continue;
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "rb");
        uint8_t *buf = NULL;
        long len = -1;
        if (f && !fseek(f, 0, SEEK_END) && (len = ftell(f)) > 0 && !fseek(f, 0, SEEK_SET) && (buf = malloc((size_t)len)) &&
            fread(buf, 1, (size_t)len, f) == (size_t)len) {
            MeshPackMesh m;
            MeshPackMesh *nm = realloc(mp.m, (mp.n + 1) * sizeof *nm);
            if (nm) mp.m = nm;
            if (nm && !meshpack_parse(buf, (size_t)len, h, &m)) mp.m[mp.n++] = m;
            else printf("[meshpack] skipped %s: not a valid mesh\n", path);
        }
        free(buf);
        if (f) fclose(f);
    }
    closedir(d);
    return mp.n;
}

int meshpack_count(void) { return mp.n; }

const MeshPackMesh *meshpack_find(uint64_t hash) {
    for (int i = 0; i < mp.n; i++)
        if (mp.m[i].hash == hash) return &mp.m[i];
    return NULL;
}

int meshpack_memo_get(uint64_t key, uint64_t *hash) {
    uint32_t s = (uint32_t)(key ^ key >> 29) & 255;
    for (int i = 0; i < 8; i++, s = (s + 1) & 255)
        if (mp.memo[s].key == key) {
            *hash = mp.memo[s].hash;
            return 1;
        }
    return 0;
}

void meshpack_memo_set(uint64_t key, uint64_t hash) {
    uint32_t first = (uint32_t)(key ^ key >> 29) & 255, s = first;
    int i;
    for (i = 0; i < 8; i++, s = (s + 1) & 255)
        if (!mp.memo[s].key || mp.memo[s].key == key) break;
    if (i == 8) s = first;   /* all probes taken: the first one is overwritten */
    mp.memo[s].key = key;
    mp.memo[s].hash = hash;
}

void meshpack_close(void) {
    for (int i = 0; i < mp.n; i++) free((void *)mp.m[i].s0);
    free(mp.m);
    memset(&mp, 0, sizeof mp);
}
