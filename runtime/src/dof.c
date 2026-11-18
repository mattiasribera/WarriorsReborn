/* --cutscene-dof options, bars gate, cut test and the focus / CoC math (dof.h). Keep
 * dof_focus_pick, dof_grid_weight and dof_focus_step in step with shaders/dof_focus.frag and
 * dof_coc with shaders/dof_common.glsl. */
#include "dof.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int dof_arg(DofConfig *c, const char *opt, const char *val) {
    if (!strcmp(opt, "--cutscene-dof") || !strcmp(opt, "--no-cutscene-dof")) {
        c->on = opt[2] == 'c';
        return 1;
    }
    if (!strcmp(opt, "--cutscene-dof-strength")) {
        char *end;
        if (!val || !*val) return -1;
        const float f = (float)strtod(val, &end);
        if (*end || !(f >= DOF_STRENGTH_MIN && f <= DOF_STRENGTH_MAX)) return -1;   /* NaN fails the range check */
        c->strength = f;
        return 2;
    }
    if (!strcmp(opt, "--cutscene-dof-speaker")) {
        if (!val || (strcmp(val, "0") && strcmp(val, "1"))) return -1;
        c->speaker = val[0] == '1';
        return 2;
    }
    return 0;
}

static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

/* Tuning: at strength 1 a background far behind a close-up's focus
 * blurs over 1% of the frame height (about 11 pixels at 1080p): soft like a long lens, the faces
 * and the scene still readable. A near shoulder may blur up to 1.25x that (a speaker close to the
 * camera stays readable: 1.6x hid the face in level51's intro). A wide shot focused far away blurs less (down to 0.4x from a focus at 7.5 view units: the title's units are about
 * metres). Without characters in view the focus is the 35th percentile of the centre's distances
 * (the nearer third wins), smoothed over 0.25 s; with characters the sharp range spans the nearest
 * to the farthest one on the character grid, 15% wider on each side (a body's depth, the floor at
 * their feet), and every character pixel is sharp (dof_coc_range). */
void dof_params(float strength, RDof *p) {
    const float s = clampf(strength, DOF_STRENGTH_MIN, DOF_STRENGTH_MAX);
    memset(p, 0, sizeof *p);
    p->coc = 0.010f * s;
    p->near_max = 1.25f;
    p->focus_ref = 3.0f;
    p->focus_min_scale = 0.4f;
    p->focus_min = 0.4f;
    p->pct = 0.35f;
    p->tau = 0.25f;
    p->char_margin = 0.15f;
    p->char_blur = 0.5f;
    p->char_cap = 0.4f;
}

float dof_gate_step(DofGate *g, int bars, float dt, int *started) {
    const float step = clampf(dt, 0, 0.1f) / DOF_FADE_S;
    const float was = g->t;
    g->t = clampf(g->t + (bars ? step : -step), 0, 1);
    if (g->t > 0.9999f) g->t = 1;
    *started = was == 0 && g->t > 0;
    return g->t * g->t * (3 - 2 * g->t);
}

void dof_eye(const float m[16], float eye[3]) {
    for (int i = 0; i < 3; i++) eye[i] = -(m[12] * m[i * 4] + m[13] * m[i * 4 + 1] + m[14] * m[i * 4 + 2]);
}

/* The cosine between column c of two matrices' 3x3 parts (the camera's axes in the world). */
static float axis_cos(const float a[16], const float b[16], int c) {
    float d = 0, la = 0, lb = 0;
    for (int i = 0; i < 3; i++) {
        d += a[i * 4 + c] * b[i * 4 + c];
        la += a[i * 4 + c] * a[i * 4 + c];
        lb += b[i * 4 + c] * b[i * 4 + c];
    }
    return la > 0 && lb > 0 ? d / sqrtf(la * lb) : -1;
}

