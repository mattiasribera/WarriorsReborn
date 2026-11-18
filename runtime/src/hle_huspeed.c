/* Per-character speed caps for mods (see hle_huspeed.h). */
#include "hle_huspeed.h"
#include "hle_devlua.h"
#include "platform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One entry per human slot: the handle that set the cap (its serial tells a new character in the
 * same slot apart) and the cap. used = 0: no entry. */
static struct { uint32_t handle; float cap; int used, rising; } caps[HUSPEED_SLOTS];
static int patched;
int huspeed_sim60;

static uint32_t rd32(uint32_t a) { return *(const volatile uint32_t *)(uintptr_t)a; }

static int slot_of(uint32_t human) {
    if (human < HUSPEED_POOL || human >= HUSPEED_POOL + HUSPEED_SLOTS * HUSPEED_STRIDE ||
        (human - HUSPEED_POOL) % HUSPEED_STRIDE)
        return -1;
    return (int)((human - HUSPEED_POOL) / HUSPEED_STRIDE);
}

/* The human a handle names (high word: slot of the handle table, low word: its serial), 0 when the
 * handle is stale or not a human of the pool. */
static uint32_t human_of(uint32_t handle) {
    const uint32_t slot = handle >> 16;
    if (!handle || slot >= 0x4000) return 0;
    const uint32_t e = HUSPEED_HANDLES + 8u * slot;
    if ((rd32(e + 4) & 0xffff) != (handle & 0xffff)) return 0;
    const uint32_t h = rd32(e);
    return slot_of(h) >= 0 ? h : 0;
}

float huspeed_target(uint32_t human, float target) {
    const int i = slot_of(human);
    if (i < 0 || !caps[i].used) return target;
    if (human_of(caps[i].handle) != human) {   /* the character left; the slot is someone else */
        caps[i].used = caps[i].rising = 0;
        return target;
    }
    return target * caps[i].cap;
}

/* The title's ramp of human+0x35c (0x16b02c-0x16b069), then the cap: a capped character's multiplier
 * stays at or under his cap. A character whose cap was removed rises back at the retail rate per
 * 30 Hz frame (x sqrt(1.002) per update with --sim60, the same rate per second). */
void huspeed_update(uint32_t human) {
    float *p = (float *)(uintptr_t)(human + HUSPEED_MULT);
    float m = *p;
    const int i = slot_of(human);
    int ours = i >= 0 && (caps[i].used || caps[i].rising);
    if (ours && human_of(caps[i].handle) != human) {
        caps[i].used = caps[i].rising = 0;
        ours = 0;
    }
    if (1.0f > m) {
        static float ramp60;
        if (!ramp60) ramp60 = sqrtf(HUSPEED_RAMP);
        m *= ours && huspeed_sim60 ? ramp60 : HUSPEED_RAMP;
    }
    if (m > 1.0f) m = 1.0f;
    if (ours && caps[i].used && m > caps[i].cap) m = caps[i].cap;
    if (ours && !caps[i].used && m >= 1.0f) caps[i].rising = 0;
    *p = m;
}

__attribute__((force_align_arg_pointer, cdecl, used)) static void hook_ramp(uint32_t human) { huspeed_update(human); }

/* The retail ramp (0x16b02c-0x16b069). */
static const uint8_t sig_ramp[] = {
    0xf3, 0x0f, 0x10, 0x05, 0xc8, 0xff, 0x40, 0x00, 0x0f, 0x2f, 0x85, 0x5c, 0x03, 0x00, 0x00, 0x76,
    0x18, 0xf3, 0x0f, 0x10, 0x8d, 0x5c, 0x03, 0x00, 0x00, 0xf3, 0x0f, 0x59, 0x0d, 0x38, 0xe9, 0x42,
    0x00, 0xf3, 0x0f, 0x11, 0x8d, 0x5c, 0x03, 0x00, 0x00, 0xf3, 0x0f, 0x10, 0x8d, 0x5c, 0x03, 0x00,
    0x00, 0x0f, 0x2f, 0xc8, 0x76, 0x08, 0xf3, 0x0f, 0x11, 0x85, 0x5c, 0x03, 0x00, 0x00};

