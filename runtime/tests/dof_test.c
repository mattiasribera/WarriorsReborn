/* Game-independent --cutscene-dof test (src/dof.c): option parsing, the parameters per strength,
 * the bars gate (0.3 s fades, the start flag), the camera-cut test, the focus pick (centre
 * percentile, nothing drawn, nearest focus), its smoothing and snap, and the circle of confusion
 * (sharp at the focus, far and near signs, limits, a far focus blurs less), and the characters' sharp
 * range (a back turned to the camera and the speaker behind it both sharp, the wall behind blurred),
 * and the focus on the speaker (the face of a bone palette, the tracker
 * that finds who talks with no flicker on short pauses, the speaker sharp and a nearer character
 * mildly soft, no speaker = step 1 exactly, a smooth pull between speakers). */
#include "../src/dof.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static void test_args(void) {
    DofConfig c = DOF_DEFAULTS;
    CHECK(!c.on && c.strength == 1.0f, "defaults: off, strength 1");
    CHECK(dof_arg(&c, "--cutscene-dof", NULL) == 1 && c.on, "--cutscene-dof turns it on");
    CHECK(dof_arg(&c, "--no-cutscene-dof", NULL) == 1 && !c.on, "--no-cutscene-dof turns it off");
    CHECK(dof_arg(&c, "--cutscene-dof-strength", "1.5") == 2 && c.strength == 1.5f, "--cutscene-dof-strength 1.5");
    CHECK(dof_arg(&c, "--cutscene-dof-strength", "2.5") == -1 && c.strength == 1.5f, "strength 2.5 rejected, unchanged");
    CHECK(dof_arg(&c, "--cutscene-dof-strength", "0.1") == -1 && c.strength == 1.5f, "strength 0.1 rejected");
    CHECK(dof_arg(&c, "--cutscene-dof-strength", "nan") == -1, "strength nan rejected");
    CHECK(dof_arg(&c, "--cutscene-dof-strength", NULL) == -1, "strength without a value rejected");
    CHECK(dof_arg(&c, "--filmic", NULL) == 0, "other options are not consumed");
    CHECK(c.speaker == 1, "speaker focus on by default");
    CHECK(dof_arg(&c, "--cutscene-dof-speaker", "0") == 2 && c.speaker == 0, "--cutscene-dof-speaker 0");
    CHECK(dof_arg(&c, "--cutscene-dof-speaker", "1") == 2 && c.speaker == 1, "--cutscene-dof-speaker 1");
    CHECK(dof_arg(&c, "--cutscene-dof-speaker", "2") == -1 && c.speaker == 1, "--cutscene-dof-speaker 2 rejected, unchanged");
    CHECK(dof_arg(&c, "--cutscene-dof-speaker", NULL) == -1, "--cutscene-dof-speaker without a value rejected");
}

static void test_params(void) {
    RDof p1, p2, p0;
    dof_params(1, &p1);
    dof_params(2, &p2);
    dof_params(0, &p0);
    CHECK(p1.coc > 0.005f && p1.coc < 0.02f, "strength 1: far CoC %.4f of the height", p1.coc);
    CHECK(fabsf(p2.coc - 2 * p1.coc) < 1e-6f, "strength 2 doubles it");
    CHECK(fabsf(p0.coc - 0.25f * p1.coc) < 1e-6f, "strength clamped to 0.25");
    CHECK(p1.near_max > 1 && p1.focus_ref > 0 && p1.focus_min_scale > 0 && p1.focus_min_scale < 1, "near limit, reference, floor");
    CHECK(p1.tau > 0.1f && p1.tau < 0.5f && p1.pct > 0 && p1.pct < 0.5f, "tau %.2f s, percentile %.2f", p1.tau, p1.pct);
}

