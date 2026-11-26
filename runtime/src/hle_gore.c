/* Blood settings for mods (see hle_gore.h). */
#include "hle_gore.h"
#include "cinema_bars.h"
#include "hle_devlua.h"
#include "platform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int hle_gore_trace;

static GoreConfig cfg = {1.0, 0.0, 1.0, 128, 0};
static int patched_drops, patched_hold, patched_char;
static uint32_t level_state;   /* Lua state of the last ModBlood call */

/* Held pools, oldest first (a ring). A pool is still ours while its phase duration is the marker
 * we wrote (the hold); a task freed and reused for something else no longer carries it. */
static uint32_t ring[GORE_POOLS_MAX];
static int ring_head, ring_count;
static float hold_marker;

/* Lowest health percent per character: the human object and the handle that named it (handle
 * slot << 16 | serial). The title reuses a human's memory, and its body record (+0xb0), for the
 * next character of that pool slot, e.g. the actors of a cutscene; the handle's serial tells them
 * apart. GORE_NO_HANDLE: none named it yet. */
#define GORE_NO_HANDLE 0xffffffffu
static struct { uint32_t human, handle; float low; } chars[GORE_CHARS];
static volatile int in_cutscene;   /* the last frame drawn showed the cutscene letterbox bars */

static struct { unsigned effects, drops_in, drops_out, held, evicted; } stats;

static uint32_t rd32(uint32_t a) { return *(const volatile uint32_t *)(uintptr_t)a; }

int gore_drops(int n) {
    __atomic_add_fetch(&stats.effects, 1, __ATOMIC_RELAXED);
    int m = n;
    if (n >= 1 && cfg.drops > 1) {   /* every punch seen gives 1; more takes the mist + drops path */
        double v = n * cfg.drops + 0.5;
        m = v > 64 ? 64 : (int)v;
        if (m < n) m = n;
    }
    __atomic_add_fetch(&stats.drops_in, (unsigned)(n > 0 ? n : 0), __ATOMIC_RELAXED);
    __atomic_add_fetch(&stats.drops_out, (unsigned)(m > 0 ? m : 0), __ATOMIC_RELAXED);
    if (hle_gore_trace) {
        static int lines;
        if (lines++ < 200) printf("[blood] effect: %d drops%s\n", n, m != n ? " (more)" : "");
    }
    return m;
}

static void set_phase(uint32_t task, float seconds) {
    double f = seconds * 60.0;
    *(volatile float *)(uintptr_t)(task + 0x58) = seconds;
    *(volatile uint16_t *)(uintptr_t)(task + 0x62) = (uint16_t)(f > 65535 ? 65535 : f);
}

static int still_held(uint32_t task) {
    return *(const volatile float *)(uintptr_t)(task + 0x58) == hold_marker;
}

/* Free particle tasks above the title's blood threshold (0x2b4b40); tests replace it. */
static int title_room(void) { return ((int(__attribute__((cdecl)) *)(int))(uintptr_t)GORE_ROOM_FN)(0) & 0xff; }
static int (*room_fn)(void) = title_room;

int gore_pool_end(uint32_t task) {
    uint16_t ticks = *(const volatile uint16_t *)(uintptr_t)(task + 0x62);
    if (cfg.hold > 0 && ticks == GORE_RETAIL_HOLD && room_fn()) {
        /* the retail hold just ended at full size: hold on at that size */
        set_phase(task, hold_marker);
        stats.held++;
        while (ring_count && !still_held(ring[ring_head])) {   /* pools already gone */
            ring_head = (ring_head + 1) % GORE_POOLS_MAX;
            ring_count--;
        }
        while (ring_count >= cfg.pool_max) {
            uint32_t old = ring[ring_head];
            ring_head = (ring_head + 1) % GORE_POOLS_MAX;
            ring_count--;
            if (old != task && still_held(old)) {
                set_phase(old, 0.0f);   /* its hold ends at its next update; the fade follows */
                stats.evicted++;
            }
        }
        ring[(ring_head + ring_count) % GORE_POOLS_MAX] = task;
        ring_count++;
        if (hle_gore_trace && stats.held % 10 == 1)
            printf("[blood] pools: %u held so far, %d held now, %u pushed out\n", stats.held, ring_count, stats.evicted);
        return 1;
    }
    set_phase(task, 3.0f);   /* the fade: 3 s, as the retail fade (it keeps the 3 s hold's duration) */
    return 0;
}

