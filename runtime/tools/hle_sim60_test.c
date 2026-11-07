/* Game-independent --sim60 patch test: a host buffer stands in for guest memory. Checks the
 * option parsing, that the patch set is written only when enabled and only as a whole (one
 * mismatching or malformed entry writes nothing and is reported), that an already patched image
 * is left alone, and that every entry of the title's table applies to a planted copy of its own
 * original bytes, with the key sites (step pairs, world gate, time step) changed as intended. */
#include "../src/hle_sim60.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static float rdf(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }

/* Target of a jmp rel32 (e9) at va in the image, 0 when there is none. */
static uint32_t jmp_target(const uint8_t *img, uint32_t va) {
    if (img[va] != 0xe9) return 0;
    return va + 5 + rd32(img + va + 1);
}
static uint32_t call_target(const uint8_t *img, uint32_t va) {
    if (img[va] != 0xe8) return 0;
    return va + 5 + rd32(img + va + 1);
}
/* test byte [0x5825fc],1 at va (the world step counter's low bit). */
static int gates_parity(const uint8_t *img, uint32_t va) {
    return !memcmp(img + va, (const uint8_t[]){0xf6, 0x05, 0xfc, 0x25, 0x58, 0x00, 0x01}, 7);
}

enum { BASE = 0x00100000u, SIZE = 0x1000 };
static uint8_t mem[SIZE], before[SIZE];

/* Guest image stand-in for the title table: VAs up to the end of the image's last page. */
enum { IMG_SIZE = 0x005d5000 };
static uint8_t img[IMG_SIZE];

static const Sim60Patch fakes[] = {
    /* jne rel32 -> 6-byte nop */
    {"gate", BASE + 0x10, 6, {0x0f, 0x85, 0x03, 0x08, 0x00, 0x00}, {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00}},
    /* mov dword [esp+0x14], 2 -> 1 */
    {"pairs", BASE + 0x40, 8, {0xc7, 0x44, 0x24, 0x14, 0x02, 0x00, 0x00, 0x00}, {0xc7, 0x44, 0x24, 0x14, 0x01, 0x00, 0x00, 0x00}},
    /* movss xmm0, [0x410440] -> [0x410448] */
    {"dt", BASE + 0x80, 8, {0xf3, 0x0f, 0x10, 0x05, 0x40, 0x04, 0x41, 0x00}, {0xf3, 0x0f, 0x10, 0x05, 0x48, 0x04, 0x41, 0x00}},
};
enum { NFAKE = 3 };

static void plant(void) {
    memset(mem, 0xcc, sizeof mem);
    for (int i = 0; i < NFAKE; i++) memcpy(mem + (fakes[i].va - BASE), fakes[i].orig, fakes[i].len);
    memcpy(before, mem, sizeof mem);
}


#if defined(__linux__) && defined(__i386__)
/* Runs the held/tap pad tests of the patched image as the title's binding loop (FUN_0006b460)
 * does: once per pad poll, the held table (FUN_00069440, B -> tackle 0xe) before the tap table
 * (FUN_00069350, B -> grab 0xd). The image is copied to its own addresses (the caves use
 * absolute operands); a pad record is 0x50 bytes at 0x4ac790 with an 8-entry button history at
 * +0x1c and its index at +0x2c; CfgButtonHeldFrames N = [0x458774] = 7. */
#include <sys/mman.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
enum { G_LO = 0x10000u, G_HI = 0x4b0000u, PAD0 = 0x4ac790u, PAD_SIZE = 0x50u, HELD_N = 7 };
typedef uint8_t (*PadTest)(uint32_t pad, uint32_t mask);   /* cdecl, result in al */
static uint8_t *G(uint32_t va) { return (uint8_t *)(uintptr_t)va; }
static void pad_poll(uint32_t pad, uint16_t buttons) {
    uint32_t idx = (rd32(G(pad + 0x2c)) + 1) & 7;
    memcpy(G(pad + 0x2c), &idx, 4);
    memcpy(G(pad + 0x1c + 2 * idx), &buttons, 2);
}
static uint16_t pad_entry(uint32_t pad, uint32_t back) { uint16_t v; memcpy(&v, G(pad + 0x1c + 2 * ((rd32(G(pad + 0x2c)) - back) & 7)), 2); return v; }
/* FUN_00069350 as shipped: up now, down one poll ago, and up again within the 8 entries (a down
 * run of at most 6 polls). Used when the image leaves the tap test unpatched. */
static int tap_shipped(uint32_t pad, uint32_t mask) {
    if (pad_entry(pad, 0) & mask || !(pad_entry(pad, 1) & mask)) return 0;
    for (uint32_t k = 2; k < 8; k++) if (!(pad_entry(pad, k) & mask)) return 1;
    return 0;
}
/* Presses of 1..40 polls at 60 Hz: one action per press, as at 30 Hz with presses of the same
 * length in time: the grab (tap) when released before 2N polls, else the tackle (hold) at 2N. */
