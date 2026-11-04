/* Game-independent draw-distance test: a host buffer stands in for guest memory from 0x320000
 * to 0x422000, with the shipped bytes of each patch site planted at its address. Checks the
 * option parsing, that the defaults write only the steady-fog nops, that --draw-distance and
 * --prop-detail write their operands and constants, that a patch whose bytes differ at any of
 * its sites writes nothing, and that nothing outside the patch sites changes. */
#include "../src/hle_drawdist.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

enum { BASE = 0x00320000u, SIZE = 0x00102000u };
static uint8_t mem[SIZE], before[SIZE];
static intptr_t bias;   /* host = guest va + bias */

static uint8_t *at(uint32_t va) { return mem + (va - BASE); }
static uint32_t rd32(uint32_t va) { uint32_t v; memcpy(&v, at(va), 4); return v; }
static float rdf(uint32_t va) { float v; memcpy(&v, at(va), 4); return v; }
static void wr32(uint32_t va, uint32_t v) { memcpy(at(va), &v, 4); }

/* Shipped bytes, from the XBE disassembly. */
static const uint8_t steady[] = {0xf3, 0x0f, 0x10, 0x0d, 0x2c, 0x06, 0x4b, 0x00, 0x0f, 0x2f, 0xc8, 0x76, 0x77};
static const uint8_t cap_scale[] = {0xf3, 0x0f, 0x59, 0x05, 0x30, 0xb8, 0x41, 0x00};
static const uint8_t cap_max[] = {0xf3, 0x0f, 0x10, 0x05, 0x38, 0x02, 0x43, 0x00};

static void plant(void) {
    memset(mem, 0xcc, sizeof mem);
    memcpy(at(0x32a5dc), steady, sizeof steady);
    memcpy(at(0x32a510), cap_scale, sizeof cap_scale);
    memcpy(at(0x32a64d), cap_max, sizeof cap_max);
    wr32(0x4212d4, 0x39d1b717u);   /* 0.0004f */
    wr32(0x4212d8, 0x3a03126fu);   /* 0.0005f */
    wr32(0x42142c, 0x4a1ba3bdu);   /* 2549999.25f */
    memcpy(before, mem, sizeof mem);
}

/* Bytes that differ from the planted image outside [va, va + n) ranges listed in keep. */
static int changed_outside(const uint32_t (*keep)[2], int nkeep) {
    int n = 0;
    for (uint32_t i = 0; i < SIZE; i++) {
        if (mem[i] == before[i]) continue;
        int inside = 0;
        for (int k = 0; k < nkeep; k++) inside |= BASE + i >= keep[k][0] && BASE + i < keep[k][0] + keep[k][1];
        n += !inside;
    }
    return n;
}

static int run(const DrawDistOptions *o, char *report, size_t cap) {
    return drawdist_install_at(o, bias, report, cap);
}

