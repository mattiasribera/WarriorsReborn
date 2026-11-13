/* Game-independent --gamma-ramp test (src/gamma_ramp.c): the options, the Xbox D3DGAMMARAMP
 * layout (bytes, red then green then blue), identity detection, the shader's lookup table and its
 * CPU reference on B8G8R8A8 pixels. */
#include "../src/gamma_ramp.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

/* A guest ramp: identity, or red = 255 * x^p, green = i / 2, blue = 255 - i. */
static void guest_ramp(uint8_t raw[GAMMA_RAMP_BYTES], int identity, double p) {
    for (int i = 0; i < 256; i++) {
        raw[i] = identity ? (uint8_t)i : (uint8_t)lround(255.0 * pow(i / 255.0, p));
        raw[256 + i] = identity ? (uint8_t)i : (uint8_t)(i / 2);
        raw[512 + i] = identity ? (uint8_t)i : (uint8_t)(255 - i);
    }
}

static void test_args(void) {
    int on = 1;
    CHECK(gamma_ramp_arg(&on, "--no-gamma-ramp") == 1 && on == 0, "--no-gamma-ramp turns it off");
    CHECK(gamma_ramp_arg(&on, "--gamma-ramp") == 1 && on == 1, "--gamma-ramp turns it on");
    CHECK(gamma_ramp_arg(&on, "--gamma") == 0 && on == 1, "other options are not consumed");
    on = 0;
    CHECK(gamma_ramp_arg(&on, "--gamma-ramp=1") == 0 && on == 0, "no value forms");
}

static void test_parse(void) {
    uint8_t raw[GAMMA_RAMP_BYTES + 4];
    guest_ramp(raw, 0, 0.8);
    GammaRamp g;
    memset(&g, 0xab, sizeof g);
    CHECK(sizeof(GammaRamp) == GAMMA_RAMP_BYTES, "D3DGAMMARAMP is 768 bytes (%u)", (unsigned)sizeof(GammaRamp));
    CHECK(gamma_ramp_parse(raw, GAMMA_RAMP_BYTES, &g) == 0, "parse: a full ramp");
    CHECK(g.red[128] == raw[128] && g.green[128] == 64 && g.blue[0] == 255 && g.blue[255] == 0,
          "parse: red, green, blue at bytes 0, 256, 512 (red[128] %u, green[128] %u)", g.red[128], g.green[128]);
    CHECK(g.red[128] >= 146 && g.red[128] <= 148, "parse: x^0.8 lifts mid-grey 128 to %u", g.red[128]);
    GammaRamp before = g;
    CHECK(gamma_ramp_parse(raw, GAMMA_RAMP_BYTES - 1, &g) == -1 && !memcmp(&g, &before, sizeof g), "parse: 767 bytes rejected, unchanged");
    CHECK(gamma_ramp_parse(NULL, GAMMA_RAMP_BYTES, &g) == -1 && !memcmp(&g, &before, sizeof g), "parse: NULL rejected, unchanged");
    CHECK(gamma_ramp_parse(raw, sizeof raw, &g) == 0, "parse: a larger buffer reads the first 768 bytes");
}

static void test_identity(void) {
    uint8_t raw[GAMMA_RAMP_BYTES];
    GammaRamp g;
    guest_ramp(raw, 1, 1);
    gamma_ramp_parse(raw, sizeof raw, &g);
    CHECK(gamma_ramp_is_identity(&g), "identity: i -> i on every channel");
    for (int c = 0; c < 3; c++) {
        GammaRamp h = g;
        uint8_t *ch = c == 0 ? h.red : c == 1 ? h.green : h.blue;
        ch[200] = 201;
        CHECK(!gamma_ramp_is_identity(&h), "identity: one entry off in channel %d is not", c);
    }
    guest_ramp(raw, 0, 0.8);
    gamma_ramp_parse(raw, sizeof raw, &g);
    CHECK(!gamma_ramp_is_identity(&g), "identity: the level ramp is not");
}

static void test_lut(void) {
    uint8_t raw[GAMMA_RAMP_BYTES];
    GammaRamp g;
    uint32_t lut[256];
    guest_ramp(raw, 0, 0.8);
    gamma_ramp_parse(raw, sizeof raw, &g);
    gamma_ramp_lut(&g, lut);
    int packed = 1;
    for (int i = 0; i < 256; i++)
        if (lut[i] != ((uint32_t)g.red[i] | (uint32_t)g.green[i] << 8 | (uint32_t)g.blue[i] << 16)) packed = 0;
    CHECK(packed, "lut: entry i = red | green << 8 | blue << 16 (lut[128] %06x)", lut[128]);
    /* 0xAARRGGBB: r = 128, g = 100, b = 10, alpha 0x7f */
    const uint32_t px = 0x7f80640au, out = gamma_ramp_apply(lut, px);
    CHECK((out >> 16 & 0xff) == g.red[128] && (out >> 8 & 0xff) == g.green[100] && (out & 0xff) == g.blue[10] && out >> 24 == 0x7f,
          "apply: each channel through its table, alpha kept (%08x -> %08x)", px, out);
    guest_ramp(raw, 1, 1);
    gamma_ramp_parse(raw, sizeof raw, &g);
    gamma_ramp_lut(&g, lut);
    int same = 1;
    for (uint32_t v = 0; v < 1u << 24; v += 4099) same &= gamma_ramp_apply(lut, 0xff000000u | v) == (0xff000000u | v);
    CHECK(same, "apply: the identity table keeps every pixel");
}

int main(void) {
    test_args();
    test_parse();
    test_identity();
    test_lut();
    if (failures) printf("%d FAILED\n", failures);
    else printf("all passed\n");
    return failures ? 1 : 0;
}
