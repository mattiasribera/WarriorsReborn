/* Effect pools of the title (see hle_fx.h). */
#include "hle_fx.h"
#include "mem.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int hle_fx_report;

/* A pool object or field pointer the reader may follow: inside the contiguous window (the main
 * heap) or the guest arena. Anything else (not yet constructed, freed, garbage) is skipped. */
static int guest_ptr(uint32_t p) {
    return (p >= CONTIG_BASE && p - CONTIG_BASE < CONTIG_SIZE - 0x40) ||
           (p >= GUEST_ARENA_START && p < GUEST_ARENA_END - 0x40);
}
static uint32_t rd32(uint32_t a) { return *(const volatile uint32_t *)(uintptr_t)a; }

static void pool_at(FxPools *p, int i, uint32_t obj, uint32_t cap_off, uint32_t used_off) {
    if (!guest_ptr(obj)) return;
    p->valid[i] = 1;
    p->cap[i] = rd32(obj + cap_off);
    p->used[i] = rd32(obj + used_off);
}

void fx_pools_read(FxPools *p) {
    memset(p, 0, sizeof *p);
    uint32_t tm = rd32(FX_TASK_MANAGER);
    if (guest_ptr(tm)) {
        pool_at(p, FX_POOL_PTASK, rd32(tm + 0x838), 0xc, 0x10);    /* FreeListContainer */
        pool_at(p, FX_POOL_PTASK2, rd32(tm + 0x83c), 0xc, 0x10);
        pool_at(p, FX_POOL_GLASS, rd32(tm + 0x84c), 0xc, 0x10);
    }
    pool_at(p, FX_POOL_SOUND, rd32(FX_SOUND_LIST), 0x8, 0xc);      /* FreeList */
    pool_at(p, FX_POOL_SCRIPT, rd32(FX_SCRIPT_LIST), 0x8, 0xc);
}

/* Sprite pools, keyed by their size: written by the guest threads that flush particle systems,
 * read and reset by the host thread once per [fx] line. Relaxed atomics: diagnostics only. */
#define FX_SPRITE_KEYS 32
static struct { uint32_t max, peak, drops, peak_run, drops_run; } sprites[FX_SPRITE_KEYS];

