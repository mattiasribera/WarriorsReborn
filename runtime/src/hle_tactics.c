/* Smarter enemy group tactics (--enemy-tactics); see hle_tactics.h. */
#include "hle_tactics.h"
#include "hle_aidbg.h"
#include "hle_allies.h"
#include "platform.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Defined by hle_aidbg.c in the runtime; weak so the unit test can link without it. */
uint32_t (*ai_gate_filter)(uint32_t brain, uint32_t code) __attribute__((weak));
int hle_aidbg_install(void) __attribute__((weak));

#define TWO_PI 6.28318530718f
#define PI_F 3.14159265359f

static float wrap_angle(float a) {
    a = fmodf(a, TWO_PI);
    return a < 0.0f ? a + TWO_PI : a;
}

void tactics_flank_assign(const float *ang, int n, float *out) {
    if (n <= 0) return;
    if (n > TACT_MAX_WAIT) n = TACT_MAX_WAIT;
    if (n == 1) {
        out[0] = wrap_angle(ang[0]) < PI_F ? PI_F / 2 : 3 * PI_F / 2;
        return;
    }
    int idx[TACT_MAX_WAIT];
    float a[TACT_MAX_WAIT];
    for (int i = 0; i < n; i++) idx[i] = i, a[i] = wrap_angle(ang[i]);
    for (int i = 1; i < n; i++)   /* insertion sort by angle (stable: ties keep their order) */
        for (int j = i; j > 0 && a[idx[j - 1]] > a[idx[j]]; j--) {
            int t = idx[j];
            idx[j] = idx[j - 1], idx[j - 1] = t;
        }
    for (int k = 0; k < n; k++) out[idx[k]] = PI_F / 2 + (float)k * PI_F / (float)(n - 1);
}

int tactics_strafe_dir(float cur, float goal) {
    float d = wrap_angle(goal - cur);
    if (d > PI_F) d -= TWO_PI;
    if (fabsf(d) <= TACT_FLANK_TOL_RAD) return 0;
    return d > 0 ? 2 : 6;
}

int tactics_turn_over(uint32_t held_ms, int waiters) { return waiters > 0 && held_ms >= TACT_TURN_MS; }

int tactics_guard_level(int k) { return k > 0 && k < TACT_GUARD_LEVEL ? TACT_GUARD_LEVEL : k; }

void tactics_weights(const uint8_t in[TACT_ATTACK_IDS], uint8_t out[TACT_ATTACK_IDS]) {
    for (int i = 0; i < TACT_ATTACK_IDS; i++) {
        unsigned w = in[i];
        if (w && (i == TACT_ID_GRAB || i == TACT_ID_GUARD_BREAK)) w = w * 3 < 30 ? 30 : w * 3;
        else if (w && i >= 6 && i <= 9) w *= 2;
        out[i] = (uint8_t)(w > 255 ? 255 : w);
    }
}

int tactics_idle_update(TactIdle *t, uint32_t now, const float pos[3], int striking) {
    float dx = pos[0] - t->anchor[0], dy = pos[1] - t->anchor[1], dz = pos[2] - t->anchor[2];
    if (!t->init || striking || dx * dx + dy * dy + dz * dz > TACT_IDLE_M * TACT_IDLE_M) {
        memcpy(t->anchor, pos, sizeof t->anchor);
        t->active_ms = now;
        t->init = 1;
    }
    return now - t->active_ms >= TACT_IDLE_MS;
}

int tactics_score_adjust(const TactScoreIn *in) {
    if (!in->enemy) return 0;
    int adj = 0;
    if (in->cand_crew && in->cand_hp_max > 0 && in->cand_hp > 0 && in->cand_hp * 100 < in->cand_hp_max * TACT_WEAK_PCT)
        adj += TACT_WEAK_BONUS;
    if (in->cand_holds_chooser_gang) adj += TACT_HELP_BONUS;
    return adj;
}