static void test_gate(void) {
    DofGate g = {0};
    int started = 0;
    float w = dof_gate_step(&g, 0, 1.0f / 60, &started);
    CHECK(w == 0 && !started, "no bars: off");
    w = dof_gate_step(&g, 1, 1.0f / 60, &started);
    CHECK(w > 0 && w < 0.05f && started, "bars: fades in (%.4f), started", w);
    float t = 1.0f / 60;
    int frames = 1;
    while (w < 1 && frames < 1000) {
        w = dof_gate_step(&g, 1, 1.0f / 60, &started);
        t += 1.0f / 60;
        frames++;
        if (started) break;
    }
    CHECK(!started, "started only once");
    while (w < 1 && frames < 1000) {
        w = dof_gate_step(&g, 1, 1.0f / 60, &started);
        t += 1.0f / 60;
        frames++;
    }
    CHECK(fabsf(t - DOF_FADE_S) < 0.02f, "full after %.3f s", t);
    w = dof_gate_step(&g, 1, 5.0f, &started);
    CHECK(w == 1, "held at 1");
    w = dof_gate_step(&g, 0, 0.15f, &started);
    CHECK(w > 0.5f && w < 1, "a long frame steps at most 0.1 s (%.3f)", w);
    t = 0;
    while (w > 0 && t < 2) {
        w = dof_gate_step(&g, 0, 1.0f / 60, &started);
        t += 1.0f / 60;
    }
    CHECK(w == 0 && t < DOF_FADE_S, "fades out (%.3f s more)", t);
    w = dof_gate_step(&g, 1, 1.0f / 60, &started);
    CHECK(started, "the next cutscene starts again");
}

