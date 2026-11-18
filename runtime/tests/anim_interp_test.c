/* Game-independent --anim-interp test (src/anim_interp.c): palette-change rate tracking (holds,
 * n-th draw of an identity, identities that come and go), palette blending (skinning matrices lerped:
 * joints kept in place, sheared bones keep their shape, ends exact) and snap detection. */
#include "../src/anim_interp.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static AnimRate rate;

static void test_rate_holds(void) {
    AnimRate *r = &rate;
    anim_rate_init(r);
    /* identity 1 changes every frame, identity 2 every other frame, identity 3 never */
    int first[3] = {0, 0, 0}, later_new[3] = {0, 0, 0}, later_held[3] = {0, 0, 0};
    for (uint32_t f = 0; f < 40; f++) {
        anim_rate_frame(r);
        const int a = anim_rate_draw(r, 1, 100 + f), b = anim_rate_draw(r, 2, 200 + f / 2), c = anim_rate_draw(r, 3, 300);
        const int res[3] = {a, b, c};
        for (int k = 0; k < 3; k++) {
            if (f == 0) first[k] += res[k] == -1;
            else if (res[k] == 1) later_new[k]++;
            else if (res[k] == 0) later_held[k]++;
        }
    }
    CHECK(first[0] && first[1] && first[2], "first frame: no previous pose for any identity");
    CHECK(later_new[0] == 39 && later_held[0] == 0, "every-frame identity: 39 new, 0 held (%d, %d)", later_new[0], later_held[0]);
    CHECK(later_new[1] == 19 && later_held[1] == 20, "every-other-frame identity: 19 new, 20 held (%d, %d)", later_new[1], later_held[1]);
    CHECK(later_new[2] == 0 && later_held[2] == 39, "static identity: all held (%d, %d)", later_new[2], later_held[2]);
    CHECK(r->draws == 117 && r->fresh == 58, "window: 117 matched draws, 58 new (%llu, %llu)", (unsigned long long)r->draws,
          (unsigned long long)r->fresh);
    /* holds completed: identity 1: 39 of 1 frame (the first starts at frame 0); identity 2: 19 of 2 */
    CHECK(r->hold[0] == 39 && r->hold[1] == 19 && r->hold[2] == 0 && r->hold[3] == 0, "holds 1/2/3/4+: 39/19/0/0 (%llu/%llu/%llu/%llu)",
          (unsigned long long)r->hold[0], (unsigned long long)r->hold[1], (unsigned long long)r->hold[2], (unsigned long long)r->hold[3]);
    char line[200];
    anim_rate_format(r, line, sizeof line);
    CHECK(strstr(line, "draws 117") && strstr(line, "new 49.6%") && strstr(line, "39/19/0/0"), "format: %s", line);
    anim_rate_window(r);
    CHECK(r->draws == 0 && r->fresh == 0 && r->hold[0] == 0, "window cleared");
    anim_rate_frame(r);
    CHECK(anim_rate_draw(r, 2, 200 + 40 / 2) == 1, "identities survive a window reset");
}

static void test_rate_occurrences(void) {
    AnimRate *r = &rate;
    anim_rate_init(r);
    /* one identity drawn twice a frame (e.g. a main and a shadow pass): occurrence 0 moves, 1 holds */
    int ok = 1;
    for (uint32_t f = 0; f < 6; f++) {
        anim_rate_frame(r);
        const int a = anim_rate_draw(r, 7, 1000 + f), b = anim_rate_draw(r, 7, 5);
        if (f && (a != 1 || b != 0)) ok = 0;
    }
    CHECK(ok, "the n-th draw of an identity matches last frame's n-th");
    /* a burst: hash held 3 frames then changes; the identity vanishes for a frame -> no hold counted */
    anim_rate_init(r);
    const uint64_t hs[] = {1, 1, 1, 2, 2, 2, 2, 2, 3};
    for (uint32_t f = 0; f < 9; f++) {
        anim_rate_frame(r);
        anim_rate_draw(r, 9, hs[f]);
    }
    CHECK(r->hold[2] == 1 && r->hold[3] == 1, "holds of 3 and 5 frames counted as 3 and 4+ (%llu, %llu)", (unsigned long long)r->hold[2],
          (unsigned long long)r->hold[3]);
    anim_rate_frame(r);   /* gone a frame */
    anim_rate_frame(r);
    CHECK(anim_rate_draw(r, 9, 4) == -1, "an identity missing last frame has no previous pose");
}

