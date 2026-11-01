/* Spawn scaling experiment (see hle_spawn.h). */
#include "hle_spawn.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int hle_spawn_report;

static int spawn_scale = 1;
static volatile int stores_scaled;   /* diagnostics: patched stores that changed a value */

int spawn_scale_value(int value, int scale, int cap) {
    if (value <= 0 || scale <= 1 || value >= cap) return value;
    int v = value * scale;   /* value < cap <= 32767 and scale <= 8: no overflow */
    return v > cap ? cap : v;
}

int spawn_scale_parse(const char *s) {
    if (!s || s[0] < '1' || s[0] > '0' + SPAWN_SCALE_MAX || s[1]) return -1;
    return s[0] - '0';
}

/* Called from the store stubs (cdecl, guest thread): the scripts' max-alive word -> scaled. */
static int __attribute__((cdecl, used)) spawner_max(int value) {
    int v = spawn_scale_value(value, spawn_scale, SPAWN_SPAWNER_CAP);
    if (v != value) __atomic_add_fetch(&stores_scaled, 1, __ATOMIC_RELAXED);
    return v;
}

static int16_t *world_field(uint32_t off) {
    uint32_t w = *(volatile uint32_t *)(uintptr_t)SPAWN_WORLD_PTR;
    return w ? (int16_t *)(uintptr_t)(w + off) : NULL;
}

/* Replacements of SetSpawnMax / SetCopSpawnMax's cores (cdecl, one word argument pushed as a
 * dword, plain ret): the same store with the scaled value. */
static void __attribute__((cdecl, used)) set_spawn_max(uint32_t arg) {
    int16_t *f = world_field(0x420);
    int v = spawn_scale_value((int16_t)arg, spawn_scale, SPAWN_POOL_SLOTS);
    if (f) *f = (int16_t)v;
    printf("[spawn] SetSpawnMax(%d) -> %d\n", (int16_t)arg, v);
}
static void __attribute__((cdecl, used)) set_cop_spawn_max(uint32_t arg) {
    int16_t *f = world_field(0x31e);
    int v = spawn_scale_value((int16_t)arg, spawn_scale, SPAWN_POOL_SLOTS);
    if (f) *f = (int16_t)v;
    printf("[spawn] SetCopSpawnMax(%d) -> %d\n", (int16_t)arg, v);
}

/* A 16-bit register store "mov word [mem], reg16" of `len` bytes at `site` becomes
 * "call stub" + nops. The stub saves flags, eax, ecx and edx, passes the sign-extended register
 * to spawner_max, writes the result into the saved register's low word, restores and then runs
 * the original store (which addresses memory through registers other than esp). The register's
 * low word keeps the scaled value afterwards; both sites overwrite or drop it right after. */
static int patch_store(uint32_t site, const uint8_t *sig, int len, int saved_slot, const char *name) {
    uint8_t *p = (uint8_t *)(uintptr_t)site;
    if (memcmp(p, sig, (size_t)len)) {
        printf("[spawn] skip %s at %08x: unexpected bytes\n", name, site);
        return 0;
    }
    uint8_t *s = plat_alloc_exec(64), *q = s;
    if (!s) { printf("[spawn] skip %s: no stub memory\n", name); return 0; }
    *q++ = 0x9c;                                   /* pushfd */
    *q++ = 0x50; *q++ = 0x51; *q++ = 0x52;         /* push eax, ecx, edx: [esp] edx, [esp+4] ecx */
    *q++ = 0x0f; *q++ = 0xbf; *q++ = 0x44; *q++ = 0x24; *q++ = (uint8_t)saved_slot;   /* movsx eax, word [esp+slot] */
    *q++ = 0x50;                                   /* push eax */
    int32_t rel = (int32_t)((uintptr_t)spawner_max - (uintptr_t)(q + 5));
    *q++ = 0xe8; memcpy(q, &rel, 4); q += 4;       /* call spawner_max */
    *q++ = 0x83; *q++ = 0xc4; *q++ = 0x04;         /* add esp, 4 */
    *q++ = 0x66; *q++ = 0x89; *q++ = 0x44; *q++ = 0x24; *q++ = (uint8_t)saved_slot;   /* mov [esp+slot], ax */
    *q++ = 0x5a; *q++ = 0x59; *q++ = 0x58; *q++ = 0x9d;   /* pop edx, ecx, eax; popfd */
    memcpy(q, sig, (size_t)len); q += len;         /* the original store */
    *q++ = 0xc3;                                   /* ret */
    rel = (int32_t)((uintptr_t)s - (uintptr_t)(p + 5));
    p[0] = 0xe8; memcpy(p + 1, &rel, 4);
    memset(p + 5, 0x90, (size_t)len - 5);
    printf("[spawn] %s at %08x: max alive x%d (clamped to %d)\n", name, site, spawn_scale, SPAWN_SPAWNER_CAP);
    return 1;
}