/* pushad; push ebp; call hook_ramp; add esp, 4; popad; jmp 0x16b06a; the rest int3. The ramp's
 * xmm0/xmm1 are not read after it (the code after it loads them again). */
static void write_ramp(void) {
    uint8_t *p = (uint8_t *)(uintptr_t)HUSPEED_RAMP_SITE, *q = p;
    *q++ = 0x60;
    *q++ = 0x55;
    int32_t rel = (int32_t)((uintptr_t)hook_ramp - (uintptr_t)(q + 5));
    *q++ = 0xe8; memcpy(q, &rel, 4); q += 4;
    *q++ = 0x83; *q++ = 0xc4; *q++ = 0x04;
    *q++ = 0x61;
    rel = (int32_t)(HUSPEED_RAMP_END - (uint32_t)(uintptr_t)(q + 5));
    *q++ = 0xe9; memcpy(q, &rel, 4); q += 4;
    while (q < p + sizeof sig_ramp) *q++ = 0xcc;
}

__attribute__((force_align_arg_pointer, cdecl, used)) static void hook_target(uint32_t human, float *target) {
    *target = huspeed_target(human, *target);
}

static const uint8_t sig_site[] = {0x8b, 0xce, 0xe8};   /* mov ecx, esi; call rel32 (to 0x2b17a0) */

static int site_ok(void) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)HUSPEED_SITE;
    int32_t rel;
    memcpy(&rel, p + 3, 4);
    return !memcmp(p, sig_site, sizeof sig_site) && HUSPEED_SITE + 7 + (uint32_t)rel == HUSPEED_SPEED_FN;
}

/* The site calls the stub instead of 0x2b17a0:
 *   pushad; lea eax, [esp + 0x24 + 0x10]; push eax; push esi; call hook_target; add esp, 8; popad;
 *   mov ecx, esi; jmp 0x2b17a0      (it returns to the site's next instruction)
 * The target [esp+0x10] of the site's frame is at esp + 0x20 (pushad) + 4 (our return address). */
static int patch_site(void) {
    if (!site_ok() || memcmp((const void *)(uintptr_t)HUSPEED_RAMP_SITE, sig_ramp, sizeof sig_ramp)) {
        printf("[huspeed] skip speed caps at %08x and %08x: unexpected bytes\n", HUSPEED_SITE, HUSPEED_RAMP_SITE);
        return -1;
    }
    uint8_t *s = plat_alloc_exec(32), *q = s;
    if (!s) return -1;
    *q++ = 0x60;
    *q++ = 0x8d; *q++ = 0x44; *q++ = 0x24; *q++ = (uint8_t)(0x24 + HUSPEED_TARGET_OFS);
    *q++ = 0x50;
    *q++ = 0x56;
    int32_t rel = (int32_t)((uintptr_t)hook_target - (uintptr_t)(q + 5));
    *q++ = 0xe8; memcpy(q, &rel, 4); q += 4;
    *q++ = 0x83; *q++ = 0xc4; *q++ = 0x08;
    *q++ = 0x61;
    *q++ = 0x8b; *q++ = 0xce;
    rel = (int32_t)(HUSPEED_SPEED_FN - (uint32_t)(uintptr_t)(q + 5));
    *q++ = 0xe9; memcpy(q, &rel, 4); q += 4;
    uint8_t *p = (uint8_t *)(uintptr_t)HUSPEED_SITE;
    rel = (int32_t)((uintptr_t)s - (uintptr_t)(p + 5));
    p[0] = 0xe8; memcpy(p + 1, &rel, 4);
    p[5] = 0x90; p[6] = 0x90;
    write_ramp();
    printf("[huspeed] speed caps at %08x (AI target speed) and %08x (the player's speed multiplier)\n", HUSPEED_SITE,
           HUSPEED_RAMP_SITE);
    return 1;
}

