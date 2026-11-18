/* Game-independent test of --hair's options, hair mask and cache (src/hair.c), on a synthetic
 * head: a UV sphere (u = longitude / 2 pi, v = latitude from the top / pi) whose front (+y) is
 * weighted to the jaw (a face joint), its bottom cap to the spine (the body) and the rest to the head. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/hair.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define NLON 32
#define NLAT 16
#define NV ((NLON + 1) * (NLAT + 1))
#define NI (NLON * NLAT * 6)
#define MW 64
#define MH 64
static float pos[NV][3], nrm[NV][3], uv[NV][2], wt[NV][3];
static int slot[NV][3];
static uint32_t idx[NI];
static const double PI = 3.14159265358979323846;

static void head(void) {
    for (int j = 0; j <= NLAT; j++)
        for (int i = 0; i <= NLON; i++) {
            const int k = j * (NLON + 1) + i;
            const double th = PI * j / NLAT, ph = 2 * PI * i / NLON;
            const float x = (float)(sin(th) * cos(ph)), y = (float)(sin(th) * sin(ph)), z = (float)cos(th);
            nrm[k][0] = x, nrm[k][1] = y, nrm[k][2] = z;
            pos[k][0] = 0.1f * x, pos[k][1] = 0.1f * y, pos[k][2] = 1.6f + 0.1f * z;
            uv[k][0] = (float)i / NLON, uv[k][1] = (float)j / NLAT;
            int s = 4;                                                         /* bip_sw_head */
            if (y > 0.55f && th > PI * 0.3 && th < PI * 0.75) s = 6;           /* bip_jaw: the face */
            if (th > PI * 0.85) s = 1;                                         /* bip_sw_spine1: the body */
            slot[k][0] = s, slot[k][1] = -1, slot[k][2] = -1;
            wt[k][0] = 1, wt[k][1] = 0, wt[k][2] = 0;
        }
    int n = 0;
    for (int j = 0; j < NLAT; j++)
        for (int i = 0; i < NLON; i++) {
            const uint32_t a = j * (NLON + 1) + i, b = a + 1, c = a + NLON + 1, d = c + 1;
            idx[n++] = a, idx[n++] = c, idx[n++] = b;
            idx[n++] = b, idx[n++] = c, idx[n++] = d;
        }
}
static HairMesh mesh(void) {
    HairMesh m = {NV, (const float (*)[3])pos, (const float (*)[3])nrm, (const float (*)[2])uv, (const int (*)[3])slot,
                  (const float (*)[3])wt, idx, NI};
    return m;
}
static uint32_t at(const uint32_t *m, float u, float v) { return m[(int)(v * MH) * MW + (int)(u * MW)]; }
static int R(uint32_t t) { return t >> 16 & 255; }
static int G(uint32_t t) { return t >> 8 & 255; }
static int B(uint32_t t) { return t & 255; }
static int A(uint32_t t) { return t >> 24 & 255; }

static void test_options(void) {
    HairConfig c = HAIR_DEFAULTS;
    CHECK(!c.on && c.strength == 1.0f, "off by default, strength 1");
    CHECK(hair_arg(&c, "--char-lighting", NULL) == 0 && !c.on, "other options are not consumed");
    CHECK(hair_arg(&c, "--hair", NULL) == 1 && c.on, "--hair turns it on");
    CHECK(hair_arg(&c, "--no-hair", NULL) == 1 && !c.on, "--no-hair turns it off");
    CHECK(hair_arg(&c, "--hair-strength", "1.5") == 2 && c.strength == 1.5f, "--hair-strength 1.5");
    CHECK(hair_arg(&c, "--hair-strength", "9") == -1 && c.strength == 1.5f, "--hair-strength 9 is refused");
    CHECK(hair_arg(&c, "--hair-strength", NULL) == -1, "--hair-strength without a value is refused");
    CHECK(hair_arg(&c, "--hair-strength", "x") == -1, "--hair-strength x is refused");
}

static void test_geometry(void) {
    static uint32_t m[MW * MH];
    memset(m, 0, sizeof m);
    HairMesh hm = mesh();
    float ref[3] = {0, 0, 0};
    int have = 0;
    const uint32_t n = hair_mask_add(m, MW, MH, &hm, NULL, ref, &have);
    CHECK(n > MW * MH / 4, "hair texels written (%u)", n);
    /* u 0.75 = longitude 270 degrees: the back of the head (-y) */
    const uint32_t back = at(m, 0.76f, 0.5f), top = at(m, 0.76f, 0.12f), front = at(m, 0.26f, 0.5f), body = at(m, 0.76f, 0.97f);
    CHECK(R(back) > 200, "the back of the head is hair (R %d)", R(back));
    CHECK(R(top) > 200, "the top of the head is hair (R %d)", R(top));
    CHECK(R(front) == 0, "the face is not hair (R %d)", R(front));
    CHECK(R(body) == 0, "the body is not hair (R %d)", R(body));
    CHECK(R(at(m, 0.26f, 0.06f)) > 200, "the top of the head in front (facing up) is hair (R %d)", R(at(m, 0.26f, 0.06f)));
    CHECK(G(top) > G(back) + 40, "height: the crown is higher than the back (%d over %d)", G(top), G(back));
    CHECK(G(at(m, 0.76f, 0.75f)) < 60, "height: the lowest hair is near 0 (%d)", G(at(m, 0.76f, 0.75f)));
    /* on the back the strands run down the head: +v (v grows down the image, downwards on the sphere) */
    CHECK(abs(B(back) - 128) < 16 && A(back) > 240, "the strand direction is down the head (du %d, dv %d)", B(back) - 128, A(back) - 128);
    const uint32_t side = at(m, 0.51f, 0.5f);   /* longitude 180: the left side, still down */
    CHECK(abs(B(side) - 128) < 16 && A(side) > 240, "and on the side too (du %d, dv %d)", B(side) - 128, A(side) - 128);
}