static int patch_setter(uint32_t site, const uint8_t *sig, void *fn, const char *name) {
    uint8_t *p = (uint8_t *)(uintptr_t)site;
    if (memcmp(p, sig, 19)) {
        printf("[spawn] skip %s at %08x: unexpected bytes\n", name, site);
        return 0;
    }
    int32_t rel = (int32_t)((uintptr_t)fn - (uintptr_t)(p + 5));
    p[0] = 0xe9; memcpy(p + 1, &rel, 4);
    printf("[spawn] %s at %08x: cap x%d (clamped to %d)\n", name, site, spawn_scale, SPAWN_POOL_SLOTS);
    return 1;
}

/* Guest bytes of the four sites in the title (verified by disassembly, see hle_spawn.h). */
static const uint8_t sig_add_store[] = {0x66, 0x89, 0x8f, 0x8e, 0x06, 0x00, 0x00};               /* mov [edi+0x68e], cx */
static const uint8_t sig_setmax_store[] = {0x66, 0x89, 0x94, 0x31, 0x8e, 0x06, 0x00, 0x00};      /* mov [ecx+esi+0x68e], dx */
static const uint8_t sig_setspawnmax[] = {0x66, 0x8b, 0x44, 0x24, 0x04, 0x8b, 0x0d, 0x78, 0x9f, 0x5b,
                                          0x00, 0x66, 0x89, 0x81, 0x20, 0x04, 0x00, 0x00, 0xc3};
static const uint8_t sig_setcopmax[] = {0x66, 0x8b, 0x44, 0x24, 0x04, 0x8b, 0x0d, 0x78, 0x9f, 0x5b,
                                        0x00, 0x66, 0x89, 0x81, 0x1e, 0x03, 0x00, 0x00, 0xc3};

/* "mov eax, imm32" followed by the stores of ax to both caps (+0x420, +0x31e through esi). */
static const uint8_t sig_caps_stores[] = {0x66, 0x89, 0x86, 0x20, 0x04, 0x00, 0x00, 0x66, 0x89, 0x86, 0x1e, 0x03, 0x00, 0x00};

/* The immediate stored to both caps becomes the scaled value: the world constructor's 12 and the
 * level reset's 16 (levels that never call SetSpawnMax / SetCopSpawnMax keep these). */
static int patch_caps_imm(uint32_t site, int32_t value, const char *name) {
    uint8_t *p = (uint8_t *)(uintptr_t)site;
    int32_t imm;
    memcpy(&imm, p + 1, 4);
    if (p[0] != 0xb8 || imm != value || memcmp(p + 5, sig_caps_stores, sizeof sig_caps_stores)) {
        printf("[spawn] skip %s at %08x: unexpected bytes\n", name, site);
        return 0;
    }
    int32_t v = spawn_scale_value(value, spawn_scale, SPAWN_POOL_SLOTS);
    memcpy(p + 1, &v, 4);
    printf("[spawn] %s spawn and cop caps at %08x: %d -> %d\n", name, site, value, v);
    return 1;
}