int main(void) {
    bias = (intptr_t)((uintptr_t)mem - BASE);
    char report[512];
    float f = -1;

    /* Parsing. */
    CHECK(drawdist_parse_factor("2", DRAWDIST_MIN, DRAWDIST_MAX, &f) && f == 2.0f, "factor 2 parsed");
    CHECK(drawdist_parse_factor("1.0", DRAWDIST_MIN, DRAWDIST_MAX, &f) && f == 1.0f, "factor 1.0 parsed (lower bound)");
    CHECK(drawdist_parse_factor("4", DRAWDIST_MIN, DRAWDIST_MAX, &f) && f == 4.0f, "factor 4 parsed (upper bound)");
    CHECK(drawdist_parse_factor("2.5", DRAWDIST_MIN, DRAWDIST_MAX, &f) && f == 2.5f, "factor 2.5 parsed");
    f = 7;
    CHECK(!drawdist_parse_factor("0.99", DRAWDIST_MIN, DRAWDIST_MAX, &f) && !drawdist_parse_factor("4.01", DRAWDIST_MIN, DRAWDIST_MAX, &f) &&
              !drawdist_parse_factor("", DRAWDIST_MIN, DRAWDIST_MAX, &f) && !drawdist_parse_factor("2x", DRAWDIST_MIN, DRAWDIST_MAX, &f) &&
              !drawdist_parse_factor("abc", DRAWDIST_MIN, DRAWDIST_MAX, &f) && !drawdist_parse_factor("nan", DRAWDIST_MIN, DRAWDIST_MAX, &f) &&
              !drawdist_parse_factor("inf", DRAWDIST_MIN, DRAWDIST_MAX, &f) && !drawdist_parse_factor("-2", DRAWDIST_MIN, DRAWDIST_MAX, &f) &&
              f == 7,
          "factor rejects out of range and junk, value unchanged");

    {
        char *argv[] = {"xbrun", "--draw-distance", "2", "--prop-detail", "8", "--fog-breathing", "--frames", "--prop-detail"};
        int argc = 8, i = 1;
        DrawDistOptions o = DRAWDIST_DEFAULTS;
        CHECK(o.steady_fog == 1 && o.draw_distance == 1.0f && o.prop_detail == 1.0f, "defaults: steady fog, 1x, 1x");
        CHECK(drawdist_parse_arg(argc, argv, &i, &o) == 1 && i == 2 && o.draw_distance == 2.0f, "--draw-distance 2 consumed");
        i = 3;
        CHECK(drawdist_parse_arg(argc, argv, &i, &o) == 1 && i == 4 && o.prop_detail == 8.0f, "--prop-detail 8 consumed");
        i = 5;
        CHECK(drawdist_parse_arg(argc, argv, &i, &o) == 1 && i == 5 && o.steady_fog == 0, "--fog-breathing turns steady fog off");
        i = 6;
        CHECK(drawdist_parse_arg(argc, argv, &i, &o) == 0 && i == 6, "other options not consumed");
        i = 7;
        CHECK(drawdist_parse_arg(argc, argv, &i, &o) == -1, "--prop-detail without a value: error");
        char *bad[] = {"xbrun", "--draw-distance", "5", "--prop-detail", "0.5"};
        i = 1;
        CHECK(drawdist_parse_arg(5, bad, &i, &o) == -1 && o.draw_distance == 2.0f, "--draw-distance 5: error, value kept");
        i = 3;
        CHECK(drawdist_parse_arg(5, bad, &i, &o) == -1 && o.prop_detail == 8.0f, "--prop-detail 0.5: error, value kept");
    }

    /* Defaults: only the low-fps branch becomes two nops. */
    {
        DrawDistOptions o = DRAWDIST_DEFAULTS;
        plant();
        CHECK(run(&o, report, sizeof report) == 1, "defaults: 1 patch applied");
        CHECK(*at(0x32a5e7) == 0x90 && *at(0x32a5e8) == 0x90, "steady fog: jbe 0x32a660 -> nop nop");
        static const uint32_t keep[][2] = {{0x32a5e7, 2}};
        CHECK(changed_outside(keep, 1) == 0, "defaults: nothing else changed");
        CHECK(!strcmp(report, "[drawdist] applied: steady fog (0032a5e7); skipped: none"), "defaults report: '%s'", report);
    }

    /* --fog-breathing alone: nothing written. */
    {
        DrawDistOptions o = {0, 1.0f, 1.0f};
        plant();
        CHECK(run(&o, report, sizeof report) == 0 && !memcmp(mem, before, sizeof mem), "--fog-breathing: memory untouched");
        CHECK(strstr(report, "applied: none") && strstr(report, "fog breathing kept"), "--fog-breathing report: '%s'", report);
    }

    /* --draw-distance 2 --prop-detail 4. */
    {
        DrawDistOptions o = {1, 2.0f, 4.0f};
        plant();
        CHECK(run(&o, report, sizeof report) == 3, "draw 2, prop 4: 3 patches applied");
        CHECK(rd32(0x32a514) == (uint32_t)(uintptr_t)&drawdist_cap_scale && drawdist_cap_scale == 3.0f,
              "cap scale operand -> runtime float 3.0 (%08x, %g)", rd32(0x32a514), drawdist_cap_scale);
        CHECK(rd32(0x32a651) == (uint32_t)(uintptr_t)&drawdist_cap_max && drawdist_cap_max == 600.0f,
              "cap max operand -> runtime float 600 (%08x, %g)", rd32(0x32a651), drawdist_cap_max);
        CHECK(rdf(0x4212d4) == 0.0004f / 16 && rdf(0x4212d8) == 0.0005f / 16, "prop cull/fade start / 16 (%g, %g)",
              rdf(0x4212d4), rdf(0x4212d8));
        CHECK(rdf(0x42142c) == 2549999.25f * 16, "prop fade scale x 16 (%g)", rdf(0x42142c));
        float full = (rdf(0x4212d8) - rdf(0x4212d4)) * rdf(0x42142c);
        CHECK(full > 254.9f && full < 255.1f, "fade still reaches alpha 255 at the fade end (%g)", full);
        static const uint32_t keep[][2] = {{0x32a5e7, 2}, {0x32a514, 4}, {0x32a651, 4}, {0x4212d4, 8}, {0x42142c, 4}};
        CHECK(changed_outside(keep, 5) == 0, "draw 2, prop 4: nothing outside the patch sites changed");
        CHECK(strstr(report, "draw distance 2.00x (0032a514, 0032a651)") && strstr(report, "prop detail 4.00x (004212d4, 0042142c)"),
              "report: '%s'", report);
        /* Applied again: the sites no longer hold the shipped bytes, so nothing is rewritten. */
        memcpy(before, mem, sizeof mem);
        CHECK(run(&o, report, sizeof report) == 0 && !memcmp(mem, before, sizeof mem), "second application writes nothing");
    }

    /* One differing byte at the second site of a patch: neither site of that patch is written. */
    {
        DrawDistOptions o = {1, 3.0f, 2.0f};
        plant();
        *at(0x32a650) = 0x11;           /* cap max operand's low byte */
        wr32(0x42142c, 0x4a1ba3beu);    /* fade scale off by one ulp */
        memcpy(before, mem, sizeof mem);
        CHECK(run(&o, report, sizeof report) == 1, "mismatches: only steady fog applied");
        CHECK(!memcmp(at(0x32a510), cap_scale, sizeof cap_scale), "draw distance: cap scale left as shipped");
        CHECK(rd32(0x4212d4) == 0x39d1b717u && rd32(0x4212d8) == 0x3a03126fu, "prop detail: cull constants left as shipped");
        static const uint32_t keep[][2] = {{0x32a5e7, 2}};
        CHECK(changed_outside(keep, 1) == 0, "mismatches: nothing else changed");
        CHECK(strstr(report, "skipped: draw distance 3.00x (bytes differ), prop detail 2.00x (bytes differ)") != NULL,
              "mismatch report: '%s'", report);
    }

    /* Steady fog on a different branch byte: skipped. */
    {
        DrawDistOptions o = DRAWDIST_DEFAULTS;
        plant();
        *at(0x32a5e8) = 0x78;
        memcpy(before, mem, sizeof mem);
        CHECK(run(&o, report, sizeof report) == 0 && !memcmp(mem, before, sizeof mem), "steady fog mismatch: memory untouched");
        CHECK(strstr(report, "skipped: steady fog (bytes differ)") != NULL, "steady fog mismatch report: '%s'", report);
    }

    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