static void held_tap_presses(const uint8_t *image) {
    void *m = mmap((void *)(uintptr_t)G_LO, G_HI - G_LO, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m != (void *)(uintptr_t)G_LO) { CHECK(0, "held/tap: guest range %08x-%08x mappable", G_LO, G_HI); return; }
    memcpy(G(G_LO), image + G_LO, G_HI - G_LO);
    uint32_t n = HELD_N;
    memcpy(G(0x458774), &n, 4);
    G(0x69357)[0] = 0xb0, G(0x69357)[1] = 0x5a, G(0x69357)[2] = 0xc3;   /* stand-in body: mov al,0x5a; ret */
    PadTest held = (PadTest)(uintptr_t)0x69440, tap = (PadTest)(uintptr_t)0x69350;
    int tap_patched = jmp_target(image, 0x69350) != 0;
    const uint32_t pad = PAD0 + PAD_SIZE, b = 0x20;   /* pad 1, B */
    int bad = 0, dead = 0, both = 0;
    for (int len = 1; len <= 40; len++) {
        int holds = 0, taps = 0, hold_at = -1, tap_at = -1;
        for (int p = 0; p < len + 10; p++) {
            pad_poll(pad, p < len ? (uint16_t)b : 0);
            if (held(pad, b) & 1) holds++, hold_at = p;
            if ((tap_patched ? tap(pad, b) : tap_shipped(pad, b)) & 1) taps++, tap_at = p;
        }
        int want_hold = len >= 2 * HELD_N;
        int ok = want_hold ? holds == 1 && hold_at == 2 * HELD_N - 1 && !taps : taps == 1 && tap_at == len && !holds;
        if (!holds && !taps) dead++;
        if (holds && taps) both++;
        if (!ok) { bad++; printf("     press of %2d polls: hold %d (poll %d), tap %d (poll %d)\n", len, holds, hold_at, taps, tap_at); }
    }
    CHECK(!bad && !dead && !both, "held/tap: B presses of 1-40 polls give the tap below %d polls, the hold at %d (%d wrong, %d with neither)",
          2 * HELD_N, 2 * HELD_N, bad, dead);
    /* A button the held table does not count: the tap falls back to the title's own test. */
    pad_poll(pad, 0x40), pad_poll(pad, 0x40), pad_poll(pad, 0);
    CHECK(tap_patched && tap(pad, 0x40) == 0x5a, "tap: a button without a counted run uses the title's 8-entry test");
    munmap(m, G_HI - G_LO);
}

/* Pose sample time of the anim sampler FUN_000180e0 (cursor +0x18 seconds x 30 at 0x18104,
 * result in xmm0, continuing at 0x18114) for one cursor, run as the patched image does. A clip
 * header holds the event list (+0x1c + u16 +0x10 + u16 +0x12 + u32 +0x14, u16 count +0x1a,
 * 0x18-byte records {u16 frame, u16 type}); the cursor holds the clip (+4), its time (+0x18), the
 * whole-frame counter (+0x1c) and the event frame base (+0x128). */
static float sample_frame(uint8_t *cursor) {
    float f;
    __asm__ volatile("push %%ebp\n\tmov %%esi, %%ebp\n\tcall *%%edi\n\tpop %%ebp\n\tmovss %%xmm0, %0"
                     : "=m"(f) : "S"(cursor), "D"(0x18104u) : "eax", "ecx", "edx", "xmm0", "memory");
    return f;
}
static float snap_case(uint8_t *cursor, uint8_t *clip, float frame, uint16_t counter, uint16_t base, uint16_t ev_frame,
                       uint16_t ev_type, uint16_t n_events) {
    static uint8_t data[0x100];
    memset(clip, 0, 0x40), memset(cursor, 0, 0x130), memset(data, 0, sizeof data);
    uint32_t keys = (uint32_t)(uintptr_t)data, off = 0x10;   /* events at keys + 0x8 + 0x4 + 0x4 */
    memcpy(clip + 0x1c, &keys, 4);
    clip[0x10] = 0x8, clip[0x12] = 0x4;
    memcpy(clip + 0x14, (const uint32_t[]){4}, 4);
    memcpy(clip + 0x1a, &n_events, 2);
    memcpy(data + off, (const uint16_t[]){3, 0x32}, 4);   /* another event first */
    memcpy(data + off + 0x18, (const uint16_t[]){ev_frame, ev_type}, 4);
    uint32_t c = (uint32_t)(uintptr_t)clip;
    memcpy(cursor + 4, &c, 4);
    float t = frame / 30.0f;
    memcpy(cursor + 0x18, &t, 4);
    memcpy(cursor + 0x1c, &counter, 2);
    memcpy(cursor + 0x128, &base, 2);
    return sample_frame(cursor);
}
/* Climb clips (gen_climb_wall_stand_p2 ...) jump the hips by 1.35-2.55 m between key 0 and key 1
 * and cancel it with an actor offset teleport (event type 0x40 at frame 1) that fires when the
 * whole-frame counter reaches the frame. At 60 Hz the pose was sampled half way across the jump
 * on the tick between the two keys: the snap samples that interval at the counter instead. */