int tactics_parse_arg(const char *arg, int *on) {
    if (!strcmp(arg, "--enemy-tactics")) { *on = 1; return 1; }
    if (!strcmp(arg, "--no-enemy-tactics")) { *on = 0; return 1; }
    return 0;
}

/* ---- guest state (identity-mapped, static .bss tables; called on the guest thread) ---- */
#define U32(a) (*(uint32_t *)(uintptr_t)(a))
#define GOAL_GRABBING_VT 0x00431b14u
#define GOAL_MOUNTING_VT 0x00431b48u
static uint32_t brain_of(int slot) { return AI_BRAINS + (uint32_t)slot * AI_BRAIN_STRIDE; }
static uint32_t char_of(int slot) { return AI_CHARS + (uint32_t)slot * AI_CHAR_STRIDE; }
static int alive_slot(int s) { return s >= 0 && s < AI_SLOTS && U32(brain_of(s)) == char_of(s); }
static int brain_slot(uint32_t b) {
    if (b < AI_BRAINS || (b - AI_BRAINS) % AI_BRAIN_STRIDE) return -1;
    int s = (int)((b - AI_BRAINS) / AI_BRAIN_STRIDE);
    return alive_slot(s) ? s : -1;
}
static int char_slot(uint32_t ch) {
    if (ch < AI_CHARS || (ch - AI_CHARS) % AI_CHAR_STRIDE) return -1;
    int s = (int)((ch - AI_CHARS) / AI_CHAR_STRIDE);
    return alive_slot(s) ? s : -1;
}
static int handle_slot(uint32_t h) {
    uint32_t s = h >> 16;
    if (h == 0xffffffffu || !(h & 0xffff) || s >= AI_SLOTS) return -1;
    if (*(const uint16_t *)(uintptr_t)(AI_HANDLES + s * 8 + 4) != (h & 0xffff)) return -1;
    return U32(AI_HANDLES + s * 8) == char_of((int)s) ? (int)s : -1;
}
static int player_slot(void) {
    for (int i = 0; i < AI_SLOTS; i++)
        if (alive_slot(i) && *(const int8_t *)(uintptr_t)(char_of(i) + 0x180) == 0) return i;
    return -1;
}
static uint32_t gang_of(int s) { return U32(brain_of(s) + 0x1f8); }
static int is_crew(int s, int player) { return s >= 0 && player >= 0 && s != player && gang_of(s) && gang_of(s) == gang_of(player); }
static int is_enemy(int s, int player) {
    return alive_slot(s) && player >= 0 && s != player && gang_of(s) && gang_of(s) != gang_of(player);
}
static int target_of(int s) { return handle_slot(U32(brain_of(s) + 0x110)); }
static int health_of(int s) { return *(const int16_t *)(uintptr_t)(AI_BODIES + (uint32_t)s * AI_BODY_STRIDE + AI_BODY_HEALTH); }
static const float *pos_of(int s) { return (const float *)(uintptr_t)(AI_POS + (uint32_t)s * AI_POS_STRIDE); }
/* Facing: the record's quaternion (x, y, z, w) at +0x10; forward is its Y axis (0x23f2d0). */
static float yaw_of(int s) {
    const float *q = (const float *)(uintptr_t)(AI_POS + (uint32_t)s * AI_POS_STRIDE + 0x10);
    return atan2f(1.0f - 2.0f * (q[0] * q[0] + q[2] * q[2]), 2.0f * (q[0] * q[1] - q[3] * q[2]));
}
static int listed_on(int attacker, int victim) {
    for (int k = 0; k < 4; k++)
        if (U32(brain_of(victim) + 0x1dc + 4 * (uint32_t)k) == char_of(attacker)) return 1;
    return 0;
}
static uint32_t now_ms(void) {
    uint32_t clk = U32(0x004aca1cu);
    return clk ? U32(clk + 0x40) : 0;
}
static int top_goal_vt(int s) {
    int8_t top = *(const int8_t *)(uintptr_t)(brain_of(s) + 0x20);
    uint32_t g = top >= 0 && top < 8 ? U32(brain_of(s) + 0x34 + 4 * (uint32_t)top) : 0;
    return g ? (int)U32(g) : 0;
}

