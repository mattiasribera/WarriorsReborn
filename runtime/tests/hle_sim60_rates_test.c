/* Game-independent check of the --sim60 rate entries for counters that count world updates:
 * the litter (both pools), the airborne fail-safe and the combat
 * counters. A host buffer stands in for the guest image; every table entry is planted with its
 * own original bytes, the whole table is applied, and the sites must then hold the values that
 * keep each duration in game time with twice as many updates. */
#include "../src/hle_sim60.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

enum { IMG_SIZE = 0x005d5000 };
static uint8_t img[IMG_SIZE];

static uint32_t rd32(uint32_t va) { uint32_t v; memcpy(&v, img + va, 4); return v; }
static uint16_t rd16(uint32_t va) { uint16_t v; memcpy(&v, img + va, 2); return v; }
static float rdf(uint32_t va) { float v; memcpy(&v, img + va, 4); return v; }
static int has(uint32_t va, const uint8_t *b, size_t n) { return !memcmp(img + va, b, n); }
#define HAS(va, ...) has(va, (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))

/* The table entry that starts at va, or NULL. */
static const Sim60Patch *entry_at(uint32_t va) {
    for (int i = 0; i < sim60_count; i++)
        if (sim60_table[i].va == va) return &sim60_table[i];
    return NULL;
}

