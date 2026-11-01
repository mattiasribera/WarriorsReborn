/* Game-independent test of the rate probe (src/rateprobe.c): option parsing, the game-time
 * snapshot schedule, and the snapshot file (header, full copies, changed-dword lists, a range
 * behind a pointer that moves, unreadable samples), decoded back here. No game data. */
#include "../src/rateprobe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static void test_parse(void) {
    char path[64];
    uint32_t iv = 0;
    CHECK(rateprobe_parse_spec("out.rp", path, sizeof path, &iv) == 0 && !strcmp(path, "out.rp") &&
          iv == RATEPROBE_DEFAULT_INTERVAL_MS, "spec: plain file, default interval (%s, %u)", path, iv);
    CHECK(rateprobe_parse_spec("out.rp:250", path, sizeof path, &iv) == 0 && !strcmp(path, "out.rp") && iv == 250,
          "spec: file:250 (%s, %u)", path, iv);
    CHECK(rateprobe_parse_spec("C:\\tmp\\a.rp", path, sizeof path, &iv) == 0 && !strcmp(path, "C:\\tmp\\a.rp") &&
          iv == RATEPROBE_DEFAULT_INTERVAL_MS, "spec: a Windows path stays a path (%s)", path);
    CHECK(rateprobe_parse_spec("C:\\a.rp:50", path, sizeof path, &iv) == 0 && !strcmp(path, "C:\\a.rp") && iv == 50,
          "spec: Windows path with interval (%s, %u)", path, iv);
    CHECK(rateprobe_parse_spec("out.rp:0", path, sizeof path, &iv) < 0, "spec: interval 0 rejected");
    CHECK(rateprobe_parse_spec("out.rp:60001", path, sizeof path, &iv) < 0, "spec: interval above 60 s rejected");
    CHECK(rateprobe_parse_spec("", path, sizeof path, &iv) < 0, "spec: empty rejected");
    CHECK(rateprobe_parse_spec(":100", path, sizeof path, &iv) < 0, "spec: no file rejected");

    RateProbeRange r;
    CHECK(rateprobe_parse_range("0x44c080:0x100", &r) == 0 && r.addr == 0x44c080 && r.size == 0x100 && !r.deref && !r.off,
          "range: ADDR:SIZE");
    CHECK(rateprobe_parse_range("@0x5a7570+0x40:512", &r) == 0 && r.addr == 0x5a7570 && r.size == 512 && r.deref &&
          r.off == 0x40, "range: @PTR+OFF:SIZE");
    CHECK(rateprobe_parse_range("@0x5a7570:0x10", &r) == 0 && r.deref && r.off == 0, "range: @PTR:SIZE");
    CHECK(rateprobe_parse_range("0x44c081:0x100", &r) < 0, "range: unaligned address rejected");
    CHECK(rateprobe_parse_range("0x44c080:6", &r) < 0, "range: unaligned size rejected");
    CHECK(rateprobe_parse_range("0x44c080:0", &r) < 0, "range: empty rejected");
    CHECK(rateprobe_parse_range("0xfffffff0:0x20", &r) < 0, "range: wrapping rejected");
    CHECK(rateprobe_parse_range("zz:4", &r) < 0 && rateprobe_parse_range("0x10", &r) < 0 &&
          rateprobe_parse_range("0x10:4x", &r) < 0, "range: garbage rejected");

    RateProbeOptions o;
    memset(&o, 0, sizeof o);
    char *argv[] = {"xbrun", "--rate-probe", "a.rp:200", "--rate-probe-range", "0x1000:16", "--other",
                    "--rate-probe-range", "bad", "--rate-probe"};
    int i = 1;
    CHECK(rateprobe_parse_arg(9, argv, &i, &o) == 1 && i == 2 && !strcmp(o.path, "a.rp") && o.interval_ms == 200,
          "arg: --rate-probe FILE:MS consumed (i=%d)", i);
    i = 3;
    CHECK(rateprobe_parse_arg(9, argv, &i, &o) == 1 && i == 4 && o.nranges == 1 && o.ranges[0].size == 16,
          "arg: --rate-probe-range added (%d ranges)", o.nranges);
    i = 5;
    CHECK(rateprobe_parse_arg(9, argv, &i, &o) == 0 && i == 5, "arg: other option not consumed");
    i = 6;
    CHECK(rateprobe_parse_arg(9, argv, &i, &o) < 0, "arg: malformed range is an error");
    i = 8;
    CHECK(rateprobe_parse_arg(9, argv, &i, &o) < 0, "arg: missing value is an error");
}