static void test_rate_full(void) {
    AnimRate *r = &rate;
    anim_rate_init(r);
    anim_rate_frame(r);
    int tracked = 0;
    for (uint64_t i = 0; i < ANIM_RATE_SLOTS + 50; i++) tracked += anim_rate_draw(r, i * 0x9E3779B97F4A7C15ull + 1, i) == -1;
    CHECK(tracked == ANIM_RATE_SLOTS + 50 && r->dropped >= 50, "a full table drops the rest (%llu dropped), every draw new or untracked",
          (unsigned long long)r->dropped);
    anim_rate_frame(r);
    int matched = 0;
    for (uint64_t i = 0; i < 100; i++) matched += anim_rate_draw(r, i * 0x9E3779B97F4A7C15ull + 1, i) == 0;
    CHECK(matched == 100, "tracked identities still match after a full frame (%d)", matched);
}

/* rows for a bone: rotation about z by deg, scale s, translation t */
static void bone(float (*m)[4], int row, float deg, float s, float tx, float ty, float tz) {
    const float a = deg * 3.14159265f / 180.0f, c = cosf(a) * s, sn = sinf(a) * s;
    const float r[3][4] = {{c, -sn, 0, tx}, {sn, c, 0, ty}, {0, 0, s, tz}};
    memcpy(m[row], r, sizeof r);
}

/* bone rows: rotation about z by deg around the joint j (x, y): p -> R (p - j) + j, plus a shear */
static void joint_bone(float (*m)[4], int row, float deg, float jx, float jy, float shear) {
    const float a = deg * 3.14159265f / 180.0f, c = cosf(a), sn = sinf(a);
    const float r[3][3] = {{c, -sn + shear, 0}, {sn, c, 0}, {0, 0, 1}};
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) m[row + i][k] = r[i][k];
        const float jj[3] = {jx, jy, 0};
        m[row + i][3] = jj[i] - (r[i][0] * jx + r[i][1] * jy);
    }
}

static void test_blend(void) {
    static float a[192][4], b[192][4], o[192][4];
    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);
    for (int i = 0; i < 192; i++) o[i][0] = o[i][1] = o[i][2] = o[i][3] = -7;
    const int base = 120;
    CHECK(anim_palette_bones(base) == 24 && anim_palette_bones(190) == 0 && anim_palette_bones(-1) == 0 && anim_palette_bones(0) == 32 &&
              anim_palette_bones(60) == 32,
          "palette sizes: 24 bones from row 120, none from 190 or -1, at most 32 (from 0 or the title's row 60, before c69 at row 165)");
    /* skinning matrices (bone x inverse bind pose): a jaw turning 0 -> 40 degrees about its joint far
     * from the model origin (0.3, 1.6), and a sheared, non-uniformly scaled lip bone that only moves */
    joint_bone(a, base, 0, 0.3f, 1.6f, 0);
    joint_bone(b, base, 40, 0.3f, 1.6f, 0);
    joint_bone(a, base + 3, 10, 0.2f, 1.5f, 0.06f);
    joint_bone(b, base + 3, 10, 0.2f, 1.5f, 0.06f);
    b[base + 3][3] += 0.02f;
    a[base + 4][1] = b[base + 4][1] = 1.13f;   /* non-uniform scale */
    anim_blend_bones((const float (*)[4])a, (const float (*)[4])b, base, 2, 0.5f, o);
    float jo[3];
    for (int i = 0; i < 3; i++) jo[i] = o[base + i][0] * 0.3f + o[base + i][1] * 1.6f + o[base + i][3];
    CHECK(fabsf(jo[0] - 0.3f) < 1e-5f && fabsf(jo[1] - 1.6f) < 1e-5f && fabsf(jo[2]) < 1e-5f,
          "the jaw's joint stays where both poses put it (%.4f %.4f %.4f)", jo[0], jo[1], jo[2]);
    int shape = 1;
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) shape &= fabsf(o[base + 3 + i][k] - a[base + 3 + i][k]) < 1e-6f;
    CHECK(shape && fabsf(o[base + 3][3] - (a[base + 3][3] + 0.01f)) < 1e-6f, "a sheared, scaled bone that only moves keeps its shape (%.4f %.4f)",
          o[base + 3][1], o[base + 4][1]);
    int lerp = 1;
    anim_blend_bones((const float (*)[4])a, (const float (*)[4])b, base, 2, 0.25f, o);
    for (int i = base; i < base + 6; i++)
        for (int k = 0; k < 4; k++) lerp &= fabsf(o[i][k] - (a[i][k] + (b[i][k] - a[i][k]) * 0.25f)) < 1e-6f;
    CHECK(lerp, "t = 0.25 is the lerp of the skinning matrices");
    CHECK(o[base - 1][0] == -7 && o[base + 6][0] == -7, "rows outside the palette are not written");
    anim_blend_bones((const float (*)[4])a, (const float (*)[4])b, base, 2, 0, o);
    int same = 1;
    for (int i = base; i < base + 6; i++)
        for (int j = 0; j < 4; j++) same &= o[i][j] == a[i][j];
    CHECK(same, "t = 0 gives a exactly");
    anim_blend_bones((const float (*)[4])a, (const float (*)[4])b, base, 2, 1, o);
    same = 1;
    for (int i = base; i < base + 6; i++)
        for (int j = 0; j < 4; j++) same &= o[i][j] == b[i][j];
    CHECK(same, "t = 1 gives b exactly");
}

