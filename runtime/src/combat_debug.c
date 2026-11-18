/* --combat-debug (combat_debug.h). */
#include "combat_debug.h"
#include "hle_warp.h"
#include "platform.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- title ---- */

#define STEP_COUNTER 0x005825fcu  /* ++ per 1/60 s step (FUN_0016d8d0) */
#define CHAR_POOL 0x004ff878u     /* humans, CHAR_SLOTS x CHAR_STRIDE */
#define CHAR_STRIDE 0x668u
#define CHAR_SLOTS 60
#define SITE_PART 0x0013f460u     /* strike part on/off (ecx human; part, on): mov ecx,[ecx+0x170] */
#define SITE_TEST 0x00251130u     /* hit test of a body (ecx world; body): sub esp,0xaa8 */
#define SITE_CONTACT 0x0014dbc0u  /* human vtable +0x7c (ecx attacker; contact): sub esp,0x34; push ebx; mov ebx,[esp+0x3c] */
#define SITE_WCONTACT 0x002a5250u /* held weapon +0x78 (ecx weapon; contact): sub esp,0x18; push esi; mov esi,ecx */
#define SITE_DAMAGE 0x001867c0u   /* AddDamage (target, amount, source, ...): mov eax,[esp+8]; test eax,eax */
/* The AI block decision FUN_001bc350 (ecx goal; goal+4 brain): when the brain's threat count +0x1ec
 * (incoming attack messages since its last tick, reset by the tick 0x1a88ab) is non-zero it rolls
 * rand(100) < the character's block chance (FUN_00140960) and on success pushes a Blocking goal,
 * initialised by FUN_001cad90 (its only caller 0x1bc42e). The Fight goal's update calls the decision
 * once per world update (0x1d127c), the brain's message handler on attack messages. */
#define SITE_BLOCKROLL 0x001bc350u /* push ecx; push esi; push edi; mov edi,ecx; mov esi,[edi+4] */
#define SITE_BLOCKED 0x001cad90u   /* Blocking goal init (ecx goal; brain): mov eax,ecx; mov ecx,[esp+4] */
/* The Blocking goal update FUN_001caff0 (esi goal, ebp the blocker): its per-update counter roll
 * (0x1cb151, rand(100) < counter chance) and the counter chosen (0x1cb186, action 3 follows). */
#define SITE_CROLL 0x001cb151u     /* push 0x64; mov ecx,0x5a6f14 */
#define SITE_COUNTER 0x001cb186u   /* mov al,[esi+0x18]; test al,al */
#define AI_BRAINS 0x00599080u
#define AI_BRAIN_STRIDE 0x2d0u

