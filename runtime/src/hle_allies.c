/* Smarter allies (--smart-allies); see hle_allies.h. */
#include "hle_allies.h"
#include "hle_aidbg.h"
void (*ai_gate_after)(uint32_t brain, uint32_t code) __attribute__((weak));
#include "platform.h"
#include <stdio.h>
#include <string.h>

#define GOAL_GRABBING_VT 0x00431b14u   /* Grabbing goal: holding another character */
#define GOAL_MOUNTING_VT 0x00431b48u   /* Mounting goal: on top of a downed character */

int allies_score_adjust(const AllyScoreIn *in) {
    if (!in->crew) return 0;
    int adj = 0;
    if (!in->self_listed && in->max_attackers > 0 && in->attackers >= in->max_attackers) adj -= ALLY_FULL_PENALTY;
    adj -= ALLY_SHARE_PENALTY * in->crew_on_it;
    if (in->targets_player) adj += ALLY_PROTECT_BONUS;
    if (in->grabbing_friend) adj += ALLY_GRAB_BONUS;
    return adj;
}

uint32_t allies_circle_value(int crew, uint32_t ms) { return crew && ms > ALLY_CIRCLE_MS ? ALLY_CIRCLE_MS : ms; }

int allies_parse_arg(const char *arg, int *on) {
    if (!strcmp(arg, "--smart-allies")) { *on = 1; return 1; }
    if (!strcmp(arg, "--no-smart-allies")) { *on = 0; return 1; }
    return 0;
}

/* ---- guest state (identity-mapped, static .bss tables; called on the guest thread) ---- */
#define U32(a) (*(const uint32_t *)(uintptr_t)(a))
static uint32_t brain_of(int slot) { return AI_BRAINS + (uint32_t)slot * AI_BRAIN_STRIDE; }
static int char_slot(uint32_t ch) {
    if (ch < AI_CHARS || (ch - AI_CHARS) % AI_CHAR_STRIDE) return -1;
    uint32_t s = (ch - AI_CHARS) / AI_CHAR_STRIDE;
    return s < AI_SLOTS && U32(brain_of((int)s)) == ch ? (int)s : -1;
}
static int handle_slot(uint32_t h) {
    uint32_t s = h >> 16;
    if (h == 0xffffffffu || !(h & 0xffff) || s >= AI_SLOTS) return -1;
    if (*(const uint16_t *)(uintptr_t)(AI_HANDLES + s * 8 + 4) != (h & 0xffff)) return -1;
    return U32(AI_HANDLES + s * 8) == AI_CHARS + s * AI_CHAR_STRIDE ? (int)s : -1;
}
static int player_slot(void) {
    for (int i = 0; i < AI_SLOTS; i++) {
        uint32_t ch = U32(brain_of(i));
        if (ch == AI_CHARS + (uint32_t)i * AI_CHAR_STRIDE && *(const int8_t *)(uintptr_t)(ch + 0x180) == 0) return i;
    }
    return -1;
}
static int is_crew(int slot, int player) {
    return slot >= 0 && player >= 0 && slot != player && U32(brain_of(slot)) &&
           U32(brain_of(slot) + 0x1f8) && U32(brain_of(slot) + 0x1f8) == U32(brain_of(player) + 0x1f8);
}
static int brain_target(int slot) { return handle_slot(U32(brain_of(slot) + 0x110)); }

/* Fills the inputs for chooser self (character) and candidate cand (character). */
static void score_inputs(uint32_t self, uint32_t cand, AllyScoreIn *in) {
    memset(in, 0, sizeof *in);
    int si = char_slot(self), ci = char_slot(cand), player = player_slot();
    if (si < 0 || ci < 0 || !is_crew(si, player)) return;
    in->crew = 1;
    uint32_t cb = brain_of(ci);
    for (int k = 0; k < 4; k++) {
        uint32_t a = U32(cb + 0x1dc + 4 * (uint32_t)k);
        in->attackers += a != 0;
        in->self_listed |= a == self;
    }
    in->max_attackers = *(const uint8_t *)(uintptr_t)(cb + (ci == player ? 0x137 : 0x136));
    for (int i = 0; i < AI_SLOTS; i++)
        if (i != si && is_crew(i, player) && brain_target(i) == ci) in->crew_on_it++;
    int ct = brain_target(ci);
    in->targets_player = ct >= 0 && ct == player;
    int8_t top = *(const int8_t *)(uintptr_t)(cb + 0x20);   /* goal stack: +0x20 top, +0x34 goals */
    uint32_t goal = top >= 0 && top < 8 ? U32(cb + 0x34 + 4 * (uint32_t)top) : 0;
    in->grabbing_friend = goal && (U32(goal) == GOAL_GRABBING_VT || U32(goal) == GOAL_MOUNTING_VT) && ct >= 0 && (ct == player || is_crew(ct, player));
}