/* The character handle that names human now (slot << 16 | serial), GORE_NO_HANDLE when none does
 * (e.g. a knocked-out fighter still fading out). hint: the handle to check first. */
static uint32_t handle_of(uint32_t human, uint32_t hint) {
    uint32_t i = hint >> 16;
    if (hint != GORE_NO_HANDLE && i < GORE_HANDLE_CHARS && rd32(GORE_HANDLES + 8u * i) == human)
        return i << 16 | (rd32(GORE_HANDLES + 8u * i + 4) & 0xffff);
    for (i = 0; i < GORE_HANDLE_CHARS; i++)
        if (rd32(GORE_HANDLES + 8u * i) == human) return i << 16 | (rd32(GORE_HANDLES + 8u * i + 4) & 0xffff);
    return GORE_NO_HANDLE;
}

float gore_char_pct(uint32_t human, float pct) {
    if (__atomic_load_n(&in_cutscene, __ATOMIC_RELAXED)) return pct;   /* retail; nothing recorded */
    if (cfg.keep) {
        int i, free_slot = -1;
        for (i = 0; i < GORE_CHARS; i++) {
            if (chars[i].human == human) break;
            if (!chars[i].human && free_slot < 0) free_slot = i;
        }
        if (i == GORE_CHARS) {
            i = free_slot >= 0 ? free_slot : (int)((human >> 4) % GORE_CHARS);
            chars[i].human = human;
            chars[i].handle = handle_of(human, GORE_NO_HANDLE);
            chars[i].low = pct;
        } else {
            uint32_t hd = handle_of(human, chars[i].handle);
            if (hd != GORE_NO_HANDLE && hd != chars[i].handle) {
                if (chars[i].handle != GORE_NO_HANDLE) chars[i].low = pct;   /* another character here */
                chars[i].handle = hd;
            }
        }
        if (pct < chars[i].low) chars[i].low = pct;
        pct = chars[i].low;
    }
    if (cfg.gain > 1) {
        pct = 100.0f - (100.0f - pct) * (float)cfg.gain;
        if (pct < 0) pct = 0;
    }
    return pct;
}

void gore_reset_level(void) {
    if (stats.effects || stats.held)
        printf("[blood] level done: %u blood effects (%u drops, %u after \"drops\"), %u pools held, %u faded early "
               "(over pool_max)\n", stats.effects, stats.drops_in, stats.drops_out, stats.held, stats.evicted);
    memset(&stats, 0, sizeof stats);
    ring_head = ring_count = 0;
    memset(chars, 0, sizeof chars);
}

GoreConfig gore_config(void) { return cfg; }

void gore_set_cutscene(int on) {
    if (hle_gore_trace && on != in_cutscene) printf("[blood] cutscene %s: fighters' blood %s\n", on ? "starts" : "ends",
                                                    on ? "as the title" : "as set");
    __atomic_store_n(&in_cutscene, on != 0, __ATOMIC_RELAXED);
}

/* ---- patches ---- */

/* The hook stubs. Each runs where the title's code was and returns to the next instruction. */
typedef float __attribute__((fastcall)) (*PctFn)(uint32_t human);

__attribute__((force_align_arg_pointer, cdecl, used)) static int hook_pool_end(uint32_t task) { return gore_pool_end(task); }

__attribute__((force_align_arg_pointer, cdecl, used)) static int hook_drops(int n) { return gore_drops(n); }

__attribute__((force_align_arg_pointer, fastcall, used)) static float hook_char(uint32_t human) {
    float pct = ((PctFn)(uintptr_t)GORE_HEALTH_PCT_FN)(human);
    return gore_char_pct(human, pct);
}

static uint8_t *emit_call(uint8_t *q, const void *target) {
    int32_t rel = (int32_t)((uintptr_t)target - (uintptr_t)(q + 5));
    *q++ = 0xe8;
    memcpy(q, &rel, 4);
    return q + 4;
}