/* The victim an enemy fights (the player or a crew member), or -1. */
static int enemy_victim(int s, int player) {
    if (!is_enemy(s, player)) return -1;
    int v = target_of(s);
    return v >= 0 && (v == player || is_crew(v, player)) ? v : -1;
}

/* ---- per-run state and stats ---- */
static int tactics_active;   /* 0: --ai-debug only (observe: stats, the title's answers and weights) */
static struct {
    uint32_t wait_ms[AI_SLOTS];    /* last refusal */
    int wait_victim[AI_SLOTS];
    uint32_t hold_ms[AI_SLOTS];    /* attacking without a break since (0 = not) */
    int hold_victim[AI_SLOTS];
    uint32_t ask_ms[AI_SLOTS];     /* last gate answer */
    uint8_t armed[AI_SLOTS];       /* guard break armed by the raised level only */
    int hp_max[AI_SLOTS];
    TactIdle idle;
    int passive;
    uint32_t last_sample, last_print, passive_ms, player_hp, player_slot_seen;
    unsigned asks, attacks_player, attacks_crew, turns, flanks, rings, guard_raise, boosted, weak_bonus, help_bonus;
    unsigned player_hits, player_damage, ko_player, ko_crew, ko_enemy;
    int last_hp[AI_SLOTS];
    uint8_t last_side[AI_SLOTS];   /* 1 player, 2 crew, 3 enemy */
    unsigned choice_active[TACT_ATTACK_IDS + 1], choice_passive[TACT_ATTACK_IDS + 1];
    unsigned ans_player[10];       /* the title's gate answers to enemies on the player (9 = other) */
    uint32_t longest_hold_ms;
} st;

static void print_stats(uint32_t ms) {
    printf("[tactics] %u ms enemy gate asks %u attacks on player %u on crew %u turns %u flank-steps %u rings %u "
           "guard-armed %u passive %.1f s boosted-choices %u weak-bonus %u help-bonus %u player-hits %u player-damage %u ko player %u crew %u enemies %u\n",
           ms, st.asks, st.attacks_player, st.attacks_crew, st.turns, st.flanks, st.rings, st.guard_raise,
           st.passive_ms / 1000.0, st.boosted, st.weak_bonus, st.help_bonus, st.player_hits, st.player_damage, st.ko_player, st.ko_crew, st.ko_enemy);
    printf("[tactics] %u ms gate answers on the player: 0=%u 1=%u 2=%u 3=%u 4=%u 5=%u 6=%u 7=%u 8=%u other=%u longest streak %u ms\n", ms,
           st.ans_player[0], st.ans_player[1], st.ans_player[2], st.ans_player[3], st.ans_player[4], st.ans_player[5], st.ans_player[6],
           st.ans_player[7], st.ans_player[8], st.ans_player[9], st.longest_hold_ms);
    for (int p = 0; p < 2; p++) {
        unsigned *c = p ? st.choice_passive : st.choice_active, n = 0;
        char buf[512];
        int len = snprintf(buf, sizeof buf, "[tactics] %u ms attack ids on the player (%s):", ms, p ? "passive" : "active");
        for (int i = 0; i <= TACT_ATTACK_IDS && len < (int)sizeof buf - 16; i++)
            if (c[i]) len += snprintf(buf + len, sizeof buf - (size_t)len, " %d=%u", i, c[i]), n += c[i];
        printf("%s (%u)\n", buf, n);
    }
}