static uint32_t rgb(float r, float g, float b) {
    return 0xFF000000u | (uint32_t)(r * 255 + 0.5f) << 16 | (uint32_t)(g * 255 + 0.5f) << 8 | (uint32_t)(b * 255 + 0.5f);
}
static void test_colours(void) {
    static uint32_t m[MW * MH], alb[MW * MH];
    const uint32_t hair = rgb(0.18f, 0.12f, 0.09f), skin = rgb(0.75f, 0.52f, 0.40f), cap = rgb(0.10f, 0.25f, 0.85f);
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW; x++) {
            const float u = (x + 0.5f) / MW;
            uint32_t c = hair;
            if (u > 0.15f && u < 0.35f) c = skin;     /* the face (and its sides) */
            if (u > 0.55f && u < 0.65f) c = skin;     /* a bald patch on the back left */
            if (u > 0.85f && u < 0.92f) c = cap;      /* a blue cap */
            alb[y * MW + x] = c;
        }
    memset(m, 0, sizeof m);
    HairMesh hm = mesh();
    float ref[3] = {0, 0, 0};
    int have = 0;
    hair_mask_add(m, MW, MH, &hm, alb, ref, &have);
    CHECK(have && fabsf(ref[0] - 0.75f) < 0.02f && fabsf(ref[2] - 0.40f) < 0.02f, "the face's skin colour (%.2f %.2f %.2f)", ref[0], ref[1], ref[2]);
    CHECK(R(at(m, 0.76f, 0.5f)) > 200, "dark hair stays hair (R %d)", R(at(m, 0.76f, 0.5f)));
    CHECK(R(at(m, 0.60f, 0.5f)) < 30, "skin-coloured texels in the hair region are left out (R %d)", R(at(m, 0.60f, 0.5f)));
    CHECK(R(at(m, 0.885f, 0.5f)) < 60, "a bright blue cap is left out (R %d)", R(at(m, 0.885f, 0.5f)));
    /* a second call without a face keeps the reference */
    static uint32_t m2[MW * MH];
    memset(m2, 0, sizeof m2);
    for (int k = 0; k < NV; k++)
        if (slot[k][0] == 6) slot[k][0] = 4;
    ref[0] = 0.75f, ref[1] = 0.52f, ref[2] = 0.40f;
    hm = mesh();
    hair_mask_add(m2, MW, MH, &hm, alb, ref, &have);
    CHECK(have && fabsf(ref[0] - 0.75f) < 0.02f, "a mesh without a face keeps the previous skin colour (%.2f)", ref[0]);
    CHECK(R(at(m2, 0.60f, 0.5f)) < 30, "and still leaves the bald patch out (R %d)", R(at(m2, 0.60f, 0.5f)));
    head();
}

static void test_downsample(void) {
    const uint32_t src[4] = {0xFF000000u, 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u};
    uint32_t d = 0;
    hair_downsample(src, 2, 2, &d, 1, 1);
    CHECK(d == 0xFF404040u, "2x2 -> 1 box filter (%08x)", d);
    uint32_t mw, mh;
    hair_mask_size(512, 256, &mw, &mh);
    CHECK(mw == HAIR_MASK_MAX && mh == HAIR_MASK_MAX / 2, "mask size of 512x256: %ux%u", mw, mh);
    hair_mask_size(64, 32, &mw, &mh);
    CHECK(mw == 64 && mh == 32, "mask size of 64x32: %ux%u", mw, mh);
}

static uint32_t version_of(void *ctx, uint32_t version, uint32_t mw, uint32_t mh, const uint32_t *mask) {
    (void)ctx;
    return mask && mw == MW && mh == MH ? version : 0;
}
static void test_cache(void) {
    hair_cache_clear();
    HairMesh hm = mesh();
    CHECK(hair_cache_wants(0x1234, 7), "a new skin is wanted");
    CHECK(hair_cache_with(0x1234, version_of, NULL) == 0, "no mask before its mesh");
    CHECK(hair_cache_add(0x1234, 7, &hm, NULL, MW, MH) > 0, "added");
    CHECK(!hair_cache_wants(0x1234, 7), "the same mesh is not wanted again");
    CHECK(hair_cache_wants(0x1234, 8), "another mesh of the skin is");
    CHECK(hair_cache_with(0x1234, version_of, NULL) == 1, "version 1");
    hair_cache_add(0x1234, 8, &hm, NULL, MW, MH);
    CHECK(hair_cache_with(0x1234, version_of, NULL) == 1, "a second mesh with the same hair adds nothing: still version 1");
    for (int k = 0; k < NV; k++)
        if (uv[k][0] > 0.5f) slot[k][0] = 1;   /* a draw holding the front half of the head */
    hm = mesh();
    hair_cache_add(0x55, 1, &hm, NULL, MW, MH);
    head();
    hm = mesh();
    hair_cache_add(0x55, 2, &hm, NULL, MW, MH);
    CHECK(hair_cache_with(0x55, version_of, NULL) == 2, "a second draw with the rest of the hair: version 2");
    hair_cache_add(0x99, 1, &hm, NULL, 256, 256);
    CHECK(hair_cache_with(0x99, version_of, NULL) == 0, "a 256x256 skin has a 128x128 mask (not 64)");
    uint32_t skins, with;
    hair_cache_stats(&skins, &with);
    CHECK(skins == 3 && with == 3, "stats: %u skins, %u with hair", skins, with);
    for (int k = 0; k < NV; k++) slot[k][0] = 1;   /* a body without a head */
    hm = mesh();
    CHECK(hair_cache_add(0x77, 1, &hm, NULL, MW, MH) == 0, "no head: no hair");
    CHECK(hair_cache_with(0x77, version_of, NULL) == 0, "and no mask to bind");
    CHECK(!hair_cache_wants(0x77, 1), "but it is not tried again");
    head();
    hair_cache_clear();
    CHECK(hair_cache_wants(0x1234, 7), "cleared");
}