static int rel_target_ok(const uint8_t *p, uint32_t site, uint32_t target) {
    int32_t rel;
    memcpy(&rel, p + 1, 4);
    return p[0] == 0xe8 && site + 5 + (uint32_t)rel == target;
}

static void write_call(uint32_t site, const void *stub, int len) {
    uint8_t *p = (uint8_t *)(uintptr_t)site;
    emit_call(p, stub);
    for (int i = 5; i < len; i++) p[i] = 0x90;
}

/* Pool hold: at the end of the retail hold (phase 4 starts the fade) the stub asks gore_pool_end;
 * 1 = a new long phase at the same size and colour, the state is not advanced (the handler's own
 * "continue" exit, 0x2bf520: pop edi; xor al, al; pop esi; add esp, 0x408; ret); 0 = the fade, as
 * the title (the replaced instruction, then the rest of the phase-4 branch). */
static int patch_hold(void) {
    static const uint8_t sig_fade[] = {0x8b, 0x86, 0xa4, 0x00, 0x00, 0x00};
    static const uint8_t sig_exit[] = {0xff, 0x07, 0x5f, 0x32, 0xc0, 0x5e, 0x81, 0xc4, 0x08, 0x04, 0x00, 0x00};
    if (memcmp((const uint8_t *)(uintptr_t)GORE_FADE_SITE, sig_fade, sizeof sig_fade) ||
        memcmp((const uint8_t *)(uintptr_t)(GORE_CONTINUE_EXIT - 2), sig_exit, sizeof sig_exit)) {
        printf("[blood] skip pool hold at %08x: unexpected bytes\n", GORE_FADE_SITE);
        return 0;
    }
    uint8_t *s = plat_alloc_exec(64), *q = s;
    if (!s) return 0;
    *q++ = 0x60; *q++ = 0x56;                               /* pushad; push esi */
    q = emit_call(q, (const void *)hook_pool_end);
    *q++ = 0x83; *q++ = 0xc4; *q++ = 0x04;                  /* add esp, 4 */
    *q++ = 0x85; *q++ = 0xc0;                               /* test eax, eax */
    *q++ = 0x61;                                            /* popad (flags kept) */
    *q++ = 0x75; *q++ = (uint8_t)(sizeof sig_fade + 1);     /* jnz hold */
    memcpy(q, sig_fade, sizeof sig_fade); q += sizeof sig_fade;
    *q++ = 0xc3;                                            /* ret: back into the phase-4 branch */
    *q++ = 0x83; *q++ = 0xc4; *q++ = 0x04;                  /* hold: add esp, 4 (our return address) */
    int32_t rel = (int32_t)(GORE_CONTINUE_EXIT - (uint32_t)(uintptr_t)(q + 5));
    *q++ = 0xe9; memcpy(q, &rel, 4); q += 4;                /* jmp 0x2bf520 */
    write_call(GORE_FADE_SITE, s, 6);
    printf("[blood] pool hold at %08x\n", GORE_FADE_SITE);
    return 1;
}

/* Drop count: call 0x2b5f10 (the count), then eax = hook_drops(eax). */
static int patch_drops(void) {
    if (!rel_target_ok((const uint8_t *)(uintptr_t)GORE_DROPS_SITE, GORE_DROPS_SITE, GORE_POP_INT_FN)) {
        printf("[blood] skip drop count at %08x: unexpected bytes\n", GORE_DROPS_SITE);
        return 0;
    }
    uint8_t *s = plat_alloc_exec(32), *q = s;
    if (!s) return 0;
    q = emit_call(q, (const void *)(uintptr_t)GORE_POP_INT_FN);
    *q++ = 0x50;                                   /* push eax */
    q = emit_call(q, (const void *)hook_drops);
    *q++ = 0x83; *q++ = 0xc4; *q++ = 0x04; *q++ = 0xc3;
    write_call(GORE_DROPS_SITE, s, 5);
    printf("[blood] drop count at %08x\n", GORE_DROPS_SITE);
    return 1;
}