static void test_clock(void) {
    RateProbeClock c = {100, 0, 0, 0};
    static const uint32_t ms[] = {1234, 1250, 1299, 1300, 1301, 1399, 1650, 1700, 400, 450, 500};
    static const int due[] = {1, 0, 0, 1, 0, 0, 1, 1, 1, 0, 1};
    int ok = 1;
    for (int k = 0; k < 11; k++) {
        int d = rateprobe_due(&c, ms[k]);
        if (d != due[k]) { ok = 0; printf("     at %u ms: due %d, expected %d\n", ms[k], d, due[k]); }
    }
    CHECK(ok, "clock: first reading, boundaries, a skipped gap and a reset");
}

/* Fake guest: a "data" range at 0x1000 and a pointer at 0x2000 to one of two objects. */
static uint8_t data[64], objA[32], objB[32];
static uint32_t ptr_val;
static int data_readable = 1;
static const void *rd(uint32_t addr, uint32_t len, void *ctx) {
    (void)ctx;
    if (addr >= 0x1000 && addr + len <= 0x1000 + sizeof data) return data_readable ? data + (addr - 0x1000) : NULL;
    if (addr == 0x2000 && len == 4) return &ptr_val;
    if (addr >= 0x3000 && addr + len <= 0x3000 + sizeof objA) return objA + (addr - 0x3000);
    if (addr >= 0x4000 && addr + len <= 0x4000 + sizeof objB) return objB + (addr - 0x4000);
    return NULL;
}

static uint32_t get32(FILE *f) {
    uint32_t v = 0;
    if (fread(&v, 4, 1, f) != 1) return 0xdeadbeef;
    return v;
}

typedef struct { uint32_t base, kind, count; uint8_t bytes[64]; } RangeRec;
static int read_snap(FILE *f, uint32_t *frame, uint32_t *ms, RangeRec *rr, int n, const uint32_t *sizes) {
    if (get32(f) != RATEPROBE_SNAP) return -1;
    *frame = get32(f);
    *ms = get32(f);
    if (get32(f) != (uint32_t)n) return -1;
    for (int k = 0; k < n; k++) {
        rr[k].base = get32(f);
        rr[k].kind = get32(f);
        rr[k].count = get32(f);
        if (rr[k].kind == RATEPROBE_FULL) {
            if (rr[k].count != sizes[k] || fread(rr[k].bytes, 1, sizes[k], f) != sizes[k]) return -1;
        } else if (rr[k].kind == RATEPROBE_DELTA) {
            for (uint32_t j = 0; j < rr[k].count; j++) {
                uint32_t off = get32(f), v = get32(f);
                if (off + 4 > sizes[k]) return -1;
                memcpy(rr[k].bytes + off, &v, 4);
            }
        } else if (rr[k].kind != RATEPROBE_NONE) return -1;
    }
    return 0;
}