int hle_spawn_install(int scale) {
    if (scale <= 1) return 0;
    spawn_scale = scale > SPAWN_SCALE_MAX ? SPAWN_SCALE_MAX : scale;
    int n = 0;
    n += patch_store(SPAWN_ADD_STORE, sig_add_store, sizeof sig_add_store, 4, "GangAddSpawner store");          /* cx */
    n += patch_store(SPAWN_SETMAX_STORE, sig_setmax_store, sizeof sig_setmax_store, 0, "GangSetMaxConcurrent store"); /* dx */
    n += patch_setter(SPAWN_SETSPAWNMAX, sig_setspawnmax, (void *)set_spawn_max, "SetSpawnMax");
    n += patch_setter(SPAWN_SETCOPMAX, sig_setcopmax, (void *)set_cop_spawn_max, "SetCopSpawnMax");
    n += patch_caps_imm(SPAWN_DEFAULT_CAPS, 12, "default");
    n += patch_caps_imm(SPAWN_RESET_CAPS, 16, "level reset");
    return n;
}

void spawn_pool_read(SpawnPool *p) {
    memset(p, 0, sizeof *p);
    const volatile uint32_t *h = (const volatile uint32_t *)(uintptr_t)SPAWN_HANDLE_TABLE;
    for (int i = 0; i < SPAWN_POOL_SLOTS; i++) p->slots += h[i * 2] != 0;
    const volatile int16_t *f = world_field(0);
    if (!f) return;
    p->valid = 1;
    p->live = f[0x41e / 2];
    p->cap = f[0x420 / 2];
    p->cops = f[0x31c / 2];
    p->cop_cap = f[0x31e / 2];
}

/* Host thread, per presented frame: samples the pool and prints the last value and the peak of
 * each 60-frame window. */
void hle_spawn_frame(uint32_t swap) {
    static int frames, peak_live, peak_slots;
    SpawnPool p;
    spawn_pool_read(&p);
    if (p.live > peak_live) peak_live = p.live;
    if (p.slots > peak_slots) peak_slots = p.slots;
    if (++frames < 60) return;
    if (p.valid)
        printf("[pool] swap %u: slots %d/%d (peak %d), spawned alive %d (peak %d) cap %d, cops %d cap %d, scale %d, stores scaled %d\n",
               swap, p.slots, SPAWN_POOL_SLOTS, peak_slots, p.live, peak_live, p.cap, p.cops, p.cop_cap, spawn_scale,
               __atomic_load_n(&stores_scaled, __ATOMIC_RELAXED));
    else
        printf("[pool] swap %u: slots %d/%d (peak %d), no world object\n", swap, p.slots, SPAWN_POOL_SLOTS, peak_slots);
    frames = peak_live = peak_slots = 0;
}

#ifdef HLE_SPAWN_TEST
/* Game-independent test: the scaling arithmetic and option parsing, then the patches on planted
 * copies of the four sites (mapped at their guest addresses), run with the guest's registers and
 * calling convention, and the pool reader on a fake world object and handle table. */
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

/* cdecl/stdcall call with arguments; *esp_delta = esp after the call - esp before the pushes. */
__asm__(".text\n.globl tcall\ntcall:\n"
        "push %ebp\npush %ebx\npush %esi\npush %edi\nmov %esp,%ebp\n"
        "mov 20(%ebp),%eax\nmov 24(%ebp),%ecx\nmov 28(%ebp),%esi\n"
        "1: test %ecx,%ecx\njz 2f\ndec %ecx\npushl (%esi,%ecx,4)\njmp 1b\n"
        "2: call *%eax\nmov %esp,%ecx\nsub %ebp,%ecx\nmov 32(%ebp),%edx\nmov %ecx,(%edx)\n"
        "mov %ebp,%esp\npop %edi\npop %esi\npop %ebx\npop %ebp\nret\n");
uint32_t tcall(uint32_t fn, int nargs, const uint32_t *args, int32_t *esp_delta);

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)
#define AT(a) ((uint8_t *)(uintptr_t)(a))
#define W16(a) (*(int16_t *)(uintptr_t)(a))
#define D32(a) (*(uint32_t *)(uintptr_t)(a))

