/* Game-independent test of --enemy-tactics (src/hle_tactics.c): the rules, the option parser, and
 * the patched sites run on a fake guest (brains, characters, positions, handle table and the game
 * clock at their guest addresses; stand-ins for the gang level, attack choice and MoveTo action). */
#include "../src/hle_aidbg.h"
#include "../src/hle_allies.h"
#include "../src/hle_tactics.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* The runtime's hle_aidbg.c owns these; the test stands in for it. */
uint32_t (*ai_gate_filter)(uint32_t brain, uint32_t code);
void (*ai_gate_after)(uint32_t brain, uint32_t code);
static int gate_installs;
int hle_aidbg_install(void) { gate_installs++; return 1; }

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)
#define AT(a) ((uint8_t *)(uintptr_t)(a))
#define D32(a) (*(uint32_t *)(uintptr_t)(a))
#define F32(a) (*(float *)(uintptr_t)(a))
#define PI_F 3.14159265f

static void map_at(uint32_t lo, uint32_t len) {
    void *m = mmap((void *)(uintptr_t)lo, len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m != (void *)(uintptr_t)lo) { perror("mmap"); exit(1); }
    memset(m, 0, len);
}
static uint32_t brain(int i) { return AI_BRAINS + AI_BRAIN_STRIDE * (uint32_t)i; }
static uint32_t chr(int i) { return AI_CHARS + AI_CHAR_STRIDE * (uint32_t)i; }
static uint32_t handle(int i) { return ((uint32_t)i << 16) | (uint32_t)(0x200 + i); }
/* Facing yaw (radians from +x): the record's quaternion, whose Y axis is the forward direction. */
static void face(int i, float yaw) {
    float h = (yaw - PI_F / 2) / 2;
    F32(AI_POS + AI_POS_STRIDE * (uint32_t)i + 0x10) = 0, F32(AI_POS + AI_POS_STRIDE * (uint32_t)i + 0x14) = 0;
    F32(AI_POS + AI_POS_STRIDE * (uint32_t)i + 0x18) = sinf(h), F32(AI_POS + AI_POS_STRIDE * (uint32_t)i + 0x1c) = cosf(h);
}
static void character(int i, int player, uint32_t gang, int target, float x, float y, int hp) {
    D32(brain(i)) = chr(i);
    AT(chr(i) + 0x180)[0] = (uint8_t)(int8_t)player;
    D32(brain(i) + 0x1f8) = gang;
    D32(brain(i) + 0x110) = target >= 0 ? handle(target) : 0xffffffffu;
    AT(brain(i) + 0x20)[0] = 0xff;   /* empty goal stack */
    D32(AI_HANDLES + 8 * (uint32_t)i) = chr(i);
    *(uint16_t *)(uintptr_t)(AI_HANDLES + 8 * (uint32_t)i + 4) = (uint16_t)(0x200 + i);
    F32(AI_POS + AI_POS_STRIDE * (uint32_t)i) = x, F32(AI_POS + AI_POS_STRIDE * (uint32_t)i + 4) = y;
    face(i, 0.0f);   /* facing +x */
    *(int16_t *)(uintptr_t)(AI_BODIES + AI_BODY_STRIDE * (uint32_t)i + AI_BODY_HEALTH) = (int16_t)hp;
}
static void set_clock(uint32_t ms) { D32(0x20000000u + 0x40) = ms; }

/* call site with ecx = this and 1 / 3 stack arguments, pushed as the title's caller would. */
__asm__(".text\n.globl call_this1\ncall_this1:\n"
        "push %ebx\npush %esi\npush %edi\npush %ebp\n"
        "mov 20(%esp),%eax\nmov 24(%esp),%ecx\nmov 28(%esp),%esi\n"
        "push $1f\npush 36(%esp)\njmp *%eax\n1:\n"
        "pop %ebp\npop %edi\npop %esi\npop %ebx\nret\n");