static void test_file(void) {
    FILE *f = tmpfile();
    if (!f) { CHECK(0, "file: tmpfile"); return; }
    RateProbeRange r[2] = {{0x1000, 64, 0, 0}, {0x2000, 16, 1, 8}};
    for (int k = 0; k < 64; k++) data[k] = (uint8_t)k;
    for (int k = 0; k < 32; k++) { objA[k] = (uint8_t)(0xa0 + k); objB[k] = (uint8_t)(0xb0 + k); }
    ptr_val = 0x3000;
    RateProbe *p = rateprobe_open(f, 100, RATEPROBE_FLAG_SIM60, r, 2);
    CHECK(p != NULL, "file: open");
    if (!p) return;
    CHECK(rateprobe_frame(p, 1, 1000, rd, NULL) == 1, "file: first frame writes a snapshot");
    CHECK(rateprobe_frame(p, 2, 1050, rd, NULL) == 0, "file: not due, nothing written");
    data[8] = 0x55;                 /* dword 8 changes */
    data[40] = 0x66; data[43] = 0x77;   /* dword 40 changes */
    CHECK(rateprobe_frame(p, 3, 1100, rd, NULL) == 1, "file: due again");
    ptr_val = 0x4000;               /* the object moved */
    CHECK(rateprobe_frame(p, 4, 1200, rd, NULL) == 1, "file: pointer moved");
    data_readable = 0;
    CHECK(rateprobe_frame(p, 5, 1300, rd, NULL) == 1, "file: unreadable sample");
    data_readable = 1;
    for (int k = 0; k < 64; k++) data[k] ^= 0xff;   /* everything changed: a full copy is smaller */
    CHECK(rateprobe_frame(p, 6, 1400, rd, NULL) == 1, "file: after unreadable");
    data[0] ^= 1;
    CHECK(rateprobe_frame(p, 7, 1500, rd, NULL) == 1, "file: one dword changed");
    uint32_t snaps = 0;
    uint64_t bytes = 0;
    rateprobe_counts(p, &snaps, &bytes);
    CHECK(snaps == 6 && bytes > 0, "file: counts %u snapshots, %llu bytes", snaps, (unsigned long long)bytes);
    /* Decode (rateprobe_close would close f: flush through the frame calls instead). */
    fflush(f);
    rewind(f);
    int ok = get32(f) == RATEPROBE_MAGIC && get32(f) == RATEPROBE_VERSION && get32(f) == 100 &&
             get32(f) == RATEPROBE_FLAG_SIM60 && get32(f) == 2;
    for (int k = 0; k < 2 && ok; k++)
        ok = get32(f) == r[k].addr && get32(f) == r[k].size && get32(f) == r[k].deref && get32(f) == r[k].off;
    CHECK(ok, "file: header");
    const uint32_t sizes[2] = {64, 16};
    RangeRec rr[2];
    memset(rr, 0, sizeof rr);
    uint32_t fr, ms;
    uint8_t expect[64];
    for (int k = 0; k < 64; k++) expect[k] = (uint8_t)k;
    ok = !read_snap(f, &fr, &ms, rr, 2, sizes) && fr == 1 && ms == 1000 && rr[0].kind == RATEPROBE_FULL &&
         rr[0].base == 0x1000 && !memcmp(rr[0].bytes, expect, 64) && rr[1].kind == RATEPROBE_FULL &&
         rr[1].base == 0x3008 && !memcmp(rr[1].bytes, objA + 8, 16);
    CHECK(ok, "file: snapshot 1 full copies (pointer range at 0x3008)");
    expect[8] = 0x55; expect[40] = 0x66; expect[43] = 0x77;
    ok = !read_snap(f, &fr, &ms, rr, 2, sizes) && fr == 3 && ms == 1100 && rr[0].kind == RATEPROBE_DELTA &&
         rr[0].count == 2 && !memcmp(rr[0].bytes, expect, 64) && rr[1].kind == RATEPROBE_DELTA && rr[1].count == 0;
    CHECK(ok, "file: snapshot 2 lists the 2 changed dwords (%u) and none for the object", rr[0].count);
    ok = !read_snap(f, &fr, &ms, rr, 2, sizes) && fr == 4 && rr[0].kind == RATEPROBE_DELTA && rr[0].count == 0 &&
         rr[1].kind == RATEPROBE_FULL && rr[1].base == 0x4008 && !memcmp(rr[1].bytes, objB + 8, 16);
    CHECK(ok, "file: snapshot 3 copies the moved object in full");
    ok = !read_snap(f, &fr, &ms, rr, 2, sizes) && fr == 5 && rr[0].kind == RATEPROBE_NONE && rr[0].base == 0 &&
         rr[1].kind == RATEPROBE_DELTA;
    CHECK(ok, "file: snapshot 4 marks the unreadable range");
    for (int k = 0; k < 64; k++) expect[k] ^= 0xff;
    ok = !read_snap(f, &fr, &ms, rr, 2, sizes) && fr == 6 && rr[0].kind == RATEPROBE_FULL && !memcmp(rr[0].bytes, expect, 64);
    CHECK(ok, "file: snapshot 5 is a full copy after the unreadable one");
    expect[0] ^= 1;
    ok = !read_snap(f, &fr, &ms, rr, 2, sizes) && fr == 7 && rr[0].kind == RATEPROBE_DELTA && rr[0].count == 1 &&
         !memcmp(rr[0].bytes, expect, 64);
    CHECK(ok, "file: snapshot 6 lists one dword");
    CHECK(fgetc(f) == EOF, "file: nothing after the last snapshot");
    rateprobe_close(p);
    CHECK(rateprobe_open(NULL, 100, 0, r, 2) == NULL, "open: no file rejected");
}