/* A view matrix (row vectors) of a camera at eye looking along yaw (radians about z, world z up). */
static void view(float m[16], const float eye[3], float yaw) {
    const float f[3] = {cosf(yaw), sinf(yaw), 0}, up[3] = {0, 0, 1};
    const float r[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    memset(m, 0, 16 * sizeof *m);
    for (int i = 0; i < 3; i++) {
        m[i * 4 + 0] = r[i];
        m[i * 4 + 1] = u[i];
        m[i * 4 + 2] = -f[i];   /* right-handed: the camera looks down -z */
    }
    for (int j = 0; j < 3; j++) m[12 + j] = -(eye[0] * m[j] + eye[1] * m[4 + j] + eye[2] * m[8 + j]);
    m[15] = 1;
}

static void test_cut(void) {
    float a[16], b[16], e[3];
    const float eye[3] = {10, -4, 1.6f};
    view(a, eye, 0.3f);
    dof_eye(a, e);
    CHECK(fabsf(e[0] - 10) < 1e-4f && fabsf(e[1] + 4) < 1e-4f && fabsf(e[2] - 1.6f) < 1e-4f, "eye recovered (%.3f %.3f %.3f)", e[0], e[1], e[2]);
    CHECK(!dof_cut(a, a), "same camera: no cut");
    const float near_eye[3] = {10.5f, -4, 1.6f}, far_eye[3] = {14, -4, 1.6f};
    view(b, near_eye, 0.35f);
    CHECK(!dof_cut(a, b), "a dolly of 0.5 and a 3 degree pan: no cut");
    view(b, far_eye, 0.3f);
    CHECK(dof_cut(a, b), "a jump of 4 units: cut");
    view(b, eye, 0.3f + 0.6f);
    CHECK(dof_cut(a, b), "a 34 degree turn: cut");
    memcpy(b, a, sizeof b);
    b[5] = NAN;
    CHECK(dof_cut(a, b), "not finite: cut");
}

static void test_focus(void) {
    RDof p;
    dof_params(1, &p);
    /* A character at 2.5 in the middle third, a wall at 12 behind. */
    float d[DOF_GRID_X * DOF_GRID_Y], w[DOF_GRID_X * DOF_GRID_Y];
    int n = 0;
    for (int y = 0; y < DOF_GRID_Y; y++)
        for (int x = 0; x < DOF_GRID_X; x++, n++) {
            const float u = DOF_GRID_U0 + (DOF_GRID_U1 - DOF_GRID_U0) * x / (DOF_GRID_X - 1);
            const float v = DOF_GRID_V0 + (DOF_GRID_V1 - DOF_GRID_V0) * y / (DOF_GRID_Y - 1);
            d[n] = fabsf(u - 0.5f) < 0.1f ? 2.5f : 12.0f;
            w[n] = dof_grid_weight(u, v);
        }
    CHECK(dof_grid_weight(0.5f, 0.5f) > dof_grid_weight(0.3f, 0.5f) && dof_grid_weight(0.3f, 0.5f) > 0, "centre weighted");
    float f = dof_focus_pick(d, w, n, p.pct, p.focus_min);
    CHECK(fabsf(f - 2.5f) < 1e-4f, "the character in the centre wins over the wall (%.2f)", f);
    for (int i = 0; i < n; i++) d[i] = 12.0f;
    d[0] = 0.8f;   /* a near shoulder in a corner of the grid */
    f = dof_focus_pick(d, w, n, p.pct, p.focus_min);
    CHECK(fabsf(f - 12) < 1e-4f, "one near sample at the edge does not take the focus (%.2f)", f);
    for (int i = 0; i < n; i++) d[i] = 0;
    CHECK(dof_focus_pick(d, w, n, p.pct, p.focus_min) == 0, "nothing drawn: no meter");
    for (int i = 0; i < n; i++) d[i] = 0.1f;
    CHECK(dof_focus_pick(d, w, n, p.pct, p.focus_min) == p.focus_min, "no nearer than focus_min");

    float fl = dof_focus_step(0, 3, 1, 1.0f / 60, p.tau, 1);
    CHECK(fl == 3, "snap");
    fl = dof_focus_step(fl, 1, 0, 1.0f / 60, p.tau, 0);
    CHECK(fl == 3, "no meter: holds");
    float t = 0;
    while (fl > 1 + 2 * 0.3679f && t < 5) {   /* within e^-1 of the 2-stop step */
        fl = dof_focus_step(fl, 1, 1, 1.0f / 60, p.tau, 0);
        t += 1.0f / 60;
    }
    CHECK(fabsf(t - p.tau) < 0.03f, "time constant %.3f s", t);
}

static void test_coc(void) {
    RDof p;
    dof_params(1, &p);
    const float h = 1080;
    CHECK(fabsf(dof_coc(&p, 2, 2, h)) < 1e-4f, "sharp at the focus");
    const float far = dof_coc(&p, 2, 1e6f, h), mid = dof_coc(&p, 2, 6, h), nr = dof_coc(&p, 2, 1.2f, h);
    CHECK(far > mid && mid > 0, "far field positive, growing (%.2f, %.2f px)", mid, far);
    CHECK(fabsf(far - p.coc * h) < 0.01f, "far limit coc x height (%.2f)", far);
    CHECK(nr < 0, "near field negative (%.2f)", nr);
    CHECK(fabsf(dof_coc(&p, 2, 0.01f, h) + p.near_max * p.coc * h) < 0.01f, "near limit");
    CHECK(dof_coc(&p, 20, 1e6f, h) < 0.5f * far && dof_coc(&p, 20, 1e6f, h) >= p.focus_min_scale * far - 0.01f,
          "a far focus blurs less (%.2f px)", dof_coc(&p, 20, 1e6f, h));
    int mono = 1;
    for (int i = 1; i < 400; i++)
        if (dof_coc(&p, 3, 0.05f * i, h) < dof_coc(&p, 3, 0.05f * (i - 1), h)) mono = 0;
    CHECK(mono, "monotonic in depth (the gather's in-front test)");
}

/* Characters are never the blurred subject. A back-facing character
 * at 1.5 near the camera and the speaker at 3.0 behind him, a wall at 10 behind both. */
static void test_chars(void) {
    RDof p;
    dof_params(1, &p);
    const float h = 1080;
    CHECK(p.char_margin > 0 && p.char_margin < 0.5f, "character margin %.2f", p.char_margin);
    CHECK(dof_char_visible(3.0f, 3.0f) && dof_char_visible(3.0f, 3.02f), "the mask's surface is visible there");
    CHECK(!dof_char_visible(3.0f, 2.0f), "a surface in front hides the character");
    CHECK(!dof_char_visible(0, 10.0f) && !dof_char_visible(NAN, 10.0f), "no character in the mask");
    float d[DOF_GRID_X * DOF_GRID_Y], w[DOF_GRID_X * DOF_GRID_Y], cd[DOF_CGRID_X * DOF_CGRID_Y];
    int n = 0, cn = 0;
    for (int y = 0; y < DOF_GRID_Y; y++)
        for (int x = 0; x < DOF_GRID_X; x++, n++) {
            const float u = DOF_GRID_U0 + (DOF_GRID_U1 - DOF_GRID_U0) * x / (DOF_GRID_X - 1);
            const float v = DOF_GRID_V0 + (DOF_GRID_V1 - DOF_GRID_V0) * y / (DOF_GRID_Y - 1);
            d[n] = u < 0.5f ? 1.5f : u < 0.62f ? 3.0f : 10.0f;   /* the back in the left half, the speaker right of it */
            w[n] = dof_grid_weight(u, v);
        }
    for (int y = 0; y < DOF_CGRID_Y; y++)
        for (int x = 0; x < DOF_CGRID_X; x++, cn++) {
            const float u = DOF_CGRID_U0 + (DOF_CGRID_U1 - DOF_CGRID_U0) * x / (DOF_CGRID_X - 1);
            cd[cn] = u < 0.5f ? 1.5f : u < 0.62f ? 3.0f : 0;
        }
    const float old = dof_focus_pick(d, w, n, p.pct, p.focus_min);
    CHECK(fabsf(old - 1.5f) < 1e-4f && dof_coc(&p, old, 3.0f, h) > 2, "before: the back takes the focus, the speaker blurs (%.2f px)",
          dof_coc(&p, old, 3.0f, h));
    float r[2] = {0, 0};
    CHECK(dof_focus_range(&p, d, w, n, cd, cn, r) == 2, "characters on the grid: the range spans them");
    CHECK(fabsf(r[0] - 1.5f / (1 + p.char_margin)) < 1e-4f && fabsf(r[1] - 3.0f * (1 + p.char_margin)) < 1e-4f, "range %.3f-%.3f", r[0], r[1]);
    CHECK(dof_coc_range(&p, r[0], r[1], 1.5f, 0, h) == 0, "the near back-facing character at 1.5: CoC 0");
    CHECK(dof_coc_range(&p, r[0], r[1], 3.0f, 0, h) == 0, "the speaker at 3.0: CoC 0");
    const float wall = dof_coc_range(&p, r[0], r[1], 10.0f, 0, h);
    CHECK(wall > 2, "the wall at 10 blurs (%.2f px)", wall);
    CHECK(dof_coc_range(&p, r[0], r[1], 0.6f, 0, h) < -1, "scenery in front of the nearest character blurs (%.2f px)", dof_coc_range(&p, r[0], r[1], 0.6f, 0, h));
    CHECK(dof_coc_range(&p, r[0], r[1], 10.0f, 1, h) == 0 && dof_coc_range(&p, r[0], r[1], 0.6f, 1, h) == 0 &&
              dof_coc_range(&p, r[0], r[1], 1e6f, 1, h) == 0,
          "a character pixel anywhere: CoC 0");
    int same = 1, mono = 1;
    for (int i = 1; i < 400; i++) {
        const float di = 0.05f * i;
        if (dof_coc_range(&p, 2.5f, 2.5f, di, 0, h) != dof_coc(&p, 2.5f, di, h)) same = 0;
        if (dof_coc_range(&p, r[0], r[1], di, 0, h) < dof_coc_range(&p, r[0], r[1], 0.05f * (i - 1), 0, h)) mono = 0;
    }
    CHECK(same, "near == far is the single focus exactly");
    CHECK(mono, "monotonic in depth outside characters");
    for (int i = 0; i < cn; i++) cd[i] = 0;
    float r2[2] = {7, 7};
    CHECK(dof_focus_range(&p, d, w, n, cd, cn, r2) == 1 && r2[0] == old && r2[1] == old, "no character: the centre meter as before (%.2f)", r2[0]);
    for (int i = 0; i < n; i++) d[i] = 0;
    r2[0] = r2[1] = 7;
    CHECK(dof_focus_range(&p, d, w, n, cd, cn, r2) == 0 && r2[0] == 7 && r2[1] == 7, "nothing drawn: unchanged");
    cd[0] = 0.1f;
    CHECK(dof_focus_range(&p, d, w, n, cd, cn, r2) == 2 && r2[0] == p.focus_min && r2[1] >= p.focus_min, "no nearer than focus_min");
}

/* ---- focus on the speaker ---- */
/* A palette (slot k at rows 3k..3k+2 from row `base`) of a character turned by `yaw` about z at
 * position pos: every slot the same rotation, the jaw (slot 6) and the lips (7, 13) opened by `jaw`
 * radians about x against the head. */
static void rot_z(float a, float m[9]) {
    const float c = cosf(a), s = sinf(a);
    const float r[9] = {c, -s, 0, s, c, 0, 0, 0, 1};
    memcpy(m, r, sizeof r);
}
static void make_palette(float c[192][4], int base, float yaw, const float pos[3], float jaw) {
    memset(c, 0, 192 * 16);
    float r[9], j[9], rj[9];
    rot_z(yaw, r);
    const float cj = cosf(jaw), sj = sinf(jaw);
    const float x[9] = {1, 0, 0, 0, cj, -sj, 0, sj, cj};
    memcpy(j, x, sizeof j);
    for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++) rj[a * 3 + b] = r[a * 3] * j[b] + r[a * 3 + 1] * j[3 + b] + r[a * 3 + 2] * j[6 + b];
    for (int k = 0; k < DOF_PAL_SLOTS; k++) {
        const float *m = k == 6 || k == 7 || k == 13 ? rj : r;
        for (int a = 0; a < 3; a++) {
            for (int b = 0; b < 3; b++) c[base + 3 * k + a][b] = m[a * 3 + b];
            c[base + 3 * k + a][3] = pos[a];
        }
    }
}
static void make_face(DofFace *f, float yaw, const float pos[3], float jaw, float dist, float x) {
    static float c[192][4];
    make_palette(c, 60, yaw, pos, jaw);
    dof_face_palette((const float (*)[4])c, 60, f);
    f->dist = dist;
    f->ndc[0] = x;
    f->ndc[1] = 0;
}