/* Player tracking, sampled from every hook (guest thread). */
static int sample(uint32_t now) {
    int player = player_slot();
    if (player < 0) return -1;
    const uint32_t body = U32(char_of(player) + 0x170);
    int striking = body && U32(body + 0xa0) != 0;
    uint32_t dt = st.last_sample && now > st.last_sample ? now - st.last_sample : 0;
    int was = st.passive;
    st.passive = tactics_idle_update(&st.idle, now, pos_of(player), striking);
    if (was && st.passive && dt < 1000) st.passive_ms += dt;
    for (int i = 0; i < AI_SLOTS; i++) {   /* knock-outs: health from above 0 to 0, or the character gone */
        int h = alive_slot(i) ? health_of(i) : 0;
        if (st.last_hp[i] > 0 && h <= 0) {
            if (st.last_side[i] == 1) st.ko_player++;
            else if (st.last_side[i] == 2) st.ko_crew++;
            else if (st.last_side[i] == 3) st.ko_enemy++;
        }
        st.last_hp[i] = h;
        st.last_side[i] = (uint8_t)(!alive_slot(i) ? 0 : i == player ? 1 : is_crew(i, player) ? 2 : is_enemy(i, player) ? 3 : 0);
    }
    int hp = health_of(player);
    if ((uint32_t)player == st.player_slot_seen && hp < (int)st.player_hp && hp >= 0) st.player_hits++, st.player_damage += st.player_hp - (uint32_t)hp;
    st.player_hp = (uint32_t)(hp < 0 ? 0 : hp), st.player_slot_seen = (uint32_t)player;
    st.last_sample = now;
    if (now >= st.last_print + 10000) {
        st.last_print = now - now % 10000;
        print_stats(now);
    }
    return player;
}

/* Attack gate answer for brain (hook after 0x1abb80). */
static uint32_t tactics_gate(uint32_t brain, uint32_t code) {
    uint32_t now = now_ms();
    int player = sample(now), e = brain_slot(brain), v = e >= 0 ? enemy_victim(e, player) : -1;
    if (v < 0) return code;
    st.asks++;
    if (v == player) st.ans_player[code < 9 ? code : 9]++;
    if (code == 0 || code == 7 || code == 8) st.wait_ms[e] = now ? now : 1, st.wait_victim[e] = v;
    /* An attack streak: answers 1 on the same target without a refusal or a pause between; the
     * holder's own cooldown between strings (answer 2) keeps its attacker entry, so it continues it. */
    if ((code != 1 && code != 2) || st.hold_victim[e] != v || now - st.ask_ms[e] > TACT_GAP_MS) st.hold_ms[e] = 0;
    st.ask_ms[e] = now;
    if (code != 1) return code;
    if (!st.hold_ms[e]) st.hold_ms[e] = now ? now : 1, st.hold_victim[e] = v;
    if (now - st.hold_ms[e] > st.longest_hold_ms) st.longest_hold_ms = now - st.hold_ms[e];
    if (tactics_active) {
        int waiters = 0;
        for (int j = 0; j < AI_SLOTS; j++)
            if (j != e && st.wait_ms[j] && st.wait_victim[j] == v && now - st.wait_ms[j] < TACT_WAIT_MS && enemy_victim(j, player) == v &&
                !st.hold_ms[j])
                waiters++;
        if (tactics_turn_over(now - st.hold_ms[e], waiters)) {
            for (int k = 0; k < 4; k++)
                if (U32(brain_of(v) + 0x1dc + 4 * (uint32_t)k) == char_of(e)) U32(brain_of(v) + 0x1dc + 4 * (uint32_t)k) = 0;
            U32(brain + 0x1d4) = now + TACT_REST_MS;
            st.hold_ms[e] = 0;
            st.turns++;
            return 2;
        }
    }
    if (v == player) st.attacks_player++;
    else st.attacks_crew++;
    return code;
}

/* The flanking angle (world, radians) of enemy e waiting for the player v, or 0 with *ok = 0. The front
 * is where the attackers stand (else the player's facing); the waiting enemies (on the player without
 * an attack slot, within 12 m) spread over its sides and back. */