int huspeed_set(uint32_t handle, double scale, float *cap) {
    const uint32_t human = human_of(handle);
    if (!human) return -1;
    const int i = slot_of(human);
    if ((caps[i].used || caps[i].rising) && caps[i].handle != handle) caps[i].used = caps[i].rising = 0;
    if (scale >= 0) {
        const float c = (float)(scale < HUSPEED_MIN ? HUSPEED_MIN : scale > 1 ? 1 : scale);
        if (c < 1.0f && !patched) patched = patch_site();
        if (c < 1.0f && patched == 1) {
            caps[i].handle = handle;
            caps[i].cap = c;
            caps[i].used = 1;
            caps[i].rising = 1;
        } else if (caps[i].used) {
            caps[i].used = 0;   /* the multiplier rises back (rising stays until it is at 1) */
        }
    }
    if (cap) *cap = caps[i].used ? caps[i].cap : 1.0f;
    return 0;
}

int huspeed_patched(void) { return patched; }

int huspeed_active(void) {
    int n = 0;
    for (int i = 0; i < HUSPEED_SLOTS; i++) n += caps[i].used;
    return n;
}

static void (*push_number)(uint32_t L, double n);
static void (*push_nil)(uint32_t L);

void huspeed_lua_init(void (*pushnumber)(uint32_t L, double n), void (*pushnil)(uint32_t L)) {
    push_number = pushnumber;
    push_nil = pushnil;
}

/* ModSpeed(character, scale): the character's cap after the call, or nil. */
__attribute__((force_align_arg_pointer, cdecl)) int hle_huspeed_lua(uint32_t L) {
    const DevLuaApi *a = &devlua_title_api;
    double scale = -1;
    float c = 1;
    int ok = -1;
    if (a->type(L, 1) == 2) {
        const double h = strtod(a->tostring(L, 1), NULL);
        if (a->type(L, 2) == 2) scale = strtod(a->tostring(L, 2), NULL);
        if (isfinite(h) && h > 0 && h < 4294967296.0 && isfinite(scale)) ok = huspeed_set((uint32_t)h, scale, &c);
    }
    if (ok == 0 && push_number) push_number(L, c);
    else if (push_nil) push_nil(L);
    return 1;
}

#ifdef HLE_HUSPEED_TEST
/* Game-independent test: caps on planted humans and handles, and the hook on a planted copy of the
 * site inside a frame like the locomotion function's, run with esi = the human. */
#include <sys/mman.h>

const DevLuaApi devlua_title_api;   /* not used by the test */

/* Calls fn with esi = arg; returns eax. ebx, edi, ebp must come back unchanged (checked in *kept). */
__asm__(".text\n.globl esicall\nesicall:\n"
        "push %ebp\npush %ebx\npush %esi\npush %edi\n"
        "mov 20(%esp),%eax\nmov 24(%esp),%esi\nmov $0x1234,%ebx\nmov $0x9abc,%edi\nmov $0x5678,%ebp\n"
        "call *%eax\n"
        "mov 28(%esp),%ecx\nmovl $0,(%ecx)\n"
        "cmp $0x1234,%ebx\njne 1f\ncmp $0x9abc,%edi\njne 1f\ncmp $0x5678,%ebp\njne 1f\nmov 24(%esp),%edx\ncmp %edx,%esi\njne 1f\n"
        "movl $1,(%ecx)\n1:\n"
        "pop %edi\npop %esi\npop %ebx\npop %ebp\nret\n");
uint32_t esicall(uint32_t fn, uint32_t esi, int *kept);