int allies_pick_open(const AllyCand *c, int n, int current) {
    int best = -1;
    for (int i = 0; i < n; i++)
        if (c[i].open && c[i].slot != current && c[i].dist <= ALLY_SWITCH_M && (best < 0 || c[i].dist < c[best].dist)) best = i;
    return best;
}

static uint32_t now_ms(void) {
    uint32_t clk = U32(0x004aca1cu);
    return clk ? U32(clk + 0x40) : 0;
}
static int open_now(int slot, uint32_t ms) {
    uint32_t b = brain_of(slot);
    int n = 0;
    for (int k = 0; k < 4; k++) n += U32(b + 0x1dc + 4 * (uint32_t)k) != 0;
    int max = *(const uint8_t *)(uintptr_t)(b + 0x136);
    return max > 0 && n < max && (n > 0 || ms > U32(b + 0x1d8));
}
typedef void (__attribute__((thiscall)) *TargetSetFn)(uint32_t brain, uint32_t ch, uint32_t flag);
static uint32_t switched_at[AI_SLOTS];
static void allies_gate_after(uint32_t brain, uint32_t code) {
    if (code != 0 || brain < AI_BRAINS || (brain - AI_BRAINS) % AI_BRAIN_STRIDE) return;
    int si = (int)((brain - AI_BRAINS) / AI_BRAIN_STRIDE), player = player_slot();
    if (si >= AI_SLOTS || !is_crew(si, player)) return;
    uint32_t ms = now_ms();
    if (switched_at[si] && ms - switched_at[si] < ALLY_SWITCH_MS) return;
    const float *p = (const float *)(uintptr_t)(AI_POS + (uint32_t)si * AI_POS_STRIDE);
    AllyCand c[AI_SLOTS];
    int n = 0;
    for (int j = 0; j < AI_SLOTS; j++) {
        if (j == si || j == player || is_crew(j, player) || U32(brain_of(j)) != AI_CHARS + (uint32_t)j * AI_CHAR_STRIDE) continue;
        int t = brain_target(j);
        if (t < 0 || (t != player && !is_crew(t, player))) continue;
        if (*(const int16_t *)(uintptr_t)(AI_BODIES + (uint32_t)j * AI_BODY_STRIDE + AI_BODY_HEALTH) <= 0) continue;
        const float *q = (const float *)(uintptr_t)(AI_POS + (uint32_t)j * AI_POS_STRIDE);
        float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
        c[n].slot = j, c[n].dist = __builtin_sqrtf(dx * dx + dy * dy + dz * dz), c[n].open = open_now(j, ms);
        n++;
    }
    int k = allies_pick_open(c, n, brain_target(si));
    if (k < 0) return;
    switched_at[si] = ms ? ms : 1;
    ((TargetSetFn)(uintptr_t)ALLY_TARGET_SET)(brain, AI_CHARS + (uint32_t)c[k].slot * AI_CHAR_STRIDE, 0);
}

typedef int (*ScoreFn)(uint32_t self, uint32_t cand, uint32_t mask);
static int allies_on;
int (*ai_score_hook)(uint32_t self, uint32_t cand, int score);
int (*ai_circle_hook)(uint32_t action, uint32_t *args);
/* Replaces the call at ALLY_SCORE_CALL (cdecl, guest thread). */
static int allies_score(uint32_t self, uint32_t cand, uint32_t mask) {
    int score = ((ScoreFn)(uintptr_t)ALLY_SCORE_FN)(self, cand, mask);
    if (score <= -999) return score;   /* not a candidate */
    if (allies_on) {
        AllyScoreIn in;
        score_inputs(self, cand, &in);
        score += allies_score_adjust(&in);
    }
    return ai_score_hook ? ai_score_hook(self, cand, score) : score;
}

