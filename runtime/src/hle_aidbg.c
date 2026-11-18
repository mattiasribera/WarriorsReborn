/* Ally AI diagnostics (--ai-debug); see hle_aidbg.h. */
#include "hle_aidbg.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CODE_LO 0x00010000u
#define CODE_HI 0x003c0000u

static uint32_t rd32(AiRead rd, void *ctx, uint32_t a, int *ok) {
    const void *p = rd(a, 4, ctx);
    if (!p) { *ok = 0; return 0; }
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

void ai_class_name(AiRead rd, void *ctx, uint32_t vtable, char *out, int cap) {
    out[0] = 0;
    int ok = 1, k = 0;
    uint32_t a = vtable;
    while (k < 40) {   /* the code pointers of the vtable */
        uint32_t v = rd32(rd, ctx, a, &ok);
        if (!ok) return;
        if (v < CODE_LO || v >= CODE_HI) break;
        a += 4, k++;
    }
    if (k < 2) return;
    const char *s = rd(a, (uint32_t)cap, ctx);
    if (!s) return;
    int n = 0;
    while (n < cap - 1 && s[n] && ((s[n] >= 'A' && s[n] <= 'Z') || (s[n] >= 'a' && s[n] <= 'z') ||
                                   (s[n] >= '0' && s[n] <= '9') || s[n] == '_'))
        n++;
    if (n < 3 || s[n] || !(s[0] >= 'A' && s[0] <= 'Z')) return;
    memcpy(out, s, (size_t)n);
    out[n] = 0;
}

int ai_handle_slot(AiRead rd, void *ctx, uint32_t h) {
    uint32_t slot = h >> 16, serial = h & 0xffff;
    if (h == 0xffffffffu || !serial || slot >= AI_SLOTS) return -1;
    const uint8_t *e = rd(AI_HANDLES + slot * 8, 8, ctx);
    if (!e) return -1;
    uint32_t ptr;
    uint16_t s;
    memcpy(&ptr, e, 4);
    memcpy(&s, e + 4, 2);
    if (s != serial || ptr != AI_CHARS + slot * AI_CHAR_STRIDE) return -1;
    return (int)slot;
}

/* Names cached by vtable (the set is small: about 200 classes). */
static struct { uint32_t vt; char name[AI_NAME_MAX]; } name_cache[256];
static void cached_name(AiRead rd, void *ctx, uint32_t vt, char *out) {
    unsigned h = (vt >> 2) & 255;
    for (unsigned k = 0; k < 256; k++, h = (h + 1) & 255) {
        if (name_cache[h].vt == vt) { strcpy(out, name_cache[h].name); return; }
        if (!name_cache[h].vt) {
            ai_class_name(rd, ctx, vt, out, AI_NAME_MAX);
            name_cache[h].vt = vt;
            strcpy(name_cache[h].name, out);
            return;
        }
    }
    ai_class_name(rd, ctx, vt, out, AI_NAME_MAX);
}
#ifdef HLE_AIDBG_TEST
void ai_name_cache_clear(void) { memset(name_cache, 0, sizeof name_cache); }
#endif

static void object_name(AiRead rd, void *ctx, uint32_t obj, char *out) {
    int ok = 1;
    out[0] = 0;
    if (!obj) return;
    uint32_t vt = rd32(rd, ctx, obj, &ok);
    if (ok && vt) cached_name(rd, ctx, vt, out);
}

int ai_snapshot(AiRead rd, void *ctx, AiChar out[AI_SLOTS]) {
    int player = -1;
    memset(out, 0, sizeof(AiChar) * AI_SLOTS);
    for (int i = 0; i < AI_SLOTS; i++) {
        AiChar *c = &out[i];
        c->target = -1;
        const uint8_t *b = rd(AI_BRAINS + (uint32_t)i * AI_BRAIN_STRIDE, AI_BRAIN_STRIDE, ctx);
        if (!b) continue;
        uint32_t ch, goal, gang;
        memcpy(&ch, b, 4);
        if (ch != AI_CHARS + (uint32_t)i * AI_CHAR_STRIDE) continue;
        const uint8_t *cp = rd(ch, AI_CHAR_STRIDE, ctx);
        if (!cp) continue;
        c->used = 1;
        c->player = (int8_t)cp[0x180];
        memcpy(&gang, b + 0x1f8, 4);
        c->gang = gang;
        int8_t top = (int8_t)b[0x20];
        goal = 0;
        if (top >= 0 && top < 8) memcpy(&goal, b + 0x34 + 4 * top, 4);
        object_name(rd, ctx, goal, c->goal);
        int8_t n = (int8_t)b[0x22], first = (int8_t)b[0x23];
        if (n > 0 && first >= 0 && first < 8) {
            uint32_t act;
            memcpy(&act, b + 0x5c + 4 * first, 4);
            object_name(rd, ctx, act, c->action);
        }
        uint32_t th;
        memcpy(&th, b + 0x110, 4);
        int ts = ai_handle_slot(rd, ctx, th);
        if (ts >= 0 && ts != i) c->target = ts;
        if (c->target < 0 && goal) {
            const uint8_t *g = rd(goal, 0x30, ctx);
            for (int k = 1; g && k < 12; k++) {
                uint32_t h;
                memcpy(&h, g + 4 * k, 4);
                int s = ai_handle_slot(rd, ctx, h);
                if (s >= 0 && s != i) { c->target = s; break; }
            }
        }
        const float *p = rd(AI_POS + (uint32_t)i * AI_POS_STRIDE, 12, ctx);
        if (p) memcpy(c->pos, p, 12);
        const int16_t *hp = rd(AI_BODIES + (uint32_t)i * AI_BODY_STRIDE + AI_BODY_HEALTH, 2, ctx);
        c->hp = hp ? *hp : 0;
        for (int k = 0; k < 4; k++) {
            uint32_t a;
            memcpy(&a, b + 0x1dc + 4 * k, 4);
            c->attackers += a != 0;
        }
        c->max_attackers = b[c->player == 0 ? 0x137 : 0x136];
        if (c->player == 0) player = i;
    }
    for (int i = 0; i < AI_SLOTS; i++) {
        int t = out[i].target;
        if (!out[i].used || t < 0) continue;
        const uint8_t *tb = rd(AI_BRAINS + (uint32_t)t * AI_BRAIN_STRIDE + 0x1dc, 16, ctx);
        for (int k = 0; tb && k < 4; k++) {
            uint32_t a;
            memcpy(&a, tb + 4 * k, 4);
            out[i].listed |= a == AI_CHARS + (uint32_t)i * AI_CHAR_STRIDE;
        }
    }
    if (player >= 0)
        for (int i = 0; i < AI_SLOTS; i++)
            out[i].ally = out[i].used && i != player && out[i].gang && out[i].gang == out[player].gang;
    return player;
}

static int has(const char *s, const char *const *set) {
    for (; *set; set++) if (!strcmp(s, *set)) return 1;
    return 0;
}
int ai_is_engaging(const AiChar *c) {
    static const char *const goals[] = {"Grabbing", "GrabTarget", "Mounting", "HTLGrabbing", NULL};
    static const char *const actions[] = {"Attack", "Dive", "Block", NULL};
    return c->listed || has(c->goal, goals) || has(c->action, actions);
}

int ai_is_crew_enemy(const AiChar *all, int i, int player) {
    const AiChar *c = &all[i];
    if (!c->used || c->ally || i == player || c->hp <= 0 || c->target < 0) return 0;
    return c->target == player || all[c->target].ally;
}

static float dist(const AiChar *a, const AiChar *b) {
    float dx = a->pos[0] - b->pos[0], dy = a->pos[1] - b->pos[1], dz = a->pos[2] - b->pos[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

void ai_stats_add(AiStats *s, const AiChar *all, int player, double dt_ms) {
    int enemy[AI_SLOTS] = {0};
    if (player < 0) return;
    for (int i = 0; i < AI_SLOTS; i++) enemy[i] = ai_is_crew_enemy(all, i, player);
    /* Enemies stay enemies while their health drops (a hit can clear the target). */
    for (int i = 0; s->have_last && i < AI_SLOTS; i++)
        if (!enemy[i] && all[i].used && !all[i].ally && i != player && s->last_hp[i] > all[i].hp && s->last_target[i] >= 0)
            enemy[i] = 1;
    int per_target[AI_SLOTS] = {0};
    for (int i = 0; i < AI_SLOTS; i++) {
        const AiChar *c = &all[i];
        if (!c->ally || c->hp <= 0) continue;
        int near = 0;
        for (int j = 0; j < AI_SLOTS && !near; j++) near = enemy[j] && dist(c, &all[j]) < AI_NEAR_M;
        int eng = ai_is_engaging(c);
        if (near) s->near_ms[i] += dt_ms;
        if (near && !eng) s->idle_near_ms[i] += dt_ms;
        if (eng) s->engaged_ms[i] += dt_ms;
        if (c->target >= 0 && enemy[c->target]) per_target[c->target]++;
        if (s->have_last && c->hp < s->last_hp[i]) s->hits_taken[i]++;
    }
    for (int j = 0; j < AI_SLOTS; j++) {
        if (!per_target[j]) continue;
        s->pile_samples++;
        s->pile_sum += per_target[j];
        if (per_target[j] > s->pile_max) s->pile_max = per_target[j];
    }
    if (s->have_last)
        for (int j = 0; j < AI_SLOTS; j++) {
            int drop = s->last_hp[j] - all[j].hp;
            if (!enemy[j] || drop <= 0 || drop > 2000) continue;
            s->enemy_damage += drop;
            for (int i = 0; i < AI_SLOTS; i++)   /* credited to the crew members targeting it */
                if (all[i].ally && s->last_target[i] == j) s->dealt[i] += drop;
        }
    for (int i = 0; i < AI_SLOTS; i++) {
        s->last_hp[i] = all[i].used ? all[i].hp : 0;
        s->last_target[i] = all[i].target;
    }
    s->have_last = 1;
}

#ifndef HLE_AIDBG_TEST
#include "mem.h"
#include "platform.h"

static unsigned gate_win[AI_GATE_CODES], gate_total[AI_GATE_CODES];
void (*ai_gate_after)(uint32_t brain, uint32_t code);
uint32_t (*ai_gate_filter)(uint32_t brain, uint32_t code);
static void gate_tally(uint32_t brain, uint32_t code);
/* cdecl, guest thread: one answer of the attack gate for brain; returns the answer the Fight goal
 * gets (the title's, or ai_gate_filter's). */
static uint32_t gate_count(uint32_t brain, uint32_t code) {
    if (hle_aidbg_active) gate_tally(brain, code);
    if (ai_gate_filter) code = ai_gate_filter(brain, code);
    if (ai_gate_after) ai_gate_after(brain, code);
    return code;
}
static void gate_tally(uint32_t brain, uint32_t code) {
    if (brain < AI_BRAINS || (brain - AI_BRAINS) % AI_BRAIN_STRIDE) return;
    int slot = (int)((brain - AI_BRAINS) / AI_BRAIN_STRIDE);
    if (slot >= AI_SLOTS) return;
    uint32_t ch = *(const uint32_t *)(uintptr_t)brain, gang = *(const uint32_t *)(uintptr_t)(brain + 0x1f8);
    if (ch != AI_CHARS + (uint32_t)slot * AI_CHAR_STRIDE || *(const int8_t *)(uintptr_t)(ch + 0x180) == 0) return;
    for (int i = 0; i < AI_SLOTS; i++) {   /* crew only: the player's gang record */
        uint32_t b = AI_BRAINS + (uint32_t)i * AI_BRAIN_STRIDE, c = *(const uint32_t *)(uintptr_t)b;
        if (c == AI_CHARS + (uint32_t)i * AI_CHAR_STRIDE && *(const int8_t *)(uintptr_t)(c + 0x180) == 0) {
            if (!gang || *(const uint32_t *)(uintptr_t)(b + 0x1f8) != gang) return;
            unsigned k = code < AI_GATE_CODES - 1 ? code : AI_GATE_CODES - 1;
            gate_win[k]++, gate_total[k]++;
            return;
        }
    }
}

int hle_aidbg_install(void) {
    static int done;
    if (done) return 1;
    done = 1;
    uint8_t *p = (uint8_t *)(uintptr_t)AI_GATE_CALL;
    int32_t rel;
    memcpy(&rel, p + 1, 4);
    if (p[0] != 0xe8 || AI_GATE_CALL + 5 + (uint32_t)rel != AI_GATE_FN) { printf("[ai] gate counter: unexpected bytes\n"); return 0; }
    uint8_t *s = plat_alloc_exec(64), *q = s;
    if (!s) return 0;
    static const uint8_t code[] = {
        0x51,                         /* push ecx (brain) */
        0xff, 0x74, 0x24, 0x0c,       /* push dword [esp+0xc] (arg 2) */
        0xff, 0x74, 0x24, 0x0c,       /* push dword [esp+0xc] (arg 1) */
        0xe8, 0, 0, 0, 0,             /* call 0x1abb80 (ecx = brain, ret 8) */
        0x59,                         /* pop ecx */
        0x50, 0x50, 0x51,             /* push eax; push eax (code); push ecx (brain) */
        0xe8, 0, 0, 0, 0,             /* call gate_count */
        0x83, 0xc4, 0x0c,             /* add esp, 12 (eax = gate_count's answer) */
        0x90,                         /* nop */
        0xc2, 0x08, 0x00,             /* ret 8 */
    };
    memcpy(q, code, sizeof code);
    rel = (int32_t)(AI_GATE_FN - (uint32_t)(uintptr_t)(q + 14));
    memcpy(q + 10, &rel, 4);
    rel = (int32_t)((uintptr_t)gate_count - (uintptr_t)(q + 23));
    memcpy(q + 19, &rel, 4);
    rel = (int32_t)((uintptr_t)s - (uintptr_t)(p + 5));
    memcpy(p + 1, &rel, 4);
    printf("[ai] gate counter on the Fight goal's attack gate call %08x\n", AI_GATE_CALL);
    return 1;
}

static void gate_line(const char *what, unsigned *g, uint32_t ms) {
    unsigned n = 0;
    for (int k = 0; k < AI_GATE_CODES; k++) n += g[k];
    printf("[ai] %s %u ms crew attack gate %u asks:", what, ms, n);
    for (int k = 0; k < AI_GATE_CODES; k++) if (g[k]) printf(" %d=%u", k, g[k]);
    printf("\n");
}

int hle_aidbg_active;

static const void *guest_read(uint32_t addr, uint32_t len, void *ctx) {
    (void)ctx;
    if (!addr || !len || addr + len <= addr) return NULL;
    uint32_t last = addr + len - 1;
    if (!mem_is_mapped(addr) || ((addr >> 12) != (last >> 12) && !mem_is_mapped(last))) return NULL;
    return (const void *)(uintptr_t)addr;
}

#define SAMPLE_MS 100u
#define SUMMARY_MS 10000u

static AiChar prev[AI_SLOTS];
static AiStats win, total;
static uint32_t next_ms, last_ms, summary_at;
static int started;

static void summary(const char *what, const AiStats *s, const AiChar *all, uint32_t ms) {
    double near = 0, idle = 0, eng = 0;
    int dealt = 0, taken = 0, crew = 0;
    for (int i = 0; i < AI_SLOTS; i++) {
        if (!all[i].ally) continue;   /* the crew now (a slot can change hands) */
        crew++;
        near += s->near_ms[i], idle += s->idle_near_ms[i], eng += s->engaged_ms[i];
        dealt += s->dealt[i], taken += s->hits_taken[i];
        printf("[ai] %s %u ms b%d near %.1f s idle-near %.1f s (%.0f%%) engaged %.1f s dealt %d hits-taken %d\n", what, ms, i,
               s->near_ms[i] / 1000, s->idle_near_ms[i] / 1000, s->near_ms[i] > 0 ? 100 * s->idle_near_ms[i] / s->near_ms[i] : 0,
               s->engaged_ms[i] / 1000, s->dealt[i], s->hits_taken[i]);
    }
    printf("[ai] %s %u ms crew %d near %.1f s idle-near %.1f s (%.0f%%) engaged %.1f s enemy-damage %d (crew-credited %d) "
           "crew-hits-taken %d pile mean %.2f max %d\n",
           what, ms, crew, near / 1000, idle / 1000, near > 0 ? 100 * idle / near : 0, eng / 1000, s->enemy_damage, dealt, taken,
           s->pile_samples ? (double)s->pile_sum / s->pile_samples : 0, s->pile_max);
}

static void log_changes(const AiChar *all, int player, uint32_t ms, uint32_t swap) {
    for (int i = 0; i < AI_SLOTS; i++) {
        const AiChar *c = &all[i], *p = &prev[i];
        int watch = c->ally || i == player || ai_is_crew_enemy(all, i, player);
        if (!c->used || !watch) continue;
        if (p->used && !strcmp(c->goal, p->goal) && !strcmp(c->action, p->action) && c->target == p->target &&
            c->attackers == p->attackers) continue;
        float d = c->target >= 0 && all[c->target].used ? dist(c, &all[c->target]) : -1;
        printf("[ai] %u ms swap %u b%d %s goal %s act %s tgt %d d %.1f hp %d attackers %d/%d%s\n", ms, swap, i,
               i == player ? "player" : c->ally ? "crew" : "enemy", c->goal[0] ? c->goal : "-", c->action[0] ? c->action : "-",
               c->target, d, c->hp, c->attackers, c->max_attackers, c->listed ? " slot" : "");
    }
}

void hle_aidbg_swap(uint32_t swap, uint32_t game_ms, int last) {
    static AiChar all[AI_SLOTS];
    if (!started || game_ms < last_ms) {   /* first call or a clock reset */
        started = 1;
        next_ms = game_ms;
        last_ms = game_ms;
        summary_at = game_ms + SUMMARY_MS;
        memset(&win, 0, sizeof win);
        memset(prev, 0, sizeof prev);
    }
    if (game_ms >= next_ms) {
        int player = ai_snapshot(guest_read, NULL, all);
        double dt = game_ms - last_ms;
        if (dt > 1000) dt = SAMPLE_MS;   /* a pause or a load */
        ai_stats_add(&win, all, player, dt);
        ai_stats_add(&total, all, player, dt);
        log_changes(all, player, game_ms, swap);
        memcpy(prev, all, sizeof prev);
        last_ms = game_ms;
        next_ms = game_ms + SAMPLE_MS;
        if (game_ms >= summary_at) {
            summary("window", &win, all, game_ms);
            gate_line("window", gate_win, game_ms);
            memset(gate_win, 0, sizeof gate_win);
            AiStats keep = win;
            memset(&win, 0, sizeof win);
            memcpy(win.last_hp, keep.last_hp, sizeof win.last_hp);
            memcpy(win.last_target, keep.last_target, sizeof win.last_target);
            win.have_last = keep.have_last;
            summary_at = game_ms + SUMMARY_MS;
        }
    }
    if (last) {
        summary("total", &total, all, game_ms);
        gate_line("total", gate_total, game_ms);
    }
}
#endif