/* Character blood stage: the health-percent call goes to hook_char (same convention). */
static int patch_char(void) {
    if (!rel_target_ok((const uint8_t *)(uintptr_t)GORE_CHAR_SITE, GORE_CHAR_SITE, GORE_HEALTH_PCT_FN)) {
        printf("[blood] skip character blood at %08x: unexpected bytes\n", GORE_CHAR_SITE);
        return 0;
    }
    write_call(GORE_CHAR_SITE, (const void *)hook_char, 5);
    cinema_bars_listener = gore_set_cutscene;
    printf("[blood] character blood stage at %08x\n", GORE_CHAR_SITE);
    return 1;
}

static int parse_num(const char *s, double *out) {
    if (!s || !*s) return -1;
    char *end;
    double v = strtod(s, &end);
    while (*end == ' ') end++;
    if (*end || !isfinite(v)) return -1;
    *out = v;
    return 0;
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

int gore_set(const char *option, const char *value) {
    double v;
    if (!option || parse_num(value, &v)) return -1;
    if (!strcmp(option, "drops")) {
        cfg.drops = clampd(v, 1, 4);
        if (cfg.drops > 1 && !patched_drops) patched_drops = patch_drops() ? 1 : -1;
    } else if (!strcmp(option, "pool_hold")) {
        cfg.hold = v <= 0 ? 0 : clampd(v, GORE_HOLD_MIN, GORE_HOLD_MAX);
        hold_marker = (float)cfg.hold;
        if (cfg.hold > 0 && !patched_hold) patched_hold = patch_hold() ? 1 : -1;
    } else if (!strcmp(option, "pool_max")) {
        cfg.pool_max = (int)clampd(v, 16, GORE_POOLS_MAX);
    } else if (!strcmp(option, "char_gain")) {
        cfg.gain = clampd(v, 1, 4);
        if (cfg.gain > 1 && !patched_char) patched_char = patch_char() ? 1 : -1;
    } else if (!strcmp(option, "char_keep")) {
        cfg.keep = v != 0;
        if (cfg.keep && !patched_char) patched_char = patch_char() ? 1 : -1;
    } else {
        return -1;
    }
    return 0;
}

/* ModBlood(option, value). Logs `[blood] option = value` (or why it was refused). */
__attribute__((force_align_arg_pointer, cdecl)) int hle_gore_lua(uint32_t L) {
    const DevLuaApi *a = &devlua_title_api;
    const char *opt = a->type(L, 1) == 3 ? a->tostring(L, 1) : NULL;
    const char *val = a->type(L, 2) == 2 || a->type(L, 2) == 3 ? a->tostring(L, 2) : NULL;
    if (L != level_state) {
        level_state = L;
        gore_reset_level();
    }
    if (gore_set(opt, val))
        printf("[blood] ModBlood(%.40s, %.40s): unknown option or not a number; ignored\n", opt ? opt : "nil", val ? val : "nil");
    else if (hle_gore_trace)
        printf("[blood] %s = %s\n", opt, val);
    fflush(stdout);
    return 0;
}

#ifdef HLE_GORE_TEST
/* Game-independent test: options, the hook bodies on planted objects, and the patches on planted
 * copies of the title's bytes at their guest addresses, run with the guest's registers. */
#include <sys/mman.h>

const DevLuaApi devlua_title_api;   /* not used by the test */

/* Calls fn with eax, ecx, edx, ebx, esi, edi loaded from r[0..5] and stores them back. */
__asm__(".text\n.globl regcall\nregcall:\n"
        "push %ebp\npush %ebx\npush %esi\npush %edi\n"
        "mov 20(%esp),%eax\nmov 24(%esp),%ebp\npush %eax\npush %ebp\n"
        "mov 0(%ebp),%eax\nmov 4(%ebp),%ecx\nmov 8(%ebp),%edx\nmov 12(%ebp),%ebx\nmov 16(%ebp),%esi\nmov 20(%ebp),%edi\n"
        "call *4(%esp)\n"
        "xchg %ebp,(%esp)\n"
        "mov %eax,0(%ebp)\nmov %ecx,4(%ebp)\nmov %edx,8(%ebp)\nmov %ebx,12(%ebp)\nmov %esi,16(%ebp)\nmov %edi,20(%ebp)\n"
        "add $8,%esp\npop %edi\npop %esi\npop %ebx\npop %ebp\nret\n");
void regcall(uint32_t fn, uint32_t *r);

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)
#define AT(a) ((uint8_t *)(uintptr_t)(a))
#define F32(a) (*(float *)(uintptr_t)(a))
#define U16(a) (*(uint16_t *)(uintptr_t)(a))

