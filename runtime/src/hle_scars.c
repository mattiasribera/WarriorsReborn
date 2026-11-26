/* Healed marks for mods (see hle_scars.h). */
#include "hle_scars.h"
#include "hle_devlua.h"
#include "mem.h"
#include "platform.h"
#include "xtex.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int hle_scars_trace;

static struct { int id, step; } marks[SCARS_CHARS];
static int nmarks;


int scars_set(int char_id, double level) {
    if (char_id < 1 || char_id > 65535 || !isfinite(level)) return -1;
    int step = level <= 0 ? 0 : (int)ceil((level > 1 ? 1 : level) * SCARS_STEPS - 1e-9);
    int i;
    for (i = 0; i < nmarks && marks[i].id != char_id; i++) {}
    if (!step) {
        if (i < nmarks) marks[i] = marks[--nmarks];
        return 0;
    }
    if (i == nmarks) {
        if (nmarks == SCARS_CHARS) return -1;
        nmarks++;
    }
    marks[i].id = char_id;
    marks[i].step = step;
    return 0;
}

void scars_clear(void) { nmarks = 0; }

int scars_step(int char_id) {
    for (int i = 0; i < nmarks; i++)
        if (marks[i].id == char_id) return marks[i].step;
    return 0;
}

/* ---- the healed atlas ---- */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float smooth(float e0, float e1, float x) { float t = clampf((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t); }
static uint32_t pack(float r, float g, float b, float a) {
    return (uint32_t)(clampf(a, 0, 1) * 255 + 0.5f) << 24 | (uint32_t)(clampf(r, 0, 1) * 255 + 0.5f) << 16 |
           (uint32_t)(clampf(g, 0, 1) * 255 + 0.5f) << 8 | (uint32_t)(clampf(b, 0, 1) * 255 + 0.5f);
}

uint32_t scars_heal_texel(uint32_t blood, int step) {
    float a = (blood >> 24) / 255.0f;
    if (a <= 0 || step <= 0) return 0;
    float r = ((blood >> 16) & 255) / 255.0f, g = ((blood >> 8) & 255) / 255.0f, b = (blood & 255) / 255.0f;
    /* The piece's own shading (its darker texels) kept as a relief; never brighter than the base. */
    float lum = clampf((0.6f * r + 0.3f * g + 0.1f * b) / 0.45f, 0.55f, 1.0f);
    /* Strong pieces (a cut's line) dry to a dark red scab, soft ones (a bruise) to a dark plum and
     * olive-brown at their faint edge (an older bruise). */
    float cut = smooth(0.55f, 0.85f, a), edge = 1 - smooth(0.15f, 0.45f, a);
    /* Dark tones only: the game lays the atlas over the skin by its alpha, so a colour lighter than
     * the skin would show as a pale speck on dark skin. These are darker (Rec. 709 luma about 0.11)
     * than the darkest skin of the crew: they darken every skin, tinted plum or red. */
    float br = 0.20f - 0.04f * edge, bg = 0.08f + 0.03f * edge, bb = 0.17f - 0.11f * edge;
    float cr = 0.30f, cg = 0.06f, cb = 0.05f;
    float k = (float)step / SCARS_STEPS;
    float oa = a * (0.22f + 0.43f * k) * (1 - 0.15f * cut);
    return pack((br + (cr - br) * cut) * lum, (bg + (cg - bg) * cut) * lum, (bb + (cb - bb) * cut) * lum, oa);
}

uint32_t scars_over(uint32_t a, uint32_t b) {
    float aa = (a >> 24) / 255.0f, ba = (b >> 24) / 255.0f;
    float oa = aa + ba * (1 - aa);
    if (oa <= 0) return 0;
    float c[3];
    for (int i = 0; i < 3; i++) {
        float ca = ((a >> (16 - 8 * i)) & 255) / 255.0f, cb = ((b >> (16 - 8 * i)) & 255) / 255.0f;
        c[i] = (ca * aa + cb * ba * (1 - aa)) / oa;
    }
    return pack(c[0], c[1], c[2], oa);
}

/* The plaster: a small tan strip with rounded ends on the cheekbone. Every model maps the atlas'
 * top-right face pieces over the eye (x 40-47, y 0-7), the cheekbone under it (x 40-47, y 8-15), and
 * the nose, the jaw and parts of the legs around them (x 32-39 and 48-63, y 16-31: probed on Cleon,
 * docs/MODDING.md "Lasting scars"), so the strip stays inside the cheekbone texels (x 41-47, y 9-14)
 * and lands as one patch; a larger one spilled over those other parts as pale specks. */
#define PLASTER_X0 42.8f
#define PLASTER_Y0 12.0f
#define PLASTER_X1 46.0f
#define PLASTER_Y1 11.1f
#define PLASTER_HALF 1.4f
static uint32_t plaster_texel(int x, int y) {
    float dx = PLASTER_X1 - PLASTER_X0, dy = PLASTER_Y1 - PLASTER_Y0, len = sqrtf(dx * dx + dy * dy);
    float px = x + 0.5f - PLASTER_X0, py = y + 0.5f - PLASTER_Y0;
    float t = (px * dx + py * dy) / len, d = (px * -dy + py * dx) / len;   /* along, across */
    float over = fmaxf(fmaxf(-t, t - len), 0);
    float dist = sqrtf(over * over + d * d);
    float cover = 1 - smooth(PLASTER_HALF - 0.1f, PLASTER_HALF + 0.5f, dist);
    if (cover <= 0) return 0;
    float pad = fabsf(t - len / 2) < len * 0.3f ? 0.92f : 1.0f;   /* the gauze pad in the middle */
    float l = pad * (1 - 0.15f * smooth(PLASTER_HALF - 0.8f, PLASTER_HALF + 0.3f, dist));   /* a soft edge */
    return pack(0.66f * l, 0.49f * l, 0.34f * l, 0.95f * cover);
}

void scars_build(const uint32_t *d1, const uint32_t *d2, int step, uint32_t *out) {
    const uint32_t *src = step <= SCARS_STEPS / 2 ? d1 : d2;
    for (int y = 0; y < SCARS_TEX; y++)
        for (int x = 0; x < SCARS_TEX; x++) {
            uint32_t c = scars_heal_texel(src[y * SCARS_TEX + x], step);
            if (step > SCARS_STEPS / 2) {
                uint32_t p = plaster_texel(x, y);
                if (p) c = scars_over(p, c);
            }
            out[y * SCARS_TEX + x] = c;
        }
}

/* ---- textures in guest memory ---- */

#ifndef HLE_SCARS_TEST
static uint32_t rd32(uint32_t a) { return *(const volatile uint32_t *)(uintptr_t)a; }

static uint32_t atlas_src[3];                       /* the game's atlas textures we decoded */
static uint32_t *decoded[3];
/* Our textures: [step][fresh stage + 1] (0 = marks only), D3D headers in guest memory. */
static uint32_t tex[SCARS_STEPS + 1][4];

static uint32_t game_atlas(int i) {
    uint32_t rm = rd32(SCARS_RESMGR), o = rm ? rd32(rm + 0x58 + 4 * i) : 0, w = o ? rd32(o + 0xc) : 0;
    return w ? rd32(w + 4) : 0;
}

static const uint32_t *atlas(int i) {
    uint32_t t = game_atlas(i);
    if (!t) return NULL;
    if (t != atlas_src[i]) {   /* new (or first): decode it; our textures from the old one go */
        XTexInfo ti;
        if (xtex_info(rd32(t + 0xc), rd32(t + 0x10), &ti) || ti.w != SCARS_TEX || ti.h != SCARS_TEX) return NULL;
        if (!decoded[i] && !(decoded[i] = malloc(SCARS_TEX * SCARS_TEX * 4))) return NULL;
        if (xtex_decode_level(&ti, 0, (const uint8_t *)(uintptr_t)(rd32(t + 4) | CONTIG_BASE), decoded[i])) return NULL;
        atlas_src[i] = t;
        memset(tex, 0, sizeof tex);
    }
    return decoded[i];
}

/* A swizzled A8R8G8B8 64x64 texture with one level, header like the game's atlas header. */
static uint32_t *pool_hdr[(SCARS_STEPS + 1) * 4];
static uint32_t make_texture(const uint32_t *texels, uint32_t like, int slot) {
    uint32_t *h = pool_hdr[slot];
    if (!h) {
        uint32_t m = contig_alloc(0x1000 + SCARS_TEX * SCARS_TEX * 4, 0x1000, 0, 0);
        if (!m) return 0;
        h = pool_hdr[slot] = (uint32_t *)(uintptr_t)m;
    }
    uint32_t data = (uint32_t)(uintptr_t)h + 0x1000, *px = (uint32_t *)(uintptr_t)data;
    for (uint32_t y = 0; y < SCARS_TEX; y++)
        for (uint32_t x = 0; x < SCARS_TEX; x++) px[xbox_swizzle_offset(x, y, SCARS_TEX, SCARS_TEX)] = texels[y * SCARS_TEX + x];
    h[0] = (rd32(like) & ~0xFFFFu) | 1;
    h[1] = data & ~CONTIG_BASE;
    h[2] = 0;
    h[3] = (rd32(like + 0xc) & 0xFu) | 2u << 4 | 0x06u << 8 | 1u << 16 | 6u << 20 | 6u << 24;
    h[4] = 0;
    return (uint32_t)(uintptr_t)h;
}

static uint32_t texture(int step, int fresh) {
    if (tex[step][fresh + 1]) return tex[step][fresh + 1];
    const uint32_t *d1 = atlas(0), *d2 = atlas(1), *f = fresh >= 0 ? atlas(fresh) : NULL;
    if (!d1 || !d2 || (fresh >= 0 && !f)) return 0;
    static uint32_t buf[SCARS_TEX * SCARS_TEX];
    scars_build(d1, d2, step, buf);
    if (f)
        for (int i = 0; i < SCARS_TEX * SCARS_TEX; i++) buf[i] = scars_over(f[i], buf[i]);
    uint32_t t = make_texture(buf, atlas_src[0], step * 4 + fresh + 1);
    if (t && hle_scars_trace) {
        if (fresh >= 0) printf("[scars] texture: marks step %d + fresh blood d%d -> %08x\n", step, fresh + 1, t);
        else printf("[scars] texture: marks step %d -> %08x\n", step, t);
    }
    return tex[step][fresh + 1] = t;
}

/* The draw's choice: the texture for stage 1, or 0 (no overlay). As the title: nothing under the
 * no-blood word, the game's atlas under 90 % health; with marks, our atlas (fresh blood over it). */
__attribute__((force_align_arg_pointer, cdecl, used)) static uint32_t scars_pick(uint32_t draw, float pct, uint32_t blood) {
    uint32_t g = rd32(SCARS_GAME);
    if (g && *(const volatile uint16_t *)(uintptr_t)(g + 0x42a)) return 0;
    float limit = *(const volatile float *)(uintptr_t)SCARS_BLOOD_PCT;
    uint32_t game_tex = blood ? rd32(rd32(blood + 0xc) + 4) : 0;
    int fresh = -1;
    if (limit > pct) {   /* the title's test (comiss; jbe skips: NaN shows none) */
        for (int i = 0; i < 3; i++)
            if (game_atlas(i) == game_tex) fresh = i;
    }
    uint32_t human = rd32(draw + 0x2a8);
    int step = nmarks && human ? scars_step((int)rd32(human + 0xa8)) : 0;
    if (step) {
        uint32_t t = texture(step, fresh);
        if (t) return t;
    }
    return limit > pct ? game_tex : 0;
}

int scars_patch(void) {
    static int state;   /* 0 not tried, 1 in place, -1 failed */
    static const uint8_t sig[] = {0xa1, 0x78, 0x9f, 0x5b, 0x00, 0x66, 0x83, 0xb8, 0x2a, 0x04, 0x00, 0x00, 0x00, 0x75, 0x27,
                                  0xf3, 0x0f, 0x10, 0x05, 0x1c, 0x0c, 0x42, 0x00};
    if (state) return state > 0;
    state = -1;
    if (memcmp((const void *)(uintptr_t)SCARS_SITE, sig, sizeof sig)) {
        printf("[scars] skip healed marks at %08x: unexpected bytes\n", SCARS_SITE);
        return 0;
    }
    uint8_t *s = plat_alloc_exec(64), *q = s;
    if (!s) return 0;
    *q++ = 0x60;                                            /* pushad */
    *q++ = 0x57;                                            /* push edi: the game's atlas object */
    *q++ = 0xff; *q++ = 0x74; *q++ = 0x24; *q++ = 0x34;     /* push [esp+0x34]: health percent ([esp+0x10] at the site) */
    *q++ = 0x56;                                            /* push esi: the draw */
    int32_t rel = (int32_t)((uintptr_t)scars_pick - (uintptr_t)(q + 5));
    *q++ = 0xe8; memcpy(q, &rel, 4); q += 4;
    *q++ = 0x83; *q++ = 0xc4; *q++ = 0x0c;                  /* add esp, 12 */
    *q++ = 0x89; *q++ = 0x44; *q++ = 0x24; *q++ = 0x1c;     /* mov [esp+0x1c], eax (popad's eax) */
    *q++ = 0x61;                                            /* popad */
    *q++ = 0x85; *q++ = 0xc0;                               /* test eax, eax */
    *q++ = 0x74; *q++ = 21;                                 /* jz none */
    uint32_t v = SCARS_RENDERER;
    *q++ = 0x8b; *q++ = 0x0d; memcpy(q, &v, 4); q += 4;     /* mov ecx, [0x4cfe8c] */
    *q++ = 0x50;                                            /* push eax */
    *q++ = 0x6a; *q++ = 0x01;                               /* push 1 */
    v = SCARS_SETTEX_FN;
    *q++ = 0xb8; memcpy(q, &v, 4); q += 4;                  /* mov eax, 0xb8f50 */
    *q++ = 0xff; *q++ = 0xd0;                               /* call eax */
    rel = (int32_t)(SCARS_BLOOD_BACK - (uint32_t)(uintptr_t)(q + 5));
    *q++ = 0xe9; memcpy(q, &rel, 4); q += 4;                /* jmp 0x8fe42: push 4 */
    rel = (int32_t)(SCARS_NONE_BACK - (uint32_t)(uintptr_t)(q + 5));
    *q++ = 0xe9; memcpy(q, &rel, 4); q += 4;                /* none: jmp 0x8fe46: push 2 */
    uint8_t *p = (uint8_t *)(uintptr_t)SCARS_SITE;
    rel = (int32_t)((uintptr_t)s - (SCARS_SITE + 5));
    p[0] = 0xe9; memcpy(p + 1, &rel, 4);
    state = 1;
    printf("[scars] healed marks at %08x\n", SCARS_SITE);
    return 1;
}

/* ModScar(char_id, level) / ModScar(). */
__attribute__((force_align_arg_pointer, cdecl)) int hle_scars_lua(uint32_t L) {
    const DevLuaApi *a = &devlua_title_api;
    if (a->type(L, 1) <= 0) {   /* none or nil */
        if (hle_scars_trace && nmarks) printf("[scars] marks cleared\n");
        scars_clear();
        return 0;
    }
    const char *id = a->type(L, 1) == 2 || a->type(L, 1) == 3 ? a->tostring(L, 1) : NULL;
    const char *lv = a->type(L, 2) == 2 || a->type(L, 2) == 3 ? a->tostring(L, 2) : NULL;
    char *e1 = NULL, *e2 = NULL;
    long n = id ? strtol(id, &e1, 10) : 0;
    double l = lv ? strtod(lv, &e2) : NAN;
    if (!id || !lv || *e1 || *e2 || scars_set((int)n, l)) {
        printf("[scars] ModScar(%.40s, %.40s): not a character id and a level, or %d ids already marked; ignored\n",
               id ? id : "nil", lv ? lv : "nil", SCARS_CHARS);
    } else {
        if (scars_step((int)n)) scars_patch();
        if (hle_scars_trace) printf("[scars] character %ld: marks step %d\n", n, scars_step((int)n));
    }
    fflush(stdout);
    return 0;
}
#endif

#ifdef HLE_SCARS_TEST
/* Game-independent test: the table of marks, the healed texels and atlas. */
static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)
#define CH(c, s) (((c) >> (s)) & 255u)