/* --skin: the skin mask (skin_geom_add, skin_mask_build) on the same head. Colours: skin
 * everywhere but dark brown hair on the back, a blue-green tattoo and a dark leather vest on the
 * body (the bottom cap). */
static void test_skin_options(void) {
    SkinConfig c = SKIN_DEFAULTS;
    CHECK(!c.on && c.strength == 1.0f, "skin: off by default, strength 1");
    CHECK(skin_arg(&c, "--hair", NULL) == 0 && !c.on, "skin: other options are not consumed");
    CHECK(skin_arg(&c, "--skin", NULL) == 1 && c.on, "--skin turns it on");
    CHECK(skin_arg(&c, "--no-skin", NULL) == 1 && !c.on, "--no-skin turns it off");
    CHECK(skin_arg(&c, "--skin-strength", "0.5") == 2 && c.strength == 0.5f, "--skin-strength 0.5");
    CHECK(skin_arg(&c, "--skin-strength", "4") == -1 && c.strength == 0.5f, "--skin-strength 4 is refused");
    CHECK(skin_arg(&c, "--skin-strength", NULL) == -1, "--skin-strength without a value is refused");
    CHECK(skin_arg(&c, "--skin-strength", "1x") == -1, "--skin-strength 1x is refused");
}
static void skin_albedo(uint32_t *alb) {
    const uint32_t hair = rgb(0.18f, 0.12f, 0.09f), skin = rgb(0.75f, 0.52f, 0.40f), ink = rgb(0.15f, 0.30f, 0.28f), vest = rgb(0.25f, 0.15f, 0.10f);
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW; x++) {
            const float u = (x + 0.5f) / MW, v = (y + 0.5f) / MH;
            uint32_t c = skin;
            if (u > 0.62f && u < 0.88f && v < 0.8f) c = hair;
            if (v > 0.88f && u > 0.30f && u < 0.45f) c = ink;
            if (v > 0.88f && u > 0.50f && u < 0.62f) c = vest;
            alb[y * MW + x] = c;
        }
}
static void test_skin_mask(void) {
    static uint32_t hm_[MW * MH], alb[MW * MH], sk[MW * MH];
    static uint8_t region[MW * MH], thin[MW * MH];
    skin_albedo(alb);
    memset(hm_, 0, sizeof hm_);
    memset(region, 0, sizeof region);
    memset(thin, 0, sizeof thin);
    HairMesh hm = mesh();
    float ref[3] = {0, 0, 0};
    int have = 0;
    hair_mask_add(hm_, MW, MH, &hm, alb, ref, &have);
    skin_geom_add(region, thin, MW, MH, &hm);
    CHECK(region[(int)(0.5f * MH) * MW + (int)(0.26f * MW)] == SKIN_REGION_FACE, "region: the face (%d)", region[(int)(0.5f * MH) * MW + (int)(0.26f * MW)]);
    CHECK(region[(int)(0.5f * MH) * MW + (int)(0.76f * MW)] == SKIN_REGION_HEAD, "region: the back of the head");
    CHECK(region[(int)(0.95f * MH) * MW + (int)(0.1f * MW)] == SKIN_REGION_BODY, "region: the body");
    memset(sk, 0, sizeof sk);
    CHECK(skin_mask_build(sk, MW, MH, region, thin, alb, NULL, hm_) == 0 && sk[MW * MH / 2] == 0, "no face colour: no skin");
    const uint32_t n = skin_mask_build(sk, MW, MH, region, thin, alb, ref, hm_);
    CHECK(n > MW * MH / 3, "skin texels (%u)", n);
    const uint32_t face = at(sk, 0.26f, 0.5f), body = at(sk, 0.1f, 0.95f), ear = at(sk, 0.02f, 0.5f);
    CHECK(R(face) > 200, "the face is skin (R %d)", R(face));
    CHECK(R(body) > 200, "the bare body is skin (R %d)", R(body));
    CHECK(R(at(sk, 0.76f, 0.5f)) == 0, "hair is not skin (R %d)", R(at(sk, 0.76f, 0.5f)));
    CHECK(R(at(sk, 0.375f, 0.95f)) < 30, "a tattoo is not skin (R %d)", R(at(sk, 0.375f, 0.95f)));
    CHECK(R(at(sk, 0.56f, 0.95f)) < 30, "a dark leather vest is not skin (R %d)", R(at(sk, 0.56f, 0.95f)));
    CHECK(B(face) > 230 && B(body) < 130, "sheen: the face's is strong (%d), the body's weaker (%d)", B(face), B(body));
    CHECK(G(ear) > G(face) + 60, "thin: the side of the head (an ear) is thinner than the front (%d over %d)", G(ear), G(face));
    CHECK(A(face) == 255, "alpha 255 (%d)", A(face));
}
static void test_skin_legs_hands(void) {
    /* the body cap weighted to a calf (trousers: never skin); then to a hand whose colour is a little
     * darker than the face's: the body's reference, so a texel of that colour on the torso is skin */
    static uint32_t hm_[MW * MH], alb[MW * MH], sk[MW * MH];
    static uint8_t region[MW * MH], thin[MW * MH];
    skin_albedo(alb);
    for (int k = 0; k < NV; k++)
        if (slot[k][0] == 1) slot[k][0] = 27;   /* bip_sw_l_calf */
    HairMesh hm = mesh();
    float ref[3] = {0, 0, 0};
    int have = 0;
    memset(hm_, 0, sizeof hm_), memset(region, 0, sizeof region), memset(thin, 0, sizeof thin);
    hair_mask_add(hm_, MW, MH, &hm, alb, ref, &have);
    skin_geom_add(region, thin, MW, MH, &hm);
    skin_mask_build(sk, MW, MH, region, thin, alb, ref, hm_);
    CHECK(R(at(sk, 0.1f, 0.95f)) == 0 && region[(int)(0.95f * MH) * MW + (int)(0.1f * MW)] == SKIN_REGION_NONE, "legs are never skin (R %d)",
          R(at(sk, 0.1f, 0.95f)));
    head();
    /* hands: the bottom cap's left half (u < 0.25); torso: the rest of the cap, coloured like the hands */
    const uint32_t hand = rgb(0.70f, 0.45f, 0.30f);   /* a warmer hue than the face: off the face's tight body gate */
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW; x++)
            if ((y + 0.5f) / MH > 0.88f && ((x + 0.5f) / MW < 0.25f || ((x + 0.5f) / MW > 0.7f && (x + 0.5f) / MW < 0.9f))) alb[y * MW + x] = hand;
    for (int k = 0; k < NV; k++)
        if (slot[k][0] == 1 && uv[k][0] < 0.27f) slot[k][0] = 17;   /* bip_sw_l_hand */
    hm = mesh();
    memset(hm_, 0, sizeof hm_), memset(region, 0, sizeof region), memset(thin, 0, sizeof thin);
    have = 0;
    hair_mask_add(hm_, MW, MH, &hm, alb, ref, &have);
    skin_geom_add(region, thin, MW, MH, &hm);
    skin_mask_build(sk, MW, MH, region, thin, alb, ref, hm_);
    CHECK(region[(int)(0.95f * MH) * MW + (int)(0.1f * MW)] == SKIN_REGION_HAND, "region: a hand");
    CHECK(R(at(sk, 0.1f, 0.95f)) > 200, "the hand is skin (R %d)", R(at(sk, 0.1f, 0.95f)));
    CHECK(R(at(sk, 0.8f, 0.95f)) > 200, "the torso coloured like the hands is skin (R %d)", R(at(sk, 0.8f, 0.95f)));
    head();
}
static void test_skin_cache(void) {
    static uint32_t alb[MW * MH];
    skin_albedo(alb);
    hair_cache_clear();
    hair_cache_set_skin(1);
    for (int k = 0; k < NV; k++)
        if (slot[k][0] != 1) slot[k][0] = 1;   /* first a draw of the body alone (no face yet) */
    HairMesh hm = mesh();
    hair_cache_add(0x88, 1, &hm, alb, MW, MH);
    CHECK(hair_cache_skin_with(0x88, version_of, NULL) == 0 && hair_cache_skin_texels(0x88) == 0, "skin: no face colour yet, no mask");
    head();
    hm = mesh();
    hair_cache_add(0x88, 2, &hm, alb, MW, MH);
    const uint32_t v = hair_cache_skin_with(0x88, version_of, NULL);
    CHECK(v >= 1, "skin: the head's draw gives the mask (version %u)", v);
    CHECK(hair_cache_skin_texels(0x88) > MW * MH / 3, "skin: texels (%u)", hair_cache_skin_texels(0x88));
    hair_cache_set_skin(0);
    hair_cache_add(0x89, 1, &hm, alb, MW, MH);
    CHECK(hair_cache_skin_with(0x89, version_of, NULL) == 0, "skin off: no skin mask");
    CHECK(hair_cache_with(0x89, version_of, NULL) == 1, "skin off: the hair mask as before");
    hair_cache_clear();
}
static uint32_t skin_body_at(void *ctx, uint32_t version, uint32_t mw, uint32_t mh, const uint32_t *mask) {
    (void)version;
    *(uint32_t *)ctx = mw == MW && mh == MH ? at(mask, 0.1f, 0.95f) : 0;
    return 1;
}
static void test_skin_order(void) {
    /* the body's texels get skin once the face's colour is known, whatever the draws' order */
    static uint32_t alb[MW * MH];
    skin_albedo(alb);
    hair_cache_clear();
    hair_cache_set_skin(1);
    for (int k = 0; k < NV; k++)
        if (uv[k][1] < 0.8f) slot[k][0] = -1, wt[k][0] = 0;   /* the body only: the head's vertices unweighted */
    HairMesh hm = mesh();
    hair_cache_add(0x90, 1, &hm, alb, MW, MH);
    head();
    for (int k = 0; k < NV; k++)
        if (uv[k][1] > 0.9f) slot[k][0] = -1, wt[k][0] = 0;   /* then the head without the body */
    hm = mesh();
    hair_cache_add(0x90, 2, &hm, alb, MW, MH);
    head();
    uint32_t body = 0;
    hair_cache_skin_with(0x90, skin_body_at, &body);
    CHECK(R(body) > 200, "the body drawn before the head is skin (R %d)", R(body));
    hair_cache_set_skin(0);
    hair_cache_clear();
}