static void test_face(void) {
    static float c[192][4];
    const float pos[3] = {4, -2, 1.5f};
    DofFace f;
    make_palette(c, 60, 0.7f, pos, 0.2f);
    CHECK(dof_face_palette((const float (*)[4])c, 60, &f) == 1, "a human palette has a face");
    CHECK(f.key[0] == 4 && f.key[1] == -2 && f.key[2] == 1.5f, "key: the head slot's translation");
    CHECK(fabsf(f.mouth[0][4] - cosf(0.2f)) < 1e-5f && fabsf(f.mouth[0][7] - sinf(0.2f)) < 1e-5f && fabsf(f.mouth[0][0] - 1) < 1e-5f,
          "the jaw against the head: its opening alone, not the body's turn");
    CHECK(fabsf(f.mouth[2][0] - 1) < 1e-5f && fabsf(f.mouth[2][4] - 1) < 1e-5f && fabsf(f.mouth[2][1]) < 1e-5f, "a still mouth corner: identity");
    DofFace g;
    make_palette(c, 60, -1.2f, pos, 0.2f);
    dof_face_palette((const float (*)[4])c, 60, &g);
    float e = 0;
    for (int k = 0; k < DOF_MOUTH_SLOTS; k++)
        for (int i = 0; i < 9; i++) e = fmaxf(e, fabsf(f.mouth[k][i] - g.mouth[k][i]));
    CHECK(e < 1e-5f, "turning the whole body leaves the mouth unchanged (%.2g)", e);
    memset(c, 0, sizeof c);
    CHECK(dof_face_palette((const float (*)[4])c, 60, &f) == 0, "a zero head matrix: no face");
    make_palette(c, 60, 0, pos, 0);
    c[60 + 12][0] = NAN;
    CHECK(dof_face_palette((const float (*)[4])c, 60, &f) == 0, "a NaN in the head: no face");
    CHECK(dof_face_palette((const float (*)[4])c, 120, &f) == 0, "a palette past the constants: no face");
}