/* (site, ecx, esi, arg) */
uint32_t call_this1(uint32_t site, uint32_t ecx, uint32_t esi, uint32_t a1);
__asm__(".text\n.globl call_this3\ncall_this3:\n"
        "push %ebx\npush %esi\npush %edi\npush %ebp\n"
        "mov 20(%esp),%eax\nmov 24(%esp),%ecx\n"
        "push $1f\npush 40(%esp)\npush 40(%esp)\npush 40(%esp)\njmp *%eax\n1:\n"
        "pop %ebp\npop %edi\npop %esi\npop %ebx\nret\n");
/* (site, ecx, a1, a2, a3) */
uint32_t call_this3(uint32_t site, uint32_t ecx, uint32_t a1, uint32_t a2, uint32_t a3);
__asm__(".text\n.globl call_this0\ncall_this0:\n"
        "push %ebx\npush %esi\npush %edi\npush %ebp\n"
        "mov 20(%esp),%eax\nmov 24(%esp),%ecx\n"
        "push $1f\njmp *%eax\n1:\n"
        "pop %ebp\npop %edi\npop %esi\npop %ebx\nret\n");
/* (site, ecx) */
uint32_t call_this0(uint32_t site, uint32_t ecx);
uint32_t call_site6(uint32_t site, uint32_t self, uint32_t b, uint32_t tgt, uint32_t mn, uint32_t mx, uint32_t z, uint32_t ms);

/* Stand-in attack choice (thiscall brain; target, predicate, 0): returns the weight the title
 * would read for the grab id (brain+0x1f4 table when set, else brain+0x280). */
static uint32_t __attribute__((thiscall)) fake_choice(uint32_t b, uint32_t t, uint32_t p, uint32_t z) {
    (void)t, (void)p, (void)z;
    uint32_t tab = D32(b + 0x1f4);
    return tab ? AT(tab)[TACT_ID_GRAB] : AT(b + 0x280)[TACT_ID_GRAB];
}
/* "call target; ret": the callee pops its arguments (thiscall), then back to the helper. */
static void plant_call(uint32_t site, uint32_t target) {
    int32_t rel = (int32_t)(target - (site + 5));
    AT(site)[0] = 0xe8;
    memcpy(AT(site + 1), &rel, 4);
    AT(site + 5)[0] = 0xc3;
}
static void plant_jmp(uint32_t at, const void *to) {
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    AT(at)[0] = 0xe9;
    memcpy(AT(at + 1), &rel, 4);
}
static int near(float a, float b) { return fabsf(a - b) < 1e-3f; }

