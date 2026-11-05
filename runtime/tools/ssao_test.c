/* Game-independent test of the --ssao options, the opaque-world split rule and the projection
 * scale (src/ssao.c). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/ssao.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)


/* --contact-shadows: options and the build from the title's camera (world up in view space). */
static const float cam_view[16] = {0.5153f, -0.1984f, 0.8337f, 0, 0.8570f, 0.1193f, -0.5013f, 0, 0, 0.9728f, 0.2315f, 0, -322.875f, -23.382f, 66.656f, 1};
static const float cam_proj[16] = {1.3971f, 0, 0, 0, 0, 1.9955f, 0, 0, 0, 0, -1.0007f, -1, 0, 0, -0.1001f, 0};
static void test_contact(void) {
    ContactConfig c = CONTACT_DEFAULTS;
    CHECK(!c.on && c.strength == CONTACT_STRENGTH_DEFAULT, "contact: off by default, strength %.2f", c.strength);
    CHECK(contact_arg(&c, "--ssao", NULL) == 0 && !c.on, "contact: other options are not consumed");
    CHECK(contact_arg(&c, "--contact-shadows", NULL) == 1 && c.on, "--contact-shadows turns them on");
    CHECK(contact_arg(&c, "--no-contact-shadows", NULL) == 1 && !c.on, "--no-contact-shadows turns them off");
    CHECK(contact_arg(&c, "--contact-shadows-strength", "1.5") == 2 && c.strength == 1.5f && !c.on, "--contact-shadows-strength sets it only");
    CHECK(contact_arg(&c, "--contact-shadows-strength", "0.1") == 2 && contact_arg(&c, "--contact-shadows-strength", "2") == 2 && c.strength == 2.0f,
          "contact strength range 0.1-2");
    const char *bad[] = {NULL, "", "0", "0.05", "2.5", "-1", "abc", "1x", "nan"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        ContactConfig d = CONTACT_DEFAULTS;
        CHECK(contact_arg(&d, "--contact-shadows-strength", bad[i]) == -1 && d.strength == CONTACT_STRENGTH_DEFAULT,
              "--contact-shadows-strength '%s' is rejected", bad[i] ? bad[i] : "(missing)");
    }
    c = (ContactConfig)CONTACT_DEFAULTS;
    c.on = 1;
    const float ab[2] = {1.0007f, 0.1001f};
    RContact p, q;
    memset(&p, 0, sizeof p);
    CHECK(contact_build(&c, cam_view, cam_proj, ab, &p) == 1, "contact: built for the title's camera");
    CHECK(p.depth_a == ab[0] && p.depth_b == ab[1] && fabsf(p.px - 1.3971f) < 1e-6f && fabsf(p.py - 1.9955f) < 1e-6f, "contact: projection");
    CHECK(fabsf(p.up_view[0]) < 1e-4f && fabsf(p.up_view[1] - 0.9728f) < 1e-3f && fabsf(p.up_view[2] - 0.2315f) < 1e-3f,
          "contact: world up in view space (%.4f %.4f %.4f)", p.up_view[0], p.up_view[1], p.up_view[2]);
    CHECK(p.strength == CONTACT_STRENGTH_DEFAULT && p.radius == CONTACT_RADIUS, "contact: strength %.2f radius %.2f", p.strength, p.radius);
    CHECK(contact_build(&c, cam_view, cam_proj, NULL, &p) == 1 && p.depth_a == SSAO_DEPTH_A_DEFAULT && p.depth_b == SSAO_DEPTH_B_DEFAULT,
          "contact: the default depth projection without a fit");
    memset(&q, 0x5a, sizeof q);
    RContact z = q;
    c.on = 0;
    CHECK(contact_build(&c, cam_view, cam_proj, ab, &q) == 0 && !memcmp(&q, &z, sizeof q), "contact off: nothing built");
    c.on = 1;
    float ortho[16];
    memcpy(ortho, cam_proj, sizeof ortho);
    ortho[11] = 0;
    ortho[15] = 1;
    CHECK(contact_build(&c, cam_view, ortho, ab, &q) == 0 && !memcmp(&q, &z, sizeof q), "contact: no perspective camera, nothing built");
    float skew[16];
    memcpy(skew, cam_view, sizeof skew);
    skew[0] = 2;
    CHECK(contact_build(&c, skew, cam_proj, ab, &q) == 0 && !memcmp(&q, &z, sizeof q), "contact: a view that is not a rotation, nothing built");
}