/* Three characters at 60 frames per second, their skeletons animated at 30 Hz (the title's cutscenes
 * without --sim60): A near the camera (1.2) with his back to it, B behind him (2.5), C (3.5). `talk`
 * says who moves his jaw and lips (0-0.3 rad at 4 Hz: about 10 per second, level51's talkers measured
 * 6-25, the listeners 0.2-4); the others' mouths rest. Returns o->changed. */
static const float spk_pos[3][3] = {{0, 0, 1.6f}, {1.5f, 1, 1.6f}, {-1.5f, 1.5f, 1.6f}};
static const float spk_dist[3] = {1.2f, 2.5f, 3.5f};
static int spk_frame(DofSpeaker *s, int frame, int talk, DofSpeakerOut *o) {
    DofFace f[4];
    const float t = (frame & ~1) / 60.0f;
    for (int i = 0; i < 3; i++)
        make_face(&f[i], 0.3f * i, spk_pos[i], i == talk ? 0.15f + 0.15f * sinf(2 * 3.14159265f * 4 * t) : 0.02f, spk_dist[i],
                  -0.5f + 0.5f * i);
    f[3] = f[1];   /* a second draw of B (his head): the same palette */
    dof_speaker_step(s, f, 4, 1.0f / 60, o);
    return o->changed;
}