/* Calls fn with ebp = arg; 0 when ebx, esi, edi came back unchanged. */
__asm__(".text\n.globl ebpcall\nebpcall:\n"
        "push %ebp\npush %ebx\npush %esi\npush %edi\n"
        "mov 20(%esp),%eax\nmov 24(%esp),%ebp\nmov $0x1234,%ebx\nmov $0x5678,%esi\nmov $0x9abc,%edi\n"
        "call *%eax\n"
        "mov %ebx,%eax\nxor $0x1234,%eax\nmov %esi,%ecx\nxor $0x5678,%ecx\nor %ecx,%eax\nmov %edi,%ecx\nxor $0x9abc,%ecx\nor %ecx,%eax\n"
        "pop %edi\npop %esi\npop %ebx\npop %ebp\nret\n");
uint32_t ebpcall(uint32_t fn, uint32_t ebp);

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)
#define AT(a) ((uint8_t *)(uintptr_t)(a))
#define U32(a) (*(uint32_t *)(uintptr_t)(a))
#define F32(a) (*(float *)(uintptr_t)(a))
#define MULT(h) F32((h) + HUSPEED_MULT)

static void map_at(uint32_t lo, uint32_t len) {
    void *m = mmap((void *)(uintptr_t)lo, len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m != (void *)(uintptr_t)lo) { perror("mmap"); exit(1); }
    memset(m, 0xcc, len);
}

static uint32_t human(int i) { return HUSPEED_POOL + (uint32_t)i * HUSPEED_STRIDE; }
static uint32_t handle(int slot, uint32_t serial) { return ((uint32_t)slot << 16) | serial; }
static void plant_handle(int slot, uint32_t serial, uint32_t obj) {
    U32(HUSPEED_HANDLES + 8u * (uint32_t)slot) = obj;
    U32(HUSPEED_HANDLES + 8u * (uint32_t)slot + 4) = serial;
}

#define FRAME 0x16b840u    /* a stand-in for the locomotion function around the site */
#define SEEN_ECX 0x2b1f00u /* the planted 0x2b17a0 stores the ecx it got here */

/* FRAME: sub esp, 0x20; mov dword [esp+0x10], TARGET; jmp site.
 * site: mov ecx, esi; call 0x2b17a0.  after: mov eax, [esp+0x10]; add esp, 0x20; ret.
 * 0x2b17a0: mov [SEEN_ECX], ecx; ret. */
static void plant(void) {
    uint8_t *f = AT(FRAME);
    *f++ = 0x83; *f++ = 0xec; *f++ = 0x20;
    *f++ = 0xc7; *f++ = 0x44; *f++ = 0x24; *f++ = 0x10; f += 4;   /* imm32 set per call */
    int32_t rel = (int32_t)(HUSPEED_SITE - (uint32_t)(uintptr_t)(f + 5));
    *f++ = 0xe9; memcpy(f, &rel, 4);
    uint8_t *p = AT(HUSPEED_SITE);
    p[0] = 0x8b; p[1] = 0xce; p[2] = 0xe8;
    rel = (int32_t)(HUSPEED_SPEED_FN - (HUSPEED_SITE + 7));
    memcpy(p + 3, &rel, 4);
    static const uint8_t after[] = {0x8b, 0x44, 0x24, 0x10, 0x83, 0xc4, 0x20, 0xc3};
    memcpy(p + 7, after, sizeof after);
    uint8_t *g = AT(HUSPEED_SPEED_FN);
    g[0] = 0x89; g[1] = 0x0d; U32(HUSPEED_SPEED_FN + 2) = SEEN_ECX; g[6] = 0xc3;
    memcpy(AT(HUSPEED_RAMP_SITE), sig_ramp, sizeof sig_ramp);
    AT(HUSPEED_RAMP_END)[0] = 0xc3;   /* ret: back to ebpcall */
    F32(0x40ffc8) = 1.0f;
    F32(0x42e938) = HUSPEED_RAMP;
}