/* --eyes, --cloth: options, the eyes mask (an eye-lid weighted patch of the face with a white eye
 * and a dark iris in skin-coloured lids) and the cloth mask (on the body cap: denim, cotton, a
 * maroon leather vest, bare skin; a blue cap on the head). */
static void test_mat_options(void) {
    EyesConfig e = EYES_DEFAULTS;
    ClothConfig c = CLOTH_DEFAULTS;
    CHECK(!e.on && e.strength == 1.0f && !c.on && c.strength == 1.0f, "eyes, cloth: off by default, strength 1");
    CHECK(eyes_arg(&e, "--cloth", NULL) == 0 && cloth_arg(&c, "--eyes", NULL) == 0 && !e.on && !c.on, "eyes, cloth: other options are not consumed");
    CHECK(eyes_arg(&e, "--eyes", NULL) == 1 && e.on, "--eyes turns it on");
    CHECK(eyes_arg(&e, "--no-eyes", NULL) == 1 && !e.on, "--no-eyes turns it off");
    CHECK(eyes_arg(&e, "--eyes-strength", "2") == 2 && e.strength == 2.0f, "--eyes-strength 2");
    CHECK(eyes_arg(&e, "--eyes-strength", "0.05") == -1 && e.strength == 2.0f, "--eyes-strength 0.05 is refused");
    CHECK(eyes_arg(&e, "--eyes-strength", NULL) == -1, "--eyes-strength without a value is refused");
    CHECK(cloth_arg(&c, "--cloth", NULL) == 1 && c.on, "--cloth turns it on");
    CHECK(cloth_arg(&c, "--no-cloth", NULL) == 1 && !c.on, "--no-cloth turns it off");
    CHECK(cloth_arg(&c, "--cloth-strength", "0.5") == 2 && c.strength == 0.5f, "--cloth-strength 0.5");
    CHECK(cloth_arg(&c, "--cloth-strength", "3.5") == -1 && c.strength == 0.5f, "--cloth-strength 3.5 is refused");
    CHECK(cloth_arg(&c, "--cloth-strength", "x") == -1, "--cloth-strength x is refused");
}
/* the eye: vertices of the face around (u 0.25, v 0.44) weighted to the eye lid */
static void eye_lid(void) {
    for (int j = 6; j <= 8; j++)
        for (int i = 7; i <= 9; i++) slot[j * (NLON + 1) + i][0] = HAIR_SLOT_EYE_LID;
}
static void mat_albedo(uint32_t *alb) {
    const uint32_t skin = rgb(0.75f, 0.52f, 0.40f), white = rgb(0.85f, 0.84f, 0.80f), iris = rgb(0.12f, 0.08f, 0.06f), denim = rgb(0.20f, 0.28f, 0.45f),
                   cotton = rgb(0.50f, 0.50f, 0.48f), vest = rgb(0.35f, 0.10f, 0.08f), cap = rgb(0.10f, 0.25f, 0.85f);
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW; x++) {
            const float u = (x + 0.5f) / MW, v = (y + 0.5f) / MH;
            uint32_t c = skin;
            if (u > 0.21f && u < 0.29f && v > 0.39f && v < 0.48f) c = u > 0.235f && u < 0.265f ? iris : white;
            if (v > 0.88f) {
                if (u > 0.05f && u < 0.20f) c = denim;
                else if (u > 0.30f && u < 0.45f) c = cotton;
                else if (u > 0.50f && u < 0.62f) c = vest;
            }
            if (v > 0.08f && v < 0.22f && u > 0.60f && u < 0.90f) c = cap;
            alb[y * MW + x] = c;
        }
}
static void mat_masks(uint32_t *ey, uint32_t *cl, uint32_t *eyes_n, uint32_t *cloth_n) {
    static uint32_t hm_[MW * MH], alb[MW * MH], sk[MW * MH];
    static uint8_t region[MW * MH], thin[MW * MH], creg[MW * MH], eye[MW * MH];
    mat_albedo(alb);
    memset(hm_, 0, sizeof hm_), memset(region, 0, sizeof region), memset(thin, 0, sizeof thin), memset(creg, 0, sizeof creg), memset(eye, 0, sizeof eye);
    HairMesh hm = mesh();
    float ref[3] = {0, 0, 0};
    int have = 0;
    hair_mask_add(hm_, MW, MH, &hm, alb, ref, &have);
    skin_geom_add(region, thin, MW, MH, &hm);
    skin_mask_build(sk, MW, MH, region, thin, alb, ref, hm_);
    mat_geom_add(creg, eye, MW, MH, &hm);
    *eyes_n = eyes_mask_build(ey, MW, MH, eye, alb, have ? ref : NULL);
    *cloth_n = cloth_mask_build(cl, MW, MH, creg, alb, sk, hm_);
}
static void test_eyes_mask(void) {
    static uint32_t ey[MW * MH], cl[MW * MH];
    eye_lid();
    uint32_t ne, nc;
    mat_masks(ey, cl, &ne, &nc);
    CHECK(ne >= 8 && ne < 60, "eye texels (%u)", ne);
    const uint32_t c = at(ey, 0.25f, 0.44f), w = at(ey, 0.22f, 0.44f), e = at(ey, 0.28f, 0.44f);
    CHECK(R(c) > 200 && R(w) > 200 && R(e) > 200, "the iris and the white are eye (R %d %d %d)", R(c), R(w), R(e));
    CHECK(R(at(ey, 0.25f, 0.34f)) == 0 && R(at(ey, 0.20f, 0.53f)) == 0, "the lids (skin) are not eye (R %d %d)", R(at(ey, 0.25f, 0.34f)), R(at(ey, 0.20f, 0.53f)));
    CHECK(R(at(ey, 0.75f, 0.44f)) == 0 && R(at(ey, 0.25f, 0.70f)) == 0, "the rest of the head is not eye");
    CHECK(abs(G(c) - 128) < 30 && abs(B(c) - 128) < 40, "the centre's offset is about 0 (%d %d)", G(c), B(c));
    CHECK(G(w) < 90 && G(e) > 166, "offsets run across the eye: left %d, right %d", G(w), G(e));
    CHECK(B(at(ey, 0.25f, 0.405f)) < B(at(ey, 0.25f, 0.47f)), "and down it (%d above %d)", B(at(ey, 0.25f, 0.405f)), B(at(ey, 0.25f, 0.47f)));
    CHECK(A(c) == 255, "eyes: alpha 255");
    static uint8_t eye0[MW * MH];
    memset(eye0, 0, sizeof eye0);
    static uint32_t alb[MW * MH];
    mat_albedo(alb);
    const float ref[3] = {0.75f, 0.52f, 0.40f};
    CHECK(eyes_mask_build(ey, MW, MH, eye0, alb, ref) == 0 && ey[MW * MH / 2] == 0, "no eye-lid triangles: no eyes");
    head();
    mat_masks(ey, cl, &ne, &nc);
    CHECK(ne == 0, "a mesh without the eye lid has no eyes (%u)", ne);
}
/* The title's eye, as the extracted characters have it: only the upper lid's line (row 7) is
 * weighted to the eye-lid joint; the eye's opening (rows 7-8: the white and the iris, down to the
 * lower lid) and a thin dark strip of upper lid above the line (rows 6-7: the lash line, the lid's
 * shadow) are drawn by the triangles around it. A blink pulls the line down: the opening's
 * triangles shrink to nothing and the strip's stretch over the whole eye, so the strip is lid,
 * never eye, whatever its colour. */