static void test_speaker(void) {
    DofSpeaker s;
    DofSpeakerOut o;
    dof_speaker_reset(&s);
    int changes = 0, frame = 0, first = -1;
    float maxstep = 0, prev = 0;
    for (; frame < 90; frame++) {   /* B talks */
        changes += spk_frame(&s, frame, 1, &o);
        if (o.weight > 0 && first < 0) first = frame;
        maxstep = fmaxf(maxstep, fabsf(o.weight - prev));
        prev = o.weight;
    }
    CHECK(first > 0 && first <= 45, "B talks: picked after %.2f s", first / 60.0f);
    CHECK(o.weight == 1 && fabsf(o.dist - 2.5f) < 1e-4f && changes == 1, "the speaker is B at %.2f (weight %.2f, %d changes)", o.dist, o.weight,
          changes);
    CHECK(maxstep <= 1.0f / 60 / DOF_SPK_FADE + 1e-4f, "the weight rises smoothly (%.3f per frame at most)", maxstep);
    for (int k = 0; k < 18; k++, frame++) changes += spk_frame(&s, frame, -1, &o);   /* a 0.3 s pause */
    for (int k = 0; k < 60; k++, frame++) changes += spk_frame(&s, frame, 1, &o);
    CHECK(changes == 1 && o.weight == 1 && fabsf(o.dist - 2.5f) < 1e-4f, "a short pause keeps B: no flicker (%d changes)", changes);
    int switched = -1;
    for (int k = 0; k < 120; k++, frame++) {   /* now A, nearer, talks */
        if (spk_frame(&s, frame, 0, &o) && switched < 0) switched = k;
        changes += o.changed;
    }
    CHECK(switched > 0 && switched <= 60 && changes == 2 && fabsf(o.dist - 1.2f) < 1e-4f, "A answers: the focus moves to him after %.2f s (%d changes)",
          switched / 60.0f, changes);
    for (int k = 0; k < 10; k++, frame++) {   /* a line shorter than the pick and hold: C says one word */
        changes += spk_frame(&s, frame, 2, &o);
    }
    for (int k = 0; k < 30; k++, frame++) changes += spk_frame(&s, frame, 0, &o);
    CHECK(changes == 2 && fabsf(o.dist - 1.2f) < 1e-4f, "a one-word interjection does not steal the focus (%d changes)", changes);
    prev = o.weight;
    maxstep = 0;
    int none = -1;
    for (int k = 0; k < 180; k++, frame++) {   /* silence */
        spk_frame(&s, frame, -1, &o);
        if (!o.id && none < 0) none = k;
        maxstep = fmaxf(maxstep, fabsf(o.weight - prev));
        prev = o.weight;
    }
    CHECK(none > 0 && o.weight == 0, "silence: the speaker is dropped after %.2f s and the weight falls to 0", none / 60.0f);
    CHECK(maxstep <= 1.0f / 60 / DOF_SPK_FADE + 1e-4f, "it falls smoothly (%.3f per frame at most)", maxstep);
    dof_speaker_reset(&s);
    for (int k = 0; k < 60; k++) dof_speaker_step(&s, NULL, 0, 1.0f / 60, &o);
    CHECK(o.weight == 0 && !o.id, "no characters: no speaker");
    /* Out of view: B talks off screen (a reaction shot on A): no speaker. */
    dof_speaker_reset(&s);
    for (int k = 0; k < 90; k++) {
        DofFace f[3];
        const float t = (k & ~1) / 60.0f;
        for (int i = 0; i < 3; i++)
            make_face(&f[i], 0, spk_pos[i], i == 1 ? 0.15f + 0.15f * sinf(2 * 3.14159265f * 4 * t) : 0.02f, i == 1 ? 0 : spk_dist[i], 0);
        dof_speaker_step(&s, f, 3, 1.0f / 60, &o);
    }
    CHECK(o.weight == 0 && !o.id, "the talker out of view: no speaker (step 1's range)");
}