int main(void) {
    for (int i = 0; i < sim60_count; i++) memcpy(img + sim60_table[i].va, sim60_table[i].orig, sim60_table[i].len);
    char report[16384];
    int n = sim60_apply(sim60_table, sim60_count, 1, (intptr_t)(uintptr_t)img, report, sizeof report);
    CHECK(n == sim60_count && strstr(report, "skipped: none"), "title table applies to a planted image (%d entries)", n);

    /* Litter pool 1 (FUN_0008bc00, 64 pieces): every duration is counted in updates. */
    CHECK(img[0x8bd70] == 40, "litter: ground probe every 40 updates (%u)", img[0x8bd70]);
    CHECK(rd32(0x8ae05) == 1800 && rd32(0x8ae0a) == 1200, "litter: settle time random(1200, 1800) updates (%u, %u)",
          rd32(0x8ae0a), rd32(0x8ae05));
    CHECK(rd16(0x8be93) == 120 && rd16(0x8bf6f) == 120, "litter: lifetime 120 updates at both sites (%u, %u)",
          rd16(0x8be93), rd16(0x8bf6f));
    CHECK(HAS(0x8adc8, 0x6a, 60, 0x6a, 2) && HAS(0x8bf80, 0x6a, 60, 0x6a, 2), "litter: wind kick every random(2, 60) updates (spawn, refresh)");
    CHECK(HAS(0x8b79b, 0x6a, 120, 0x89, 0x48, 0x0c, 0x6a, 60), "litter: tumble blend random(60, 120) updates");
    CHECK(rd32(0x8bfd4) == 0x420774u && rdf(rd32(0x8bfda)) == 0.16333334f / 2,
          "litter: gravity per update halved (x %08x, + %g)", rd32(0x8bfd4), rdf(rd32(0x8bfda)));
    /* The kick impulses are velocities (units/s): with the kick interval doubled they must keep
     * the title's 1/30, so the old halving entries are gone. */
    CHECK(!entry_at(0x8b925) && !entry_at(0x8ba36), "litter: kick velocities keep 1/30 (no entry at 0x8b925 / 0x8ba36)");
    CHECK(entry_at(0x8c390) != NULL, "litter: position step stays 1/60 per update");

    /* Litter pool 2 (FUN_0008d0c0, 36 pieces). */
    CHECK(HAS(0x8cd6f, 0x6a, 120, 0x6a, 40) && HAS(0x8d1fa, 0x6a, 120, 0x6a, 40), "litter 2: kick every random(40, 120) updates");
    CHECK(HAS(0x8d094, 0x6a, 120, 0x89, 0x48, 0x0c, 0x6a, 60), "litter 2: sway blend random(60, 120) updates");
    CHECK(rd16(0x8d1eb) == 120, "litter 2: lifetime 120 updates (%u)", rd16(0x8d1eb));

    /* Airborne fail-safe (human update, 0x16b289): +0x344 counts airborne updates and the mover's
     * +0x68 (FUN_002503a0) counts stuck moves; both must reach 2 s. */
    CHECK(img[0x16b294] == 120 && img[0x16b2aa] == 120, "airborne fail-safe: 120 airborne and 120 stuck updates (%u, %u)",
          img[0x16b294], img[0x16b2aa]);

    /* AI clocks: `counter % N == 0` periodic checks on the brain age
     * (+0x28) or a goal's update counter (+0x18), both +1 per world update, that the main table
     * does not cover. A separate table (sim60_ai_table, --no-sim60-ai-clocks for A/B): every
     * divisor N (`mov r32, N` before the div) becomes 2N, so each check keeps its period in game
     * time. */
    static const struct { uint32_t va; uint8_t op; uint32_t n; const char *what; } ai[] = {
        {0x1d11e3, 0xb9, 30, "Fight goal target check"}, {0x1e190d, 0xb9, 30, "HTLDefenseFight"},
        {0x1e2245, 0xb9, 30, "HTLOffenseFight"}, {0x2106a0, 0xb9, 30, "MoveMelee re-path"},
        {0x1cac28, 0xbe, 10, "Wounded 10"}, {0x1cac5f, 0xb9, 15, "Wounded 15"},
        {0x1a9878, 0xb9, 5, "brain stance"}, {0x1ad2fe, 0xb9, 44, "brain 44"}, {0x1ad37c, 0xb9, 5, "brain 5"},
        {0x1c3539, 0xbf, 20, "ped 20"}, {0x1d50c1, 0xb9, 30, "brain state 2, 30"}, {0x1d6ee7, 0xbe, 3, "brain state 3, 3"},
        {0x1e7fc1, 0xbf, 30, "ThrowObject"}, {0x1ed0a5, 0xb9, 45, "Scout"}, {0x1ee5a5, 0xb9, 3, "Backoff"},
        {0x1f26fd, 0xb9, 120, "goal 0x1f26d0"}, {0x21678b, 0xbf, 10, "player brain 10"}, {0x216854, 0xb9, 20, "player brain 20"},
        {0x216a89, 0xb9, 10, "player brain 10 b"}, {0x216b66, 0xb9, 300, "brain gang count"}, {0x216c68, 0xb9, 40, "brain 40"},
        {0x216cbe, 0xb9, 13, "brain scan 13"},
    };
    const int nai = (int)(sizeof ai / sizeof ai[0]);
    /* Blocking goal update FUN_001caff0 (per world update): a blocker whose attacker is in an attack
     * action rolls rand(100) < its counter chance (FUN_00140920) on every update and counters on a
     * success (action 3), so at 60 Hz it rolled twice as often per attack. The roll's entry
     * (0x1cb14a: mov al,[esi+0x15]; test al,al) goes to a cave that skips the roll on odd steps (as
     * a failed roll, to 0x1cb114). Its "guard just raised" count +0x17 (cmp al,5 at 0x1cb000) is
     * world updates too: 5 -> 10. */
    static const uint8_t counter_site[] = {0x8a, 0x46, 0x15, 0x84, 0xc0};
    static const uint8_t guard_site[] = {0x3c, 0x05};
    memset(img, 0, sizeof img);
    for (int i = 0; i < nai; i++) { img[ai[i].va] = ai[i].op; memcpy(img + ai[i].va + 1, &ai[i].n, 4); }
    memcpy(img + 0x1cb14a, counter_site, sizeof counter_site);
    memcpy(img + 0x1cb000, guard_site, sizeof guard_site);
    n = sim60_apply(sim60_ai_table, sim60_ai_count, 1, (intptr_t)(uintptr_t)img, report, sizeof report);
    CHECK(img[0x1cb000] == 0x3c && img[0x1cb001] == 10, "Blocking guard count: cmp al,5 -> 10 (%u)", img[0x1cb001]);
    {
        uint32_t cave = 0x1cb14f + rd32(0x1cb14b);   /* jmp rel32 target */
        if (cave > IMG_SIZE - 32) cave = IMG_SIZE - 32;   /* not patched: keep the reads in the image */
        CHECK(img[0x1cb14a] == 0xe9 && cave == 0x5d4c00, "Blocking counter roll: 0x1cb14a jumps to the cave at 005d4c00 (%06x)", cave);
        static const uint8_t want[] = {0xf6, 0x05, 0xfc, 0x25, 0x58, 0x00, 0x01,   /* test byte [0x5825fc],1 */
                                       0x0f, 0x85};                              /* jne (odd step: no roll) */
        CHECK(!memcmp(img + cave, want, sizeof want) && cave + 13 + rd32(cave + 9) == 0x1cb114,
              "cave: odd steps skip to 0x1cb114 (%06x)", cave + 13 + rd32(cave + 9));
        CHECK(!memcmp(img + cave + 13, counter_site, sizeof counter_site) && img[cave + 18] == 0xe9 && cave + 23 + rd32(cave + 19) == 0x1cb14f,
              "cave: even steps replay the test and continue at 0x1cb14f (%06x)", cave + 23 + rd32(cave + 19));
    }
    CHECK(n == nai + 3 && sim60_ai_count == nai + 3, "AI clock table applies to a planted image (%d of %d entries)", n, nai + 3);
    for (int i = 0; i < nai; i++)
        CHECK(img[ai[i].va] == ai[i].op && rd32(ai[i].va + 1) == 2 * ai[i].n, "AI clock %s at %06x: %% %u -> %% %u (%u)", ai[i].what, ai[i].va,
              ai[i].n, 2 * ai[i].n, rd32(ai[i].va + 1));
    /* Off (--no-sim60-ai-clocks) or a mismatching site: nothing is written. */
    memset(img, 0, sizeof img);
    for (int i = 0; i < nai; i++) { img[ai[i].va] = ai[i].op; memcpy(img + ai[i].va + 1, &ai[i].n, 4); }
    CHECK(sim60_apply(sim60_ai_table, sim60_ai_count, 0, (intptr_t)(uintptr_t)img, report, sizeof report) == 0 && rd32(ai[0].va + 1) == 30,
          "AI clock table off: bytes unchanged");
    img[ai[3].va + 1] = 31;
    CHECK(sim60_apply(sim60_ai_table, sim60_ai_count, 1, (intptr_t)(uintptr_t)img, report, sizeof report) == 0 && rd32(ai[0].va + 1) == 30,
          "AI clock table with one mismatching site: nothing written");

    if (failures) printf("%d FAILED\n", failures);
    else printf("ALL OK\n");
    return failures != 0;
}