static void test_retail_ramp(void) {
    /* huspeed_update and the retail ramp bytes agree for a character without a cap */
    plant();
    const float start[] = {0.65f, 0.3f, 0.999f, 1.0f, 1.2f, 0.0f};
    int same = 1;
    for (unsigned k = 0; k < sizeof start / sizeof start[0]; k++) {
        MULT(human(3)) = start[k];
        MULT(human(4)) = start[k];
        for (int n = 0; n < 300; n++) {
            ebpcall(HUSPEED_RAMP_SITE, human(3));
            huspeed_update(human(4));
            if (MULT(human(3)) != MULT(human(4))) same = 0;
        }
    }
    CHECK(same, "no cap: huspeed_update matches the retail ramp bytes (300 updates from 0.65, 0.3, 0.999, 1, 1.2, 0)");
}

static float run_site(uint32_t h, float target, int *kept) {
    memcpy(AT(FRAME + 7), &target, 4);
    U32(SEEN_ECX) = 0;
    uint32_t r = esicall(FRAME, h, kept);
    float out;
    memcpy(&out, &r, 4);
    if (U32(SEEN_ECX) != h) *kept = 0;   /* 0x2b17a0 must still get ecx = the human */
    return out;
}

static void test_caps(void) {
    float c = -1;
    int kept = 0;
    plant();
    plant_handle(7, 0x21, human(2));
    plant_handle(8, 0x22, 0x1000);   /* not a human of the pool */
    CHECK(huspeed_set(handle(7, 0x20), 0.5, &c) == -1 && huspeed_set(handle(8, 0x22), 0.5, &c) == -1 &&
          huspeed_set(0, 0.5, &c) == -1, "refuse stale serials, non-humans and handle 0");
    CHECK(huspeed_set(handle(7, 0x21), -1, &c) == 0 && c == 1.0f && !huspeed_patched(), "read only: cap 1, no hook");
    CHECK(huspeed_set(handle(7, 0x21), 1, &c) == 0 && !huspeed_patched() && !huspeed_active(), "cap 1: nothing written");
    CHECK(run_site(human(2), 6.5f, &kept) == 6.5f && kept, "retail site: the target as chosen, ecx = the human");

    CHECK(huspeed_set(handle(7, 0x21), 0.65, &c) == 0 && c == 0.65f && huspeed_patched() == 1 &&
          AT(HUSPEED_SITE)[0] == 0xe8 && huspeed_active() == 1, "cap 0.65: hook written, one cap, read back");
    float t = run_site(human(2), 8.0f, &kept);
    CHECK(fabsf(t - 5.2f) < 1e-5f && kept, "hooked: 8 m/s target -> %.3f, ebx/edi/ebp/esi kept, 0x2b17a0 still called with ecx", t);
    MULT(human(2)) = 1.0f;
    CHECK(ebpcall(HUSPEED_RAMP_SITE, human(2)) == 0 && MULT(human(2)) == 0.65f && AT(HUSPEED_RAMP_SITE)[0] == 0x60,
          "ramp hooked: the player's multiplier 1.0 -> 0.65, ebx/esi/edi kept, back after the ramp");
    MULT(human(2)) = 0.4f;   /* the title's own recovery state below the cap */
    ebpcall(HUSPEED_RAMP_SITE, human(2));
    CHECK(MULT(human(2)) == 0.4f * HUSPEED_RAMP, "below the cap: the retail ramp");
    MULT(human(2)) = 0.65f;
    plant_handle(9, 0x31, human(5));
    CHECK(run_site(human(5), 8.0f, &kept) == 8.0f && kept, "another character: untouched");
    huspeed_set(handle(7, 0x21), 0.1, &c);
    CHECK(c == 0.3f && fabsf(run_site(human(2), 8.0f, &kept) - 2.4f) < 1e-5f, "cap 0.1 clamps to 0.3");
    huspeed_set(handle(9, 0x31), 0.8, &c);
    CHECK(fabsf(run_site(human(5), 5.0f, &kept) - 4.0f) < 1e-5f && fabsf(run_site(human(2), 5.0f, &kept) - 1.5f) < 1e-5f &&
          huspeed_active() == 2, "per character: 0.8 and 0.3");
    huspeed_set(handle(7, 0x21), 1, &c);
    CHECK(c == 1.0f && run_site(human(2), 5.0f, &kept) == 5.0f && huspeed_active() == 1, "cap 1: the target at once");
    MULT(human(2)) = 0.3f;
    int n30 = 0;
    while (MULT(human(2)) < 1.0f && n30 < 10000) { huspeed_update(human(2)); n30++; }
    CHECK(n30 > 550 && n30 < 620, "cap 1: the multiplier rises back at the retail rate, 0.3 -> 1 in %d updates at 30 Hz", n30);
    huspeed_sim60 = 1;
    huspeed_set(handle(7, 0x21), 0.3, &c);
    huspeed_update(human(2));
    huspeed_set(handle(7, 0x21), 1, &c);
    int n60 = 0;
    while (MULT(human(2)) < 1.0f && n60 < 10000) { huspeed_update(human(2)); n60++; }
    CHECK(n60 >= 2 * n30 - 4 && n60 <= 2 * n30 + 4, "with --sim60: %d updates (twice %d, the same seconds)", n60, n30);
    huspeed_sim60 = 0;
    MULT(human(2)) = 0.65f;
    huspeed_update(human(2));
    CHECK(MULT(human(2)) == 0.65f * HUSPEED_RAMP, "back at 1 once: the title's ramp untouched again");
    CHECK(run_site(human(5), 0.0f, &kept) == 0.0f, "standing still stays still");

    /* a new character in the slot (another serial) does not inherit the cap */
    huspeed_set(handle(7, 0x21), 0.5, &c);
    plant_handle(7, 0x23, human(2));
    CHECK(run_site(human(2), 5.0f, &kept) == 5.0f && huspeed_active() == 1, "a new character in the slot: no cap");
    CHECK(huspeed_set(handle(7, 0x23), -1, &c) == 0 && c == 1.0f, "the new character reads cap 1");
}