static int holding(int j, int v) { return listed_on(j, v) || (st.hold_ms[j] && st.hold_victim[j] == v); }
static float flank_angle(int e, int v, int player, float *cur, int *ok) {
    const float *vp = pos_of(v);
    float hx = 0, hy = 0, ang[TACT_MAX_WAIT], out[TACT_MAX_WAIT];
    int n = 0, me = -1;
    *ok = 0;
    for (int j = 0; j < AI_SLOTS; j++) {
        if (enemy_victim(j, player) != v || health_of(j) <= 0) continue;
        const float *p = pos_of(j);
        float dx = p[0] - vp[0], dy = p[1] - vp[1], d = sqrtf(dx * dx + dy * dy);
        if (holding(j, v)) {
            if (d > 0.01f) hx += dx / d, hy += dy / d;
            continue;
        }
        if (d > 12.0f || n >= TACT_MAX_WAIT) continue;
        if (j == e) me = n;
        ang[n++] = atan2f(dy, dx);
    }
    if (me < 0) return 0;
    float front = hx * hx + hy * hy > 0.01f ? atan2f(hy, hx) : yaw_of(v);
    *cur = ang[me];
    for (int k = 0; k < n; k++) ang[k] -= front;
    tactics_flank_assign(ang, n, out);
    *ok = 1;
    return front + out[me];
}

/* Ring action about to be built for args[0] (brain) around args[1] (target character): an enemy
 * waiting for the player asks the gate again after at most TACT_RING_MS. */
static int tactics_circle(uint32_t action, uint32_t *args) {
    (void)action;
    uint32_t now = now_ms();
    int player = sample(now), e = brain_slot(args[0]), v = char_slot(args[1]);
    if (e < 0 || v < 0 || v != player || enemy_victim(e, player) != v || holding(e, v)) return 0;
    if (args[5] > TACT_RING_MS) args[5] = TACT_RING_MS;
    st.rings++;
    return 0;
}

/* The ring action's side-step choice (2 / 6, 0x2117e0 on the action) for an enemy waiting for the
 * player: when the title side-steps, the side towards its flanking angle while it is not there; the
 * title's other answers (8: no side-step) are kept, so it still closes in and keeps its distance. */
static uint32_t tactics_strafe(uint32_t action, uint32_t code) {
    int player = player_slot(), e = brain_slot(U32(action + 4));
    if ((code != 2 && code != 6) || e < 0 || player < 0 || enemy_victim(e, player) != player || holding(e, player)) return code;
    int ok;
    float cur, goal = flank_angle(e, player, player, &cur, &ok);
    if (!ok) return code;
    uint32_t dir = (uint32_t)tactics_strafe_dir(cur, goal);
    if (!dir) return code;
    st.flanks++;
    return dir;
}

/* Target score for chooser self and candidate cand (after the title's and --smart-allies'). */
static int tactics_score(uint32_t self, uint32_t cand, int score) {
    int player = player_slot(), s = char_slot(self), c = char_slot(cand);
    if (s < 0 || c < 0 || !is_enemy(s, player) || (c != player && !is_crew(c, player))) return score;
    TactScoreIn in = {0};
    in.enemy = 1;
    in.cand_crew = c != player;
    in.cand_hp = health_of(c);
    if (in.cand_hp > st.hp_max[c]) st.hp_max[c] = in.cand_hp;
    in.cand_hp_max = st.hp_max[c];
    int vt = top_goal_vt(c), ct = target_of(c);
    in.cand_holds_chooser_gang = (vt == (int)GOAL_GRABBING_VT || vt == (int)GOAL_MOUNTING_VT) && ct >= 0 && gang_of(ct) == gang_of(s);
    int adj = tactics_score_adjust(&in);
    if (in.cand_holds_chooser_gang) st.help_bonus++;
    else if (adj) st.weak_bonus++;
    return score + adj;
}

/* Gang level for the guard-break check of the Fight goal whose brain is given. */
static uint32_t tactics_guard(uint32_t brain, uint32_t k) {
    int player = player_slot(), e = brain_slot(brain);
    if (e < 0 || enemy_victim(e, player) < 0) return k;
    int k2 = tactics_guard_level((int)k);
    /* Count the times the raised level alone lets the block counter (brain+0x134) pass the threshold. */
    int c = *(const int16_t *)(uintptr_t)(brain + 0x134), on = c > (7 - k2) * 6 && !(c > (7 - (int)k) * 6);
    if (on && !st.armed[e]) st.guard_raise++;
    st.armed[e] = (uint8_t)on;
    return (uint32_t)k2;
}