static void atomic_max(uint32_t *a, uint32_t v) {
    uint32_t cur = __atomic_load_n(a, __ATOMIC_RELAXED);
    while (v > cur && !__atomic_compare_exchange_n(a, &cur, v, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

/* Called from the flush stub (cdecl, guest thread) with the particle system before its count
 * (+0x28) is cleared. */
static void __attribute__((cdecl, used)) sprite_flush(uint32_t ps) {
    uint32_t max = rd32(ps + 0x24), count = rd32(ps + 0x28);
    if (!max || !count) return;
    for (int i = 0; i < FX_SPRITE_KEYS; i++) {
        uint32_t k = __atomic_load_n(&sprites[i].max, __ATOMIC_RELAXED);
        if (!k) {
            uint32_t zero = 0;
            if (!__atomic_compare_exchange_n(&sprites[i].max, &zero, max, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) && zero != max)
                continue;
        } else if (k != max) continue;
        atomic_max(&sprites[i].peak, count);
        atomic_max(&sprites[i].peak_run, count);
        if (count > max) {
            __atomic_add_fetch(&sprites[i].drops, count - max, __ATOMIC_RELAXED);
            __atomic_add_fetch(&sprites[i].drops_run, count - max, __ATOMIC_RELAXED);
        }
        return;
    }
}

int fx_sprites_take(FxSprites *out, int n) {
    int m = 0;
    for (int i = 0; i < FX_SPRITE_KEYS && m < n; i++) {
        uint32_t max = __atomic_load_n(&sprites[i].max, __ATOMIC_RELAXED);
        if (!max) continue;
        FxSprites s = {max, __atomic_exchange_n(&sprites[i].peak, 0, __ATOMIC_RELAXED),
                       __atomic_exchange_n(&sprites[i].drops, 0, __ATOMIC_RELAXED),
                       __atomic_load_n(&sprites[i].peak_run, __ATOMIC_RELAXED),
                       __atomic_load_n(&sprites[i].drops_run, __ATOMIC_RELAXED)};
        int j = m++;
        while (j > 0 && out[j - 1].max > max) { out[j] = out[j - 1]; j--; }
        out[j] = s;
    }
    return m;
}

static const uint8_t sig_flush[] = {0xc7, 0x46, 0x28, 0x00, 0x00, 0x00, 0x00};   /* mov dword [esi+0x28], 0 */

/* The flush store becomes "call stub" + 2 nops. The stub saves the flags and all registers,
 * passes esi (the particle system) to sprite_flush, restores, runs the original store and
 * returns. Both branches of 0xc0480 that skip the draw jump to the call itself. */
static int patch_flush(void) {
    uint8_t *p = (uint8_t *)(uintptr_t)FX_SPRITE_FLUSH;
    if (memcmp(p, sig_flush, sizeof sig_flush)) {
        printf("[fx] skip sprite flush counter at %08x: unexpected bytes\n", FX_SPRITE_FLUSH);
        return 0;
    }
    uint8_t *s = plat_alloc_exec(32), *q = s;
    if (!s) { printf("[fx] skip sprite flush counter: no stub memory\n"); return 0; }
    *q++ = 0x9c; *q++ = 0x60;                      /* pushfd; pushad */
    *q++ = 0x56;                                   /* push esi */
    int32_t rel = (int32_t)((uintptr_t)sprite_flush - (uintptr_t)(q + 5));
    *q++ = 0xe8; memcpy(q, &rel, 4); q += 4;       /* call sprite_flush */
    *q++ = 0x83; *q++ = 0xc4; *q++ = 0x04;         /* add esp, 4 */
    *q++ = 0x61; *q++ = 0x9d;                      /* popad; popfd */
    memcpy(q, sig_flush, sizeof sig_flush); q += sizeof sig_flush;
    *q++ = 0xc3;                                   /* ret */
    rel = (int32_t)((uintptr_t)s - (uintptr_t)(p + 5));
    p[0] = 0xe8; memcpy(p + 1, &rel, 4);
    p[5] = p[6] = 0x90;
    printf("[fx] sprite flush counter at %08x\n", FX_SPRITE_FLUSH);
    return 1;
}

int fx_scaled(int value, double scale, int cap) {
    if (value <= 0 || !(scale > 1)) return value;
    double v = value * scale + 0.5;
    int r = v >= 0x7fffffff ? 0x7fffffff : (int)v;
    if (cap > 0 && r > cap) r = cap;
    return r < value ? value : r;
}

int fx_scale_parse(const char *s, double *out) {
    if (!s || !((s[0] >= '0' && s[0] <= '9') || s[0] == '.')) return -1;   /* no sign, space, nan, inf */
    char *end;
    double v = strtod(s, &end);
    if (*end || !(v >= 1.0 && v <= FX_SCALE_MAX)) return -1;
    *out = v;
    return 0;
}

/* An instruction with an imm32 at op_len: op bytes and the old immediate must match. */
static int patch_imm32(const char *name, uint32_t site, const uint8_t *op, int op_len, int32_t old, int32_t value) {
    uint8_t *p = (uint8_t *)(uintptr_t)site;
    int32_t cur;
    memcpy(&cur, p + op_len, 4);
    if (memcmp(p, op, (size_t)op_len) || cur != old) {
        printf("[fx] skip %s at %08x: unexpected bytes\n", name, site);
        return 0;
    }
    memcpy(p + op_len, &value, 4);
    printf("[fx] %s at %08x: %d -> %d\n", name, site, old, value);
    return 1;
}

/* "push imm8; mov r32, r32; call ctor" (9 bytes) cannot take a count above 127. It becomes
 * "jmp stub" + 4 nops; the stub runs "push imm32", the same mov, "call ctor" and jumps back to
 * the instruction after the call. Nothing jumps into the moved instructions (the branch around
 * them at 0x2b2bf5 targets 0x2b2c02). */
static int patch_push8_call(const char *name, uint32_t site, uint8_t old, const uint8_t mov[2], uint32_t ctor, int32_t value) {
    uint8_t *p = (uint8_t *)(uintptr_t)site;
    int32_t rel;
    memcpy(&rel, p + 5, 4);
    if (p[0] != 0x6a || p[1] != old || p[2] != mov[0] || p[3] != mov[1] || p[4] != 0xe8 || site + 9 + (uint32_t)rel != ctor) {
        printf("[fx] skip %s at %08x: unexpected bytes\n", name, site);
        return 0;
    }
    uint8_t *s = plat_alloc_exec(32), *q = s;
    if (!s) { printf("[fx] skip %s: no stub memory\n", name); return 0; }
    *q++ = 0x68; memcpy(q, &value, 4); q += 4;     /* push imm32 */
    *q++ = mov[0]; *q++ = mov[1];                  /* the moved mov */
    rel = (int32_t)(ctor - (uint32_t)(uintptr_t)(q + 5));
    *q++ = 0xe8; memcpy(q, &rel, 4); q += 4;       /* call ctor */
    rel = (int32_t)(site + 9 - (uint32_t)(uintptr_t)(q + 5));
    *q++ = 0xe9; memcpy(q, &rel, 4);               /* jmp back */
    rel = (int32_t)((uint32_t)(uintptr_t)s - (site + 5));
    p[0] = 0xe9; memcpy(p + 1, &rel, 4);
    memset(p + 5, 0x90, 4);
    printf("[fx] %s at %08x: %d -> %d\n", name, site, old, value);
    return 1;
}

static const uint8_t op_push[] = {0x68}, op_cmp_eax[] = {0x3d}, op_mov_esi_mem[] = {0xc7, 0x06};
static const uint8_t mov_ecx_eax[] = {0x8b, 0xc8}, mov_esi_ecx[] = {0x8b, 0xf1};

static int install_big_heap(void) {
    int n = 0;
    n += patch_imm32("probe block highest address", FX_PROBE_HIGHEST, op_push, 1, 0x4000000, (int32_t)FX_HEAP_HIGHEST);
    n += patch_imm32("main heap size", FX_HEAP_SIZE_SITE, op_mov_esi_mem, 2, 0x2d80000, (int32_t)FX_HEAP_SIZE);
    n += patch_imm32("main heap highest address", FX_HEAP_HIGHEST_SITE, op_push, 1, 0x4000000, (int32_t)FX_HEAP_HIGHEST);
    /* The window's layout at start-up: 64 KB kept free, the probe block, then the heap. */
    double left = (CONTIG_SIZE - 0x10000 - 0x1280000 - FX_HEAP_SIZE) / 1048576.0;
    if (n == 3)
        printf("[fx] main heap %.1f -> %.1f MB; %.1f MB of the %u MB contiguous window stay free for D3D data\n",
               0x2d70000 / 1048576.0, FX_HEAP_SIZE / 1048576.0, left, CONTIG_SIZE >> 20);
    return n;
}

/* The sprite pools' VBs live in the device's dynamic VB heap: it must grow first, by the extra
 * bytes of every pool raised (+ 64 KB for block headers and alignment, rounded up to 64 KB). */
static int install_sprites(double scale) {
    static const struct { const char *name; uint32_t site; int32_t old; } pools[] = {
        {"part_page1 sprites", FX_SPRITE640_SITE, 640}, {"part_page1 sprites", FX_SPRITE400_SITE, 400},
        {"part_page1 sprites", FX_SPRITE200_SITE, 200}, {"part_fire sprites", FX_FIRE256_SITE, 256},
    };
    const uint8_t *heap = (const uint8_t *)(uintptr_t)FX_VB_HEAP_SITE;
    int32_t heap_size;
    memcpy(&heap_size, heap + 1, 4);
    if (heap[0] != 0x68 || heap_size != 0x200000) {
        printf("[fx] skip the sprite pools: dynamic vertex buffer heap at %08x has unexpected bytes\n", FX_VB_HEAP_SITE);
        return 0;
    }
    int n = 0;
    uint32_t extra = 0;
    for (size_t i = 0; i < sizeof pools / sizeof pools[0]; i++) {
        int v = fx_scaled(pools[i].old, scale, FX_SPRITE_CAP);
        if (v != pools[i].old && patch_imm32(pools[i].name, pools[i].site, op_push, 1, pools[i].old, v)) {
            n++;
            extra += (uint32_t)(v - pools[i].old) * FX_SPRITE_VB_BYTES;
        }
    }
    if (extra) n += patch_imm32("dynamic vertex buffer heap", FX_VB_HEAP_SITE, op_push, 1, 0x200000,
                                (int32_t)(0x200000 + ((extra + 0x10000 + 0xffff) & ~0xffffu)));
    return n;
}

static int install_scaled(double scale) {
    int n = 0, v;
    if ((v = fx_scaled(1400, scale, 0)) != 1400) n += patch_imm32("particle tasks", FX_PTASK_SITE, op_push, 1, 1400, v);
    if ((v = fx_scaled(100, scale, 0)) != 100) n += patch_push8_call("particle tasks (second pool)", FX_PTASK2_SITE, 100, mov_ecx_eax, FX_PTASK_CTOR, v);
    if ((v = fx_scaled(100, scale, 0)) != 100) n += patch_push8_call("glass tasks", FX_GLASS_SITE, 100, mov_esi_ecx, FX_GLASS_CTOR, v);
    if ((v = fx_scaled(256, scale, 0)) != 256) n += patch_imm32("sound tasks", FX_SOUND_SITE, op_push, 1, 256, v);
    if ((v = fx_scaled(512, scale, 0)) != 512) n += patch_imm32("blood fade at free particle tasks", FX_FADE_SITE, op_cmp_eax, 1, 512, v);
    return n + install_sprites(scale);
}

int hle_fx_install(double scale, int big_heap, int stats) {
    int n = 0;
    if (stats) n += patch_flush();
    if (big_heap) n += install_big_heap();
    if (scale > 1) {
        printf("[fx] effect pools x%.2f\n", scale);
        n += install_scaled(scale);
    }
    return n;
}

/* Host thread, per presented frame: samples the pools and prints, every 60 frames, the last
 * value and the window peak of each pool with its run peak, the sprite pools of 64 or more
 * sprites, and how much of the contiguous window is handed out. */
void hle_fx_frame(uint32_t swap) {
    static int frames;
    static uint32_t peak[FX_POOLS], peak_run[FX_POOLS];
    static const char *const names[FX_POOLS] = {"particles", "particles2", "glass", "sound", "script"};
    FxPools p;
    fx_pools_read(&p);
    for (int i = 0; i < FX_POOLS; i++) {
        if (p.used[i] > peak[i]) peak[i] = p.used[i];
        if (p.used[i] > peak_run[i]) peak_run[i] = p.used[i];
    }
    if (++frames < 60) return;
    char line[1024];
    int n = snprintf(line, sizeof line, "[fx] swap %u:", swap);
    for (int i = 0; i < FX_POOLS && n < (int)sizeof line; i++)
        if (p.valid[i])
            n += snprintf(line + n, sizeof line - (size_t)n, " %s %u/%u (peak %u, run %u)", names[i], p.used[i], p.cap[i], peak[i], peak_run[i]);
    FxSprites s[FX_SPRITE_KEYS];
    int k = fx_sprites_take(s, FX_SPRITE_KEYS);
    uint32_t dropped = 0, dropped_run = 0;
    for (int i = 0; i < k; i++) {
        dropped += s[i].drops;
        dropped_run += s[i].drops_run;
        if (s[i].max >= 64 && n < (int)sizeof line)
            n += snprintf(line + n, sizeof line - (size_t)n, " | sprites%u peak %u (run %u) drop %u (run %u)", s[i].max, s[i].peak,
                          s[i].peak_run, s[i].drops, s[i].drops_run);
    }
    if (n < (int)sizeof line)
        snprintf(line + n, sizeof line - (size_t)n, " | sprites dropped %u (run %u) | contiguous %.1f/%u MB", dropped, dropped_run,
                 contig_top() / 1048576.0, CONTIG_SIZE >> 20);
    puts(line);
    frames = 0;
    memset(peak, 0, sizeof peak);
}

#ifdef HLE_FX_TEST
/* Game-independent test: the --stats reader on planted pool objects, the flush counter on a
 * planted copy of the store at its guest address, run with the guest's registers. */
#include <sys/mman.h>

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
#define D32(a) (*(uint32_t *)(uintptr_t)(a))

static void *map_at(uint32_t lo, uint32_t len) {
    void *m = mmap((void *)(uintptr_t)lo, len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m != (void *)(uintptr_t)lo) { perror("mmap"); exit(1); }
    memset(m, 0xcc, len);
    return m;
}

static const uint8_t flush_store[] = {0xc7, 0x46, 0x28, 0x00, 0x00, 0x00, 0x00};

/* Runs the planted flush store with esi = rec; checks the other registers are preserved. */
static void run_flush(uint32_t rec) {
    uint32_t r[6] = {0x11111111, 0x22222222, 0x33333333, 0x44444444, rec, 0x66666666};
    regcall(FX_SPRITE_FLUSH, r);
    CHECK(r[0] == 0x11111111 && r[1] == 0x22222222 && r[2] == 0x33333333 && r[3] == 0x44444444 &&
          r[4] == rec && r[5] == 0x66666666, "flush: registers preserved");
}

static void test_stats(void) {
    /* Pool objects live in the main heap (contiguous window); plant them there. */
    uint32_t h = CONTIG_BASE;
    map_at(h, 0x10000);
    map_at(0x4a6000, 0x1000);    /* sound list pointer 0x4a6f64 */
    map_at(0x5a7000, 0x6000);    /* script list 0x5a752c, task manager 0x5aceb8 */
    FxPools p;
    D32(FX_TASK_MANAGER) = 0; D32(FX_SOUND_LIST) = 0; D32(FX_SCRIPT_LIST) = 0;
    fx_pools_read(&p);
    CHECK(!p.valid[FX_POOL_PTASK] && !p.valid[FX_POOL_SOUND] && !p.valid[FX_POOL_SCRIPT], "pools: nothing before start-up");

    uint32_t tm = h + 0x1000, c1 = h + 0x3000, c2 = h + 0x3100, gl = h + 0x3200, snd = h + 0x3300, scr = h + 0x3400;
    D32(FX_TASK_MANAGER) = tm;
    D32(tm + 0x838) = c1; D32(tm + 0x83c) = c2; D32(tm + 0x84c) = gl;
    D32(c1 + 0xc) = 1400; D32(c1 + 0x10) = 812;
    D32(c2 + 0xc) = 100; D32(c2 + 0x10) = 3;
    D32(gl + 0xc) = 100; D32(gl + 0x10) = 7;
    D32(FX_SOUND_LIST) = snd; D32(snd + 0x8) = 256; D32(snd + 0xc) = 40;
    D32(FX_SCRIPT_LIST) = scr; D32(scr + 0x8) = 100; D32(scr + 0xc) = 12;
    fx_pools_read(&p);
    CHECK(p.valid[FX_POOL_PTASK] && p.used[FX_POOL_PTASK] == 812 && p.cap[FX_POOL_PTASK] == 1400, "pools: particle tasks 812/1400");
    CHECK(p.valid[FX_POOL_PTASK2] && p.used[FX_POOL_PTASK2] == 3 && p.cap[FX_POOL_PTASK2] == 100, "pools: second particle pool 3/100");
    CHECK(p.valid[FX_POOL_GLASS] && p.used[FX_POOL_GLASS] == 7 && p.cap[FX_POOL_GLASS] == 100, "pools: glass 7/100");
    CHECK(p.valid[FX_POOL_SOUND] && p.used[FX_POOL_SOUND] == 40 && p.cap[FX_POOL_SOUND] == 256, "pools: sound tasks 40/256");
    CHECK(p.valid[FX_POOL_SCRIPT] && p.used[FX_POOL_SCRIPT] == 12 && p.cap[FX_POOL_SCRIPT] == 100, "pools: script objects 12/100");
    D32(tm + 0x84c) = 0x1234;    /* not a guest heap pointer */
    fx_pools_read(&p);
    CHECK(!p.valid[FX_POOL_GLASS] && p.valid[FX_POOL_PTASK], "pools: a wild pointer is not followed");

    /* Flush counter. */
    map_at(0xc0000, 0x1000);
    memcpy(AT(FX_SPRITE_FLUSH), flush_store, sizeof flush_store);
    AT(FX_SPRITE_FLUSH)[sizeof flush_store] = 0xc3;
    CHECK(hle_fx_install(1.0, 0, 0) == 0 && !memcmp(AT(FX_SPRITE_FLUSH), flush_store, sizeof flush_store), "no --stats: flush untouched");
    AT(FX_SPRITE_FLUSH)[3] = 1;
    CHECK(hle_fx_install(1.0, 0, 1) == 0 && AT(FX_SPRITE_FLUSH)[0] == 0xc7, "--stats, unexpected bytes: flush untouched");
    AT(FX_SPRITE_FLUSH)[3] = 0;
    CHECK(hle_fx_install(1.0, 0, 1) == 1 && AT(FX_SPRITE_FLUSH)[0] == 0xe8 && AT(FX_SPRITE_FLUSH)[5] == 0x90 &&
          AT(FX_SPRITE_FLUSH)[6] == 0x90, "--stats: flush store -> call + 2 nops");
    uint32_t rec = h + 0x4000, rec2 = h + 0x4100;
    D32(rec + 0x24) = 640; D32(rec + 0x28) = 700;
    run_flush(rec);
    CHECK(D32(rec + 0x28) == 0, "flush: count cleared by the original store");
    D32(rec + 0x28) = 300;
    run_flush(rec);
    D32(rec2 + 0x24) = 256; D32(rec2 + 0x28) = 12;
    run_flush(rec2);
    FxSprites s[8];
    int n = fx_sprites_take(s, 8);
    CHECK(n == 2 && s[0].max == 256 && s[1].max == 640, "sprites: two pools, sorted by size (got %d)", n);
    CHECK(s[1].peak == 700 && s[1].drops == 60 && s[0].peak == 12 && s[0].drops == 0, "sprites: 640 pool peak 700 dropped 60, 256 pool 12");
    D32(rec + 0x28) = 100;
    run_flush(rec);
    n = fx_sprites_take(s, 8);
    CHECK(n == 2 && s[1].peak == 100 && s[1].drops == 0 && s[1].peak_run == 700 && s[1].drops_run == 60,
          "sprites: new window peak 100, run keeps 700 and 60");
    CHECK(s[0].peak == 0 && s[0].peak_run == 12, "sprites: idle pool window 0, run 12");
}

/* ---- D1-D3: the planted sites (the title's bytes, see hle_fx.h) ---- */
static const uint8_t probe_hi[] = {0x68, 0x00, 0x00, 0x00, 0x04};
static const uint8_t heap_size[] = {0xc7, 0x06, 0x00, 0x00, 0xd8, 0x02};
static const uint8_t heap_hi[] = {0x68, 0x00, 0x00, 0x00, 0x04};
static const uint8_t ptask[] = {0x68, 0x78, 0x05, 0x00, 0x00};
static const uint8_t ptask2[] = {0x6a, 0x64, 0x8b, 0xc8, 0xe8, 0xc0, 0x8b, 0xff, 0xff};   /* call 0x2ab7c0 */
static const uint8_t glass[] = {0x6a, 0x64, 0x8b, 0xf1, 0xe8, 0x86, 0x01, 0x00, 0x00};    /* call 0x29f860 */
static const uint8_t sound[] = {0x68, 0x00, 0x01, 0x00, 0x00};
static const uint8_t fade[] = {0x3d, 0x00, 0x02, 0x00, 0x00};
static const uint8_t spr640[] = {0x68, 0x80, 0x02, 0x00, 0x00};
static const uint8_t spr400[] = {0x68, 0x90, 0x01, 0x00, 0x00};
static const uint8_t spr200[] = {0x68, 0xc8, 0x00, 0x00, 0x00};
static const uint8_t fire256[] = {0x68, 0x00, 0x01, 0x00, 0x00};
static const uint8_t vbheap[] = {0x68, 0x00, 0x00, 0x20, 0x00};
static const struct { uint32_t site; const uint8_t *b; int n; } planted[] = {
    {FX_PROBE_HIGHEST, probe_hi, 5}, {FX_HEAP_SIZE_SITE, heap_size, 6}, {FX_HEAP_HIGHEST_SITE, heap_hi, 5},
    {FX_PTASK_SITE, ptask, 5}, {FX_PTASK2_SITE, ptask2, 9}, {FX_GLASS_SITE, glass, 9}, {FX_SOUND_SITE, sound, 5},
    {FX_FADE_SITE, fade, 5}, {FX_SPRITE640_SITE, spr640, 5}, {FX_SPRITE400_SITE, spr400, 5},
    {FX_SPRITE200_SITE, spr200, 5}, {FX_FIRE256_SITE, fire256, 5}, {FX_VB_HEAP_SITE, vbheap, 5},
};
#define NPLANTED ((int)(sizeof planted / sizeof planted[0]))
#define CTOR_ECX 0x0002a000u   /* the fake constructors store their this pointer here */

/* The fake FreeList constructors: record ecx, return the count argument, ret 4 (thiscall). */
static void plant_ctor(uint32_t at) {
    static const uint8_t ctor[] = {0x89, 0x0d, 0x00, 0xa0, 0x02, 0x00, 0x8b, 0x44, 0x24, 0x04, 0xc2, 0x04, 0x00};
    memcpy(AT(at), ctor, sizeof ctor);
}
static void plant(void) {
    for (int i = 0; i < NPLANTED; i++) memcpy(AT(planted[i].site), planted[i].b, (size_t)planted[i].n);
    AT(FX_PTASK2_SITE + 9)[0] = 0xc3;   /* the trampolines jump back to site + 9: return from there */
    AT(FX_GLASS_SITE + 9)[0] = 0xc3;
    plant_ctor(FX_PTASK_CTOR);
    plant_ctor(FX_GLASS_CTOR);
}
static int unchanged(void) {
    for (int i = 0; i < NPLANTED; i++)
        if (memcmp(AT(planted[i].site), planted[i].b, (size_t)planted[i].n)) return 0;
    return 1;
}
static int32_t imm(uint32_t site, int off) { int32_t v; memcpy(&v, AT(site + (uint32_t)off), 4); return v; }

static void test_scale(void) {
    CHECK(fx_scaled(1400, 4, 0) == 5600 && fx_scaled(100, 4, 0) == 400 && fx_scaled(512, 4, 0) == 2048, "x4: 1400 -> 5600, 100 -> 400, 512 -> 2048");
    CHECK(fx_scaled(100, 2.5, 0) == 250 && fx_scaled(256, 1.5, 0) == 384 && fx_scaled(200, 1.25, 0) == 250, "x2.5, x1.5, x1.25 round");
    CHECK(fx_scaled(640, 4, FX_SPRITE_CAP) == 2048 && fx_scaled(400, 4, FX_SPRITE_CAP) == 1600, "sprites: 640 x4 clamped to 2048, 400 x4 = 1600");
    CHECK(fx_scaled(3000, 2, FX_SPRITE_CAP) == 3000, "a value above the cap is not lowered");
    CHECK(fx_scaled(640, 1, FX_SPRITE_CAP) == 640 && fx_scaled(0, 4, 0) == 0 && fx_scaled(-1, 4, 0) == -1, "x1 unchanged; 0 and -1 kept");
    double v = 0;
    CHECK(!fx_scale_parse("1", &v) && v == 1 && !fx_scale_parse("4", &v) && v == 4 && !fx_scale_parse("2.5", &v) && v == 2.5 &&
          !fx_scale_parse("1.0", &v) && v == 1, "parse 1, 4, 2.5, 1.0");
    CHECK(fx_scale_parse("0.9", &v) && fx_scale_parse("4.1", &v) && fx_scale_parse("0", &v) && fx_scale_parse("-2", &v),
          "reject 0.9, 4.1, 0, -2");
    CHECK(fx_scale_parse("", &v) && fx_scale_parse(NULL, &v) && fx_scale_parse("2x", &v) && fx_scale_parse(" 2", &v) &&
          fx_scale_parse("nan", &v) && fx_scale_parse("inf", &v), "reject '', NULL, '2x', ' 2', nan, inf");
}

static void test_patches(void) {
    map_at(0x28000, 0x3000);      /* sound 0x280a3, fake-constructor this at 0x2a000 */
    map_at(0x29f000, 0x1000);     /* glass 0x29f6d1, its constructor 0x29f860 */
    map_at(0x2ab000, 0x1000);     /* particle constructor 0x2ab7c0 */
    map_at(0x2b2000, 0x3000);     /* particle pools 0x2b2bba / 0x2b2bf7, fade 0x2b4b56 */
    map_at(0x6b000, 0x1000);      /* heap 0x6bd97 .. 0x6bdcd */
    map_at(0xa6000, 0x1000);      /* sprite pools */
    map_at(0xbb000, 0x1000);      /* dynamic vertex buffer heap 0xbb12f */

    plant();
    CHECK(hle_fx_install(1.0, 0, 0) == 0 && unchanged(), "scale 1, no big heap: nothing patched, bytes unchanged");

    plant();
    CHECK(hle_fx_install(1.0, 1, 0) == 3, "big heap alone: 3 sites");
    CHECK(imm(FX_PROBE_HIGHEST, 1) == (int32_t)FX_HEAP_HIGHEST && imm(FX_HEAP_HIGHEST_SITE, 1) == (int32_t)FX_HEAP_HIGHEST &&
          imm(FX_HEAP_SIZE_SITE, 2) == (int32_t)FX_HEAP_SIZE && AT(FX_HEAP_SIZE_SITE)[0] == 0xc7 && AT(FX_HEAP_SIZE_SITE)[1] == 0x06,
          "big heap: highest 128 MB for probe and heap, heap size %#x", FX_HEAP_SIZE);
    CHECK(!memcmp(AT(FX_PTASK_SITE), ptask, 5) && !memcmp(AT(FX_SPRITE640_SITE), spr640, 5), "big heap alone: pools untouched");

    plant();
    CHECK(hle_fx_install(4.0, 1, 0) == 13, "x4 + big heap: 13 sites");
    CHECK(imm(FX_PTASK_SITE, 1) == 5600 && imm(FX_SOUND_SITE, 1) == 1024 && imm(FX_FADE_SITE, 1) == 2048 &&
          AT(FX_PTASK_SITE)[0] == 0x68 && AT(FX_FADE_SITE)[0] == 0x3d, "x4: particles 5600, sound 1024, fade at 2048 free");
    CHECK(imm(FX_SPRITE640_SITE, 1) == 2048 && imm(FX_SPRITE400_SITE, 1) == 1600 && imm(FX_SPRITE200_SITE, 1) == 800 &&
          imm(FX_FIRE256_SITE, 1) == 1024, "x4: sprites 2048 (clamped) / 1600 / 800, fire 1024");
    /* 3976 more quads x 3 VBs x 0x60 bytes = 0x117900, + 64 KB, rounded up to 64 KB = 0x130000. */
    CHECK(imm(FX_VB_HEAP_SITE, 1) == 0x200000 + 0x130000, "x4: dynamic VB heap 2 MB + the sprites' extra VBs = %#x", imm(FX_VB_HEAP_SITE, 1));
    CHECK(AT(FX_PTASK2_SITE)[0] == 0xe9 && AT(FX_PTASK2_SITE)[5] == 0x90 && AT(FX_PTASK2_SITE)[8] == 0x90 &&
          AT(FX_GLASS_SITE)[0] == 0xe9 && AT(FX_GLASS_SITE)[8] == 0x90, "push imm8 sites: jmp + 4 nops");
    /* Run the trampolines: the constructor gets the scaled count and the this pointer of the
     * moved mov, and every other register survives. */
    uint32_t r[6] = {0x5000, 0x22222222, 0x33333333, 0x44444444, 0x55555555, 0x66666666};
    D32(CTOR_ECX) = 0;
    regcall(FX_PTASK2_SITE, r);
    CHECK(r[0] == 400 && r[1] == 0x5000 && D32(CTOR_ECX) == 0x5000 && r[2] == 0x33333333 && r[3] == 0x44444444 &&
          r[4] == 0x55555555 && r[5] == 0x66666666, "particles2 trampoline: ctor(this=eax, 400), registers kept");
    uint32_t g[6] = {0x11111111, 0x7000, 0x33333333, 0x44444444, 0x55555555, 0x66666666};
    D32(CTOR_ECX) = 0;
    regcall(FX_GLASS_SITE, g);
    CHECK(g[0] == 400 && g[1] == 0x7000 && g[4] == 0x7000 && D32(CTOR_ECX) == 0x7000 && g[2] == 0x33333333 &&
          g[3] == 0x44444444 && g[5] == 0x66666666, "glass trampoline: esi = this, ctor(this, 400), registers kept");

    plant();
    CHECK(hle_fx_install(2.5, 0, 0) == 10 && imm(FX_VB_HEAP_SITE, 1) == 0x2b0000 && imm(FX_PTASK_SITE, 1) == 3500 && imm(FX_FADE_SITE, 1) == 1280 &&
          imm(FX_SPRITE640_SITE, 1) == 1600 && imm(FX_FIRE256_SITE, 1) == 640 && !memcmp(AT(FX_HEAP_SIZE_SITE), heap_size, 6),
          "x2.5 without big heap: 10 sites, VB heap 0x2b0000, particles 3500, fade 1280, sprites 1600, fire 640, heap untouched");
    g[0] = 0; g[1] = 0x7100;
    regcall(FX_GLASS_SITE, g);
    CHECK(g[0] == 250 && g[4] == 0x7100, "x2.5: glass 250");

    /* Sites with other bytes are refused one by one, and left as they are. */
    plant();
    AT(FX_PTASK_SITE)[1] = 0x79;                 /* push 0x579 */
    AT(FX_GLASS_SITE + 5)[0] = 0x87;             /* call elsewhere */
    AT(FX_HEAP_SIZE_SITE)[1] = 0x07;             /* mov [edi], ... */
    AT(FX_SPRITE200_SITE)[1] = 0xc9;
    CHECK(hle_fx_install(4.0, 1, 0) == 9, "four mismatching sites: 9 of 13 patched");
    CHECK(AT(FX_PTASK_SITE)[1] == 0x79 && imm(FX_PTASK_SITE, 1) == 0x579 && !memcmp(AT(FX_GLASS_SITE), "\x6a\x64\x8b\xf1\xe8\x87", 6) &&
          AT(FX_HEAP_SIZE_SITE)[1] == 0x07 && imm(FX_HEAP_SIZE_SITE, 2) == 0x2d80000 && imm(FX_SPRITE200_SITE, 1) == 0xc9,
          "mismatching sites untouched");
    CHECK(imm(FX_VB_HEAP_SITE, 1) == 0x200000 + 0x100000, "VB heap grows only for the sprite pools patched (%#x)", imm(FX_VB_HEAP_SITE, 1));

    /* Without the VB heap the larger sprite pools would not fit in it: none is raised. */
    plant();
    AT(FX_VB_HEAP_SITE)[3] = 0x10;               /* push 0x100000 */
    CHECK(hle_fx_install(4.0, 1, 0) == 8, "VB heap site differs: 8 sites (heap + pools), no sprite pool");
    CHECK(!memcmp(AT(FX_SPRITE640_SITE), spr640, 5) && !memcmp(AT(FX_FIRE256_SITE), fire256, 5) && imm(FX_VB_HEAP_SITE, 1) == 0x100000 &&
          imm(FX_PTASK_SITE, 1) == 5600, "sprite pools and VB heap untouched, particle pools raised");
}

int main(void) {
    test_scale();
    test_patches();
    test_stats();
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
#endif