int combat_debug_on;

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static uint8_t rd8(uint32_t a) { return *(const uint8_t *)(uintptr_t)a; }
static void rdv(uint32_t a, float v[3]) { memcpy(v, (const void *)(uintptr_t)a, 12); }
static float rdf(uint32_t a) { float v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static uint32_t step_now(void) { return rd32(STEP_COUNTER); }

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float frames(uint32_t steps) { return (float)steps * 0.5f; }   /* 1/60 s steps -> 30 Hz frames */

static int human_index(uint32_t h) {
    if (h < CHAR_POOL || h >= CHAR_POOL + CHAR_SLOTS * CHAR_STRIDE || (h - CHAR_POOL) % CHAR_STRIDE) return -1;
    return (int)((h - CHAR_POOL) / CHAR_STRIDE);
}
static uint32_t human_at(int i) { return CHAR_POOL + (uint32_t)i * CHAR_STRIDE; }
static uint32_t body_of(uint32_t h) {
    const uint32_t b = rd32(h + 0x170);
    return b && rd32(b + 0x34) == h ? b : 0;
}
static int move_of(uint32_t h) { const uint32_t s = rd32(h + 0xb0); return s ? (int)rd32(s + 0x20) : -1; }
/* Guarding: the character's AI brain (0x599080 + 0x2d0 x slot) has the Blocking goal (vtable
 * 0x431ae0) on top of its goal stack (+0x20 top index, +0x34 goal pointers). */
static int guarding(uint32_t h) {
    const int i = (int)((h - CHAR_POOL) / CHAR_STRIDE);
    const uint32_t brain = 0x00599080u + 0x2d0u * (uint32_t)i;
    const int8_t top = (int8_t)rd8(brain + 0x20);
    if (top < 0 || top > 15) return 0;
    const uint32_t goal = rd32(brain + 0x34 + 4u * (uint32_t)top);
    return goal && rd32(goal) == 0x00431ae0u;
}
static int health_of(uint32_t h) { const uint32_t s = rd32(h + 0xb0); return s ? (int)(int16_t)(rd32(s + 0x144) & 0xffff) : -1; }

/* A shape as a segment a-b (a sphere: a == b) and radius; 0 for other types. */
static int shape_geom(uint32_t s, float a[3], float b[3], float *r) {
    switch (rd8(s + 0x28)) {
    case 2: rdv(s + 0x30, a); memcpy(b, a, 12); *r = rdf(s + 0x2c); return 1;
    case 4: rdv(s + 0x2c, a); rdv(s + 0x3c, b); *r = rdf(s + 0x4c); return 1;
    default: return 0;
    }
}

/* Distance between the swept strike shape s (from its previous to its current position, as the
 * title's pair tests sweep it) and the hurt shape t, minus both radii: <= 0 means overlap. */
static float strike_dist(uint32_t s, uint32_t t) {
    float a[3], b[3], r, ta[3], tb[3], tr;
    if (!shape_geom(s, a, b, &r) || !shape_geom(t, ta, tb, &tr)) return 1e9f;
    float p[3];
    rdv(s + 0x18, p);   /* previous position (of the sphere's centre / the capsule's first point) */
    const float d[3] = {a[0] - p[0], a[1] - p[1], a[2] - p[2]};
    if (rd8(s + 0x28) == 2) return cd_seg_seg_dist(p, a, ta, tb) - r - tr;
    /* Capsule: its axis at the previous and the current position, and its first point's sweep. */
    const float pb[3] = {b[0] - d[0], b[1] - d[1], b[2] - d[2]};
    float m = cd_seg_seg_dist(a, b, ta, tb);
    const float m2 = cd_seg_seg_dist(p, pb, ta, tb), m3 = cd_seg_seg_dist(p, a, ta, tb);
    if (m2 < m) m = m2;
    if (m3 < m) m = m3;
    return m - r - tr;
}

static float sweep_len(uint32_t s) {
    float a[3], p[3];
    rdv(s + (rd8(s + 0x28) == 4 ? 0x2cu : 0x30u), a);
    rdv(s + 0x18, p);
    const float d[3] = {a[0] - p[0], a[1] - p[1], a[2] - p[2]};
    return sqrtf(dot3(d, d));
}

/* Position of a human (the table 0x4fefe8 indexed by the short at human+0x7a, 32 bytes per entry,
 * as FUN_00251130 reads it). */
static void pos_of(uint32_t h, float p[3]) { rdv(0x004fefe8u + 0x20u * (uint32_t)(int16_t)(rd32(h + 0x7a) & 0xffff), p); }
static float dist3(const float a[3], const float b[3]) {
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return sqrtf(dot3(d, d));
}

/* A handle (high word: slot of the table 0x5a7570 of {pointer, serial}; low word: the serial, as
 * FUN_0029fc30 resolves it) to a character slot, -1 when it is not a live character. */
static int handle_human(uint32_t handle) {
    const uint32_t slot = handle >> 16;
    if (!handle || slot >= 0x4000) return -1;
    const uint32_t e = 0x005a7570u + 8u * slot;
    if ((rd32(e + 4) & 0xffff) != (handle & 0xffff)) return -1;
    return human_index(rd32(e));
}

/* Per character: the open strike window and the move's frame data. */
typedef struct {
    int open;
    uint32_t on_step, mask;
    int move, tests, hits, weapon;
    float sweep;
    float best[CHAR_SLOTS];
    int best_part[CHAR_SLOTS];
    uint8_t hit[CHAR_SLOTS];      /* contacts on that character in this window */
    uint8_t why[CHAR_SLOTS];      /* CD_WHY_* seen while overlapping it */
    float d0[CHAR_SLOTS];         /* distance between the two characters at the window's first test */
    int target;                   /* the attacker's target (human+0xa4 handle) when the window opened */
    int target_guard;             /* tests during which that target was guarding */
    int guard;                    /* guarding at the last update (FUN_00144af0) */
    uint32_t guard_step;          /* step its guard went up */
    CdMove fd;
} CdChar;
static CdChar chars[CHAR_SLOTS];

/* Why an overlapping target can be skipped by FUN_00251130 (what the log can see; line of sight and
 * the bounds broadphase are not replicated). */
enum { CD_WHY_TEAM = 1, CD_WHY_LISTED = 2, CD_WHY_FLAGS = 4 };
static uint8_t skip_reasons(uint32_t body, uint32_t tb) {
    uint8_t why = 0;
    const int16_t team = (int16_t)(rd32(body + 0x3c) & 0xffff);
    if (team && team == (int16_t)(rd32(tb + 0x3c) & 0xffff)) why |= CD_WHY_TEAM;
    const uint32_t victim = rd32(tb + 0x34);
    for (uint32_t n = rd32(body + 0x38), k = 0; n && k < 64; n = rd32(n + 8), k++)
        if (rd32(n + 4) == victim) why |= CD_WHY_LISTED;
    if (!(rd8(tb + 0x40) & 0x30)) why |= CD_WHY_FLAGS;
    return why;
}

static void window_close(int i, uint32_t step) {
    CdChar *c = &chars[i];
    char near[512];
    int n = 0;
    near[0] = 0;
    for (int j = 0; j < CHAR_SLOTS && n < (int)sizeof near - 48; j++)
        if (c->best[j] < 1.0f) {
            n += snprintf(near + n, sizeof near - (size_t)n, " h#%d:%.3f(p%d)@%.2f", j, c->best[j], c->best_part[j], c->d0[j]);
            if (c->best[j] <= 0 && !c->hit[j])   /* an overlap without a contact on it */
                n += snprintf(near + n, sizeof near - (size_t)n, "[miss%s%s%s]", c->why[j] & CD_WHY_TEAM ? " team" : "",
                              c->why[j] & CD_WHY_LISTED ? " already-hit" : "", c->why[j] & CD_WHY_FLAGS ? " not-hittable" : "");
        }
    /* The move's table record (state+0x160, 16 bytes per move): reach +4 (FUN_00178e70), damage +10 (FUN_00178d20). */
    const uint32_t st = rd32(human_at(i) + 0xb0), tab = st ? rd32(st + 0x160) : 0;
    const int known = tab && c->move >= 0 && c->move < 1024;
    printf("[combat] strike h#%d tgt h#%d move %d %s mask %08x steps %u-%u (%.1f frames) tests %d sweep %.3f reach %.2f damage %d guarded %d hits %d near%s\n", i,
           c->target, c->move, c->weapon ? "weapon" : "parts", c->mask, c->on_step, step, frames(step - c->on_step), c->tests, c->sweep,
           known ? rdf(tab + 4 + 16u * (uint32_t)c->move) : -1.0f, known ? (int)(int16_t)(rd32(tab + 10 + 16u * (uint32_t)c->move) & 0xffff) : -1,
           c->target_guard, c->hits, n ? near : " none");
    c->open = 0;
}

/* Strike shapes of a body: its part shapes with flags 3, and its weapon spheres while they live. */
static void test_strikes(int i, uint32_t body, uint32_t step) {
    CdChar *c = &chars[i];
    const uint32_t h = human_at(i);
    float sweep = 0, best = 1e9f;
    int best_j = -1, best_p = -1;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1 && !rd32(body + 0xb0)) break;
        for (uint32_t s = rd32(body + (pass ? 0xa4u : 0x6cu)); s; s = rd32(s + 4)) {
            if (!pass && (rd8(s + 0x2a) & 3) != 3) continue;
            const float sw = sweep_len(s);
            if (sw > sweep) sweep = sw;
            for (int j = 0; j < CHAR_SLOTS; j++) {
                const uint32_t tb = j == i ? 0 : body_of(human_at(j));
                if (!tb) continue;
                if (c->d0[j] < 0) {
                    float pa[3], pb[3];
                    pos_of(h, pa);
                    pos_of(human_at(j), pb);
                    c->d0[j] = dist3(pa, pb);
                }
                for (uint32_t t = rd32(tb + 0x6c); t; t = rd32(t + 4)) {
                    if (!(rd8(t + 0x2a) & 4)) continue;
                    const float d = strike_dist(s, t);
                    if (d <= 0) c->why[j] |= skip_reasons(body, tb);
                    if (d < c->best[j]) { c->best[j] = d; c->best_part[j] = rd8(t + 0x29); }
                    if (d < best) { best = d; best_j = j; best_p = rd8(t + 0x29); }
                }
            }
        }
    }
    c->sweep += sweep;
    c->tests++;
    /* The first strike volume's centre in the attacker's frame (facing quaternion human+0x14). */
    float lp[3] = {0, 0, 0};
    for (uint32_t s = rd32(body + 0x6c); s; s = rd32(s + 4))
        if ((rd8(s + 0x2a) & 3) == 3) {
            float a[3], b[3], r, o[3], q[4];
            shape_geom(s, a, b, &r);
            pos_of(h, o);
            memcpy(q, (const void *)(uintptr_t)(h + 0x14), sizeof q);
            const float v[3] = {a[0] - o[0], a[1] - o[1], a[2] - o[2]};
            /* v' = conj(q) v q: rotate by the inverse of the facing. */
            const float u[3] = {-q[0], -q[1], -q[2]}, w = q[3];
            const float t[3] = {2 * (u[1] * v[2] - u[2] * v[1]), 2 * (u[2] * v[0] - u[0] * v[2]), 2 * (u[0] * v[1] - u[1] * v[0])};
            lp[0] = v[0] + w * t[0] + (u[1] * t[2] - u[2] * t[1]);
            lp[1] = v[1] + w * t[1] + (u[2] * t[0] - u[0] * t[2]);
            lp[2] = v[2] + w * t[2] + (u[0] * t[1] - u[1] * t[0]);
            break;
        }
    printf("[combat] test s=%u h#%d move %d sweep %.3f local %.3f %.3f %.3f", step, i, move_of(h), sweep, lp[0], lp[1], lp[2]);
    if (best_j >= 0 && best < 2.0f) printf(" nearest h#%d part %d d=%.3f%s", best_j, best_p, best, best <= 0 ? " overlap" : "");
    printf("\n");
}