int dof_cut(const float prev[16], const float cur[16]) {
    for (int i = 0; i < 16; i++)
        if (!isfinite(prev[i]) || !isfinite(cur[i])) return 1;
    float a[3], b[3];
    dof_eye(prev, a);
    dof_eye(cur, b);
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    if (dx * dx + dy * dy + dz * dz > DOF_CUT_DIST * DOF_CUT_DIST) return 1;
    const float c = cosf(DOF_CUT_DEG * 3.14159265f / 180);
    return axis_cos(prev, cur, 2) < c || axis_cos(prev, cur, 1) < c;
}

float dof_grid_weight(float u, float v) {
    const float du = u - 0.5f, dv = v - 0.5f;
    return expf(-(du * du / (2 * 0.12f * 0.12f) + dv * dv / (2 * 0.16f * 0.16f)));
}

float dof_focus_pick(const float *d, const float *w, int n, float pct, float focus_min) {
    float total = 0;
    for (int i = 0; i < n; i++)
        if (d[i] > 0 && isfinite(d[i])) total += w[i];
    if (!(total > 0)) return 0;
    float best = 0;
    for (int i = 0; i < n; i++) {
        if (!(d[i] > 0 && isfinite(d[i]))) continue;
        const float di = d[i] > focus_min ? d[i] : focus_min;
        if (best > 0 && di >= best) continue;
        float below = 0;   /* the weight at this distance or nearer */
        for (int k = 0; k < n; k++)
            if (d[k] > 0 && isfinite(d[k]) && (d[k] > focus_min ? d[k] : focus_min) <= di) below += w[k];
        if (below >= pct * total) best = di;
    }
    return best;
}

float dof_focus_step(float focus_log, float meter_log, int meter_valid, float dt, float tau, int snap) {
    if (!meter_valid) return focus_log;
    if (snap) return meter_log;
    const float k = 1 - expf(-clampf(dt, 0, 0.1f) / (tau > 0 ? tau : 1e-3f));
    return focus_log + (meter_log - focus_log) * k;
}

float dof_coc(const RDof *p, float f, float d, float height) {
    const float fs = f > p->focus_min ? f : p->focus_min;
    const float k = p->coc * height * clampf(p->focus_ref / fs, p->focus_min_scale, 1);
    return clampf(k * (1 - fs / (d > 1e-4f ? d : 1e-4f)), -p->near_max * k, k);
}

int dof_char_visible(float dc, float ds) {
    if (!(dc > 0 && isfinite(dc)) || !(ds > 0 && isfinite(ds))) return 0;
    return fabsf(dc - ds) <= 0.02f + 0.01f * ds;
}

int dof_focus_range(const RDof *p, const float *d, const float *w, int n, const float *cd, int cn, float range[2]) {
    float cnear = 0, cfar = 0;
    for (int i = 0; i < cn; i++) {
        if (!(cd[i] > 0 && isfinite(cd[i]))) continue;
        if (cnear == 0 || cd[i] < cnear) cnear = cd[i];
        if (cd[i] > cfar) cfar = cd[i];
    }
    if (cfar > 0) {
        const float a = cnear / (1 + p->char_margin), b = cfar * (1 + p->char_margin);
        range[0] = a > p->focus_min ? a : p->focus_min;
        range[1] = b > p->focus_min ? b : p->focus_min;
        return 2;
    }
    const float m = dof_focus_pick(d, w, n, p->pct, p->focus_min);
    if (!(m > 0)) return 0;
    range[0] = range[1] = m;
    return 1;
}

float dof_coc_range(const RDof *p, float near, float far, float d, int character, float height) {
    if (character) return 0;
    const float ns = near > p->focus_min ? near : p->focus_min;
    const float fs = far > ns ? far : ns;
    if (d < ns) return dof_coc(p, ns, d, height);
    if (d > fs) return dof_coc(p, fs, d, height);
    return 0;
}

