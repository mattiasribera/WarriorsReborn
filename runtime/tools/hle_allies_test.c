/* Game-independent test of --smart-allies (src/hle_allies.c): the score and circle rules, the
 * option parser, and the two patched call sites run on a fake guest (brains, characters, handle
 * table at their guest addresses; a stand-in target score function and ring action). */
#include "../src/hle_aidbg.h"
#include "../src/hle_allies.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)
#define AT(a) ((uint8_t *)(uintptr_t)(a))
#define D32(a) (*(uint32_t *)(uintptr_t)(a))

static void map_at(uint32_t lo, uint32_t len) {
    void *m = mmap((void *)(uintptr_t)lo, len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m != (void *)(uintptr_t)lo) { perror("mmap"); exit(1); }
    memset(m, 0, len);
}
static uint32_t brain(int i) { return AI_BRAINS + AI_BRAIN_STRIDE * (uint32_t)i; }
static uint32_t chr(int i) { return AI_CHARS + AI_CHAR_STRIDE * (uint32_t)i; }
static uint32_t handle(int i) { return ((uint32_t)i << 16) | (uint32_t)(0x200 + i); }
static void character(int i, int player, uint32_t gang, int target) {
    D32(brain(i)) = chr(i);
    AT(chr(i) + 0x180)[0] = (uint8_t)(int8_t)player;
    D32(brain(i) + 0x1f8) = gang;
    D32(brain(i) + 0x110) = target >= 0 ? handle(target) : 0xffffffffu;
    D32(AI_HANDLES + 8 * (uint32_t)i) = chr(i);
    *(uint16_t *)(uintptr_t)(AI_HANDLES + 8 * (uint32_t)i + 4) = (uint16_t)(0x200 + i);
}

/* The sites are planted as "call target" then "add esp, 12; ret" (cdecl) or "ret" (thiscall, the
 * callee pops): the helpers push a return address, then the arguments, and jump to the site, so
 * its call sees them as the title's caller left them. */
/* cdecl (self, cand, mask) through the patched call. */
__asm__(".text\n.globl call_site3\ncall_site3:\n"
        "push %ebx\npush %esi\npush %edi\npush %ebp\n"
        "mov 20(%esp),%eax\npush $1f\npush 36(%esp)\npush 36(%esp)\npush 36(%esp)\njmp *%eax\n1:\n"
        "pop %ebp\npop %edi\npop %esi\npop %ebx\nret\n");
int call_site3(uint32_t site, uint32_t a, uint32_t b, uint32_t c);
/* thiscall (ecx = this; brain, target, min, max, 0, ms) through the patched call. */
__asm__(".text\n.globl call_site6\ncall_site6:\n"
        "push %ebx\npush %esi\npush %edi\npush %ebp\n"
        "mov 20(%esp),%eax\nmov 24(%esp),%ecx\n"
        "push $1f\n"
        "push 52(%esp)\npush 52(%esp)\npush 52(%esp)\npush 52(%esp)\npush 52(%esp)\npush 52(%esp)\n"
        "jmp *%eax\n1:\n"
        "pop %ebp\npop %edi\npop %esi\npop %ebx\nret\n");
uint32_t call_site6(uint32_t site, uint32_t self, uint32_t brain, uint32_t tgt, uint32_t mn, uint32_t mx, uint32_t z, uint32_t ms);