static void knock_check(int j, uint32_t step);

/* The human whose body FUN_00251130 is testing: the attacker of the contacts it reports. */
static uint32_t testing;

static void on_test(WarpFrame *f) {
    const uint32_t body = rd32(f->esp + 4);
    const uint32_t h = body ? rd32(body + 0x34) : 0;
    testing = h;
    const int i = human_index(h);
    if (i < 0 || body_of(h) != body) return;
    CdChar *c = &chars[i];
    const uint32_t step = step_now(), mask = rd32(body + 0xa0);
    knock_check(i, step);
    const int g = guarding(h);
    if (g != c->guard) {
        if (g) c->guard_step = step;
        else printf("[combat] guard h#%d steps %u-%u (%.1f frames)\n", i, c->guard_step, step, frames(step - c->guard_step));
        c->guard = g;
    }
    const int weapon = rd32(body + 0xb0) != 0;
    const int on = mask || weapon, move = move_of(h);
    CdMoveReport r;
    if (cd_move_update(&c->fd, move, step, on, &r))
        printf("[combat] move h#%d id %d startup %.1f active %.1f recovery %.1f frames, windows %d, tests %d, hits %d\n", i, r.move,
               r.startup, r.active, r.recovery, r.windows, r.tests, r.hits);
    if (c->open && (!on || move != c->move)) window_close(i, step);
    if (!on) return;
    if (!c->open) {
        c->open = 1;
        c->on_step = step;
        c->mask = mask;
        c->move = move;
        c->tests = c->hits = 0;
        c->weapon = weapon;
        c->sweep = 0;
        c->target = handle_human(rd32(h + 0xa4));
        c->target_guard = 0;
        for (int j = 0; j < CHAR_SLOTS; j++) c->best[j] = 1e9f, c->best_part[j] = -1, c->hit[j] = 0, c->why[j] = 0, c->d0[j] = -1;
    }
    c->mask |= mask;
    if (c->target >= 0 && guarding(human_at(c->target))) c->target_guard++;
    test_strikes(i, body, step);
}

