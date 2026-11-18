/* Game-independent test of the --ai-debug core (src/hle_aidbg.c built with HLE_AIDBG_TEST): a
 * fake guest (brain table, character pool, handle table, positions, bodies, vtables followed by
 * class names, goal and action objects) read through the same callback the runtime uses. */
#include "../src/hle_aidbg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ai_name_cache_clear(void);

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

/* Fake guest: [0x400000, 0x5c0000) image + bss, [0x20000000, 0x20010000) heap. */
#define IMG_LO 0x00400000u
#define IMG_HI 0x005c0000u
#define HEAP_LO 0x20000000u
#define HEAP_HI 0x20010000u
static uint8_t *img, *heap;
static const void *rd(uint32_t a, uint32_t len, void *ctx) {
    (void)ctx;
    if (a >= IMG_LO && a + len <= IMG_HI && a + len > a) return img + (a - IMG_LO);
    if (a >= HEAP_LO && a + len <= HEAP_HI && a + len > a) return heap + (a - HEAP_LO);
    return NULL;
}
static uint8_t *at(uint32_t a) { return a >= HEAP_LO ? heap + (a - HEAP_LO) : img + (a - IMG_LO); }
static void w32(uint32_t a, uint32_t v) { memcpy(at(a), &v, 4); }
static void w16(uint32_t a, int16_t v) { memcpy(at(a), &v, 2); }

/* A vtable of n code pointers followed by a name. */
static uint32_t vt_next = 0x431000;
static uint32_t vtable(const char *name, int n) {
    uint32_t vt = vt_next;
    for (int i = 0; i < n; i++) w32(vt + 4 * i, 0x1d0000 + 0x10 * i);
    strcpy((char *)at(vt + 4 * n), name);
    vt_next += 0x40;
    return vt;
}
static uint32_t heap_next = HEAP_LO + 0x100;
static uint32_t object(uint32_t vt) {
    uint32_t o = heap_next;
    w32(o, vt);
    heap_next += 0x40;
    return o;
}
static uint32_t brain(int i) { return AI_BRAINS + AI_BRAIN_STRIDE * (uint32_t)i; }
static uint32_t handle(int slot) { return ((uint32_t)slot << 16) | (uint32_t)(0x100 + slot); }
static void character(int i, int player, uint32_t gang, uint32_t goal, uint32_t action, float x, int hp) {
    uint32_t ch = AI_CHARS + AI_CHAR_STRIDE * (uint32_t)i;
    w32(brain(i), ch);
    at(ch + 0x180)[0] = (uint8_t)(int8_t)player;
    w32(brain(i) + 0x1f8, gang);
    at(brain(i) + 0x20)[0] = goal ? 1 : 0xff;   /* goal stack top */
    w32(brain(i) + 0x34 + 4, goal);
    at(brain(i) + 0x22)[0] = action ? 1 : 0;
    at(brain(i) + 0x23)[0] = 3;
    w32(brain(i) + 0x5c + 4 * 3, action);
    w32(AI_HANDLES + 8 * (uint32_t)i, ch);
    w16(AI_HANDLES + 8 * (uint32_t)i + 4, (int16_t)(0x100 + i));
    float p[3] = {x, 0, 0};
    memcpy(at(AI_POS + AI_POS_STRIDE * (uint32_t)i), p, 12);
    w16(AI_BODIES + AI_BODY_STRIDE * (uint32_t)i + AI_BODY_HEALTH, (int16_t)hp);
}