/* Called from the circle stub with the action being built and the ring action's arguments
 * (args[0] brain, [1] target character, [2] min, [3] max, [4] 0, [5] ms); 1 = the hook built
 * another action in its place, 0 = build the ring action with args (possibly changed). */
static int ai_circle(uint32_t action, uint32_t *args) {
    uint32_t brain = args[0];
    int slot = brain >= AI_BRAINS && (brain - AI_BRAINS) % AI_BRAIN_STRIDE == 0 ? (int)((brain - AI_BRAINS) / AI_BRAIN_STRIDE) : -1;
    if (allies_on) args[5] = allies_circle_value(slot >= 0 && slot < AI_SLOTS && is_crew(slot, player_slot()), args[5]);
    return ai_circle_hook ? ai_circle_hook(action, args) : 0;
}

static int patch_call(uint32_t site, uint32_t old_target, const void *new_target, const char *name) {
    uint8_t *p = (uint8_t *)(uintptr_t)site;
    int32_t rel;
    memcpy(&rel, p + 1, 4);
    if (p[0] != 0xe8 || site + 5 + (uint32_t)rel != old_target) {
        printf("[allies] skip %s at %08x: unexpected bytes\n", name, site);
        return 0;
    }
    rel = (int32_t)((uintptr_t)new_target - (uintptr_t)(p + 5));
    memcpy(p + 1, &rel, 4);
    return 1;
}

/* thiscall 0x210e40(action; brain, target, min, max, 0, ms): ai_circle(action, &brain), then
 * either on to 0x210e40 or, when the hook built another action, return as the ctor would (ret 0x18). */
static void *circle_stub(void) {
    uint8_t *s = plat_alloc_exec(64), *q = s;
    if (!s) return NULL;
    static const uint8_t head[] = {
        0x51,                         /* push ecx (action) */
        0x8d, 0x44, 0x24, 0x08,       /* lea eax, [esp+8] (args) */
        0x50,                         /* push eax */
        0x51,                         /* push ecx */
        0xe8, 0, 0, 0, 0,             /* call ai_circle */
    };
    memcpy(q, head, sizeof head);
    q += sizeof head;
    int32_t rel = (int32_t)((uintptr_t)ai_circle - (uintptr_t)q);
    memcpy(q - 4, &rel, 4);
    static const uint8_t tail[] = {
        0x83, 0xc4, 0x08,             /* add esp, 8 */
        0x59,                         /* pop ecx */
        0x85, 0xc0,                   /* test eax, eax */
        0x75, 0x05,                   /* jnz +5 (handled) */
        0xe9, 0, 0, 0, 0,             /* jmp 0x210e40 */
        0xc2, 0x18, 0x00,             /* ret 0x18 */
    };
    memcpy(q, tail, sizeof tail);
    q += sizeof tail;
    rel = (int32_t)(ALLY_MOVETO_FN - (uint32_t)(uintptr_t)(q - 3));
    memcpy(q - 7, &rel, 4);
    return s;
}

static int calls(uint32_t site, const void *target) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)site;
    int32_t rel;
    memcpy(&rel, p + 1, 4);
    return p[0] == 0xe8 && (uintptr_t)(p + 5) + (uintptr_t)rel == (uintptr_t)target;
}
int ai_sites_install(void) {
    static void *stub;
    int n = calls(ALLY_SCORE_CALL, (const void *)allies_score) ||
            patch_call(ALLY_SCORE_CALL, ALLY_SCORE_FN, (const void *)allies_score, "target score");
    if (!stub) stub = circle_stub();
    if (stub) n += calls(ALLY_CIRCLE_CALL, stub) || patch_call(ALLY_CIRCLE_CALL, ALLY_MOVETO_FN, stub, "circle wait");
    return n;
}

int hle_allies_install(int on) {
    if (!on) return 0;
    allies_on = 1;
    int n = ai_sites_install();
    ai_gate_after = allies_gate_after;   /* the gate stub itself is hle_aidbg_install's */
    printf("[allies] smart allies: %d of 2 sites patched (full -%d, shared -%d, protect +%d, grab +%d, circle <= %u ms)\n", n,
           ALLY_FULL_PENALTY, ALLY_SHARE_PENALTY, ALLY_PROTECT_BONUS, ALLY_GRAB_BONUS, ALLY_CIRCLE_MS);
    return n;
}