static void on_part(WarpFrame *f) {
    const uint32_t h = f->ecx;
    printf("[combat] part s=%u h#%d move %d part %d %s\n", step_now(), human_index(h), move_of(h), (int)rd32(f->esp + 4),
           rd8(f->esp + 8) ? "on" : "off");
}

/* After a contact: how far the victim and the attacker move in the next KNOCK_STEPS steps. */
#define KNOCK_STEPS 20
typedef struct { int on, att, move; uint32_t step; float v0[3], a0[3]; } CdKnock;
static CdKnock knock[CHAR_SLOTS];
static void knock_check(int j, uint32_t step) {
    CdKnock *k = &knock[j];
    if (!k->on || step - k->step < KNOCK_STEPS) return;
    float v1[3], a1[3];
    pos_of(human_at(j), v1);
    pos_of(human_at(k->att), a1);
    printf("[combat] knock h#%d by h#%d move %d: victim moved %.3f, attacker %.3f, distance %.3f -> %.3f in %.1f frames\n", j, k->att,
           k->move, dist3(k->v0, v1), dist3(k->a0, a1), dist3(k->v0, k->a0), dist3(v1, a1), frames(step - k->step));
    k->on = 0;
}

/* Recent contacts for the 3D view. */
#define CD_HITS 16
static struct { float p[3]; uint32_t step; } hits[CD_HITS];
static int hit_next;