/* Attack choice wrapper: while the player stands still, an enemy attacking it draws from boosted
 * weights (brain+0x1f4 points to them for the duration of the call). */
static uint8_t boost_table[TACT_ATTACK_IDS];
static uint32_t boost_brain, boost_old;
static int choice_on_player, choice_passive;
static void tactics_choice_begin(uint32_t brain) {
    uint32_t now = now_ms();
    int player = sample(now), e = brain_slot(brain);
    choice_on_player = e >= 0 && enemy_victim(e, player) == player;
    choice_passive = choice_on_player && st.passive;
    boost_brain = 0;
    if (!choice_passive || !tactics_active) return;
    uint32_t old = U32(brain + 0x1f4);
    tactics_weights(old ? (const uint8_t *)(uintptr_t)old : (const uint8_t *)(uintptr_t)(brain + 0x280), boost_table);
    boost_brain = brain, boost_old = old;
    U32(brain + 0x1f4) = (uint32_t)(uintptr_t)boost_table;
}
static uint32_t tactics_choice_end(uint32_t choice) {
    if (boost_brain) U32(boost_brain + 0x1f4) = boost_old, boost_brain = 0, st.boosted++;
    if (choice_on_player) (choice_passive ? st.choice_passive : st.choice_active)[choice < TACT_ATTACK_IDS ? choice : TACT_ATTACK_IDS]++;
    choice_on_player = 0;
    return choice;
}

/* ---- patching ---- */
static int site_calls(uint32_t site, uint32_t target) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)site;
    int32_t rel;
    memcpy(&rel, p + 1, 4);
    return p[0] == 0xe8 && site + 5 + (uint32_t)rel == target;
}
static void put_rel(uint8_t *at, const void *to) {   /* rel32 at at[0..3], relative to at + 4 */
    int32_t rel = (int32_t)((uintptr_t)to - (uintptr_t)(at + 4));
    memcpy(at, &rel, 4);
}
static int route(uint32_t site, uint32_t target, uint8_t *stub, const char *name) {
    if (!stub) return 0;
    if (site_calls(site, (uint32_t)(uintptr_t)stub)) return 1;   /* already routed (observe, then on) */
    if (!site_calls(site, target)) {
        printf("[tactics] skip %s at %08x: unexpected bytes\n", name, site);
        return 0;
    }
    put_rel((uint8_t *)(uintptr_t)(site + 1), stub);
    return 1;
}

/* thiscall 0x7f020(gang table; type) -> tactics_guard([esi+4] = the Fight goal's brain, level). */
static uint8_t *guard_stub(void) {
    uint8_t *s = plat_alloc_exec(64);
    if (!s) return NULL;
    static const uint8_t code[] = {
        0xff, 0x74, 0x24, 0x04,       /* push dword [esp+4] (type) */
        0xe8, 0, 0, 0, 0,             /* call 0x7f020 (ret 4) */
        0x50,                         /* push eax (level) */
        0xff, 0x76, 0x04,             /* push dword [esi+4] (brain) */
        0xe8, 0, 0, 0, 0,             /* call tactics_guard */
        0x83, 0xc4, 0x08,             /* add esp, 8 */
        0xc2, 0x04, 0x00,             /* ret 4 */
    };
    memcpy(s, code, sizeof code);
    put_rel(s + 5, (const void *)(uintptr_t)TACT_GUARD_FN);
    put_rel(s + 14, (const void *)tactics_guard);
    return s;
}