static const uint8_t add_store[] = {0x66, 0x89, 0x8f, 0x8e, 0x06, 0x00, 0x00};
static const uint8_t setmax_store[] = {0x66, 0x89, 0x94, 0x31, 0x8e, 0x06, 0x00, 0x00};
static const uint8_t setspawnmax[] = {0x66, 0x8b, 0x44, 0x24, 0x04, 0x8b, 0x0d, 0x78, 0x9f, 0x5b,
                                      0x00, 0x66, 0x89, 0x81, 0x20, 0x04, 0x00, 0x00, 0xc3};
static const uint8_t setcopmax[] = {0x66, 0x8b, 0x44, 0x24, 0x04, 0x8b, 0x0d, 0x78, 0x9f, 0x5b,
                                    0x00, 0x66, 0x89, 0x81, 0x1e, 0x03, 0x00, 0x00, 0xc3};

static const uint8_t default_caps[] = {0xb8, 0x0c, 0x00, 0x00, 0x00, 0x66, 0x89, 0x86, 0x20, 0x04,
                                       0x00, 0x00, 0x66, 0x89, 0x86, 0x1e, 0x03, 0x00, 0x00};
static const uint8_t reset_caps[] = {0xb8, 0x10, 0x00, 0x00, 0x00, 0x66, 0x89, 0x86, 0x20, 0x04,
                                     0x00, 0x00, 0x66, 0x89, 0x86, 0x1e, 0x03, 0x00, 0x00};

static void plant(void) {   /* each store is followed by a ret so it can be called on its own */
    memcpy(AT(SPAWN_ADD_STORE), add_store, sizeof add_store);
    AT(SPAWN_ADD_STORE)[sizeof add_store] = 0xc3;
    memcpy(AT(SPAWN_SETMAX_STORE), setmax_store, sizeof setmax_store);
    AT(SPAWN_SETMAX_STORE)[sizeof setmax_store] = 0xc3;
    memcpy(AT(SPAWN_SETSPAWNMAX), setspawnmax, sizeof setspawnmax);
    memcpy(AT(SPAWN_SETCOPMAX), setcopmax, sizeof setcopmax);
    memcpy(AT(SPAWN_DEFAULT_CAPS), default_caps, sizeof default_caps);
    memcpy(AT(SPAWN_RESET_CAPS), reset_caps, sizeof reset_caps);
}

static void *map_at(uint32_t lo, uint32_t len) {
    void *m = mmap((void *)(uintptr_t)lo, len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m != (void *)(uintptr_t)lo) { perror("mmap"); exit(1); }
    memset(m, 0xcc, len);
    return m;
}

/* Runs the GangAddSpawner store with cx = v; returns the stored word, checks the registers. */
static int run_add_store(int16_t v, uint8_t *rec) {
    uint32_t r[6] = {0x11111111, 0xabcd0000u | (uint16_t)v, 0x33333333, 0x44444444, 0x55555555, (uint32_t)(uintptr_t)rec - 0x68e};
    W16(rec) = 0x7777;
    regcall(SPAWN_ADD_STORE, r);
    CHECK(r[0] == 0x11111111 && r[2] == 0x33333333 && r[3] == 0x44444444 && r[4] == 0x55555555 &&
          r[5] == (uint32_t)(uintptr_t)rec - 0x68e && (r[1] >> 16) == 0xabcd,
          "add store cx=%d: eax/edx/ebx/esi/edi and ecx's high word preserved", v);
    return W16(rec);
}

/* Runs the GangSetMaxConcurrent store with dx = v at [ecx+esi+0x68e]. */
static int run_setmax_store(int16_t v, uint8_t *rec) {
    uint32_t esi = (uint32_t)(uintptr_t)rec - 0x68e - 0x240;
    uint32_t r[6] = {0x11111111, 0x240, 0x12340000u | (uint16_t)v, 0x44444444, esi, 0x66666666};
    W16(rec) = 0x7777;
    regcall(SPAWN_SETMAX_STORE, r);
    CHECK(r[0] == 0x11111111 && r[1] == 0x240 && r[3] == 0x44444444 && r[4] == esi && r[5] == 0x66666666 &&
          (r[2] >> 16) == 0x1234, "set-max store dx=%d: eax/ecx/ebx/esi/edi and edx's high word preserved", v);
    return W16(rec);
}