int main(void) {
    SsaoConfig c = SSAO_DEFAULTS;
    CHECK(!c.on && c.strength == SSAO_STRENGTH_DEFAULT && c.radius == SSAO_RADIUS_DEFAULT, "defaults: off, %.2f, %.2f", c.strength,
          c.radius);
    CHECK(ssao_arg(&c, "--bloom", NULL) == 0 && !c.on, "other options are not consumed");
    CHECK(ssao_arg(&c, "--ssao", NULL) == 1 && c.on, "--ssao turns it on");
    CHECK(ssao_arg(&c, "--no-ssao", NULL) == 1 && !c.on, "--no-ssao turns it off (the last one wins)");
    CHECK(ssao_arg(&c, "--ssao-strength", "1.25") == 2 && c.strength == 1.25f && !c.on, "--ssao-strength sets the strength only");
    CHECK(ssao_arg(&c, "--ssao-radius", "2.5") == 2 && c.radius == 2.5f, "--ssao-radius sets the radius");
    CHECK(ssao_arg(&c, "--ssao-strength", "0.1") == 2 && ssao_arg(&c, "--ssao-strength", "2") == 2 && c.strength == 2.0f,
          "strength range 0.1-2");
    CHECK(ssao_arg(&c, "--ssao-radius", "0.1") == 2 && ssao_arg(&c, "--ssao-radius", "8") == 2 && c.radius == 8.0f, "radius range 0.1-8");
    const char *bad_strength[] = {"0", "-1", "2.5", "abc", "1x", "", "nan", NULL};
    for (int i = 0; i < 8; i++) {
        SsaoConfig d = SSAO_DEFAULTS;
        CHECK(ssao_arg(&d, "--ssao-strength", bad_strength[i]) == -1 && d.strength == SSAO_STRENGTH_DEFAULT,
              "--ssao-strength '%s' is rejected", bad_strength[i] ? bad_strength[i] : "(missing)");
    }
    const char *bad_radius[] = {"0", "0.05", "9", "inf", "r", NULL};
    for (int i = 0; i < 6; i++) {
        SsaoConfig d = SSAO_DEFAULTS;
        CHECK(ssao_arg(&d, "--ssao-radius", bad_radius[i]) == -1 && d.radius == SSAO_RADIUS_DEFAULT, "--ssao-radius '%s' is rejected",
              bad_radius[i] ? bad_radius[i] : "(missing)");
    }

    /* Opaque world draws: back buffer, depth test and depth write. */
    CHECK(ssao_opaque_draw(0, SSAO_DRAW_DEPTH_TEST, 1), "a depth-tested, depth-writing back-buffer draw is opaque world");
    CHECK(!ssao_opaque_draw(0, SSAO_DRAW_DEPTH_TEST, 0), "a depth-write-off draw (particles, smoke, halos) is not");
    CHECK(!ssao_opaque_draw(0, 0, 1), "a draw without depth test (HUD, text, rain layers) is not");
    CHECK(!ssao_opaque_draw(1, SSAO_DRAW_DEPTH_TEST, 1), "an offscreen draw (shadow map) is not");

    /* The split: after the last opaque draw that follows the last depth clear before it. */
    enum { O = SSAO_OP_OTHER, W = SSAO_OP_OPAQUE, C = SSAO_OP_CLEAR };
    {   /* the measured frame shape: clear, sky, depth clear, offscreen, world, effects, HUD */
        const uint8_t k[] = {C, O, O, W, C, O, O, O, W, W, W, W, O, O, O, O};
        CHECK(ssao_split(k, sizeof k) == 11, "split after the world's last opaque draw (%u)", ssao_split(k, sizeof k));
    }
    {   /* effects between opaque draws do not end the world early */
        const uint8_t k[] = {C, W, O, W, O, O};
        CHECK(ssao_split(k, sizeof k) == 3, "split after the last opaque draw, not the first effect (%u)", ssao_split(k, sizeof k));
    }
    {   /* a frame whose last opaque draw comes before its last depth clear: no split */
        const uint8_t k[] = {C, W, W, C, O, O};
        CHECK(ssao_split(k, sizeof k) == sizeof k, "no split when the depth was cleared after the 3D (%u)", ssao_split(k, sizeof k));
    }
    {
        const uint8_t k[] = {C, O, O, O};
        CHECK(ssao_split(k, sizeof k) == sizeof k, "no split without an opaque draw (menus, movies)");
        CHECK(ssao_split(k, 0) == 0, "empty frame: n");
    }
    {
        const uint8_t k[] = {W, O};
        CHECK(ssao_split(k, sizeof k) == 0, "no clear: the last opaque draw (%u)", ssao_split(k, sizeof k));
    }

    /* Projection scale from the world-view-projection columns. */
    {
        const float px = 1.3971f, py = 1.9955f, A = 1.0013f, B = 0.1001f;
        const float a = 0.6f, ca = cosf(a), sa = sinf(a), sc = 2.5f;   /* view rotation about y, uniform world scale */
        /* row-vector convention: clip = v * M; column j of M holds clip component j */
        const float rx[3] = {ca * sc, 0, -sa * sc}, ry[3] = {0, sc, 0}, rz[3] = {sa * sc, 0, ca * sc};
        float c0[4], c1[4], c3[4], out[2] = {-1, -1};
        for (int i = 0; i < 3; i++) {
            const float r[3] = {rx[i], ry[i], rz[i]};   /* row i of the world-view rotation (x, y, z columns) */
            c0[i] = px * r[0];
            c1[i] = py * r[1];
            c3[i] = -r[2];   /* w = -z_view (the title's right-handed projection: m[11] = -1) */
        }
        c0[3] = 0.3f;
        c1[3] = -0.2f;
        c3[3] = 4.0f;
        (void)A; (void)B;
        CHECK(ssao_proj_scale(c0, c1, c3, out) == 0 && fabsf(out[0] - px) < 1e-4f && fabsf(out[1] - py) < 1e-4f, "scale %.4f %.4f", out[0],
              out[1]);
        const float zero[4] = {0, 0, 0, 1};
        float keep[2] = {7, 7};
        CHECK(ssao_proj_scale(c0, c1, zero, keep) == -1 && keep[0] == 7, "a degenerate w column is rejected");
        float big[4] = {100 * c0[0], 100 * c0[1], 100 * c0[2], 0};
        CHECK(ssao_proj_scale(big, c1, c3, keep) == -1 && keep[0] == 7, "a scale out of 0.2-20 is rejected");
    }

    {   /* the working size: half the output size, whatever the supersampling */
        const struct { int iw, ih, ss, scale, w, h, offset; } ws[] = {
            {1920, 1080, 1, 2, 960, 540, 0},  {3840, 2160, 2, 4, 960, 540, 1}, {7680, 4320, 4, 8, 960, 540, 3},
            {1441, 1081, 1, 2, 721, 541, 0},  {2882, 2162, 2, 4, 721, 541, 1}, {640, 480, 0, 2, 320, 240, 0},
            {3, 3, 2, 4, 1, 1, 1},            {0, 0, 1, 2, 1, 1, 0},
        };
        for (unsigned i = 0; i < sizeof ws / sizeof ws[0]; i++) {
            const SsaoWorkSize r = ssao_work_size(ws[i].iw, ws[i].ih, ws[i].ss);
            CHECK(r.scale == ws[i].scale && r.w == ws[i].w && r.h == ws[i].h && r.offset == ws[i].offset,
                  "work size %dx%d ss %d: scale %d, %dx%d, offset %d (want %d, %dx%d, %d)", ws[i].iw, ws[i].ih, ws[i].ss, r.scale, r.w, r.h,
                  r.offset, ws[i].scale, ws[i].w, ws[i].h, ws[i].offset);
        }
    }
    test_contact();
    if (!fails) printf("ALL OK\n");
    return fails ? 1 : 0;
}