int main(void) {
    /* Rules. */
    AllyScoreIn in = {0};
    in.attackers = 2, in.max_attackers = 2;
    CHECK(allies_score_adjust(&in) == 0, "not crew: no change");
    in.crew = 1;
    CHECK(allies_score_adjust(&in) == -ALLY_FULL_PENALTY, "crew, candidate full (2 of 2): -%d", ALLY_FULL_PENALTY);
    in.self_listed = 1;
    CHECK(allies_score_adjust(&in) == 0, "already one of its attackers: not full for it");
    in.self_listed = 0, in.attackers = 1, in.crew_on_it = 2;
    CHECK(allies_score_adjust(&in) == -2 * ALLY_SHARE_PENALTY, "two other crew members on it: -%d", 2 * ALLY_SHARE_PENALTY);
    in.crew_on_it = 0, in.targets_player = 1;
    CHECK(allies_score_adjust(&in) == ALLY_PROTECT_BONUS, "fighting the player: +%d", ALLY_PROTECT_BONUS);
    in.targets_player = 0, in.grabbing_friend = 1;
    CHECK(allies_score_adjust(&in) == ALLY_GRAB_BONUS, "holding a Warrior: +%d", ALLY_GRAB_BONUS);
    in.grabbing_friend = 0, in.max_attackers = 0, in.attackers = 3;
    CHECK(allies_score_adjust(&in) == 0, "limit 0 (unset): not counted as full");
    CHECK(allies_circle_value(1, 2000) == ALLY_CIRCLE_MS && allies_circle_value(1, 6000) == ALLY_CIRCLE_MS &&
          allies_circle_value(1, 500) == 500 && allies_circle_value(0, 2000) == 2000,
          "circle wait: crew 2000/6000 -> %u, 500 kept, others 2000 kept", ALLY_CIRCLE_MS);
    AllyCand c[3] = {{5, 2.0f, 0}, {6, 5.0f, 1}, {7, 4.0f, 1}};
    CHECK(allies_pick_open(c, 3, 9) == 2, "refused: the nearest open enemy (slot 7 at 4 m), not the full one at 2 m");
    CHECK(allies_pick_open(c, 3, 7) == 1, "the current target is not a switch (-> slot 6)");
    c[1].dist = 9.0f;
    CHECK(allies_pick_open(c, 3, 7) == -1, "nothing open within %.0f m: stay", ALLY_SWITCH_M);
    CHECK(allies_pick_open(c, 0, -1) == -1, "no candidates: stay");
    int on = 7;
    CHECK(allies_parse_arg("--smart-allies", &on) && on == 1, "--smart-allies");
    CHECK(allies_parse_arg("--no-smart-allies", &on) && on == 0, "--no-smart-allies");
    CHECK(!allies_parse_arg("--smart-ally", &on) && on == 0, "other options are not taken");

    /* Fake guest: code pages around both sites and both callees, the tables. */
    map_at(0x1ba000, 0x1000);
    map_at(0x1bc000, 0x1000);
    map_at(0x1ca000, 0x1000);
    map_at(0x210000, 0x1000);
    map_at(0x400000, 0x1c0000);
    static const uint8_t score_fn[] = {0xb8, 20, 0, 0, 0, 0xc3};   /* mov eax, 20; ret */
    memcpy(AT(ALLY_SCORE_FN), score_fn, sizeof score_fn);
    static const uint8_t moveto_fn[] = {0x8b, 0x44, 0x24, 0x18, 0xc2, 0x18, 0x00};   /* mov eax, [esp+0x18] (ms); ret 0x18 */
    memcpy(AT(ALLY_MOVETO_FN), moveto_fn, sizeof moveto_fn);
    int32_t rel = (int32_t)(ALLY_SCORE_FN - (ALLY_SCORE_CALL + 5));
    AT(ALLY_SCORE_CALL)[0] = 0xe8; memcpy(AT(ALLY_SCORE_CALL + 1), &rel, 4); memcpy(AT(ALLY_SCORE_CALL + 5), "\x83\xc4\x0c\xc3", 4);
    rel = (int32_t)(ALLY_MOVETO_FN - (ALLY_CIRCLE_CALL + 5));
    AT(ALLY_CIRCLE_CALL)[0] = 0xe8; memcpy(AT(ALLY_CIRCLE_CALL + 1), &rel, 4); AT(ALLY_CIRCLE_CALL + 5)[0] = 0xc3;
    uint8_t before[2][5];
    memcpy(before[0], AT(ALLY_SCORE_CALL), 5);
    memcpy(before[1], AT(ALLY_CIRCLE_CALL), 5);

    /* Player 0, crew 1-3 (gang A), enemies 5, 6 (gang B). Enemy 5 is attacked by crew 1 and 2
     * (limit 2) and fights crew 1; enemy 6 fights the player. Crew 3 chooses. */
    uint32_t ga = 0x4b1c58, gb = ga + 0xabc;
    character(0, 0, ga, -1);
    character(1, -1, ga, 5);
    character(2, -1, ga, 5);
    character(3, -1, ga, -1);
    character(5, -1, gb, 1);
    character(6, -1, gb, 0);
    D32(brain(5) + 0x1dc) = chr(1), D32(brain(5) + 0x1e0) = chr(2);
    AT(brain(5) + 0x136)[0] = 2;
    AT(brain(6) + 0x136)[0] = 2;

    CHECK(hle_allies_install(0) == 0 && !memcmp(AT(ALLY_SCORE_CALL), before[0], 5) && !memcmp(AT(ALLY_CIRCLE_CALL), before[1], 5),
          "off: nothing patched, bytes unchanged");
    CHECK(call_site3(ALLY_SCORE_CALL, chr(3), chr(5), 0) == 20, "unpatched site: the title's score (20)");
    CHECK(call_site6(ALLY_CIRCLE_CALL, 0x1234, brain(3), 1, 2, 3, 0, 2000) == 2000, "unpatched site: ring wait 2000");

    AT(ALLY_CIRCLE_CALL + 1)[0] ^= 1;
    CHECK(hle_allies_install(1) == 1 && AT(ALLY_CIRCLE_CALL + 1)[0] == (before[1][1] ^ 1), "a mismatching site is left alone");
    AT(ALLY_CIRCLE_CALL + 1)[0] ^= 1;
    memcpy(AT(ALLY_SCORE_CALL), before[0], 5);
    CHECK(hle_allies_install(1) == 2, "on: both sites patched");
    int full = call_site3(ALLY_SCORE_CALL, chr(3), chr(5), 0);
    CHECK(full == 20 - ALLY_FULL_PENALTY - 2 * ALLY_SHARE_PENALTY, "crew 3 on enemy 5 (full, 2 crew on it): %d", full);
    int prot = call_site3(ALLY_SCORE_CALL, chr(3), chr(6), 0);
    CHECK(prot == 20 + ALLY_PROTECT_BONUS, "crew 3 on enemy 6 (fighting the player): %d", prot);
    CHECK(call_site3(ALLY_SCORE_CALL, chr(1), chr(5), 0) == 20 - ALLY_SHARE_PENALTY, "crew 1, already listed on 5: only the share");
    CHECK(call_site3(ALLY_SCORE_CALL, chr(5), chr(1), 0) == 20, "enemy 5 choosing: the title's score");
    AT(brain(6) + 0x20)[0] = 0;
    D32(brain(6) + 0x34) = 0x20000000;
    map_at(0x20000000, 0x1000);
    D32(0x20000000) = 0x00431b14u;   /* Grabbing */
    CHECK(call_site3(ALLY_SCORE_CALL, chr(3), chr(6), 0) == 20 + ALLY_PROTECT_BONUS + ALLY_GRAB_BONUS, "enemy 6 holds the player");
    D32(0x20000000) = 0x00431b48u;   /* Mounting */
    CHECK(call_site3(ALLY_SCORE_CALL, chr(3), chr(6), 0) == 20 + ALLY_PROTECT_BONUS + ALLY_GRAB_BONUS, "enemy 6 on top of the player");
    D32(0x20000000) = 0x004319b8u;   /* Fight */
    CHECK(call_site3(ALLY_SCORE_CALL, chr(3), chr(6), 0) == 20 + ALLY_PROTECT_BONUS, "enemy 6 only fighting the player");
    CHECK(call_site6(ALLY_CIRCLE_CALL, 0x1234, brain(3), 1, 2, 3, 0, 2000) == ALLY_CIRCLE_MS, "crew ring wait 2000 -> %u",
          ALLY_CIRCLE_MS);
    CHECK(call_site6(ALLY_CIRCLE_CALL, 0x1234, brain(5), 1, 2, 3, 0, 2000) == 2000, "enemy ring wait kept at 2000");

    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