static void contact(uint32_t att, uint32_t c, const char *what) {
    const uint32_t s = rd32(c + 4), t = rd32(c + 0x34), victim = rd32(c + 0x30);
    const int i = human_index(att), j = human_index(victim);
    float p[3];
    rdv(c + 0x6c, p);
    printf("[combat] hit s=%u %s h#%d move %d part %d -> h#%d part %d at %.2f %.2f %.2f\n", step_now(), what, i, move_of(att),
           s ? rd8(s + 0x29) : -1, j, t ? rd8(t + 0x29) : -1, p[0], p[1], p[2]);
    if (i >= 0) {
        chars[i].hits++;
        chars[i].fd.hits++;
        if (j >= 0) {
            chars[i].hit[j] = 1;
            CdKnock *k = &knock[j];
            k->on = 1, k->att = i, k->move = move_of(att), k->step = step_now();
            pos_of(victim, k->v0);
            pos_of(att, k->a0);
        }
    }
    memcpy(hits[hit_next].p, p, sizeof p);
    hits[hit_next].step = step_now();
    hit_next = (hit_next + 1) % CD_HITS;
}

static void on_contact(WarpFrame *f) {
    const uint32_t c = rd32(f->esp + 4);
    if (c && rd32(c + 4) && rd32(c + 0x34) && human_index(rd32(c + 0x30)) >= 0) contact(f->ecx, c, "parts");
}

static void on_wcontact(WarpFrame *f) {
    const uint32_t c = rd32(f->esp + 4);
    /* The contact's first word is the weapon: the attacker is the body being tested. */
    if (c && rd32(c + 4) && rd32(c + 0x34) && human_index(rd32(c + 0x30)) >= 0) contact(testing, c, "weapon");
}

static void on_damage(WarpFrame *f) {
    const uint32_t t = rd32(f->esp + 4), src = rd32(f->esp + 0xc);
    printf("[combat] damage s=%u h#%d amount %d from h#%d health %d\n", step_now(), human_index(t), (int)rd32(f->esp + 8), human_index(src),
           health_of(t));
}

static int brain_index(uint32_t b) {
    if (b < AI_BRAINS || b >= AI_BRAINS + CHAR_SLOTS * AI_BRAIN_STRIDE || (b - AI_BRAINS) % AI_BRAIN_STRIDE) return -1;
    return (int)((b - AI_BRAINS) / AI_BRAIN_STRIDE);
}

static void on_blockroll(WarpFrame *f) {
    const uint32_t brain = rd32(f->ecx + 4);
    const int i = brain_index(brain);
    if (i < 0 || !rd32(brain + 0x1ec)) return;   /* no threat: the title returns without a roll */
    printf("[combat] block-roll s=%u h#%d threats %u from %08x\n", step_now(), i, rd32(brain + 0x1ec), rd32(f->esp));
}

static void on_croll(WarpFrame *f) {
    printf("[combat] counter-roll s=%u h#%d\n", step_now(), human_index(f->ebp));
}

static void on_counter(WarpFrame *f) {
    printf("[combat] counter s=%u h#%d counters\n", step_now(), human_index(f->ebp));
}

static void on_blocked(WarpFrame *f) {
    printf("[combat] block s=%u h#%d starts blocking\n", step_now(), brain_index(rd32(f->esp + 4)));
}