/* The speaker B at 2.5 sharp, the back-facing A at 1.2 in front of him soft but less than the
 * background, the wall at 10 as blurred as with any focus at B; speaker weight 0 = step 1 exactly. */
static void test_speaker_coc(void) {
    RDof p;
    dof_params(1, &p);
    const float h = 1080, k = p.coc * h;
    CHECK(p.char_blur > 0 && p.char_blur < 1 && p.char_cap > 0 && p.char_cap < 1, "other characters: scale %.2f, cap %.2f", p.char_blur, p.char_cap);
    float r[2];
    dof_speaker_range(&p, 2.5f, r);
    CHECK(fabsf(r[0] - 2.5f / (1 + p.char_margin)) < 1e-5f && fabsf(r[1] - 2.5f * (1 + p.char_margin)) < 1e-5f, "range around him %.3f-%.3f", r[0], r[1]);
    CHECK(dof_coc_speaker(&p, r[0], r[1], 2.5f, 1, 1, h) == 0, "the speaker: CoC 0");
    const float a = dof_coc_speaker(&p, r[0], r[1], 1.2f, 1, 1, h), bg = dof_coc_speaker(&p, r[0], r[1], 1.2f, 0, 1, h);
    CHECK(a < -1.5f, "A, nearer, his back to the camera: soft (%.2f px)", a);
    CHECK(fabsf(a) <= p.char_cap * k + 1e-4f && fabsf(a) < fabsf(bg), "but capped, less than scenery at his depth (%.2f vs %.2f px)", a, bg);
    const float c = dof_coc_speaker(&p, r[0], r[1], 3.5f, 1, 1, h), wall = dof_coc_speaker(&p, r[0], r[1], 10, 0, 1, h);
    CHECK(c > 0.5f && c < wall, "C behind him: mildly soft (%.2f px), the wall more (%.2f px)", c, wall);
    CHECK(fabsf(dof_coc_speaker(&p, r[0], r[1], 1.2f, 1, 0.5f, h) - 0.5f * a) < 1e-4f, "half the speaker weight: half of it");
    int same = 1;
    for (int i = 1; i < 400; i++)
        for (int ch = 0; ch < 2; ch++) {
            const float d = 0.05f * i;
            if (dof_coc_speaker(&p, 1.0f, 3.0f, d, ch, 0, h) != dof_coc_range(&p, 1.0f, 3.0f, d, ch, h)) same = 0;
            if (dof_coc_speaker(&p, r[0], r[1], d, 0, 1, h) != dof_coc_range(&p, r[0], r[1], d, 0, h)) same = 0;
        }
    CHECK(same, "speaker weight 0, or a pixel that is no character: step 1's CoC exactly");
    /* The pull from B (2.5) to A (1.2) when A answers: dof_focus_step on each end, tau. */
    float r2[2];
    dof_speaker_range(&p, 1.2f, r2);
    float n = log2f(r[0]), m = 0;
    int t90 = -1;
    for (int i = 0; i < 120; i++) {
        const float n2 = dof_focus_step(n, log2f(r2[0]), 1, 1.0f / 60, p.tau, 0);
        m = fmaxf(m, fabsf(n2 - n));
        n = n2;
        if (t90 < 0 && fabsf(n - log2f(r2[0])) < 0.1f * fabsf(log2f(r[0]) - log2f(r2[0]))) t90 = i;
    }
    CHECK(m < 0.08f && t90 > 15 && t90 < 45, "the focus pulls smoothly (at most %.3f stop per frame, 90%% in %.2f s)", m, t90 / 60.0f);
}

int main(void) {
    test_args();
    test_params();
    test_gate();
    test_cut();
    test_focus();
    test_coc();
    test_chars();
    test_face();
    test_speaker();
    test_speaker_coc();
    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