static void test_snap(void) {
    static float a[192][4], b[192][4];
    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);
    const int base = 120;
    bone(a, base, 0, 1, 0, 0, 0);
    bone(b, base, 20, 1, 0.2f, 0, 0);   /* a step of motion */
    bone(a, base + 3, 0, 1, 0, 0, 0);
    bone(b, base + 3, 0, 1, 0, 0, 0);
    CHECK(!anim_bones_snap((const float (*)[4])a, (const float (*)[4])b, base, 2), "a 0.2-unit, 20-degree step is motion");
    bone(b, base + 3, 0, 1, 0, 0, 1.4f);
    CHECK(!anim_bones_snap((const float (*)[4])a, (const float (*)[4])b, base, 2), "a 1.4-unit limb swing (a fight's fastest) is motion");
    bone(b, base + 3, 80, 1, 0, 0, 0);
    CHECK(!anim_bones_snap((const float (*)[4])a, (const float (*)[4])b, base, 2), "an 80-degree turn is motion");
    bone(b, base + 3, 115, 1, 0, 0, 0);
    CHECK(anim_bones_snap((const float (*)[4])a, (const float (*)[4])b, base, 2), "a 115-degree turn is shown as is (the lerp would shrink it 16%%)");
    bone(b, base + 3, 0, 1, 0, 0, 2.5f);
    CHECK(anim_bones_snap((const float (*)[4])a, (const float (*)[4])b, base, 2), "a 2.5-unit jump is a snap");
    bone(b, base + 3, 40, 2, 0, 0, 0);
    CHECK(!anim_bones_snap((const float (*)[4])a, (const float (*)[4])b, base, 2), "a scaled bone's 40-degree turn is motion");
    bone(b, base + 3, 0, 1, 0, 0, 0);
    b[base + 4][1] = NAN;
    CHECK(anim_bones_snap((const float (*)[4])a, (const float (*)[4])b, base, 2), "a NaN is a snap");
}

static AnimPoses poses;
/* a palette of 2 bones at row 186: bone 0 at x = pos, turned deg about z; bone 1 fixed */
static void pal(float (*c)[4], float pos, float deg) {
    memset(c, 0, 192 * sizeof c[0]);
    for (int i = 0; i < 186; i++) c[i][0] = (float)i;   /* camera and other rows */
    bone(c, 186, deg, 1, pos, 0, 0);
    bone(c, 189, 0, 1, 0, 1, 0);
}

static void test_poses(void) {
    static float c[192][4], o[192][4];
    const AnimRows pm = {186, 2, 0, 0};
    AnimPoses *p = &poses;
    anim_poses_init(p);
    CHECK(anim_interp_options.on == 0, "--anim-interp is off by default");
    /* every frame a new pose: shown as is */
    int any = 0;
    for (int f = 0; f < 8; f++) {
        anim_poses_frame(p, 0);
        pal(c, 0.1f * f, 2.0f * f);
        any |= anim_poses_draw(p, 42, (const float (*)[4])c, &pm, o);
    }
    CHECK(!any, "a palette new every frame is never blended");
    anim_poses_free(p);
    /* a new pose every other frame: A A B B C C D D -> A A mid(A,B) B mid(B,C) C ... */
    anim_poses_init(p);
    float shown[12];
    int blended[12];
    for (int f = 0; f < 12; f++) {
        anim_poses_frame(p, 0);
        const int k = f / 2;
        pal(c, 0.1f * k, 4.0f * k);
        for (int i = 0; i < 192; i++) o[i][0] = o[i][1] = o[i][2] = o[i][3] = -9;
        blended[f] = anim_poses_draw(p, 42, (const float (*)[4])c, &pm, o);
        shown[f] = blended[f] ? o[186][3] : c[186][3];
        if (f == 4) {
            CHECK(blended[f] && fabsf(o[186][3] - 0.15f) < 1e-6f && fabsf(atan2f(o[187][0], o[186][0]) * 180 / 3.14159265f - 6) < 0.01f,
                  "frame 4 (pose C new) shows mid(B, C): x %.3f, %.2f deg", o[186][3], atan2f(o[187][0], o[186][0]) * 180 / 3.14159265f);
            CHECK(o[185][0] == -9 && o[189][3] == 0 && o[190][3] == 1, "only the palette rows are written");
        }
    }
    CHECK(!blended[0] && !blended[1] && blended[2] && !blended[3] && blended[4] && !blended[5] && blended[10] && !blended[11],
          "blended on the frames a pose is new, shown as is on the next");
    int even = 1;
    for (int f = 3; f < 11; f++) even &= fabsf((shown[f + 1] - shown[f]) - 0.05f) < 1e-5f;
    CHECK(even, "the shown position moves 0.05 every frame (%.3f %.3f %.3f %.3f)", shown[3], shown[4], shown[5], shown[6]);
    CHECK(p->draws == 12 && p->blended == 5, "counts: 12 draws, 5 blended (%llu, %llu)", (unsigned long long)p->draws, (unsigned long long)p->blended);
    /* a snap (teleport) is shown as is */
    anim_poses_frame(p, 0);
    pal(c, 50, 0);
    CHECK(!anim_poses_draw(p, 42, (const float (*)[4])c, &pm, o) && p->snaps == 1, "a teleport is not blended");
    anim_poses_frame(p, 0);
    CHECK(!anim_poses_draw(p, 42, (const float (*)[4])c, &pm, o), "held after the snap: as is");
    /* held long, then a move: a start from rest is shown as is */
    for (int f = 0; f < 8; f++) {
        anim_poses_frame(p, 0);
        anim_poses_draw(p, 42, (const float (*)[4])c, &pm, o);
    }
    anim_poses_frame(p, 0);
    pal(c, 50.1f, 0);
    CHECK(!anim_poses_draw(p, 42, (const float (*)[4])c, &pm, o), "a move after a long hold is shown as is");
    /* two draws of one identity keep their own poses (n-th occurrence) */
    anim_poses_free(p);
    anim_poses_init(p);
    int first = 0, second = 0;
    for (int f = 0; f < 6; f++) {
        anim_poses_frame(p, 0);
        pal(c, 0.1f * (f / 2), 0);
        first += anim_poses_draw(p, 7, (const float (*)[4])c, &pm, o);
        pal(c, 0.3f * f, 0);
        second += anim_poses_draw(p, 7, (const float (*)[4])c, &pm, o);
    }
    CHECK(first == 2 && second == 0, "the n-th draw of an identity has its own cadence (%d, %d)", first, second);
    anim_poses_free(p);
}