/* thiscall 0x2117e0(action) -> tactics_strafe(action, side-step). */
static uint8_t *strafe_stub(void) {
    uint8_t *s = plat_alloc_exec(64);
    if (!s) return NULL;
    static const uint8_t code[] = {
        0x51,                         /* push ecx (action) */
        0xe8, 0, 0, 0, 0,             /* call 0x2117e0 */
        0x59,                         /* pop ecx */
        0x50,                         /* push eax (side-step) */
        0x51,                         /* push ecx */
        0xe8, 0, 0, 0, 0,             /* call tactics_strafe */
        0x83, 0xc4, 0x08,             /* add esp, 8 */
        0xc3,                         /* ret */
    };
    memcpy(s, code, sizeof code);
    put_rel(s + 2, (const void *)(uintptr_t)TACT_STRAFE_FN);
    put_rel(s + 10, (const void *)tactics_strafe);
    return s;
}

/* thiscall 0x1a9f90(brain; target, predicate, 0) between tactics_choice_begin / _end. */
static uint8_t *choice_stub(void) {
    uint8_t *s = plat_alloc_exec(64);
    if (!s) return NULL;
    static const uint8_t code[] = {
        0x51,                         /* push ecx (brain) */
        0x51,                         /* push ecx */
        0xe8, 0, 0, 0, 0,             /* call tactics_choice_begin */
        0x83, 0xc4, 0x04,             /* add esp, 4 */
        0x59,                         /* pop ecx */
        0xff, 0x74, 0x24, 0x0c,       /* push dword [esp+0xc] (arg 3) */
        0xff, 0x74, 0x24, 0x0c,       /* push dword [esp+0xc] (arg 2) */
        0xff, 0x74, 0x24, 0x0c,       /* push dword [esp+0xc] (arg 1) */
        0xe8, 0, 0, 0, 0,             /* call 0x1a9f90 (ret 0xc) */
        0x50,                         /* push eax */
        0xe8, 0, 0, 0, 0,             /* call tactics_choice_end */
        0x83, 0xc4, 0x04,             /* add esp, 4 */
        0xc2, 0x0c, 0x00,             /* ret 0xc */
    };
    memcpy(s, code, sizeof code);
    put_rel(s + 3, (const void *)tactics_choice_begin);
    put_rel(s + 24, (const void *)(uintptr_t)TACT_CHOICE_FN);
    put_rel(s + 30, (const void *)tactics_choice_end);
    return s;
}

int hle_tactics_install(int on, int observe) {
    if (!on && !observe) return 0;
    memset(&st, 0, sizeof st);
    tactics_active = on;
    int n = 0;
    if (on) {
        n += ai_sites_install();   /* target score + ring action (shared with --smart-allies) */
        ai_score_hook = tactics_score;
        ai_circle_hook = tactics_circle;
        static uint8_t *gs, *ss;
        if (!gs) gs = guard_stub();
        n += route(TACT_GUARD_CALL, TACT_GUARD_FN, gs, "guard level");
        if (!ss) ss = strafe_stub();
        n += route(TACT_STRAFE_CALL, TACT_STRAFE_FN, ss, "side-step");
    }
    if (hle_aidbg_install && hle_aidbg_install()) n++;   /* attack gate (shared with --ai-debug) */
    ai_gate_filter = tactics_gate;
    static uint8_t *cs;
    if (!cs) cs = choice_stub();
    n += route(TACT_CHOICE_CALL1, TACT_CHOICE_FN, cs, "attack choice");
    n += route(TACT_CHOICE_CALL2, TACT_CHOICE_FN, cs, "attack re-choice");
    if (on)
        printf("[tactics] enemy tactics: %d of 7 sites patched (turn %u ms, rest %u ms, ring <= %u ms, guard level %d, idle %u ms, weak +%d, "
               "help +%d)\n", n, TACT_TURN_MS, TACT_REST_MS, TACT_RING_MS, TACT_GUARD_LEVEL, TACT_IDLE_MS, TACT_WEAK_BONUS, TACT_HELP_BONUS);
    else
        printf("[tactics] observing the enemies' attacks (--ai-debug): %d of 3 sites routed, behaviour unchanged\n", n);
    return n;
}