static void anim_snap_samples(const uint8_t *image) {
    static const uint32_t pages[] = {0x18000u, 0x410000u, 0x5d4000u};
    void *m[3];
    for (int i = 0; i < 3; i++) {
        m[i] = mmap((void *)(uintptr_t)pages[i], 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (m[i] != (void *)(uintptr_t)pages[i]) { CHECK(0, "anim snap: guest page %08x mappable", pages[i]); return; }
        memcpy(m[i], image + pages[i], 0x1000);
    }
    memcpy(G(0x410408), (const float[]){30.0f}, 4);
    G(0x18114)[0] = 0xc3;   /* stand-in for the rest of the sampler: return to the test */
    static uint8_t cursor[0x130], clip[0x40];
    /* 2 events: type 0x32 at frame 3, then the given one. */
    float a = snap_case(cursor, clip, 0.5f, 1, 0, 1, 0x40, 2);
    float b = snap_case(cursor, clip, 0.5f, 0, 0, 1, 0x40, 2);
    float c = snap_case(cursor, clip, 1.5f, 2, 0, 1, 0x40, 2);
    float d = snap_case(cursor, clip, 0.5f, 1, 0, 1, 0x33, 2);
    float e = snap_case(cursor, clip, 0.5f, 1, 0, 1, 0x40, 0);
    float f = snap_case(cursor, clip, 0.5f, 1, 5, 6, 0x40, 2);
    float g = snap_case(cursor, clip, 3.5f, 4, 0, 4, 0x40, 2);
    float h = snap_case(cursor, clip, 3.0f, 3, 0, 4, 0x40, 2);
    CHECK(a == 1.0f && b == 0.0f, "anim snap: between key 0 and an offset teleport at frame 1 the pose is sampled at the "
          "counter (fired: %g, not yet: %g)", a, b);
    CHECK(c == 1.5f && h == 3.0f, "anim snap: other intervals and whole frames keep their time (%g, %g)", c, h);
    CHECK(d == 0.5f && e == 0.5f, "anim snap: clips without a 0x40 event keep the 60 Hz time (%g, %g)", d, e);
    CHECK(f == 1.0f && g == 4.0f, "anim snap: the event frame counts from the cursor's base, any frame (%g, %g)", f, g);
    for (int i = 0; i < 3; i++) munmap(m[i], 0x1000);
}

/* Key interval of a half-frame sample (FUN_00018070 rotation, FUN_00015f90 position: cdecl
 * (key record, out, key frame, time x 30), interpolating record -> record + 8 at
 * (time x 30 - frame) / [record + 8]). The track cursors advance with the whole-frame counter,
 * which is rounded (trunc(time x 30 + 0.5), FUN_00015890), so on a 60 Hz tick half way to a key
 * the cursor already sits on that key and the sample extrapolated backwards from it along the
 * next interval (f = -0.5 / dt): a pose that no key holds, far off wherever the motion turns at
 * the key (an impact on the floor). The pair is moved back to the interval the time falls in. */
static void keyseg_call(uint32_t fn, uint32_t args[4]) {
    __asm__ volatile("sub $16, %%esp\n\t"
                     "mov 0(%%esi), %%eax\n\tmov %%eax, 0(%%esp)\n\tmov 4(%%esi), %%eax\n\tmov %%eax, 4(%%esp)\n\t"
                     "mov 8(%%esi), %%eax\n\tmov %%eax, 8(%%esp)\n\tmov 12(%%esi), %%eax\n\tmov %%eax, 12(%%esp)\n\t"
                     "call *%%edi\n\t"
                     "mov 0(%%esp), %%eax\n\tmov %%eax, 0(%%esi)\n\tmov 8(%%esp), %%eax\n\tmov %%eax, 8(%%esi)\n\t"
                     "add $16, %%esp"
                     : : "S"(args), "D"(fn) : "eax", "ecx", "edx", "memory", "cc");
}
/* -> 1 when the call left the record and frame as expected. */
static int keyseg_case(uint32_t fn, const uint8_t *keys, int key, uint16_t frame, float t30, int want_key, uint16_t want_frame) {
    uint32_t t, args[4] = {(uint32_t)(uintptr_t)(keys + 8 * key), 0, frame, 0};
    memcpy(&t, &t30, 4), args[3] = t;
    keyseg_call(fn, args);
    return args[0] == (uint32_t)(uintptr_t)(keys + 8 * want_key) && (args[2] & 0xffff) == want_frame;
}
static void keyseg_samples(const uint8_t *image) {
    static const uint32_t pages[] = {0x15000u, 0x18000u, 0x5d4000u};
    void *m[3];
    for (int i = 0; i < 3; i++) {
        m[i] = mmap((void *)(uintptr_t)pages[i], 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (m[i] != (void *)(uintptr_t)pages[i]) { CHECK(0, "key interval: guest page %08x mappable", pages[i]); return; }
        memcpy(m[i], image + pages[i], 0x1000);
    }
    /* Stand-ins for the bodies after the prologues (sub esp,0x20 + one load): undo and return. */
    memcpy(G(0x18078), (const uint8_t[]){0x83, 0xc4, 0x20, 0xc3}, 4);
    memcpy(G(0x15f97), (const uint8_t[]){0x83, 0xc4, 0x20, 0xc3}, 4);
    /* One track: keys at frames 0, 1 and 4 (dt bytes 0, 1, 3), then the next track (dt 0). */
    static uint8_t keys[32];
    memset(keys, 0, sizeof keys);
    keys[8] = 1, keys[16] = 3;
    const uint32_t fns[2] = {0x18070u, 0x15f90u};
    const char *names[2] = {"rotation", "position"};
    for (int i = 0; i < 2; i++) {
        uint32_t f = fns[i];
        CHECK(keyseg_case(f, keys, 2, 4, 3.5f, 1, 1) && keyseg_case(f, keys, 1, 1, 0.5f, 0, 0),
              "key interval (%s): a half-frame sample before the cursor's key uses the interval before it", names[i]);
        CHECK(keyseg_case(f, keys, 2, 4, 4.0f, 2, 4) && keyseg_case(f, keys, 2, 4, 4.5f, 2, 4) &&
              keyseg_case(f, keys, 1, 1, 1.0f, 1, 1),
              "key interval (%s): samples at or after the cursor's key keep it", names[i]);
        float nan;
        memcpy(&nan, (const uint32_t[]){0x7fc00000u}, 4);
        CHECK(keyseg_case(f, keys, 0, 0, -0.5f, 0, 0) && keyseg_case(f, keys, 2, 4, nan, 2, 4),
              "key interval (%s): the first key of a track and an unordered time are left alone", names[i]);
    }
    for (int i = 0; i < 3; i++) munmap(m[i], 0x1000);
}
#endif

int main(void) {
    intptr_t bias = (intptr_t)((uintptr_t)mem - BASE);
    char report[4096];

    /* Options. */
    int enabled = 0;
    CHECK(sim60_parse_arg("--sim60", &enabled) == 1 && enabled == 1, "--sim60 parsed, enables");
    CHECK(sim60_parse_arg("--no-sim60", &enabled) == 1 && enabled == 0, "--no-sim60 parsed, disables");
    CHECK(sim60_parse_arg("--sim6", &enabled) == 0 && enabled == 0, "other options are not sim60 options");

    /* Off (the default): nothing written. */
    plant();
    int n = sim60_apply(fakes, NFAKE, 0, bias, report, sizeof report);
    CHECK(n == 0 && !memcmp(mem, before, sizeof mem), "disabled: nothing written (n=%d)", n);
    CHECK(strstr(report, "[sim60] off") != NULL, "disabled: report says off: %s", report);

    /* On, all match: every patch written. */
    n = sim60_apply(fakes, NFAKE, 1, bias, report, sizeof report);
    CHECK(n == NFAKE, "all match: %d patches applied", n);
    int same = 1;
    for (int i = 0; i < NFAKE; i++) same &= !memcmp(mem + (fakes[i].va - BASE), fakes[i].repl, fakes[i].len);
    CHECK(same, "all match: replacement bytes in place");
    for (int i = 0; i < NFAKE; i++) memcpy(mem + (fakes[i].va - BASE), fakes[i].orig, fakes[i].len);
    CHECK(!memcmp(mem, before, sizeof mem), "all match: no byte outside the patches changed");
    CHECK(strstr(report, "[sim60] applied: gate (00100010), pairs (00100040), dt (00100080); skipped: none") != NULL,
          "all match: report %s", report);

    /* Already patched: the signatures no longer match, nothing is written again. */
    sim60_apply(fakes, NFAKE, 1, bias, report, sizeof report);
    uint8_t patched[SIZE];
    memcpy(patched, mem, sizeof mem);
    n = sim60_apply(fakes, NFAKE, 1, bias, report, sizeof report);
    CHECK(n == 0 && !memcmp(mem, patched, sizeof mem), "re-application: nothing written (n=%d)", n);

    /* One mismatch (last byte of the second site): nothing at all is written, the site is named. */
    plant();
    mem[0x47] = 0x01;
    memcpy(before, mem, sizeof mem);
    n = sim60_apply(fakes, NFAKE, 1, bias, report, sizeof report);
    CHECK(n == 0 && !memcmp(mem, before, sizeof mem), "one mismatch: nothing written (n=%d)", n);
    CHECK(strstr(report, "applied: none") && strstr(report, "skipped: pairs (00100040)") && !strstr(report, "gate ("),
          "one mismatch: report %s", report);

    /* Malformed entries (length 0, length over the maximum) are mismatches. */
    plant();
    Sim60Patch bad[NFAKE + 1];
    memcpy(bad, fakes, sizeof fakes);
    bad[NFAKE] = (Sim60Patch){"empty", BASE + 0x100, 0, {0}, {0}};
    n = sim60_apply(bad, NFAKE + 1, 1, bias, report, sizeof report);
    CHECK(n == 0 && !memcmp(mem, before, sizeof mem) && strstr(report, "empty ("), "zero length: nothing written: %s", report);
    bad[NFAKE] = (Sim60Patch){"long", BASE + 0x100, SIM60_BYTES_MAX + 1, {0}, {0}};
    n = sim60_apply(bad, NFAKE + 1, 1, bias, report, sizeof report);
    CHECK(n == 0 && !memcmp(mem, before, sizeof mem) && strstr(report, "long ("), "over-long: nothing written: %s", report);

    /* A tiny report buffer stays NUL-terminated. */
    char tiny[8];
    plant();
    sim60_apply(fakes, NFAKE, 1, bias, tiny, sizeof tiny);
    CHECK(memchr(tiny, 0, sizeof tiny) != NULL, "small report buffer stays terminated");

    /* The title table: well formed, no overlaps, each entry changes something, all apply to a
     * planted image, and the key sites hold the intended values afterwards. */
    CHECK(sim60_count >= 10, "title table has %d patches", sim60_count);
    int ok = 1, overlap = 0, nochange = 0;
    for (int i = 0; i < sim60_count; i++) {
        const Sim60Patch *p = &sim60_table[i];
        if (!p->len || p->len > SIM60_BYTES_MAX || p->va < 0x10000 || p->va + p->len > IMG_SIZE) { ok = 0; continue; }
        if (!memcmp(p->orig, p->repl, p->len)) nochange++;
        for (int j = 0; j < i; j++) {
            const Sim60Patch *q = &sim60_table[j];
            if (p->va < q->va + q->len && q->va < p->va + p->len) overlap++;
        }
        memcpy(img + p->va, p->orig, p->len);
    }
    CHECK(ok && !overlap && !nochange, "title table well formed (%d overlaps, %d no-ops)", overlap, nochange);
    n = sim60_apply(sim60_table, sim60_count, 1, (intptr_t)(uintptr_t)img, report, sizeof report);
    CHECK(n == sim60_count && strstr(report, "skipped: none"), "title table applies to a planted image (%d): %s", n, report);
    int all = 1;
    for (int i = 0; i < sim60_count; i++) all &= !memcmp(img + sim60_table[i].va, sim60_table[i].repl, sim60_table[i].len);
    CHECK(all, "title table: every replacement in place");
    CHECK(rd32(img + 0x2b24a4) == 1, "step loop runs single steps (count %u)", rd32(img + 0x2b24a4));
    CHECK(!memcmp(img + 0x2b2476, (const uint8_t[]){0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00}, 6) &&
          !memcmp(img + 0x2b2572, (const uint8_t[]){0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00}, 6),
          "step loop threshold is one step (both checks)");
    CHECK(!memcmp(img + 0x16d8e0, (const uint8_t[]){0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00}, 6), "world gate removed");
    CHECK(rdf(img + 0x45cc38) == 1.0f / 60, "initial world time step 1/60 (%g)", rdf(img + 0x45cc38));
    CHECK(rd32(img + 0x15aa44) == 0x410448u && rd32(img + 0x15aa2a) == 0x410448u, "time-scale setters use 1/60");
    CHECK(rd32(img + 0x16e0c3) == 0x410448u, "camera time step floor 1/60");
    CHECK(img[0x1afad3] == 10 && img[0x1aface] == 0x89, "AI think round-robin: counter mod 10");
    CHECK(rd32(img + 0x458860) == 60 && img[0x6c93b] == 60, "menu fixed-step clock: 60 frames per second (data and init)");
    CHECK(rd32(img + 0x458878) == 0xba7f0eu && rd32(img + 0x6c9c9) == 0xba7f0eu, "menu fixed-step clock: 1/60 s in ticks (data and init)");
    CHECK(img[0x16dc37] == 5, "character fade-out: 5 per update");
    CHECK(rdf(img + 0x44c070) == 1.0f / 240, "added constant 1/240 (%g)", rdf(img + 0x44c070));
    /* Button-tap run (HuSetButtonTapControl, FUN_00169d00): the tap interval is counted in world
     * updates, so at 60 Hz it is 2n+1 for the same presses: cap 21, offset 5, range 16.0, and the
     * per-update slow-down fraction 1/30 -> 1/60. */
    CHECK(img[0x16a0a8] == 21, "tap run: idle cap 21 updates (%u)", img[0x16a0a8]);
    CHECK(img[0x16a0c7] == 5, "tap run: interval offset 5 updates (%u)", img[0x16a0c7]);
    CHECK(rd32(img + 0x16a0d0) == 0x41ac0cu, "tap run: interval range uses 16.0 (%08x)", rd32(img + 0x16a0d0));
    CHECK(rd32(img + 0x16a160) == 0x41bb84u, "tap run: slow-down divisor uses 60.0 (%08x)", rd32(img + 0x16a160));
    /* Scene task FUN_002b0270 (cinematics, fixed scenes): reschedules itself every 2 steps (30 Hz);
     * with the world at 60 Hz it runs every step, its per-run counters at half rate. */
    CHECK(img[0x2b0301] == 1, "scene task runs every step (%u)", img[0x2b0301]);
    CHECK(rd32(img + 0x2b032e) == 0x410174u, "scene delay counter: 0.5 per run (%08x)", rd32(img + 0x2b032e));
    CHECK(rd32(img + 0x2abe6e) == 120, "scene skip lock-out: 120 runs (%u)", rd32(img + 0x2abe6e));
    /* Per-update counters that no operand patch can halve run on every second update through code
     * placed in the unused tail of the image-header page (SizeOfHeaders 0xbd4): the 0x4000-state
     * health drain, the struggle meter decay and the workout progress update. */
    CHECK(jmp_target(img, 0x17a754) == 0x10c00 && jmp_target(img, 0x10c10) == 0x17a75b,
          "health drain: jmp to the cave and back (%08x, %08x)", jmp_target(img, 0x17a754), jmp_target(img, 0x10c10));
    CHECK(gates_parity(img, 0x10c00) && !memcmp(img + 0x10c09, (const uint8_t[]){0x66, 0xff, 0x8f, 0x44, 0x01, 0x00, 0x00}, 7),
          "health drain: dec word [edi+0x144] only on even step counts");
    CHECK(jmp_target(img, 0x19d9aa) == 0x10c20 && jmp_target(img, 0x10c32) == 0x19d9b1,
          "struggle decay: jmp to the cave and back (%08x, %08x)", jmp_target(img, 0x19d9aa), jmp_target(img, 0x10c32));
    CHECK(!memcmp(img + 0x10c20, (const uint8_t[]){0x66, 0x8b, 0x0d, 0x28, 0xcb, 0x45, 0x00}, 7) && gates_parity(img, 0x10c27) &&
          img[0x10c2e] == 0x74 && img[0x10c30] == 0x31, "struggle decay: subtracts 0 on odd step counts");
    CHECK(jmp_target(img, 0x17b3f4) == 0x10c40 && jmp_target(img, 0x10c4f) == 0x17b3fa && jmp_target(img, 0x10c5f) == 0x17b481,
          "workout: jmp to the cave, back to the update (%08x) and to the store on odd counts (%08x)",
          jmp_target(img, 0x10c4f), jmp_target(img, 0x10c5f));
    CHECK(gates_parity(img, 0x10c40) && img[0x10c47] == 0x75, "workout: hold and progress step only on even step counts");
    CHECK(rd32(img + 0x45cae4) == 60, "hazard delay: 60 updates (%u)", rd32(img + 0x45cae4));
    CHECK(rd32(img + 0x17001c) == 60, "blood gush: 60 updates (%u)", rd32(img + 0x17001c));

    /* AI brain age: the brain tick FUN_001a8870 runs once per world
     * update and counts [brain+0x28]; goals act when age % N == 0 (mov r32,N; div), so every divisor
     * is doubled: the same periodic decisions per game second. */
    static const struct { uint32_t va; uint8_t op; uint32_t n; } age_div[] = {
        {0x001c0748u, 0xb9, 10},
        {0x001c0ec5u, 0xb9, 15},
        {0x001c0f06u, 0xbf, 45},
        {0x001c101eu, 0xb9, 5},
        {0x001c3da6u, 0xb9, 7},
        {0x001c61e1u, 0xb9, 30},
        {0x001c6cc0u, 0xbe, 300},
        {0x001c70edu, 0xbd, 90},
        {0x001c7559u, 0xb9, 20},
        {0x001c832cu, 0xb9, 30},
        {0x001cb841u, 0xb9, 30},
        {0x001cbac6u, 0xb9, 30},
        {0x001cbd7eu, 0xbf, 30},
        {0x001cda43u, 0xbd, 30},
        {0x001ce235u, 0xb9, 20},
        {0x001d1660u, 0xb9, 30},
        {0x001d1873u, 0xb9, 5},
        {0x001d23cau, 0xb9, 45},
        {0x001d2b97u, 0xb9, 5},
        {0x001d2d16u, 0xb9, 3},
        {0x001d3d52u, 0xb9, 10},
        {0x001d3e7du, 0xbb, 30},
        {0x001d426bu, 0xb9, 30},
        {0x001d5d8eu, 0xbe, 20},
        {0x001d641au, 0xb9, 20},
        {0x001d694eu, 0xbe, 20},
        {0x001dadefu, 0xbe, 40},
        {0x001dd9c1u, 0xb9, 90},
        {0x001ddbd7u, 0xbd, 192},
        {0x001e5ac9u, 0xbe, 45},
        {0x001e6846u, 0xb9, 30},
        {0x001e7332u, 0xb9, 10},
        {0x001e7ce1u, 0xb9, 30},
        {0x001e9cc3u, 0xb9, 60},
        {0x001e9d5cu, 0xb9, 90},
        {0x001eaab5u, 0xbf, 90},
        {0x001eb5bfu, 0xbf, 90},
        {0x001eb668u, 0xb9, 45},
        {0x001eb6aau, 0xb9, 45},
        {0x001f2b39u, 0xbd, 30},
        {0x001f3248u, 0xbf, 15},
        {0x001f4a5eu, 0xb9, 30},
        {0x001f53d1u, 0xb9, 30},
        {0x001f5943u, 0xb9, 30},
        {0x001f596du, 0xbb, 40},
        {0x001f6065u, 0xb9, 6},
        {0x001f659du, 0xb9, 10},
        {0x001f895bu, 0xb9, 30},
        {0x001f8d68u, 0xb9, 30},
        {0x001f8e40u, 0xb9, 15},
        {0x001fb894u, 0xb9, 10},
        {0x001fe701u, 0xbf, 5},
        {0x002062ddu, 0xbf, 5},
        {0x00208dcfu, 0xb9, 30},
        {0x0020938au, 0xb9, 30},
        {0x00209406u, 0xb9, 3},
        {0x0020a3f3u, 0xb9, 30},
        {0x0020bf08u, 0xbf, 30},
        {0x001e5dceu, 0xbe, 30},
        {0x001a888bu, 0xbf, 90},
    };
    int age_ok = 0, age_n = (int)(sizeof age_div / sizeof age_div[0]);
    for (int i = 0; i < age_n; i++)
        age_ok += img[age_div[i].va] == age_div[i].op && rd32(img + age_div[i].va + 1) == 2 * age_div[i].n;
    CHECK(age_ok == age_n, "brain age: %d of %d periodic divisors doubled", age_ok, age_n);
    CHECK(img[0x1a0e28] == 60, "brain age: the age == 30 test is age == 60 (%u)", img[0x1a0e28]);
    /* Goal divisors stored in the goal: FollowAndDefend +0x32 and FollowFormation +0x37 (random
     * ranges) and FollowObject +0x2c (from the caller, two constructors) are stored doubled. */
    CHECK(jmp_target(img, 0x1d5932) == 0x10c80 && call_target(img, 0x10c80) == 0x23e930 &&
          !memcmp(img + 0x10c85, (const uint8_t[]){0x02, 0xc0, 0x88, 0x46, 0x32}, 5) && jmp_target(img, 0x10c8a) == 0x1d593a,
          "FollowAndDefend: random period stored doubled at +0x32");
    CHECK(jmp_target(img, 0x1f0b0b) == 0x10ca0 && call_target(img, 0x10ca0) == 0x23e930 &&
          !memcmp(img + 0x10ca5, (const uint8_t[]){0x02, 0xc0, 0x88, 0x46, 0x37}, 5) && jmp_target(img, 0x10caa) == 0x1f0b13,
          "FollowFormation: random period stored doubled at +0x37");
    CHECK(jmp_target(img, 0x1f096c) == 0x10cc0 &&
          !memcmp(img + 0x10cc0, (const uint8_t[]){0x01, 0xc9, 0x89, 0x48, 0x2c, 0xc2, 0x10, 0x00}, 8),
          "FollowObject (0x1f0930): period stored doubled at +0x2c");
    CHECK(jmp_target(img, 0x1f5bda) == 0x10ce0 &&
          !memcmp(img + 0x10ce0, (const uint8_t[]){0xf3, 0x0f, 0x11, 0x40, 0x28, 0x01, 0xd2, 0x89, 0x50, 0x2c}, 10) &&
          jmp_target(img, 0x10cea) == 0x1f5be2, "FollowObject (0x1f5b70): period stored doubled at +0x2c");
    /* Animation root rotation (FUN_00164f50, char+0x250 != -1): the per-1/30 s rotation of the
     * clip is applied once per world update; the cave applies its square root, (q + 1) / |q + 1|
     * (half the angle about the same axis), so two 60 Hz updates turn as far as one 30 Hz update. */
    CHECK(jmp_target(img, 0x165123) == 0x10d00 && jmp_target(img, 0x10d37) == 0x165141,
          "root rotation: jmp to the cave and back (%08x, %08x)", jmp_target(img, 0x165123), jmp_target(img, 0x10d37));
    CHECK(!memcmp(img + 0x10d00, (const uint8_t[]){0x83, 0xc6, 0x10, 0x0f, 0x10, 0x06, 0x0f, 0x10, 0x0d}, 9) &&
          rd32(img + 0x10d09) == 0x10d40 && !memcmp(img + 0x10d32, (const uint8_t[]){0x0f, 0x11, 0x44, 0x24, 0x10}, 5),
          "root rotation: reads the clip quaternion, adds (0,0,0,1), stores the local quaternion");
    CHECK(!memcmp(img + 0x10d27, (const uint8_t[]){0xf3, 0x0f, 0x51, 0xc9}, 4) &&
          !memcmp(img + 0x10d2f, (const uint8_t[]){0x0f, 0x5e, 0xc1}, 3), "root rotation: normalises by sqrt(|q + 1|^2)");
    CHECK(rdf(img + 0x10d40) == 0.0f && rdf(img + 0x10d44) == 0.0f && rdf(img + 0x10d48) == 0.0f && rdf(img + 0x10d4c) == 1.0f,
          "root rotation: identity quaternion constant at 0x10d40");
    int nops = 1;
    for (uint32_t a = 0x165128; a < 0x165141; a++) nops &= img[a] == 0x90;
    CHECK(nops, "root rotation: the replaced copy is padded with nops");
    /* Root rotation sampling: before the angle test, a scripted clip's 60 Hz pair applies the
     * even step's sample twice (stored per character slot with its step count), so clip loops and
     * switches between the two steps no longer drop half a key frame's rotation. */
    CHECK(jmp_target(img, 0x165097) == 0x10f90 && rd32(img + 0x16509c) == 0x90909090,
          "root rotation sample: the angle test's argument load jumps to the selector");
    CHECK(!memcmp(img + 0x10f90, (const uint8_t[]){0x83, 0xbb, 0x50, 0x02, 0x00, 0x00, 0xff, 0x74, 0x33}, 9) &&
          rd32(img + 0x10fa4) == 0x5d4200 && rd32(img + 0x10faa) == 0x5825fc &&
          !memcmp(img + 0x10fae, (const uint8_t[]){0xf6, 0xc1, 0x01, 0x75, 0x0c, 0x0f, 0x10, 0x46, 0x10, 0x0f, 0x11, 0x00,
                                                   0x89, 0x48, 0x10}, 15),
          "root rotation sample: scripted clips store the even step's quaternion and step per slot (cache 0x5d4200)");
    CHECK(!memcmp(img + 0x10fbf, (const uint8_t[]){0x49, 0x39, 0x48, 0x10, 0x75, 0x07, 0x0f, 0x10, 0x00, 0x0f, 0x11, 0x46, 0x10}, 13),
          "root rotation sample: odd steps use the sample stored on the step before");
    CHECK(call_target(img, 0x10fd0) == 0x36970 && jmp_target(img, 0x10fd5) == 0x1650a0 &&
          !memcmp(img + 0x10fcc, (const uint8_t[]){0x8b, 0x56, 0x1c, 0x52}, 4),
          "root rotation sample: replays the angle test's argument and call, back to 0x1650a0");
    /* Turn-rate caps (FUN_00149af0, table 0x45ca90 from CfgSetTurnRates): radians per world
     * update, applied once per update by the human states; the wrapper halves the returned cap,
     * so 60 updates turn as far per second as 30. */
    CHECK(jmp_target(img, 0x149af0) == 0x10d50 && img[0x149af5] == 0x90, "turn cap: entry jumps to the wrapper (%08x)",
          jmp_target(img, 0x149af0));
    CHECK(call_target(img, 0x10d50) == 0x10d5c && !memcmp(img + 0x10d55, (const uint8_t[]){0xd8, 0x0d}, 2) &&
          rd32(img + 0x10d57) == 0x410174 && img[0x10d5b] == 0xc3,
          "turn cap: calls the body, then fmul by 0.5 (0x410174) and returns");
    CHECK(!memcmp(img + 0x10d5c, (const uint8_t[]){0x83, 0xec, 0x08, 0x56, 0x8b, 0xf1}, 6) && jmp_target(img, 0x10d62) == 0x149af6,
          "turn cap: the replaced prologue runs in the wrapper, then the body continues at 0x149af6");
    /* Held-button test (FUN_00069440, CfgButtonHeldFrames): the 8-entry per-poll history cannot
     * hold 2N polls, so a per-pad, per-button poll counter (0x10e00) fires once at 2N. */
    CHECK(jmp_target(img, 0x69440) == 0x10d70 && img[0x69445] == 0x90, "held: entry jumps to the counter (%08x)",
          jmp_target(img, 0x69440));
    CHECK(!memcmp(img + 0x10d70, (const uint8_t[]){0x8b, 0x4c, 0x24, 0x04, 0x53, 0x56, 0x57}, 7) &&
          !memcmp(img + 0x10d81, (const uint8_t[]){0x8d, 0x7c, 0x11, 0x70}, 4) && rd32(img + 0x10dac) == 0x10e00 &&
          rd32(img + 0x10dc2) == 0x10e00 && rd32(img + 0x10dc8) == 0x458774 &&
          !memcmp(img + 0x10dcc, (const uint8_t[]){0x01, 0xd2, 0x39, 0xd0, 0x0f, 0x94, 0xc0}, 7),
          "held: counts polls per pad and button at 0x10e00, fires at 2 x [0x458774]");
    CHECK(img[0x10dea] == 0x80 && rd32(img + 0x10ded) == 0x10e00 &&
          !memcmp(img + 0x10df3, (const uint8_t[]){0xc6, 0x87, 0x00, 0x0e, 0x01, 0x00, 0x00}, 7) && img[0x10dff] == 0xc3,
          "held: a release keeps the run with bit 7 set, the next up poll clears it; returns to the caller");
    /* Tap test (FUN_00069350, B -> grab): on a release, true for a counted run shorter than 2N. */
    CHECK(jmp_target(img, 0x69350) == 0x10f00 && img[0x69355] == 0x90 && img[0x69356] == 0x90,
          "tap: entry jumps to the replacement (%08x)", jmp_target(img, 0x69350));
    CHECK(rd32(img + 0x10f35) == 0x10e00 && rd32(img + 0x10f43) == 0x458774 &&
          !memcmp(img + 0x10f4b, (const uint8_t[]){0x0f, 0x92, 0xc0}, 3) && jmp_target(img, 0x10f5f) == 0x69357 &&
          !memcmp(img + 0x10f58, (const uint8_t[]){0x8b, 0x4c, 0x24, 0x04, 0x8b, 0x41, 0x2c}, 7),
          "tap: run < 2 x [0x458774] from the counter, else the replaced prologue and the title's test at 0x69357");
    /* Weapon hit spheres (FUN_00162370: a held weapon's attack adds spheres along the weapon to
     * the attacker's collision body and sets their lifetime body+0xb0 = N + 1 world updates): the
     * lifetime is stored doubled, so the spheres last as long in time as at 30 Hz. */
    CHECK(jmp_target(img, 0x1623a8) == 0x10f70 && img[0x1623ad] == 0x90 && img[0x1623b2] == 0x90,
          "weapon hit spheres: lifetime store jumps to the cave (%08x)", jmp_target(img, 0x1623a8));
    CHECK(!memcmp(img + 0x10f70, (const uint8_t[]){0x8b, 0x44, 0x24, 0x3c, 0x8d, 0x44, 0x00, 0x02, 0x89, 0x87, 0xb0, 0x00, 0x00, 0x00}, 14) &&
          jmp_target(img, 0x10f7e) == 0x1623b3, "weapon hit spheres: lifetime 2 x (N + 1) updates, back to 0x1623b3");
    /* Stealth walk (state dispatcher 0x1801a1): the walk state is kept while the current speed is
     * at least 0.25 x the walk speed (FUN_0013fa40); the speed ramps up per world update, so the
     * first 60 Hz update (half the 30 Hz step) is below it: the threshold is 0.125 (0x41bfac). */
    CHECK(!memcmp(img + 0x180453, (const uint8_t[]){0xd8, 0x0d}, 2) && rd32(img + 0x180455) == 0x41bfac,
          "stealth walk: speed threshold 0.125 x walk speed (0x41bfac, was 0.25 at 0x41b1c0)");

    /* Anim pose sampling (FUN_000180e0): time x 30 at 0x18104 jumps to the snap at 0x5d4a00. */
    CHECK(jmp_target(img, 0x18104) == 0x5d4a00 && img[0x18109] == 0x90 && img[0x18113] == 0x90,
          "anim snap: the sample time load jumps to the snap (%08x)", jmp_target(img, 0x18104));
    CHECK(jmp_target(img, 0x5d4a5e) == 0x18114, "anim snap: back to the sampler at 0x18114");
    /* Key pairs of the sampler (FUN_00018070, FUN_00015f90): entries jump to the interval check. */
    CHECK(jmp_target(img, 0x18070) == 0x5d4ab0 && jmp_target(img, 0x15f90) == 0x5d4ad0 &&
          call_target(img, 0x5d4ab0) == 0x5d4a80 && call_target(img, 0x5d4ad0) == 0x5d4a80 &&
          jmp_target(img, 0x5d4abd) == 0x18078 && jmp_target(img, 0x5d4adc) == 0x15f97,
          "key interval: both key pair functions check the interval, then run their prologue and body");

#if defined(__linux__) && defined(__i386__)
    held_tap_presses(img);
    if (jmp_target(img, 0x18104) == 0x5d4a00) anim_snap_samples(img);
    else CHECK(0, "anim snap: sampler not patched, samples not run");
    if (jmp_target(img, 0x18070) && jmp_target(img, 0x15f90)) keyseg_samples(img);
    else CHECK(0, "key interval: key pair functions not patched, samples not run");
#endif

    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