static void map_at(uint32_t lo, uint32_t len) {
    void *m = mmap((void *)(uintptr_t)lo, len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m != (void *)(uintptr_t)lo) { perror("mmap"); exit(1); }
    memset(m, 0xcc, len);
}

static void plant_call(uint32_t site, uint32_t target) {
    int32_t rel = (int32_t)(target - (site + 5));
    AT(site)[0] = 0xe8;
    memcpy(AT(site + 1), &rel, 4);
}

static void plant_handle(int slot, uint32_t serial, uint32_t obj) {
    *(uint32_t *)(uintptr_t)(GORE_HANDLES + 8u * (uint32_t)slot) = obj;
    *(uint32_t *)(uintptr_t)(GORE_HANDLES + 8u * (uint32_t)slot + 4) = serial;
}

#define HANDLER 0x2bf000u   /* a stand-in for the phase-end handler's frame around the site */
static int room = 1;
static int fake_room(void) { return room; }

static void plant(void) {
    /* handler: sub esp, 0x408; push esi; push edi; jmp 0x2bf4ea (the phase-4 branch) */
    static const uint8_t handler[] = {0x81, 0xec, 0x08, 0x04, 0x00, 0x00, 0x56, 0x57};
    memcpy(AT(HANDLER), handler, sizeof handler);
    AT(HANDLER + 8)[0] = 0xe9;
    int32_t rel = (int32_t)(GORE_FADE_SITE - (HANDLER + 13));
    memcpy(AT(HANDLER + 9), &rel, 4);
    static const uint8_t fade[] = {0x8b, 0x86, 0xa4, 0x00, 0x00, 0x00};
    memcpy(AT(GORE_FADE_SITE), fade, sizeof fade);
    /* rest of the phase-4 branch, here: pop edi; mov al, 0x77 (fade); pop esi; add esp, 0x408; ret */
    static const uint8_t fade_rest[] = {0x5f, 0xb0, 0x77, 0x5e, 0x81, 0xc4, 0x08, 0x04, 0x00, 0x00, 0xc3};
    memcpy(AT(GORE_FADE_SITE + 6), fade_rest, sizeof fade_rest);
    /* inc dword [edi]; then the "go on" exit (al = 0) */
    static const uint8_t exit_[] = {0xff, 0x07, 0x5f, 0x32, 0xc0, 0x5e, 0x81, 0xc4, 0x08, 0x04, 0x00, 0x00, 0xc3};
    memcpy(AT(GORE_CONTINUE_EXIT - 2), exit_, sizeof exit_);
    room_fn = fake_room;
    plant_call(GORE_DROPS_SITE, GORE_POP_INT_FN);
    AT(GORE_DROPS_SITE + 5)[0] = 0xc3;
    static const uint8_t pop3[] = {0xb8, 0x03, 0x00, 0x00, 0x00, 0xc3};   /* mov eax, 3; ret */
    memcpy(AT(GORE_POP_INT_FN), pop3, sizeof pop3);
    plant_call(GORE_CHAR_SITE, GORE_HEALTH_PCT_FN);
    AT(GORE_CHAR_SITE + 5)[0] = 0xd9; AT(GORE_CHAR_SITE + 6)[0] = 0x1f;   /* fstp dword [edi] */
    AT(GORE_CHAR_SITE + 7)[0] = 0xc3;
    /* health percent = the float at [ecx] (fastcall human) */
    static const uint8_t pct[] = {0xd9, 0x01, 0xc3};   /* fld dword [ecx]; ret */
    memcpy(AT(GORE_HEALTH_PCT_FN), pct, sizeof pct);
}