/* ---- focus on the speaker (dof.h) ---- */
const int dof_mouth_slot[DOF_MOUTH_SLOTS] = {6, 7, 8, 9, 10, 11, 13};

/* The 3x3 of a 3x4 row block (rows r0..r2 of the palette). */
static void m3_of(const float (*c)[4], int r, float m[9]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) m[i * 3 + j] = c[r + i][j];
}
static int m3_inverse(const float a[9], float o[9]) {
    const float c0 = a[4] * a[8] - a[5] * a[7], c1 = a[5] * a[6] - a[3] * a[8], c2 = a[3] * a[7] - a[4] * a[6];
    const float det = a[0] * c0 + a[1] * c1 + a[2] * c2;
    if (!isfinite(det) || fabsf(det) < 1e-6f) return 0;
    const float k = 1 / det;
    o[0] = c0 * k;
    o[1] = (a[2] * a[7] - a[1] * a[8]) * k;
    o[2] = (a[1] * a[5] - a[2] * a[4]) * k;
    o[3] = c1 * k;
    o[4] = (a[0] * a[8] - a[2] * a[6]) * k;
    o[5] = (a[2] * a[3] - a[0] * a[5]) * k;
    o[6] = c2 * k;
    o[7] = (a[1] * a[6] - a[0] * a[7]) * k;
    o[8] = (a[0] * a[4] - a[1] * a[3]) * k;
    return 1;
}

int dof_face_palette(const float (*c)[4], int base, DofFace *f) {
    memset(f, 0, sizeof *f);
    if (base < 0 || base + 3 * DOF_PAL_SLOTS > 192) return 0;
    const int h = base + 3 * DOF_PAL_HEAD;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) {
            if (!isfinite(c[h + i][j])) return 0;
            f->head[i * 4 + j] = c[h + i][j];
        }
    float hm[9], hi[9];
    m3_of(c, h, hm);
    if (!m3_inverse(hm, hi)) return 0;
    for (int i = 0; i < 3; i++) f->key[i] = c[h + i][3];
    for (int s = 0; s < DOF_MOUTH_SLOTS; s++) {
        float m[9];
        m3_of(c, base + 3 * dof_mouth_slot[s], m);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                const float v = hi[i * 3] * m[j] + hi[i * 3 + 1] * m[3 + j] + hi[i * 3 + 2] * m[6 + j];
                f->mouth[s][i * 3 + j] = isfinite(v) ? v : 0;
            }
    }
    return 1;
}

void dof_speaker_reset(DofSpeaker *s) {
    memset(s, 0, sizeof *s);
    s->speaker = s->pending = -1;
    s->next_id = 1;
}

static float mouth_delta(const float a[DOF_MOUTH_SLOTS][9], const float b[DOF_MOUTH_SLOTS][9]) {
    float sum = 0;
    for (int k = 0; k < DOF_MOUTH_SLOTS; k++) {
        float e = 0;
        for (int i = 0; i < 9; i++) e += (a[k][i] - b[k][i]) * (a[k][i] - b[k][i]);
        sum += sqrtf(e);
    }
    return sum;
}
static float dist3(const float a[3], const float b[3]) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}