int main(void) {
    CHECK(scars_step(5) == 0, "no marks at start");
    CHECK(scars_set(5, 1.0) == 0 && scars_step(5) == SCARS_STEPS, "level 1: the last step");
    CHECK(scars_set(5, 0.3) == 0 && scars_step(5) == 3, "level 0.3: step 3 (rounded up, a mark never vanishes early)");
    CHECK(scars_set(5, 7) == 0 && scars_step(5) == SCARS_STEPS, "level above 1 is clamped");
    CHECK(scars_set(6, 0.01) == 0 && scars_step(6) == 1, "a tiny level still shows step 1");
    CHECK(scars_set(6, 0) == 0 && scars_step(6) == 0 && scars_step(5) == SCARS_STEPS, "level 0 removes one id, the other stays");
    CHECK(scars_set(0, 1) == -1 && scars_set(70000, 1) == -1 && scars_set(7, NAN) == -1, "bad ids and levels are refused");
    int full = 1;
    for (int i = 0; i < SCARS_CHARS; i++) full &= scars_set(100 + i, 0.5) == 0 || i == SCARS_CHARS - 1;
    CHECK(full && scars_set(999, 0.5) == -1 && scars_step(999) == 0, "the table holds %d ids", SCARS_CHARS);
    CHECK(scars_set(100, 0.9) == 0 && scars_step(100) == 8, "an id already marked can still change");
    scars_clear();
    CHECK(scars_step(5) == 0 && scars_step(100) == 0, "clear removes every mark");

    uint32_t blood = 0xF07A0A08u, soft = 0x50701010u;   /* a strong dark-red texel, a soft one */
    CHECK(scars_heal_texel(0x00FF0000u, 8) == 0 && scars_heal_texel(blood, 0) == 0, "no alpha or no step: nothing");
    uint32_t h8 = scars_heal_texel(blood, 8), h1 = scars_heal_texel(blood, 1), s8 = scars_heal_texel(soft, 8);
    CHECK(CH(h8, 24) < CH(blood, 24) && CH(h1, 24) < CH(h8, 24) && CH(h1, 24) > 0, "healed marks are fainter than blood, and fade by step (%u %u)",
          CH(h8, 24), CH(h1, 24));
    CHECK(CH(h8, 16) < CH(blood, 16) * 3 / 4 && CH(h8, 16) > CH(h8, 8) * 2,
          "a cut dries dark red, darker than fresh blood (%08x)", h8);
    CHECK(CH(s8, 0) > CH(h8, 0) && CH(s8, 0) > CH(s8, 8), "a soft piece is plum (bluer than a cut, blue over green: %08x)", s8);
    /* The game lays the atlas over the skin by its alpha: every healed texel must darken (never lighten)
     * even the darkest skin of the crew, or dark skin shows pale specks. Luma of sRGB values, Rec. 709. */
    {
        static const uint32_t skins[] = {0xE0B898u, 0x9A6A50u, 0x4A3228u, 0x30201Au};
        static const uint32_t cols[] = {0x7A0A08u, 0x401010u, 0xC04040u, 0xFFFFFFu};
        int lighter = 0, n = 0;
        uint32_t worst = 0;
        for (int st = 1; st <= SCARS_STEPS; st++)
            for (uint32_t al = 8; al <= 255; al += 31)
                for (int col = 0; col < 4; col++) {
                    uint32_t h = scars_heal_texel(al << 24 | cols[col], st);
                    for (int s = 0; s < 4; s++, n++) {
                        float a = CH(h, 24) / 255.0f, l0 = 0, l1 = 0;
                        for (int c = 0; c < 3; c++) {
                            float w = c == 0 ? 0.2126f : c == 1 ? 0.7152f : 0.0722f;
                            float sk = CH(skins[s], 16 - 8 * c) / 255.0f, mk = CH(h, 16 - 8 * c) / 255.0f;
                            l0 += w * sk;
                            l1 += w * (sk + (mk - sk) * a);
                        }
                        if (l1 > l0 + 0.002f) { lighter++; worst = h; }
                    }
                }
        CHECK(!lighter, "healed marks never lighten skin, the darkest included (%d of %d lighter, e.g. %08x)", lighter, n, worst);
    }
    CHECK(scars_over(0xFF102030u, 0x80FFFFFFu) == 0xFF102030u && scars_over(0, 0x80405060u) == 0x80405060u, "over: opaque wins, clear keeps");

    static uint32_t d1[SCARS_TEX * SCARS_TEX], d2[SCARS_TEX * SCARS_TEX], out[SCARS_TEX * SCARS_TEX];
    for (int i = 0; i < SCARS_TEX * SCARS_TEX; i++) { d1[i] = (i % 7) ? 0 : soft; d2[i] = (i % 5) ? 0 : blood; }
    int py = 11, px = 44;   /* the cheekbone texels of the face piece */
    scars_build(d1, d2, 4, out);
    CHECK(out[7] == scars_heal_texel(soft, 4) && out[5] == 0 && CH(out[py * SCARS_TEX + px], 24) < 0x80, "step 4: d1 healed, no plaster");
    scars_build(d1, d2, 6, out);
    uint32_t pl = out[py * SCARS_TEX + px];
    CHECK(CH(pl, 24) > 0xd0 && CH(pl, 16) > CH(pl, 8) && CH(pl, 8) > CH(pl, 0) && CH(pl, 16) >= 0x90 && CH(pl, 16) <= 0xb8,
          "step 6: a tan plaster on the cheekbone, not white (%08x)", pl);
    CHECK(out[40 * SCARS_TEX + 40] == scars_heal_texel(d2[40 * SCARS_TEX + 40], 6), "step 6: d2 healed away from the plaster");
    /* The atlas texels around the cheekbone piece (x 40-47, y 8-15) also cover the eye, nose, jaw and
     * the legs on a model: the plaster stays inside x 41-47, y 9-14 so it is one patch, not specks. */
    static const uint32_t none[SCARS_TEX * SCARS_TEX];
    scars_build(none, none, SCARS_STEPS, out);
    int outside = 0, inside = 0;
    for (int y = 0; y < SCARS_TEX; y++)
        for (int x = 0; x < SCARS_TEX; x++) {
            int a = CH(out[y * SCARS_TEX + x], 24);
            if (x >= 41 && x <= 47 && y >= 9 && y <= 14) inside += a > 0xc0;
            else outside += a > 0;
        }
    CHECK(!outside && inside >= 12 && inside <= 30, "the plaster is one small patch inside the cheekbone texels (%d inside, %d outside)",
          inside, outside);
    printf(failures ? "FAILED: %d\n" : "ALL OK\n", failures);
    return failures != 0;
}
#endif