static void test_options(void) {
    CHECK(gore_set("drops", "abc") == -1 && gore_set("nope", "1") == -1 && gore_set(NULL, "1") == -1 &&
          gore_set("drops", NULL) == -1 && gore_set("drops", "") == -1 && gore_set("drops", "2x") == -1,
          "refuse unknown options and non-numbers");
    GoreConfig c = gore_config();
    CHECK(c.drops == 1 && c.hold == 0 && c.gain == 1 && c.pool_max == 128 && !c.keep, "defaults are retail");
    CHECK(!patched_drops && !patched_hold && !patched_char, "nothing patched before an option leaves its retail value");
    CHECK(gore_drops(5) == 5 && gore_drops(1) == 1 && gore_drops(0) == 0, "drops 1: counts unchanged");
}

static void test_hooks_on_planted_code(void) {
    map_at(0x8f000, 0x1000);
    map_at(0x140000, 0x1000);
    map_at(0x2b1000, 0x1000);
    map_at(0x2b5000, 0x1000);
    map_at(0x2bf000, 0x1000);
    map_at(0x2c2000, 0x1000);
    plant();
    uint8_t before[6];
    memcpy(before, AT(GORE_FADE_SITE), 6);
    CHECK(gore_set("pool_max", "8") == 0 && gore_config().pool_max == 16 && memcmp(before, AT(GORE_FADE_SITE), 6) == 0,
          "pool_max 8 clamps to 16 and alone writes nothing");

    /* drops: the planted pop returns 3; x2.5 -> 8 (rounded); 1 stays 1 */
    CHECK(gore_set("drops", "2.5") == 0 && patched_drops == 1 && AT(GORE_DROPS_SITE)[0] == 0xe8, "drops 2.5: site patched");
    uint32_t r[6] = {0x11111111, 0x22222222, 0x33333333, 0x44444444, 0x55555555, 0x66666666};
    regcall(GORE_DROPS_SITE, r);
    CHECK(r[0] == 8 && r[3] == 0x44444444 && r[4] == 0x55555555 && r[5] == 0x66666666, "drops: 3 -> 8, callee-saved kept");
    CHECK(gore_drops(1) == 3 && gore_drops(0) == 0 && gore_drops(40) == 64 && gore_set("drops", "9") == 0 &&
          gore_config().drops == 4, "drops: 1 -> 3, 0 stays 0, at most 64, value clamped to 4");
    gore_set("drops", "1");

    /* pool hold: the end of the retail hold (+0x62 = 180) holds on 600 s; ring of 16 */
    static uint8_t tasks[20][0x100];
    CHECK(gore_set("pool_hold", "600") == 0 && patched_hold == 1, "pool_hold 600: site patched");
    for (int i = 0; i < 20; i++) {
        uint32_t t = (uint32_t)(uintptr_t)tasks[i];
        *(uint32_t *)(tasks[i] + 0xa4) = 0xaabbcc00u + i;
        F32(t + 0x58) = 3.0000002f; U16(t + 0x62) = 180;
        uint32_t g[6] = {1, 2, 3, 4, t, 6};
        regcall(HANDLER, g);
        CHECK((g[0] & 0xff) == 0 && F32(t + 0x58) == 600.0f && U16(t + 0x62) == 36000 && g[1] == 2 && g[3] == 4 &&
              g[4] == t && g[5] == 6, "pool %d: held 600 s, state not advanced (go-on exit), registers kept", i);
    }
    int held = 0, ended = 0;
    for (int i = 0; i < 20; i++) {
        uint32_t t = (uint32_t)(uintptr_t)tasks[i];
        held += F32(t + 0x58) == 600.0f;
        ended += F32(t + 0x58) == 0.0f;
    }
    CHECK(held == 16 && ended == 4 && F32((uint32_t)(uintptr_t)tasks[0] + 0x58) == 0.0f &&
          F32((uint32_t)(uintptr_t)tasks[19] + 0x58) == 600.0f, "pool_max 16: the 4 oldest end their hold, 16 stay");
    uint32_t t0 = (uint32_t)(uintptr_t)tasks[0];
    uint32_t g[6] = {1, 2, 3, 4, t0, 6};
    regcall(HANDLER, g);
    CHECK((g[0] & 0xff) == 0x77 && (g[0] & ~0xffu) == 0xaabbcc00u && F32(t0 + 0x58) == 3.0f && U16(t0 + 0x62) == 180,
          "a pushed-out pool fades 3 s through the title's phase-4 branch (eax = [esi+0xa4])");
    uint32_t t19 = (uint32_t)(uintptr_t)tasks[19];
    uint32_t g1[6] = {1, 2, 3, 4, t19, 6};
    regcall(HANDLER, g1);
    CHECK((g1[0] & 0xff) == 0x77 && F32(t19 + 0x58) == 3.0f, "a pool whose long hold ended fades 3 s");
    /* a pool that left on its own (no marker) frees its ring slot without ending a held one */
    F32((uint32_t)(uintptr_t)tasks[5] + 0x58) = 1.0f;   /* the oldest still in the ring */
    static uint8_t extra[2][0x100];
    for (int k = 0; k < 2; k++) {
        uint32_t e = (uint32_t)(uintptr_t)extra[k];
        F32(e + 0x58) = 3.0f; U16(e + 0x62) = 180;
        uint32_t g2[6] = {1, 2, 3, 4, e, 6};
        regcall(HANDLER, g2);
    }
    CHECK(F32((uint32_t)(uintptr_t)tasks[6] + 0x58) == 600.0f && F32((uint32_t)(uintptr_t)extra[1] + 0x58) == 600.0f,
          "a pool already gone at the head of the ring frees its place instead of a held one");
    uint32_t e = (uint32_t)(uintptr_t)extra[0];
    room = 0;
    F32(e + 0x58) = 3.0f; U16(e + 0x62) = 180;
    uint32_t g4[6] = {1, 2, 3, 4, e, 6};
    regcall(HANDLER, g4);
    CHECK((g4[0] & 0xff) == 0x77 && F32(e + 0x58) == 3.0f, "512 or fewer free particle tasks: no hold, the title's fade");
    room = 1;
    gore_reset_level();
    gore_set("pool_hold", "0");
    F32(e + 0x58) = 3.0f; U16(e + 0x62) = 180;
    uint32_t g3[6] = {1, 2, 3, 4, e, 6};
    regcall(HANDLER, g3);
    CHECK((g3[0] & 0xff) == 0x77 && F32(e + 0x58) == 3.0f && U16(e + 0x62) == 180, "pool_hold 0 after patching: the retail fade");
    CHECK(gore_set("pool_hold", "3") == 0 && gore_config().hold == 10 && gore_set("pool_hold", "5000") == 0 &&
          gore_config().hold == 1000 && gore_set("pool_hold", "-1") == 0 && gore_config().hold == 0,
          "pool_hold: 3 -> 10, 5000 -> 1000, -1 -> 0");
    gore_set("pool_hold", "0");

    /* character blood stage */
    map_at(0x5a7000, 0x1000);   /* the handle table */
    static uint8_t human[0x200];
    uint32_t h = (uint32_t)(uintptr_t)human;
    float out = -1;
    uint32_t c[6] = {0, h, 0, 0, 0, (uint32_t)(uintptr_t)&out};
    F32(h) = 80.0f;
    *(uint32_t *)(human + 0xb0) = 0x1000;   /* the body record: the same for every character of the slot */
    plant_handle(7, 0x21, h);
    CHECK(gore_set("char_gain", "2") == 0 && patched_char == 1, "char_gain 2: site patched");
    regcall(GORE_CHAR_SITE, c);
    CHECK(out == 60.0f, "gain 2: 80 %% health shows as 60 %% (second blood texture)");
    gore_set("char_gain", "1");
    gore_set("char_keep", "1");
    c[1] = h; regcall(GORE_CHAR_SITE, c);
    F32(h) = 40.0f; c[1] = h; regcall(GORE_CHAR_SITE, c);
    F32(h) = 100.0f; c[1] = h; regcall(GORE_CHAR_SITE, c);
    CHECK(out == 40.0f, "keep: healed to 100 %% still shows the lowest, 40 %%");
    plant_handle(7, 0, 0);   /* his handle is freed while he is still drawn (knocked out, fading) */
    c[1] = h; regcall(GORE_CHAR_SITE, c);
    CHECK(out == 40.0f, "keep: a fighter whose handle is gone keeps his blood");
    plant_handle(7, 0x22, h);   /* a new character in the same memory, the same +0xb0, another serial */
    c[1] = h; regcall(GORE_CHAR_SITE, c);
    CHECK(out == 100.0f, "keep: a new character at a reused address (same +0xb0) starts clean");
    gore_set("char_gain", "2");
    c[1] = h; regcall(GORE_CHAR_SITE, c);
    CHECK(out == 100.0f, "keep + gain 2: a character never hit shows no blood");
    plant_handle(7, 0, 0);
    plant_handle(3, 0x40, h);   /* another one, under another handle slot */
    F32(h) = 90.0f; c[1] = h; regcall(GORE_CHAR_SITE, c);
    CHECK(out == 80.0f, "keep + gain 2: a new character under another handle slot starts from his own health");
    F32(h) = 70.0f; c[1] = h; regcall(GORE_CHAR_SITE, c);
    F32(h) = 100.0f; c[1] = h; regcall(GORE_CHAR_SITE, c);
    CHECK(out == 40.0f, "keep + gain 2: a fighter really hit keeps 70 %% x2 = 40 %% after healing");
    gore_set_cutscene(1);
    c[1] = h; regcall(GORE_CHAR_SITE, c);
    CHECK(out == 100.0f, "cutscene: the retail health percent (100 %%), no kept blood");
    F32(h) = 80.0f; c[1] = h; regcall(GORE_CHAR_SITE, c);
    CHECK(out == 80.0f, "cutscene: 80 %% health shows as 80 %% (no gain)");
    F32(h) = 10.0f; c[1] = h; regcall(GORE_CHAR_SITE, c);   /* not recorded */
    gore_set_cutscene(0);
    F32(h) = 100.0f; c[1] = h; regcall(GORE_CHAR_SITE, c);
    CHECK(out == 40.0f, "after the cutscene: the fighter's own kept blood again (a cutscene's health is not kept)");
    gore_set("char_gain", "1");
    F32(h) = 50.0f; c[1] = h; regcall(GORE_CHAR_SITE, c);
    gore_reset_level();
    F32(h) = 100.0f; c[1] = h; regcall(GORE_CHAR_SITE, c);
    CHECK(out == 100.0f, "keep: a new level starts clean");
    gore_set("char_keep", "0");
    F32(h) = 70.0f; c[1] = h; regcall(GORE_CHAR_SITE, c);
    CHECK(out == 70.0f, "keep 0, gain 1: the health percent as is");
    plant_handle(3, 0, 0);
}