static void test_refuse_other_bytes(void) {
    patched = 0;
    memset(caps, 0, sizeof caps);
    plant();
    int32_t rel = (int32_t)(HUSPEED_SPEED_FN + 0x10 - (HUSPEED_SITE + 7));   /* a call elsewhere */
    memcpy(AT(HUSPEED_SITE + 3), &rel, 4);
    uint8_t before[7], ramp[sizeof sig_ramp];
    memcpy(before, AT(HUSPEED_SITE), sizeof before);
    memcpy(ramp, AT(HUSPEED_RAMP_SITE), sizeof ramp);
    float c;
    plant_handle(9, 0x31, human(5));
    huspeed_set(handle(9, 0x31), 0.5, &c);
    CHECK(huspeed_patched() == -1 && !memcmp(before, AT(HUSPEED_SITE), sizeof before) &&
          !memcmp(ramp, AT(HUSPEED_RAMP_SITE), sizeof ramp) && !huspeed_active() && c == 1.0f,
          "other bytes at the target site: nothing written at either site, no cap kept");
    patched = 0;
    plant();
    AT(HUSPEED_RAMP_SITE + 29)[0] = 0x00;   /* the ramp with another constant address */
    memcpy(before, AT(HUSPEED_SITE), sizeof before);
    huspeed_set(handle(9, 0x31), 0.5, &c);
    CHECK(huspeed_patched() == -1 && !memcmp(before, AT(HUSPEED_SITE), sizeof before) && !huspeed_active(),
          "other bytes at the ramp: nothing written at either site");
}

int main(void) {
    map_at(0x16b000, 0x1000);
    map_at(0x2b1000, 0x1000);
    map_at(0x40f000, 0x1000);
    map_at(0x42e000, 0x1000);
    map_at(0x4ff000, 0x19000);
    map_at(0x5a7000, 0x1000);
    memset(AT(0x5a7000), 0, 0x1000);
    test_retail_ramp();
    test_caps();
    test_refuse_other_bytes();
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
#endif