static void eye_lid_line(void) {
    for (int i = 7; i <= 9; i++) slot[7 * (NLON + 1) + i][0] = HAIR_SLOT_EYE_LID;
}
static void lid_albedo(uint32_t *alb) {
    const uint32_t skin = rgb(0.75f, 0.52f, 0.40f), white = rgb(0.85f, 0.84f, 0.80f), iris = rgb(0.12f, 0.08f, 0.06f), lid = rgb(0.30f, 0.20f, 0.24f);
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW; x++) {
            const float u = (x + 0.5f) / MW, v = (y + 0.5f) / MH;
            uint32_t c = skin;
            if (u > 0.19f && u < 0.31f && v > 0.375f && v < 0.4375f) c = lid;   /* above the lid's line */
            if (u > 0.19f && u < 0.31f && v > 0.4375f && v < 0.5f) c = u > 0.235f && u < 0.265f ? iris : white;
            alb[y * MW + x] = c;
        }
}
static void test_eyes_lid_strip(void) {
    static uint32_t hm_[MW * MH], alb[MW * MH], ey[MW * MH];
    static uint8_t creg[MW * MH], eye[MW * MH];
    eye_lid_line();
    lid_albedo(alb);
    memset(hm_, 0, sizeof hm_), memset(creg, 0, sizeof creg), memset(eye, 0, sizeof eye);
    HairMesh hm = mesh();
    float ref[3] = {0, 0, 0};
    int have = 0;
    hair_mask_add(hm_, MW, MH, &hm, alb, ref, &have);
    mat_geom_add(creg, eye, MW, MH, &hm);
    const uint32_t ne = eyes_mask_build(ey, MW, MH, eye, alb, have ? ref : NULL);
    CHECK(ne >= 4, "the opening under the lid's line is eye (%u texels)", ne);
    CHECK(R(at(ey, 0.25f, 0.47f)) > 200 && R(at(ey, 0.22f, 0.47f)) > 100, "the iris and the white are eye (R %d %d)", R(at(ey, 0.25f, 0.47f)), R(at(ey, 0.22f, 0.47f)));
    int lid = 0;
    for (int y = (int)(0.375f * MH); y < (int)(0.4375f * MH); y++)
        for (int x = (int)(0.19f * MW) + 1; x < (int)(0.31f * MW); x++) lid = R(ey[y * MW + x]) > lid ? R(ey[y * MW + x]) : lid;
    CHECK(lid == 0, "the dark lid strip above the line is never eye (R up to %d)", lid);
    head();
}
/* A palette of 32 slots (3 rows each) with the head turned by yaw and the eye lid turned by lid
 * degrees more about the head's own x axis, both scaled by s. */