int main(void) {
    img = calloc(IMG_HI - IMG_LO, 1);
    heap = calloc(HEAP_HI - HEAP_LO, 1);
    char name[AI_NAME_MAX];

    /* Class names. */
    uint32_t vt_spectate = vtable("Spectate", 10), vt_fight = vtable("Fight", 10), vt_attack = vtable("Attack", 6);
    uint32_t vt_follow = vtable("FollowAndAttack", 14);
    ai_class_name(rd, NULL, vt_spectate, name, sizeof name);
    CHECK(!strcmp(name, "Spectate"), "class name after 10 code pointers: '%s'", name);
    ai_class_name(rd, NULL, vt_follow, name, sizeof name);
    CHECK(!strcmp(name, "FollowAndAttack"), "class name after 14 code pointers: '%s'", name);
    uint32_t vt_bad = vt_next;
    for (int i = 0; i < 4; i++) w32(vt_bad + 4 * i, 0x1d0000);
    strcpy((char *)at(vt_bad + 16), "lower");
    vt_next += 0x40;
    ai_class_name(rd, NULL, vt_bad, name, sizeof name);
    CHECK(name[0] == 0, "a name must start with a capital letter: '%s'", name);
    ai_class_name(rd, NULL, 0x700000, name, sizeof name);
    CHECK(name[0] == 0, "unreadable vtable: no name");

    /* Handles. */
    uint32_t gang_a = 0x4b1c58, gang_b = 0x4b1c58 + 0xabc;
    character(7, -1, gang_b, 0, 0, 0, 100);
    CHECK(ai_handle_slot(rd, NULL, handle(7)) == 7, "live handle -> slot 7");
    CHECK(ai_handle_slot(rd, NULL, handle(7) ^ 1) == -1, "wrong serial -> -1");
    CHECK(ai_handle_slot(rd, NULL, (60u << 16) | 5) == -1, "slot 60 is not a character -> -1");
    CHECK(ai_handle_slot(rd, NULL, 0xffffffffu) == -1, "-1 handle -> -1");
    w32(AI_HANDLES + 8 * 7, 0x12345678);
    CHECK(ai_handle_slot(rd, NULL, handle(7)) == -1, "entry not pointing at the character -> -1");

    /* A fight: player 0 and crew 1, 2 (gang A) against enemy 5 (gang B); civilian 9 (gang B) idle. */
    uint32_t g1 = object(vt_fight), g2 = object(vt_spectate), g5 = object(vt_fight), g9 = object(vt_spectate);
    w32(g1 + 0xc, handle(5));
    w32(g2 + 0x10, handle(5));
    w32(g5 + 0xc, handle(1));
    w32(g9 + 0x10, 0xffffffffu);
    w32(g2 + 0x4, handle(2));   /* its own handle first: skipped */
    character(0, 0, gang_a, 0, 0, 0, 1800);
    character(1, -1, gang_a, g1, object(vt_attack), 1, 1500);
    character(2, -1, gang_a, g2, 0, 3, 1500);
    character(5, -1, gang_b, g5, 0, 2, 900);
    character(9, -1, gang_b, g9, 0, 2.5f, 400);
    character(7, -1, gang_b, 0, 0, 40, 100);
    AiChar all[AI_SLOTS];
    ai_name_cache_clear();
    int player = ai_snapshot(rd, NULL, all);
    CHECK(player == 0, "player slot 0 (got %d)", player);
    CHECK(all[1].ally && all[2].ally && !all[0].ally && !all[5].ally && !all[9].ally, "crew = gang A without the player");
    CHECK(!strcmp(all[1].goal, "Fight") && !strcmp(all[1].action, "Attack") && all[1].target == 5,
          "crew 1: Fight / Attack -> 5 (%s / %s -> %d)", all[1].goal, all[1].action, all[1].target);
    CHECK(!strcmp(all[2].goal, "Spectate") && all[2].action[0] == 0 && all[2].target == 5,
          "crew 2: Spectate, no action, own handle skipped -> 5 (%s / '%s' -> %d)", all[2].goal, all[2].action, all[2].target);
    CHECK(all[5].target == 1 && all[5].hp == 900 && all[5].pos[0] == 2, "enemy 5 -> crew 1, hp 900, x 2");
    CHECK(!all[3].used && all[9].target == -1, "unused slot; -1 handle -> no target");
    w32(brain(9) + 0x110, handle(2));
    w32(brain(1) + 0x110, handle(1));   /* itself: ignored, the goal's target stays */
    ai_snapshot(rd, NULL, all);
    CHECK(all[9].target == 2 && all[1].target == 5, "brain +0x110 target first (9 -> %d), own handle ignored (1 -> %d)",
          all[9].target, all[1].target);
    w32(brain(9) + 0x110, 0xffffffffu);
    w32(brain(5) + 0x1dc, AI_CHARS + AI_CHAR_STRIDE);
    w32(brain(5) + 0x1e4, AI_CHARS + 7 * AI_CHAR_STRIDE);
    at(brain(5) + 0x136)[0] = 2;
    at(brain(0) + 0x137)[0] = 3;
    ai_snapshot(rd, NULL, all);
    CHECK(all[5].attackers == 2 && all[5].max_attackers == 2 && all[0].max_attackers == 3,
          "attackers 2 of max 2 on enemy 5; the player's max from +0x137 (%d/%d, %d)", all[5].attackers,
          all[5].max_attackers, all[0].max_attackers);
    CHECK(ai_is_crew_enemy(all, 5, player) && !ai_is_crew_enemy(all, 9, player) && !ai_is_crew_enemy(all, 1, player),
          "crew enemy: 5 (targets crew), not civilian 9, not crew 1");
    CHECK(ai_is_engaging(&all[1]) && !ai_is_engaging(&all[2]), "Fight/Attack engages, Spectate does not");
    AiChar mm = all[2];
    strcpy(mm.action, "MoveMelee");
    CHECK(!ai_is_engaging(&mm), "MoveMelee alone (circling) is not engaging");
    mm.listed = 1;
    CHECK(ai_is_engaging(&mm), "holding an attack slot is engaging");

    /* Statistics over two samples of 100 ms: crew 2 idles near enemy 5, crew 1 engages; then the
     * enemy loses 60 health, credited to both crew members targeting it. */
    AiStats s;
    memset(&s, 0, sizeof s);
    ai_stats_add(&s, all, player, 100);
    w16(AI_BODIES + AI_BODY_STRIDE * 5 + AI_BODY_HEALTH, 840);
    w16(AI_BODIES + AI_BODY_STRIDE * 1 + AI_BODY_HEALTH, 1450);
    ai_snapshot(rd, NULL, all);
    ai_stats_add(&s, all, player, 100);
    CHECK(s.near_ms[2] == 200 && s.idle_near_ms[2] == 200 && s.engaged_ms[2] == 0, "crew 2: 200 ms near, all idle");
    CHECK(s.near_ms[1] == 200 && s.idle_near_ms[1] == 0 && s.engaged_ms[1] == 200, "crew 1: 200 ms near, engaged");
    CHECK(s.enemy_damage == 60 && s.dealt[1] == 60 && s.dealt[2] == 60, "enemy damage 60, credited to both attackers");
    CHECK(s.hits_taken[1] == 1 && s.hits_taken[2] == 0, "crew 1 took one hit");
    CHECK(s.pile_samples == 2 && s.pile_sum == 4 && s.pile_max == 2, "two crew members on one enemy, twice");
    /* Far away: not near. */
    float far[3] = {50, 0, 0};
    memcpy(at(AI_POS + AI_POS_STRIDE * 5), far, 12);
    ai_snapshot(rd, NULL, all);
    ai_stats_add(&s, all, player, 100);
    CHECK(s.near_ms[2] == 200 && s.idle_near_ms[2] == 200, "enemy 50 m away: no near time added");

    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