void dof_speaker_step(DofSpeaker *s, const DofFace *f, int n, float dt, DofSpeakerOut *o) {
    if (s->next_id == 0) dof_speaker_reset(s);
    dt = clampf(dt, 0, 0.1f);
    /* The frame's characters: one per palette (the first face of each), its view distance and
     * screen position from all its draws. */
    int ci[DOF_SPK_TRACKS * 2], nc = 0;
    float cd[DOF_SPK_TRACKS * 2], cx[DOF_SPK_TRACKS * 2], cy[DOF_SPK_TRACKS * 2], cn[DOF_SPK_TRACKS * 2];
    for (int i = 0; i < n; i++) {
        int k = 0;
        while (k < nc && memcmp(f[ci[k]].head, f[i].head, sizeof f[i].head)) k++;
        if (k == nc) {
            if (nc == DOF_SPK_TRACKS * 2) continue;
            ci[nc] = i;
            cd[nc] = cx[nc] = cy[nc] = cn[nc] = 0;
            nc++;
        }
        if (f[i].dist > 0 && isfinite(f[i].dist)) {
            cd[k] += f[i].dist;
            cx[k] += f[i].ndc[0];
            cy[k] += f[i].ndc[1];
            cn[k] += 1;
        }
    }
    /* Match them to the tracks (nearest key first, within DOF_SPK_MATCH); new ones start tracks. */
    int match[DOF_SPK_TRACKS * 2];
    for (int k = 0; k < nc; k++) match[k] = -1;
    for (int t = 0; t < DOF_SPK_TRACKS; t++) s->t[t].seen = 0;
    for (;;) {
        int bk = -1, bt = -1;
        float best = DOF_SPK_MATCH;
        for (int k = 0; k < nc; k++) {
            if (match[k] >= 0) continue;
            for (int t = 0; t < DOF_SPK_TRACKS; t++) {
                if (!s->t[t].used || s->t[t].seen) continue;
                const float d = dist3(f[ci[k]].key, s->t[t].key);
                if (d < best) {
                    best = d;
                    bk = k;
                    bt = t;
                }
            }
        }
        if (bk < 0) break;
        match[bk] = bt;
        s->t[bt].seen = 1;
    }
    int stepped = 0;
    for (int k = 0; k < nc; k++) {
        const DofFace *x = &f[ci[k]];
        if (match[k] >= 0) {
            DofTrack *t = &s->t[match[k]];
            if (mouth_delta(t->mouth, x->mouth) <= DOF_SPK_JUMP) {
                stepped |= memcmp(t->key, x->key, sizeof t->key) || memcmp(t->mouth, x->mouth, sizeof t->mouth);
                continue;
            }
            t->seen = 0;   /* not the same face: another character (below, a new track) */
            if (s->speaker == match[k]) s->speaker = -1;
            if (s->pending == match[k]) s->pending = -1;
            t->used = 0;
            match[k] = -1;
        }
        int t = 0;
        while (t < DOF_SPK_TRACKS && s->t[t].used) t++;
        if (t == DOF_SPK_TRACKS) {   /* full: the track out of view longest */
            t = -1;
            for (int u = 0; u < DOF_SPK_TRACKS; u++)
                if (!s->t[u].seen && u != s->speaker && (t < 0 || s->t[u].unseen > s->t[t].unseen)) t = u;
            if (t < 0) continue;
        }
        memset(&s->t[t], 0, sizeof s->t[t]);
        s->t[t].used = s->t[t].seen = 1;
        s->t[t].id = s->next_id++;
        memcpy(s->t[t].key, x->key, sizeof x->key);
        memcpy(s->t[t].mouth, x->mouth, sizeof x->mouth);
        if (s->pending == t) s->pending = -1;
        match[k] = t;
    }
    /* Mouth speeds, at animation steps. */
    s->since_step += dt;
    if (stepped || s->since_step >= 0.1f) {   /* nothing moved for 0.1 s (3 steps at 30 Hz): every mouth rests */
        const float span = s->since_step > 1.0f / 120 ? s->since_step : 1.0f / 120;
        const float a = 1 - expf(-span / DOF_SPK_TAU);
        for (int t = 0; t < DOF_SPK_TRACKS; t++) {
            DofTrack *tr = &s->t[t];
            if (!tr->used) continue;
            float v = 0;
            for (int k = 0; k < nc; k++)
                if (match[k] == t) v = mouth_delta(tr->mouth, f[ci[k]].mouth) / span;
            tr->speed += (v - tr->speed) * a;
        }
        s->since_step = 0;
    }
    for (int k = 0; k < nc; k++) {
        if (match[k] < 0) continue;
        DofTrack *t = &s->t[match[k]];
        memcpy(t->key, f[ci[k]].key, sizeof t->key);
        memcpy(t->mouth, f[ci[k]].mouth, sizeof t->mouth);
        const int view = cn[k] > 0 && fabsf(cx[k] / cn[k]) <= 1.05f && fabsf(cy[k] / cn[k]) <= 1.05f && cd[k] / cn[k] <= DOF_SPK_MAX_DIST;
        if (view) {
            t->dist = cd[k] / cn[k];
            t->ndc[0] = cx[k] / cn[k];
            t->ndc[1] = cy[k] / cn[k];
            t->unseen = 0;
        } else {
            t->seen = 0;   /* drawn, but not in view */
        }
    }
    for (int t = 0; t < DOF_SPK_TRACKS; t++) {
        DofTrack *tr = &s->t[t];
        if (!tr->used || tr->seen) continue;
        tr->unseen += dt;
        if (tr->unseen > 2.0f && t != s->speaker) tr->used = 0;
    }
    /* The pick. */
    const int was = s->speaker;
    int cand = -1;
    for (int t = 0; t < DOF_SPK_TRACKS; t++)
        if (s->t[t].used && s->t[t].seen && t != s->speaker && (cand < 0 || s->t[t].speed > s->t[cand].speed)) cand = t;
    const DofTrack *cur = s->speaker >= 0 ? &s->t[s->speaker] : NULL;
    if (cand >= 0 && s->t[cand].speed >= DOF_SPK_ON && (!cur || !cur->seen || s->t[cand].speed >= DOF_SPK_RATIO * cur->speed)) {
        if (s->pending != cand) {
            s->pending = cand;
            s->pending_s = 0;
        }
        s->pending_s += dt;
    } else {
        s->pending = -1;
        s->pending_s = 0;
    }
    s->held_s += dt;
    if (cur) {
        s->quiet_s = cur->seen && cur->speed >= DOF_SPK_OFF ? 0 : s->quiet_s + dt;
        if (s->quiet_s >= DOF_SPK_DROP) s->speaker = -1;
    }
    if (s->pending >= 0 && s->pending_s >= DOF_SPK_PICK && (s->speaker < 0 || s->held_s >= DOF_SPK_HOLD)) {
        s->speaker = s->pending;
        s->pending = -1;
        s->pending_s = s->held_s = s->quiet_s = 0;
    }
    const DofTrack *sp = s->speaker >= 0 ? &s->t[s->speaker] : NULL;
    const int on = sp && sp->seen && sp->dist > 0;
    if (on) s->dist = sp->dist;
    const float step = dt / DOF_SPK_FADE;
    s->weight = clampf(s->weight + (on ? step : -step), 0, 1);
    if (!(s->dist > 0)) s->weight = 0;
    o->weight = s->weight;
    o->dist = s->dist;
    o->changed = s->speaker != was;
    o->id = sp ? sp->id : 0;
    o->speed = sp ? sp->speed : 0;
}

void dof_speaker_range(const RDof *p, float d, float range[2]) {
    const float a = d / (1 + p->char_margin), b = d * (1 + p->char_margin);
    range[0] = a > p->focus_min ? a : p->focus_min;
    range[1] = b > p->focus_min ? b : p->focus_min;
}

float dof_coc_speaker(const RDof *p, float near, float far, float d, int character, float spk, float height) {
    if (!character || !(spk > 0)) return dof_coc_range(p, near, far, d, character, height);
    const float c = dof_coc_range(p, near, far, d, 0, height) * p->char_blur;
    const float fs = far > p->focus_min ? far : p->focus_min, ns = near > p->focus_min ? near : p->focus_min;
    const float g = d < ns ? ns : fs;   /* the end whose far-field limit applies */
    const float cap = p->char_cap * p->coc * height * clampf(p->focus_ref / g, p->focus_min_scale, 1);
    return clampf(c, -cap, cap) * spk;
}