static int run_setter(uint32_t fn, int v, uint32_t field, uint8_t *world) {
    int32_t delta = 0;
    uint32_t a = (uint32_t)v;
    W16(world + field) = 0x7777;
    tcall(fn, 1, &a, &delta);
    CHECK(delta == -4, "setter %08x(%d): cdecl, returns with a plain ret (esp delta %d)", fn, v, delta);
    return W16(world + field);
}

int main(void) {
    /* Pure arithmetic: scale, clamp to the cap, never lower, keep 0 / -1. */
    CHECK(spawn_scale_value(2, 2, 16) == 4, "2 x2 = 4");
    CHECK(spawn_scale_value(3, 4, 16) == 12, "3 x4 = 12");
    CHECK(spawn_scale_value(10, 2, 16) == 16, "10 x2 clamped to 16");
    CHECK(spawn_scale_value(99, 2, 16) == 99, "99 above the cap is not lowered");
    CHECK(spawn_scale_value(16, 8, 16) == 16, "16 x8 stays 16");
    CHECK(spawn_scale_value(-1, 4, 16) == -1, "-1 (unlimited) kept");
    CHECK(spawn_scale_value(0, 4, 16) == 0, "0 kept");
    CHECK(spawn_scale_value(3, 1, 16) == 3, "scale 1 = unchanged");
    CHECK(spawn_scale_value(30, 4, 60) == 60, "SetSpawnMax 30 x4 clamped to the 60-slot pool");
    CHECK(spawn_scale_value(25, 2, 60) == 50, "SetSpawnMax 25 x2 = 50");
    CHECK(spawn_scale_value(32767, 8, 60) == 32767, "no overflow on a large value");

    CHECK(spawn_scale_parse("1") == 1 && spawn_scale_parse("2") == 2 && spawn_scale_parse("8") == 8, "parse 1, 2, 8");
    CHECK(spawn_scale_parse("0") == -1 && spawn_scale_parse("9") == -1 && spawn_scale_parse("-2") == -1, "reject 0, 9, -2");
    CHECK(spawn_scale_parse("2x") == -1 && spawn_scale_parse("") == -1 && spawn_scale_parse(NULL) == -1 &&
          spawn_scale_parse(" 2") == -1, "reject '2x', '', NULL, ' 2'");

    map_at(0x80000, 0x2000);       /* 0x806d1, 0x81d5d */
    map_at(0x1b6000, 0x1000);      /* 0x1b6110, 0x1b6130 */
    map_at(0x338000, 0x1000);      /* 0x338876, 0x338c2e */
    map_at(0x5a7000, 0x13000);     /* handle table 0x5a7570, world pointer 0x5b9f78 */
    static uint8_t world[0x1000], rec[16];
    D32(SPAWN_WORLD_PTR) = (uint32_t)(uintptr_t)world;

    /* Scale 1: nothing patched, the original stores run. */
    plant();
    CHECK(hle_spawn_install(1) == 0, "scale 1 patches nothing");
    CHECK(!memcmp(AT(SPAWN_ADD_STORE), add_store, sizeof add_store) && !memcmp(AT(SPAWN_SETMAX_STORE), setmax_store, sizeof setmax_store) &&
          !memcmp(AT(SPAWN_SETSPAWNMAX), setspawnmax, sizeof setspawnmax) && !memcmp(AT(SPAWN_SETCOPMAX), setcopmax, sizeof setcopmax) &&
          !memcmp(AT(SPAWN_DEFAULT_CAPS), default_caps, sizeof default_caps) &&
          !memcmp(AT(SPAWN_RESET_CAPS), reset_caps, sizeof reset_caps),
          "scale 1: guest bytes unchanged");
    CHECK(run_add_store(3, rec) == 3, "scale 1: add store writes 3");
    CHECK(run_setter(SPAWN_SETSPAWNMAX, 30, 0x420, world) == 30, "scale 1: SetSpawnMax writes 30");

    /* A site whose bytes differ is left alone. */
    plant();
    AT(SPAWN_ADD_STORE)[2] = 0x8e;
    CHECK(hle_spawn_install(2) == 5, "scale 2 with one mismatching site: 5 sites patched");
    CHECK(AT(SPAWN_ADD_STORE)[0] == 0x66 && AT(SPAWN_ADD_STORE)[2] == 0x8e, "mismatching site untouched");

    plant();
    CHECK(hle_spawn_install(2) == 6, "scale 2: 6 sites patched");
    CHECK(D32(SPAWN_DEFAULT_CAPS + 1) == 24 && !memcmp(AT(SPAWN_DEFAULT_CAPS + 5), default_caps + 5, 14),
          "scale 2: default caps 12 -> 24, stores unchanged");
    CHECK(D32(SPAWN_RESET_CAPS + 1) == 32 && !memcmp(AT(SPAWN_RESET_CAPS + 5), reset_caps + 5, 14),
          "scale 2: level reset caps 16 -> 32, stores unchanged");
    plant();
    CHECK(hle_spawn_install(4) == 6 && D32(SPAWN_RESET_CAPS + 1) == 60, "scale 4: level reset caps 16 -> 60 (pool)");
    plant();
    hle_spawn_install(2);
    CHECK(AT(SPAWN_ADD_STORE)[0] == 0xe8 && AT(SPAWN_ADD_STORE)[5] == 0x90 && AT(SPAWN_ADD_STORE)[6] == 0x90, "add store: call + 2 nops");
    CHECK(AT(SPAWN_SETMAX_STORE)[0] == 0xe8 && AT(SPAWN_SETMAX_STORE)[7] == 0x90, "set-max store: call + 3 nops");
    CHECK(run_add_store(3, rec) == 6, "scale 2: GangAddSpawner max 3 -> 6");
    CHECK(run_add_store(10, rec) == 16, "scale 2: GangAddSpawner max 10 -> 16 (gang slots)");
    CHECK(run_add_store(-1, rec) == -1, "scale 2: GangAddSpawner max -1 kept");
    CHECK(run_setmax_store(5, rec) == 10, "scale 2: GangSetMaxConcurrent 5 -> 10");
    CHECK(run_setmax_store(40, rec) == 40, "scale 2: GangSetMaxConcurrent 40 not lowered");
    CHECK(run_setter(SPAWN_SETSPAWNMAX, 25, 0x420, world) == 50, "scale 2: SetSpawnMax 25 -> 50");
    CHECK(run_setter(SPAWN_SETSPAWNMAX, 40, 0x420, world) == 60, "scale 2: SetSpawnMax 40 -> 60 (pool)");
    CHECK(run_setter(SPAWN_SETCOPMAX, 3, 0x31e, world) == 6, "scale 2: SetCopSpawnMax 3 -> 6");

    /* Pool reader. */
    W16(world + 0x41e) = 7; W16(world + 0x420) = 50; W16(world + 0x31c) = 2; W16(world + 0x31e) = 6;
    memset(AT(SPAWN_HANDLE_TABLE), 0, 0xb00 * 8);
    for (int i = 0; i < 9; i++) D32(SPAWN_HANDLE_TABLE + (uint32_t)(i * 6) * 8) = 0x1000 + i;   /* 0, 6, ..., 48 */
    D32(SPAWN_HANDLE_TABLE + 60 * 8) = 1;   /* not a character slot */
    D32(SPAWN_HANDLE_TABLE + 59 * 8 + 4) = 1;   /* second dword only: free */
    SpawnPool p;
    spawn_pool_read(&p);
    CHECK(p.valid && p.live == 7 && p.cap == 50 && p.cops == 2 && p.cop_cap == 6, "pool: counts and caps from the world object");
    CHECK(p.slots == 9, "pool: 9 occupied character slots (got %d)", p.slots);
    D32(SPAWN_WORLD_PTR) = 0;
    spawn_pool_read(&p);
    CHECK(!p.valid && p.slots == 9, "pool: no world object yet, slots still counted");

    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
#endif