static void test_poke(void) {
    RateProbePoke k;
    CHECK(rateprobe_parse_poke("0x44c078:6c6576656c393900", &k) == 0 && k.addr == 0x44c078 && k.len == 8 &&
          !memcmp(k.bytes, "level99", 8) && !k.has_expect, "poke: ADDR:HEX");
    CHECK(rateprobe_parse_poke("0x27fd99:b878c04400/e8724f0a00", &k) == 0 && k.len == 5 && k.has_expect &&
          k.bytes[0] == 0xb8 && k.expect[0] == 0xe8 && k.expect[4] == 0x00, "poke: ADDR:HEX/EXPECT");
    CHECK(rateprobe_parse_poke("0x10:abc", &k) < 0, "poke: odd hex digits rejected");
    CHECK(rateprobe_parse_poke("0x10:zz", &k) < 0, "poke: non-hex rejected");
    CHECK(rateprobe_parse_poke("0x10:", &k) < 0 && rateprobe_parse_poke("0x10", &k) < 0, "poke: empty rejected");
    CHECK(rateprobe_parse_poke("0x10:aabb/cc", &k) < 0, "poke: expect of another length rejected");
    char big[2 * RATEPROBE_POKE_MAX + 8] = "0x10:";
    for (int j = 0; j <= RATEPROBE_POKE_MAX; j++) strcat(big, "00");
    CHECK(rateprobe_parse_poke(big, &k) < 0, "poke: longer than %d bytes rejected", RATEPROBE_POKE_MAX);
    CHECK(rateprobe_parse_poke("0xffffffff:0000", &k) < 0, "poke: wrapping rejected");

    uint8_t mem[16] = {1, 2, 3, 4, 5, 6, 7, 8};
    rateprobe_parse_poke("0x2:aabb/0304", &k);
    CHECK(rateprobe_apply_poke(&k, mem + 2) == 0 && mem[2] == 0xaa && mem[3] == 0xbb && mem[4] == 5, "poke: applied");
    CHECK(rateprobe_apply_poke(&k, mem + 2) < 0 && mem[2] == 0xaa, "poke: mismatching original left alone");

    RateProbeOptions o;
    memset(&o, 0, sizeof o);
    char *argv[] = {"xbrun", "--poke", "0x100:00ff", "--poke", "bad", "--poke"};
    int i = 1;
    CHECK(rateprobe_parse_arg(6, argv, &i, &o) == 1 && i == 2 && o.npokes == 1 && o.pokes[0].len == 2,
          "arg: --poke added");
    i = 3;
    CHECK(rateprobe_parse_arg(6, argv, &i, &o) < 0, "arg: malformed poke is an error");
    i = 5;
    CHECK(rateprobe_parse_arg(6, argv, &i, &o) < 0, "arg: poke without value is an error");
}

int main(void) {
    test_parse();
    test_clock();
    test_file();
    test_poke();
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("ALL OK\n");
    return 0;
}