/* lerped rows (an object's world-to-clip matrix at rows 165-168) every other frame, and a cut */
static void test_poses_rows(void) {
    static float c[192][4], o[192][4];
    AnimPoses *p = &poses;
    anim_poses_init(p);
    const AnimRows om = {-1, 0, 165, 4};
    float shown[10];
    int blended = 0, other_ok = 1;
    for (int f = 0; f < 10; f++) {
        anim_poses_frame(p, 0);
        memset(c, 0, sizeof c);
        for (int r = 0; r < 4; r++) c[165 + r][r] = 1;
        c[165][3] = 0.5f * (f / 2);   /* x translation steps every other frame */
        c[10][0] = (float)f;          /* a row outside the motion rows changes every frame */
        for (int i = 0; i < 192; i++) o[i][0] = o[i][1] = o[i][2] = o[i][3] = -9;
        const int b = anim_poses_draw(p, 99, (const float (*)[4])c, &om, o);
        blended += b;
        shown[f] = b ? o[165][3] : c[165][3];
        if (b) other_ok &= o[10][0] == -9 && o[164][0] == -9 && o[169][0] == -9 && o[166][1] == 1;
    }
    int even = 1;
    for (int f = 3; f < 9; f++) even &= fabsf(shown[f + 1] - shown[f] - 0.25f) < 1e-6f;
    CHECK(blended == 4 && even, "lerped rows: 4 frames blended, the matrix moves 0.25 every frame (%d; %.2f %.2f %.2f %.2f)", blended, shown[3],
          shown[4], shown[5], shown[6]);
    CHECK(other_ok, "lerped rows: only the motion rows are written (other rows changing every frame do not count)");
    /* a change in a cut frame is shown as is */
    anim_poses_frame(p, 1);
    c[165][3] = 40;
    CHECK(!anim_poses_draw(p, 99, (const float (*)[4])c, &om, o), "a change in a cut frame is not blended");
    anim_poses_frame(p, 0);
    CHECK(!anim_poses_draw(p, 99, (const float (*)[4])c, &om, o), "held after the cut: as is");
    /* invalid rows */
    const AnimRows bad[] = {{-1, 0, 190, 4}, {180, 5, 0, 0}, {-1, 0, 0, 0}};
    int none = 1;
    for (unsigned k = 0; k < 3; k++) none &= !anim_poses_draw(p, 5, (const float (*)[4])c, &bad[k], o);
    CHECK(none, "rows outside the constants or none at all are refused");
    anim_poses_free(p);
}

int main(void) {
    test_rate_holds();
    test_rate_occurrences();
    test_rate_full();
    test_blend();
    test_snap();
    test_poses();
    test_poses_rows();
    if (failures) printf("%d FAILED\n", failures);
    else printf("ALL OK\n");
    return failures != 0;
}