static void test_refuse_other_bytes(void) {
    patched_drops = patched_hold = patched_char = 0;
    memset(&cfg, 0, sizeof cfg);
    cfg.drops = cfg.gain = 1; cfg.pool_max = 128;
    plant();
    AT(GORE_CONTINUE_EXIT + 1)[0] = 0xb0;
    plant_call(GORE_DROPS_SITE, GORE_POP_INT_FN + 0x10);
    AT(GORE_CHAR_SITE)[0] = 0x90;
    uint8_t h[12], d[5], c[5];
    memcpy(h, AT(GORE_FADE_SITE), 12); memcpy(d, AT(GORE_DROPS_SITE), 5); memcpy(c, AT(GORE_CHAR_SITE), 5);
    gore_set("pool_hold", "100"); gore_set("drops", "2"); gore_set("char_gain", "2");
    CHECK(patched_hold == -1 && patched_drops == -1 && patched_char == -1 && !memcmp(h, AT(GORE_FADE_SITE), 12) &&
          !memcmp(d, AT(GORE_DROPS_SITE), 5) && !memcmp(c, AT(GORE_CHAR_SITE), 5), "other bytes: nothing written");
}

int main(void) {
    test_options();
    test_hooks_on_planted_code();
    test_refuse_other_bytes();
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
#endif