static void lid_palette(float (*c)[4], float yaw, float lid, float s) {
    memset(c, 0, 3 * 32 * sizeof *c);
    for (int k = 0; k < 32; k++)
        for (int i = 0; i < 3; i++) c[3 * k + i][i] = 1;
    const float a = yaw * (float)PI / 180, b = lid * (float)PI / 180;
    const float hy[9] = {cosf(a), -sinf(a), 0, sinf(a), cosf(a), 0, 0, 0, 1};
    const float rx[9] = {1, 0, 0, 0, cosf(b), -sinf(b), 0, sinf(b), cosf(b)};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            float l = 0;
            for (int k = 0; k < 3; k++) l += hy[i * 3 + k] * rx[k * 3 + j];
            c[3 * HAIR_SLOT_HEAD + i][j] = s * hy[i * 3 + j];
            c[3 * HAIR_SLOT_EYE_LID + i][j] = s * l;
        }
    for (int i = 0; i < 3; i++) c[3 * HAIR_SLOT_HEAD + i][3] = c[3 * HAIR_SLOT_EYE_LID + i][3] = 0.3f * i;
}
static void test_eyes_lid_open(void) {
    static float c[3 * 32 + 8][4];
    lid_palette(c, 40, 0, 1);
    CHECK(eyes_lid_open((const float (*)[4])c, 0, 96) == 1.0f, "the lid at rest with the head: open (%.3f)", eyes_lid_open((const float (*)[4])c, 0, 96));
    lid_palette(c, 40, 5, 0.02f);
    CHECK(eyes_lid_open((const float (*)[4])c, 0, 96) == 1.0f, "5 degrees, a resting expression (a scaled palette): open");
    lid_palette(c, -70, 15, 1.3f);
    CHECK(eyes_lid_open((const float (*)[4])c, 0, 96) == 0.0f, "the lid turned 15 degrees (a blink's peak): shut (%.3f)", eyes_lid_open((const float (*)[4])c, 0, 96));
    lid_palette(c, 10, -15, 1);
    CHECK(eyes_lid_open((const float (*)[4])c, 0, 96) == 0.0f, "15 degrees the other way: shut");
    lid_palette(c, 10, 9, 1);
    const float half = eyes_lid_open((const float (*)[4])c, 0, 96);
    CHECK(half > 0.05f && half < 0.95f, "9 degrees: closing (%.3f)", half);
    lid_palette(c, 10, 30, 1);
    CHECK(eyes_lid_open((const float (*)[4])c, 0, 95) == 1.0f && eyes_lid_open((const float (*)[4])c, -3, 96) == 1.0f, "a palette out of range: open (as before)");
    c[3 * HAIR_SLOT_EYE_LID][0] = NAN;
    CHECK(eyes_lid_open((const float (*)[4])c, 0, 96) == 1.0f, "an unreadable palette: open");
    memset(c, 0, sizeof c);
    CHECK(eyes_lid_open((const float (*)[4])c, 0, 96) == 1.0f, "an empty palette: open");
}
static void test_cloth_mask(void) {
    static uint32_t ey[MW * MH], cl[MW * MH];
    uint32_t ne, nc;
    mat_masks(ey, cl, &ne, &nc);
    CHECK(nc > 100, "cloth texels (%u)", nc);
    const uint32_t de = at(cl, 0.12f, 0.95f), co = at(cl, 0.37f, 0.95f), le = at(cl, 0.56f, 0.95f), sk = at(cl, 0.8f, 0.95f);
    CHECK(R(le) > 200 && G(le) < 30 && B(le) < 50, "the maroon vest on the torso is leather (%08x)", le);
    CHECK(G(de) > 200 && R(de) < 30, "the blue is denim (%08x)", de);
    CHECK(B(co) > 200 && R(co) < 30 && G(co) < 30, "the grey is cotton (%08x)", co);
    CHECK((sk & 0xFFFFFF) == 0, "bare skin is not cloth (%08x)", sk);
    CHECK((at(cl, 0.75f, 0.15f) & 0xFFFFFF) == 0 && (at(cl, 0.25f, 0.5f) & 0xFFFFFF) == 0, "the head (a blue cap, the face) is not cloth");
    CHECK(R(le) + G(le) + B(le) <= 256, "the weights sum to at most 1");
    /* the cap weighted to a calf: trousers; maroon there is cotton, not leather */
    for (int k = 0; k < NV; k++)
        if (slot[k][0] == 1) slot[k][0] = 27;
    mat_masks(ey, cl, &ne, &nc);
    const uint32_t tr = at(cl, 0.56f, 0.95f);
    CHECK(R(tr) < 30 && B(tr) > 200, "maroon trousers are cotton, not leather (%08x)", tr);
    CHECK(G(at(cl, 0.12f, 0.95f)) > 200, "blue trousers are denim (jeans)");
    head();
}
static uint32_t stack_check(void *ctx, uint32_t version, uint32_t mw, uint32_t mh, const uint32_t *mask) {
    uint32_t *o = ctx;
    o[0] = mw, o[1] = mh, o[2] = mask[(size_t)(0.44f * MH) * mw + (size_t)(0.25f * MW)];   /* the skin mask at the eye */
    o[3] = mask[(size_t)(MH + 0.44f * MH) * mw + (size_t)(0.25f * MW)];                   /* the eyes */
    o[4] = mask[(size_t)(2 * MH + 0.95f * MH) * mw + (size_t)(0.56f * MW)];               /* the cloth (the vest) */
    o[5] = mask[(size_t)(0.95f * MH) * mw + (size_t)(0.8f * MW)];                         /* the skin mask on the bare body */
    return version;
}
static void test_mat_cache(void) {
    static uint32_t alb[MW * MH];
    mat_albedo(alb);
    eye_lid();
    hair_cache_clear();
    HairMesh hm = mesh();
    hair_cache_add(0xA0, 1, &hm, alb, MW, MH);
    uint32_t o[6] = {0};
    CHECK(hair_cache_mat_with(0xA0, stack_check, o) == 0, "eyes, cloth off: no stacked masks");
    hair_cache_set_mat(1, 1);
    hair_cache_add(0xA1, 1, &hm, alb, MW, MH);
    const uint32_t v = hair_cache_mat_with(0xA1, stack_check, o);
    CHECK(v >= 1 && o[0] == MW && o[1] == 3 * MH, "eyes, cloth: three masks stacked (version %u, %ux%u)", v, o[0], o[1]);
    CHECK(R(o[3]) > 200 && R(o[4]) > 200, "the eyes and the cloth in their parts (%08x %08x)", o[3], o[4]);
    CHECK(o[5] == 0, "without --skin the skin part is empty (%08x)", o[5]);
    uint32_t ne = 0, nc = 0;
    hair_cache_mat_texels(0xA1, &ne, &nc);
    CHECK(ne > 0 && nc > 0, "texels: eyes %u, cloth %u", ne, nc);
    hair_cache_set_skin(1);
    hair_cache_add(0xA2, 1, &hm, alb, MW, MH);
    hair_cache_mat_with(0xA2, stack_check, o);
    CHECK(R(o[5]) > 200, "with --skin the skin part is the skin mask (%08x)", o[5]);
    CHECK(hair_cache_skin_with(0xA2, version_of, NULL) >= 1, "and the skin mask alone as before");
    hair_cache_set_skin(0);
    hair_cache_set_mat(0, 1);
    hair_cache_add(0xA3, 1, &hm, alb, MW, MH);
    hair_cache_mat_with(0xA3, stack_check, o);
    CHECK(o[3] == 0 && R(o[4]) > 200, "cloth only: the eyes part is empty (%08x)", o[3]);
    hair_cache_set_mat(0, 0);
    hair_cache_clear();
    head();
}