int combat_debug_snapshot(CdVolume *out, int max) {
    int n = 0;
    if (!combat_debug_on) return 0;
    for (int i = 0; i < CHAR_SLOTS; i++) {
        const uint32_t body = body_of(human_at(i));
        if (!body) continue;
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1 && !rd32(body + 0xb0)) break;
            for (uint32_t s = rd32(body + (pass ? 0xa4u : 0x6cu)); s && n < max; s = rd32(s + 4)) {
                const uint8_t fl = rd8(s + 0x2a);
                const int kind = pass ? CD_VOL_WEAPON : (fl & 3) == 3 ? CD_VOL_STRIKE : (fl & 4) ? CD_VOL_HURT : -1;
                if (kind < 0 || !shape_geom(s, out[n].a, out[n].b, &out[n].r) || out[n].r <= 0) continue;
                out[n++].kind = (uint8_t)kind;
            }
        }
    }
    const uint32_t now = step_now();
    for (int k = 0; k < CD_HITS && n < max; k++) {
        if (!hits[k].step || now - hits[k].step > 20) continue;
        memcpy(out[n].a, hits[k].p, 12);
        memcpy(out[n].b, hits[k].p, 12);
        out[n].r = 0.12f;
        out[n++].kind = CD_VOL_HIT;
    }
    return n;
}

static int hook(uint32_t site, int len, const uint8_t *expect, void (*fn)(WarpFrame *)) {
    if (memcmp((const void *)(uintptr_t)site, expect, (size_t)len)) return 0;
    uint8_t *stub = plat_alloc_exec(WARP_STUB_MAX);
    if (!stub) return 0;
    warp_build_watch_stub(stub, (uint32_t)(uintptr_t)fn, (const uint8_t *)(uintptr_t)site, len, site + (uint32_t)len);
    warp_write_jmp((uint8_t *)(uintptr_t)site, site, (uint32_t)(uintptr_t)stub, len);
    return 1;
}

int combat_debug_parse_arg(const char *arg, int *enabled) {
    if (!strcmp(arg, "--combat-debug")) { *enabled = 1; return 1; }
    return 0;
}

int combat_debug_install(int enabled) {
    if (!enabled) return 0;
    static const uint8_t part[] = {0x8b, 0x89, 0x70, 0x01, 0x00, 0x00};
    static const uint8_t test[] = {0x81, 0xec, 0xa8, 0x0a, 0x00, 0x00};
    static const uint8_t cont[] = {0x83, 0xec, 0x34, 0x53, 0x8b, 0x5c, 0x24, 0x3c};
    static const uint8_t wcont[] = {0x83, 0xec, 0x18, 0x56, 0x8b, 0xf1};
    static const uint8_t dmg[] = {0x8b, 0x44, 0x24, 0x08, 0x85, 0xc0};
    static const uint8_t roll[] = {0x51, 0x56, 0x57, 0x8b, 0xf9, 0x8b, 0x77, 0x04};
    static const uint8_t blk[] = {0x8b, 0xc1, 0x8b, 0x4c, 0x24, 0x04};
    static const uint8_t croll[] = {0x6a, 0x64, 0xb9, 0x14, 0x6f, 0x5a, 0x00};
    static const uint8_t ctr[] = {0x8a, 0x46, 0x18, 0x84, 0xc0};
    const struct { uint32_t site; int len; const uint8_t *expect; void (*fn)(WarpFrame *); } sites[] = {
        {SITE_PART, 6, part, on_part}, {SITE_TEST, 6, test, on_test}, {SITE_CONTACT, 8, cont, on_contact},
        {SITE_WCONTACT, 6, wcont, on_wcontact}, {SITE_DAMAGE, 6, dmg, on_damage},
        {SITE_BLOCKROLL, 8, roll, on_blockroll}, {SITE_BLOCKED, 6, blk, on_blocked},
        {SITE_CROLL, 7, croll, on_croll}, {SITE_COUNTER, 5, ctr, on_counter},
    };
    /* All or nothing: check every site first. */
    for (size_t k = 0; k < sizeof sites / sizeof sites[0]; k++)
        if (memcmp((const void *)(uintptr_t)sites[k].site, sites[k].expect, (size_t)sites[k].len)) {
            printf("[combat] debug off: the title's bytes at %08x differ\n", sites[k].site);
            return 0;
        }
    for (int i = 0; i < CHAR_SLOTS; i++) chars[i].fd.move = -2;
    for (size_t k = 0; k < sizeof sites / sizeof sites[0]; k++) hook(sites[k].site, sites[k].len, sites[k].expect, sites[k].fn);
    combat_debug_on = 1;
    printf("[combat] debug on: strike windows, hit tests, contacts and damage are logged ([combat] lines)\n");
    return 1;
}