int main(void) {
    /* ---- rules ---- */
    float out[TACT_MAX_WAIT];
    float one_l = 0.3f, one_r = -0.3f;
    tactics_flank_assign(&one_l, 1, out);
    CHECK(near(out[0], PI_F / 2), "one waiter front-left -> left side (%.2f)", out[0]);
    tactics_flank_assign(&one_r, 1, out);
    CHECK(near(out[0], 3 * PI_F / 2), "one waiter front-right -> right side (%.2f)", out[0]);
    float two[2] = {-0.2f, 0.1f};
    tactics_flank_assign(two, 2, out);
    CHECK(near(out[1], PI_F / 2) && near(out[0], 3 * PI_F / 2), "two waiters in front -> left and right, no crossing");
    float three[3] = {3.0f, 0.5f, -1.0f};
    tactics_flank_assign(three, 3, out);
    CHECK(near(out[1], PI_F / 2) && near(out[0], PI_F) && near(out[2], 3 * PI_F / 2), "three waiters -> left, back, right in angle order");
    float four[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    tactics_flank_assign(four, 4, out);
    int fan = 1;
    for (int i = 0; i < 4; i++) fan &= near(out[i], PI_F / 2 + (float)i * PI_F / 3) && out[i] >= PI_F / 2 - 1e-3f && out[i] <= 3 * PI_F / 2 + 1e-3f;
    CHECK(fan, "four waiters fan over sides and back (60 degree steps), none in front");

    CHECK(!tactics_turn_over(TACT_TURN_MS - 1, 3) && tactics_turn_over(TACT_TURN_MS, 1) && !tactics_turn_over(TACT_TURN_MS * 5, 0),
          "turns: after %u ms and only when someone waits", TACT_TURN_MS);
    CHECK(tactics_guard_level(0) == 0 && tactics_guard_level(1) == TACT_GUARD_LEVEL && tactics_guard_level(2) == TACT_GUARD_LEVEL &&
          tactics_guard_level(4) == 4, "guard level: 0 kept, 1-2 -> %d, higher kept", TACT_GUARD_LEVEL);

    uint8_t w[TACT_ATTACK_IDS] = {0}, wo[TACT_ATTACK_IDS];
    w[0] = 10, w[6] = 60, w[8] = 0, w[9] = 200, w[TACT_ID_GUARD_BREAK] = 55, w[TACT_ID_GRAB] = 5, w[31] = 100;
    tactics_weights(w, wo);
    CHECK(wo[0] == 10 && wo[31] == 100, "other ids unchanged");
    CHECK(wo[6] == 120 && wo[9] == 255 && wo[8] == 0, "finishers x2 (cap 255), a missing finisher stays 0");
    CHECK(wo[TACT_ID_GUARD_BREAK] == 165 && wo[TACT_ID_GRAB] == 30, "guard break x3, grab x3 with a floor of 30");
    w[TACT_ID_GRAB] = 0;
    tactics_weights(w, wo);
    CHECK(wo[TACT_ID_GRAB] == 0, "a class without a grab never gets one");

    TactIdle idle = {0};
    float p0[3] = {0, 0, 0}, p1[3] = {0.5f, 0, 0}, p2[3] = {1.5f, 0, 0};
    CHECK(!tactics_idle_update(&idle, 1000, p0, 0), "idle: first sample is not passive");
    CHECK(!tactics_idle_update(&idle, 1000 + TACT_IDLE_MS - 1, p1, 0) && tactics_idle_update(&idle, 1000 + TACT_IDLE_MS, p1, 0),
          "idle: passive after %u ms within %.2f m", TACT_IDLE_MS, TACT_IDLE_M);
    CHECK(!tactics_idle_update(&idle, 5000, p1, 1), "idle: a swing resets it");
    CHECK(!tactics_idle_update(&idle, 9000, p2, 0) && !tactics_idle_update(&idle, 9000 + TACT_IDLE_MS - 1, p2, 0),
          "idle: a step resets it");

    TactScoreIn in = {0};
    in.cand_crew = 1, in.cand_hp = 10, in.cand_hp_max = 100;
    CHECK(tactics_score_adjust(&in) == 0, "not an enemy chooser: no change");
    in.enemy = 1;
    CHECK(tactics_score_adjust(&in) == TACT_WEAK_BONUS, "weak crew member (10%%): +%d", TACT_WEAK_BONUS);
    in.cand_hp = 40;
    CHECK(tactics_score_adjust(&in) == 0, "40%% is not weak");
    in.cand_crew = 0, in.cand_hp = 10;
    CHECK(tactics_score_adjust(&in) == 0, "the player is not singled out for low health");
    in.cand_holds_chooser_gang = 1;
    CHECK(tactics_score_adjust(&in) == TACT_HELP_BONUS, "holding one of my gang: +%d", TACT_HELP_BONUS);

    int on = 7;
    CHECK(tactics_parse_arg("--enemy-tactics", &on) && on == 1, "--enemy-tactics");
    CHECK(tactics_parse_arg("--no-enemy-tactics", &on) && on == 0, "--no-enemy-tactics");
    CHECK(!tactics_parse_arg("--enemy-tactic", &on) && on == 0, "other options are not taken");

    /* ---- fake guest ---- */
    map_at(0x07f000, 0x1000);
    map_at(0x1a9000, 0x1000);
    map_at(0x1ba000, 0x1000);
    map_at(0x1bc000, 0x1000);
    map_at(0x1ca000, 0x1000);
    map_at(0x1d1000, 0x1000);
    map_at(0x210000, 0x2000);
    map_at(0x400000, 0x1c0000);
    map_at(0x20000000, 0x1000);
    D32(0x004aca1cu) = 0x20000000u;
    static const uint8_t level_fn[] = {0x8b, 0x44, 0x24, 0x04, 0xc2, 0x04, 0x00};   /* mov eax, [esp+4]; ret 4 (level = arg) */
    memcpy(AT(TACT_GUARD_FN), level_fn, sizeof level_fn);
    plant_jmp(TACT_CHOICE_FN, (const void *)fake_choice);
    static const uint8_t side_fn[] = {0xb8, 2, 0, 0, 0, 0xc3};   /* mov eax, 2; ret (the title steps right; byte 1 = its choice) */
    memcpy(AT(TACT_STRAFE_FN), side_fn, sizeof side_fn);
    plant_call(TACT_STRAFE_CALL, TACT_STRAFE_FN);
    static const uint8_t ring_fn[] = {0x8b, 0x44, 0x24, 0x18, 0xc2, 0x18, 0x00};   /* mov eax, [esp+0x18] (ms); ret 0x18 */
    memcpy(AT(ALLY_MOVETO_FN), ring_fn, sizeof ring_fn);
    static const uint8_t score_fn[] = {0xb8, 20, 0, 0, 0, 0xc3};
    memcpy(AT(ALLY_SCORE_FN), score_fn, sizeof score_fn);
    plant_call(TACT_GUARD_CALL, TACT_GUARD_FN);
    plant_call(TACT_CHOICE_CALL1, TACT_CHOICE_FN);
    plant_call(TACT_CHOICE_CALL2, TACT_CHOICE_FN);
    int32_t rel = (int32_t)(ALLY_SCORE_FN - (ALLY_SCORE_CALL + 5));
    AT(ALLY_SCORE_CALL)[0] = 0xe8; memcpy(AT(ALLY_SCORE_CALL + 1), &rel, 4); memcpy(AT(ALLY_SCORE_CALL + 5), "\x83\xc4\x0c\xc3", 4);
    rel = (int32_t)(ALLY_MOVETO_FN - (ALLY_CIRCLE_CALL + 5));
    AT(ALLY_CIRCLE_CALL)[0] = 0xe8; memcpy(AT(ALLY_CIRCLE_CALL + 1), &rel, 4); AT(ALLY_CIRCLE_CALL + 5)[0] = 0xc3;
    const uint32_t sites[6] = {TACT_GUARD_CALL, TACT_CHOICE_CALL1, TACT_CHOICE_CALL2, ALLY_SCORE_CALL, ALLY_CIRCLE_CALL, TACT_STRAFE_CALL};
    uint8_t before[6][5];
    for (int i = 0; i < 6; i++) memcpy(before[i], AT(sites[i]), 5);

    /* Player 0 at the origin facing +x, crew 1 (gang A); enemies 5, 6, 7 (gang B) fight the player,
     * enemy 8 fights crew 1. 5 and 6 hold the player's two attack slots, 7 waits behind them. */
    uint32_t ga = 0x4b1c58, gb = ga + 0xabc;
    character(0, 0, ga, 5, 0, 0, 200);
    character(1, -1, ga, 8, 0, 5, 30);
    character(5, -1, gb, 0, 1.0f, 0.2f, 100);
    character(6, -1, gb, 0, 1.0f, -0.2f, 100);
    character(7, -1, gb, 0, 2.5f, 0.3f, 100);
    character(8, -1, gb, 1, 0, 6, 100);
    D32(brain(0) + 0x1dc) = chr(5), D32(brain(0) + 0x1e0) = chr(6);
    AT(brain(0) + 0x137)[0] = 2;
    AT(brain(5) + 0x280 + TACT_ID_GRAB)[0] = 20;
    set_clock(1000);

    CHECK(hle_tactics_install(0, 0) == 0 && !ai_gate_filter && !ai_score_hook && !ai_circle_hook, "off: nothing installed");
    int same = 1;
    for (int i = 0; i < 6; i++) same &= !memcmp(AT(sites[i]), before[i], 5);
    CHECK(same, "off: no byte of the six call sites changed");
    AT(TACT_CHOICE_CALL2 + 2)[0] ^= 1;
    CHECK(hle_tactics_install(0, 1) == 2 && AT(TACT_CHOICE_CALL2 + 2)[0] == (before[2][2] ^ 1), "a mismatching site is left alone (2 of 3)");
    AT(TACT_CHOICE_CALL2 + 2)[0] ^= 1;
    CHECK(call_this1(TACT_GUARD_CALL, 0, brain(5) - 4 + 4, 1) == 1, "unpatched guard site: the title's level");

    /* --ai-debug alone: gate and choice routed for the statistics, the title's answers and weights. */
    CHECK(hle_tactics_install(0, 1) == 3 && ai_gate_filter && !ai_score_hook && !ai_circle_hook, "observe: 3 sites routed, no behaviour hook");
    CHECK(!memcmp(AT(TACT_GUARD_CALL), before[0], 5) && !memcmp(AT(ALLY_SCORE_CALL), before[3], 5) && !memcmp(AT(ALLY_CIRCLE_CALL), before[4], 5) &&
          !memcmp(AT(TACT_STRAFE_CALL), before[5], 5), "observe: the guard, score, ring and side-step sites unchanged");
    set_clock(500);
    CHECK(call_this3(TACT_CHOICE_CALL1, brain(5), chr(0), 0x14a2e0, 0) == 20, "observe: choice through the stub (weight 20)");
    set_clock(500 + TACT_IDLE_MS + 10);
    CHECK(call_this3(TACT_CHOICE_CALL1, brain(5), chr(0), 0x14a2e0, 0) == 20, "observe: a still player does not boost");
    set_clock(600);
    ai_gate_filter(brain(5), 1);
    set_clock(600 + TACT_TURN_MS);
    ai_gate_filter(brain(7), 0);
    CHECK(ai_gate_filter(brain(5), 1) == 1 && ai_gate_filter(brain(5), 1) == 1 && D32(brain(0) + 0x1dc) == chr(5), "observe: no turns taken");
    int n = hle_tactics_install(1, 0);
    CHECK(n == 7, "on after observe: all 7 sites (%d; the routed choice sites are kept)", n);
    CHECK(gate_installs == 3 && ai_gate_filter && ai_score_hook && ai_circle_hook, "gate stub requested, hooks set");

    /* Guard level: the Fight goal's esi is the goal; [goal+4] = brain. */
    static uint32_t goal5[2], goal1[2];
    goal5[1] = brain(5), goal1[1] = brain(1);
    CHECK(call_this1(TACT_GUARD_CALL, 0, (uint32_t)(uintptr_t)goal5, 1) == TACT_GUARD_LEVEL, "enemy on the player: level 1 -> %d", TACT_GUARD_LEVEL);
    CHECK(call_this1(TACT_GUARD_CALL, 0, (uint32_t)(uintptr_t)goal5, 0) == 0, "level 0 stays 0");
    CHECK(call_this1(TACT_GUARD_CALL, 0, (uint32_t)(uintptr_t)goal1, 1) == 1, "crew member: the title's level");

    /* Attack choice: active player -> the title's weights; passive -> boosted, then restored. */
    float ppos[3] = {0, 0, 0};
    (void)ppos;
    set_clock(2000);
    CHECK(call_this3(TACT_CHOICE_CALL1, brain(5), chr(0), 0x14a2e0, 0) == 20, "player just seen: grab weight 20 (title)");
    set_clock(2000 + TACT_IDLE_MS + 10);
    CHECK(call_this3(TACT_CHOICE_CALL1, brain(5), chr(0), 0x14a2e0, 0) == 60, "player still for %u ms: grab weight 20 -> 60", TACT_IDLE_MS);
    CHECK(D32(brain(5) + 0x1f4) == 0, "brain+0x1f4 restored after the choice");
    CHECK(call_this3(TACT_CHOICE_CALL1, brain(8), chr(1), 0x14a2e0, 0) == 0, "enemy on a crew member: not boosted");
    D32(chr(0) + 0x170) = 0x20000800u;
    D32(0x20000800u + 0xa0) = 1;   /* the player swings */
    CHECK(call_this3(TACT_CHOICE_CALL1, brain(5), chr(0), 0x14a2e0, 0) == 20, "a swing ends the passivity");
    D32(0x20000800u + 0xa0) = 0;

    /* Take turns: 7 is refused (waiting); 5 held its slot since 2000 ms. */
    set_clock(3000);
    CHECK(ai_gate_filter(brain(5), 1) == 1, "holder 5 attacks (slot held since now)");
    CHECK(ai_gate_filter(brain(7), 0) == 0, "7 refused: waits");
    set_clock(3000 + TACT_TURN_MS - 1);
    CHECK(ai_gate_filter(brain(5), 1) == 1, "before %u ms the holder keeps attacking", TACT_TURN_MS);
    set_clock(3000 + TACT_TURN_MS);
    CHECK(ai_gate_filter(brain(7), 0) == 0, "7 still waiting");
    uint32_t t = 3000 + TACT_TURN_MS;
    CHECK(ai_gate_filter(brain(5), 1) == 2, "after %u ms with a waiter: the holder rests (answer 2)", TACT_TURN_MS);
    CHECK(D32(brain(0) + 0x1dc) == 0 && D32(brain(0) + 0x1e0) == chr(6), "its attacker entry is free, the other holder keeps its own");
    CHECK(D32(brain(5) + 0x1d4) == t + TACT_REST_MS, "its cooldown is %u ms", TACT_REST_MS);
    CHECK(ai_gate_filter(brain(6), 1) == 1, "holder 6 (slot held since now) attacks");
    set_clock(t + TACT_TURN_MS);
    CHECK(ai_gate_filter(brain(6), 1) == 1, "nobody waiting (last refusal too old): the holder keeps its slot");
    CHECK(ai_gate_filter(brain(6), 0) == 0 && ai_gate_filter(brain(7), 0) == 0 && ai_gate_filter(brain(6), 1) == 1,
          "a refusal ends the streak: back to attacking, no turn yet");
    set_clock(t + TACT_TURN_MS + 1000);
    CHECK(ai_gate_filter(brain(6), 2) == 2, "its own cooldown between strings passes through");
    set_clock(t + 2 * TACT_TURN_MS);
    CHECK(ai_gate_filter(brain(7), 0) == 0 && ai_gate_filter(brain(6), 1) == 2,
          "a cooldown does not end the streak: %u ms later with a waiter, the holder rests", TACT_TURN_MS);
    D32(brain(0) + 0x1e0) = chr(6), D32(brain(6) + 0x1d4) = 0;
    CHECK(ai_gate_filter(brain(1), 0) == 0 && ai_gate_filter(brain(8), 3) == 3, "crew and other answers pass through");

    /* Surround: 7 waits 2.5 m in front of the player (holders 5, 6 at +x): side-steps towards its
     * nearer flank (left = counter-clockwise = step 2), else the title's choice (stand-in: 2). */
    D32(brain(0) + 0x1dc) = chr(5);
    static uint32_t act7[2], act5[2], act1[2], act8[2];
    act7[1] = brain(7), act5[1] = brain(5), act1[1] = brain(1), act8[1] = brain(8);
#define STEP(a) call_this0(TACT_STRAFE_CALL, (uint32_t)(uintptr_t)(a))
    CHECK(STEP(act7) == 2, "waiter in front-left: steps counter-clockwise (2) towards the left flank");
    face(0, PI_F / 2);
    CHECK(STEP(act7) == 2, "player turned to +y: the same (front = its attackers' side)");
    F32(AI_POS + AI_POS_STRIDE * 7 + 4) = -0.3f;
    CHECK(STEP(act7) == 6, "waiter in front-right: steps clockwise (6)");
    AT(TACT_STRAFE_FN)[1] = 8;
    CHECK(STEP(act7) == 8, "the title does not side-step (8): kept");
    AT(TACT_STRAFE_FN)[1] = 2;
    F32(AI_POS + AI_POS_STRIDE * 7) = 0.2f, F32(AI_POS + AI_POS_STRIDE * 7 + 4) = 2.6f;
    AT(TACT_STRAFE_FN)[1] = 6;
    CHECK(STEP(act7) == 6, "at its flank: the title's side-step");
    AT(TACT_STRAFE_FN)[1] = 2;
    F32(AI_POS + AI_POS_STRIDE * 7) = 2.5f, F32(AI_POS + AI_POS_STRIDE * 7 + 4) = 0.3f;
    uint32_t h0 = D32(brain(0) + 0x1dc), h1 = D32(brain(0) + 0x1e0);
    D32(brain(0) + 0x1dc) = 0, D32(brain(0) + 0x1e0) = 0;
    D32(brain(5) + 0x110) = handle(1), D32(brain(6) + 0x110) = handle(1);   /* 5, 6 leave the player */
    AT(TACT_STRAFE_FN)[1] = 6;
    CHECK(STEP(act7) == 6, "no attacker: the front is the player's facing (+y), +x is already its right flank");
    AT(TACT_STRAFE_FN)[1] = 2;
    D32(brain(5) + 0x110) = handle(0), D32(brain(6) + 0x110) = handle(0);
    D32(brain(0) + 0x1dc) = h0, D32(brain(0) + 0x1e0) = h1;
    face(0, 0.0f);
    AT(TACT_STRAFE_FN)[1] = 6;
    CHECK(STEP(act5) == 6 && STEP(act1) == 6 && STEP(act8) == 6, "slot holder, crew, enemy on a crew member: the title's side-step");
    AT(TACT_STRAFE_FN)[1] = 2;
    uint32_t ms = call_site6(ALLY_CIRCLE_CALL, 0x1234, brain(7), chr(0), 0, 0, 0, 2000);
    CHECK(ms == TACT_RING_MS, "waiter's ring wait 2000 -> %u", TACT_RING_MS);
    ms = call_site6(ALLY_CIRCLE_CALL, 0x1234, brain(5), chr(0), 0, 0, 0, 2000);
    CHECK(ms == 2000, "a slot holder's ring wait: unchanged");
    ms = call_site6(ALLY_CIRCLE_CALL, 0x1234, brain(8), chr(1), 0, 0, 0, 2000);
    CHECK(ms == 2000, "an enemy waiting for a crew member: the title's ring wait");

    /* Pressure: crew 1 at 30 of 100 seen. */
    extern int call_site3(uint32_t site, uint32_t a, uint32_t b, uint32_t c);
    *(int16_t *)(uintptr_t)(AI_BODIES + AI_BODY_STRIDE * 1 + AI_BODY_HEALTH) = 100;
    CHECK(call_site3(ALLY_SCORE_CALL, chr(8), chr(1), 0) == 20, "healthy crew member: the title's score");
    *(int16_t *)(uintptr_t)(AI_BODIES + AI_BODY_STRIDE * 1 + AI_BODY_HEALTH) = 30;
    CHECK(call_site3(ALLY_SCORE_CALL, chr(8), chr(1), 0) == 20 + TACT_WEAK_BONUS, "crew member at 30%%: +%d", TACT_WEAK_BONUS);
    AT(brain(0) + 0x20)[0] = 0;
    D32(brain(0) + 0x34) = 0x20000400u;
    D32(0x20000400u) = 0x00431b14u;   /* the player's top goal: Grabbing (its target, enemy 5) */
    CHECK(call_site3(ALLY_SCORE_CALL, chr(6), chr(0), 0) == 20 + TACT_HELP_BONUS, "player holding a gang mate: +%d", TACT_HELP_BONUS);
    CHECK(call_site3(ALLY_SCORE_CALL, chr(1), chr(5), 0) == 20, "crew choosing: the title's score");

    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}

/* Same helpers as hle_allies_test: cdecl (a, b, c) and thiscall (this; 6 args) through a site. */
__asm__(".text\n.globl call_site3\ncall_site3:\n"
        "push %ebx\npush %esi\npush %edi\npush %ebp\n"
        "mov 20(%esp),%eax\npush $1f\npush 36(%esp)\npush 36(%esp)\npush 36(%esp)\njmp *%eax\n1:\n"
        "pop %ebp\npop %edi\npop %esi\npop %ebx\nret\n");
__asm__(".text\n.globl call_site6\ncall_site6:\n"
        "push %ebx\npush %esi\npush %edi\npush %ebp\n"
        "mov 20(%esp),%eax\nmov 24(%esp),%ecx\n"
        "push $1f\n"
        "push 52(%esp)\npush 52(%esp)\npush 52(%esp)\npush 52(%esp)\npush 52(%esp)\npush 52(%esp)\n"
        "jmp *%eax\n1:\n"
        "pop %ebp\npop %edi\npop %esi\npop %ebx\nret\n");