/* Headgear modeled into a skin (a cap or helmet on the head shell, the head under it): recognised by
 * the print of its head geometry (the head joint's vertex positions), never hair, whatever its colour. */
static void test_headgear(void) {
    head();
    HairMesh hm = mesh();
    const uint64_t p = hair_head_print(&hm);
    CHECK(p != 0, "a head has a print");
    /* the same head with its vertices duplicated and listed in another order: the same print */
    static float pos2[2 * NV][3], nrm2[2 * NV][3], uv2[2 * NV][2], wt2[2 * NV][3];
    static int slot2[2 * NV][3];
    for (int k = 0; k < 2 * NV; k++) {
        const int s = (2 * NV - 1 - k) % NV;
        memcpy(pos2[k], pos[s], sizeof pos2[k]), memcpy(nrm2[k], nrm[s], sizeof nrm2[k]), memcpy(uv2[k], uv[s], sizeof uv2[k]);
        memcpy(wt2[k], wt[s], sizeof wt2[k]), memcpy(slot2[k], slot[s], sizeof slot2[k]);
    }
    HairMesh h2 = {2 * NV, (const float (*)[3])pos2, (const float (*)[3])nrm2, (const float (*)[2])uv2, (const int (*)[3])slot2,
                   (const float (*)[3])wt2, idx, NI};
    CHECK(hair_head_print(&h2) == p, "the print ignores vertex order and duplicates");
    pos[0][2] += 0.01f;   /* the crown's top vertex moved: another head */
    CHECK(hair_head_print(&hm) != p, "another head shape, another print");
    head();
    CHECK(!hair_is_headgear(&hm), "a plain head is not headgear");
    static uint32_t m[MW * MH];
    memset(m, 0, sizeof m);
    float ref[3] = {0, 0, 0};
    int have = 0;
    CHECK(hair_mask_add(m, MW, MH, &hm, NULL, ref, &have) > 0, "its hair is found");
    hair_headgear_register(p);
    CHECK(hair_is_headgear(&hm), "a registered print is headgear");
    memset(m, 0, sizeof m);
    static uint32_t alb[MW * MH];
    for (int i = 0; i < MW * MH; i++) alb[i] = rgb(0.12f, 0.1f, 0.09f);   /* dark brown: hair-coloured headgear */
    for (int i = 0; i < MW * MH / 2; i++) alb[MW * MH / 2 + i] = rgb(0.8f, 0.6f, 0.5f);
    have = 0;
    CHECK(hair_mask_add(m, MW, MH, &hm, alb, ref, &have) == 0, "headgear texels are never hair");
    uint32_t any = 0;
    for (int i = 0; i < MW * MH; i++) any |= m[i];
    CHECK(!any, "the mask stays empty");
    CHECK(have, "the face's skin colour is still measured (for --skin)");
    CHECK(hair_headgear_known() >= 4, "the title's built-in headgear prints are known (%d)", hair_headgear_known());
}

int main(void) {
    head();
    test_options();
    test_geometry();
    test_colours();
    test_downsample();
    test_cache();
    test_skin_options();
    test_skin_mask();
    test_skin_legs_hands();
    test_skin_cache();
    test_skin_order();
    test_mat_options();
    test_eyes_mask();
    test_eyes_lid_strip();
    test_eyes_lid_open();
    test_cloth_mask();
    test_mat_cache();
    test_headgear();   /* last: it registers a print */
    if (fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("ALL OK\n");
    return 0;
}
