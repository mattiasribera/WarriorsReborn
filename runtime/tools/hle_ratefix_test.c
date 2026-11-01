/* Game-independent rate-fix test: a host buffer stands in for guest memory. Checks that a fix
 * whose signature matches is applied, that a mismatching one is left byte-for-byte untouched and
 * reported as skipped, that --no-rate-fixes turns every fix off, and that each entry of the
 * title's fix table applies cleanly to a planted copy of its own instruction bytes. */
#include "../src/hle_ratefix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* movss xmm0, [0x0041b658]: the lockpick update's per-call angle step (0.1f). */
static const uint8_t lockpick_bytes[] = {0xf3, 0x0f, 0x10, 0x05, 0x58, 0xb6, 0x41, 0x00};

int main(void) {
    enum { BASE = 0x00100000u, SIZE = 0x1000 };
    static uint8_t mem[SIZE], before[SIZE];
    intptr_t bias = (intptr_t)((uintptr_t)mem - BASE);
    char report[512];

    const RateFix fakes[] = {
        {"widget_a", BASE + 0x10, {0xf3, 0x0f, 0x10, 0x05, 0x58, 0xb6, 0x41, 0x00}, 8, {{BASE + 0x14, 4, 0x0041bfecu}}, 0},
        {"widget_b", BASE + 0x40, {0xd8, 0x05, 0x00, 0x01, 0x02, 0x03}, 6, {{BASE + 0x42, 4, 0x11223344u}}, 0},
        /* cmp eax, 0x1e; jb +5; mov eax, 0x1e: both immediates change together (1 and 4 bytes). */
        {"clamp", BASE + 0x80, {0x83, 0xf8, 0x1e, 0x72, 0x05, 0xb8, 0x1e, 0x00, 0x00, 0x00}, 10,
         {{BASE + 0x82, 1, 0x0f}, {BASE + 0x86, 4, 0x0f}}, 0},
    };
    memset(mem, 0xcc, sizeof mem);
    memcpy(mem + 0x10, lockpick_bytes, sizeof lockpick_bytes);
    memcpy(mem + 0x40, (const uint8_t[]){0xd8, 0x05, 0x00, 0x01, 0x02, 0x99}, 6);   /* last byte differs */
    memcpy(mem + 0x80, fakes[2].sig, 10);
    memcpy(before, mem, sizeof mem);

    /* --no-rate-fixes: the option is recognized and nothing is written. */
    int enabled = 1;
    CHECK(ratefix_parse_arg("--no-rate-fixes", &enabled) == 1 && enabled == 0, "--no-rate-fixes parsed, disables");
    CHECK(ratefix_parse_arg("--rate-fixes", &enabled) == 1 && enabled == 1, "--rate-fixes parsed, enables");
    CHECK(ratefix_parse_arg("--frames", &enabled) == 0 && enabled == 1, "other options not consumed");
    CHECK(ratefix_apply(fakes, 3, 0, bias, report, sizeof report) == 0, "disabled: 0 applied");
    CHECK(!memcmp(mem, before, sizeof mem), "disabled: memory untouched");
    CHECK(!strncmp(report, "[ratefix] disabled", 18), "disabled report: '%s'", report);

    /* Enabled: the matching fix is applied, the mismatching one is skipped and reported. */
    CHECK(ratefix_apply(fakes, 2, 1, bias, report, sizeof report) == 1, "enabled: 1 applied");
    CHECK(rd32(mem + 0x14) == 0x0041bfecu, "widget_a operand 0x41b658 -> 0x41bfec (got %08x)", rd32(mem + 0x14));
    CHECK(!memcmp(mem, before, 0x14) && !memcmp(mem + 0x18, before + 0x18, 0x80 - 0x18),
          "only widget_a's 4 operand bytes changed below the clamp");
    CHECK(!memcmp(mem + 0x40, before + 0x40, 6), "widget_b (mismatch) untouched");
    CHECK(strstr(report, "applied: widget_a") != NULL, "report lists widget_a as applied: '%s'", report);
    CHECK(strstr(report, "skipped: widget_b") != NULL, "report lists widget_b as skipped");

    /* A fix with two writes of different widths: both applied, nothing else touched. */
    CHECK(ratefix_apply(fakes + 2, 1, 1, bias, report, sizeof report) == 1, "clamp: applied");
    static const uint8_t clamp_new[] = {0x83, 0xf8, 0x0f, 0x72, 0x05, 0xb8, 0x0f, 0x00, 0x00, 0x00};
    CHECK(!memcmp(mem + 0x80, clamp_new, 10), "clamp: cmp imm8 and mov imm32 both 0x1e -> 0x0f");
    CHECK(!memcmp(mem + 0x8a, before + 0x8a, SIZE - 0x8a), "clamp: bytes after it untouched");
    /* The same fix on bytes that differ in one place writes neither immediate. */
    memcpy(mem + 0x80, fakes[2].sig, 10);
    mem[0x83] = 0x73;
    CHECK(ratefix_apply(fakes + 2, 1, 1, bias, report, sizeof report) == 0 && mem[0x82] == 0x1e && mem[0x86] == 0x1e,
          "clamp mismatch: neither write applied");

    /* Applying again: widget_a no longer matches its signature (already patched), so it is skipped. */
    CHECK(ratefix_apply(fakes, 1, 1, bias, report, sizeof report) == 0, "second application skips the patched site");
    CHECK(rd32(mem + 0x14) == 0x0041bfecu, "patched operand kept");

    /* The title's table: every entry applies to a planted copy of its bytes and writes its values. */
    CHECK(ratefix_count >= 1, "title table has %d fixes", ratefix_count);
    for (int i = 0; i < ratefix_count; i++) {
        const RateFix *f = &ratefix_table[i];
        static uint8_t page[64];
        memset(page, 0xcc, sizeof page);
        memcpy(page + 8, f->sig, f->sig_len);
        intptr_t b = (intptr_t)((uintptr_t)page + 8 - f->sig_va);
        int inside = f->sig_len >= 4 && f->sig_len <= RATEFIX_SIG_MAX && f->writes[0].width;
        for (int w = 0; w < RATEFIX_WRITES_MAX && f->writes[w].width; w++) {
            const RateFixWrite *x = &f->writes[w];
            inside &= (x->width == 1 || x->width == 4) && x->va >= f->sig_va && x->va + x->width <= f->sig_va + f->sig_len;
        }
        CHECK(inside, "%s: every write inside its %d checked bytes at %08x", f->name, f->sig_len, f->sig_va);
        int ok = inside && ratefix_apply(f, 1, 1, b, report, sizeof report) == 1;
        for (int w = 0; ok && w < RATEFIX_WRITES_MAX && f->writes[w].width; w++) {
            const RateFixWrite *x = &f->writes[w];
            const uint8_t *at = page + 8 + (x->va - f->sig_va);
            ok = x->width == 4 ? rd32(at) == x->value : *at == (uint8_t)x->value;
        }
        CHECK(ok, "%s applies its writes", f->name);
    }

    /* The fixes this table must hold, by address and new value. */
    static const struct { uint32_t sig_va, va; uint8_t width; uint32_t value; } want[] = {
        {0x000e0fd0u, 0x000e0fd4u, 4, 0x0041bfecu},   /* lockpick: 0.1f -> 0.05f */
        {0x000e036eu, 0x000e0372u, 4, 0x00424de0u},   /* instruction arrow fall: -0.5f -> -0.25f */
        {0x000e038cu, 0x000e0390u, 4, 0x0040ffc8u},   /* instruction arrow rise: 2.0f -> 1.0f */
        {0x00193b5au, 0x00193b5cu, 1, 0x0fu},         /* graffiti cursor dt clamp: cmp 30 -> 15 */
        {0x00193b5au, 0x00193b60u, 4, 0x0fu},         /* graffiti cursor dt clamp: mov 30 -> 15 */
        {0x00193cbau, 0x00193cc0u, 1, 0x09u},         /* graffiti off-path frames: > 4 -> > 9 */
    };
    for (size_t k = 0; k < sizeof want / sizeof want[0]; k++) {
        int found = 0;
        for (int i = 0; i < ratefix_count && !found; i++)
            for (int w = 0; w < RATEFIX_WRITES_MAX; w++) {
                const RateFixWrite *x = &ratefix_table[i].writes[w];
                if (ratefix_table[i].sig_va == want[k].sig_va && x->va == want[k].va && x->width == want[k].width &&
                    x->value == want[k].value)
                    found = 1;
            }
        CHECK(found, "table writes %08x (width %d) = %08x", want[k].va, want[k].width, want[k].value);
    }
    CHECK(ratefix_table[0].sig_len == sizeof lockpick_bytes && !memcmp(ratefix_table[0].sig, lockpick_bytes, 8),
          "lockpick signature is movss xmm0, [0x41b658]");

    /* Spray time (comfort): the graffiti difficulty rows at 0x45d57c, {can ms, penalty ms,
     * cursor speed} x 3. A fake .data page holds them at their address. */
    static const uint32_t rows[9] = {13000, 500, 0x3d4ccccdu, 11000, 600, 0x3d8f5c29u, 9500, 500, 0x3d958106u};
    static uint8_t data[0x100], data_before[0x100];
    intptr_t db = (intptr_t)((uintptr_t)data + 0x40 - RATEFIX_SPRAY_VA);
    memset(data, 0xcc, sizeof data);
    memcpy(data + 0x40, rows, sizeof rows);
    memcpy(data_before, data, sizeof data);
    int pct = -2;
    CHECK(ratefix_parse_spray("110", &pct) == 1 && pct == 110, "--spray-time 110 parsed");
    CHECK(ratefix_parse_spray("100", &pct) == 1 && pct == 100, "--spray-time 100 parsed");
    CHECK(ratefix_parse_spray("200", &pct) == 1 && pct == 200, "--spray-time 200 parsed");
    pct = 7;
    CHECK(!ratefix_parse_spray("99", &pct) && !ratefix_parse_spray("201", &pct) && !ratefix_parse_spray("-110", &pct) &&
              !ratefix_parse_spray("110%", &pct) && !ratefix_parse_spray("1.5", &pct) && !ratefix_parse_spray("", &pct) &&
              !ratefix_parse_spray("abc", &pct) && !ratefix_parse_spray("99999999999", &pct) && pct == 7,
          "--spray-time rejects out of range and junk, value unchanged");
    CHECK(RATEFIX_SPRAY_DEFAULT == 110, "default spray time is 110%%");

    RateFix spray;
    CHECK(ratefix_spray_fix(100, &spray) == 0, "spray time 100%%: no fix built");
    /* Only entries whose bytes lie in the fake page are applied here (the others' addresses are
     * outside it). */
    RateFix all[64];
    int nall = ratefix_build(100, 1, all, 64), spray_entries = 0;
    for (int i = 0; i < nall; i++) spray_entries += all[i].sig_va == RATEFIX_SPRAY_VA || strstr(all[i].name, "spray") != NULL;
    CHECK(nall == ratefix_count && !spray_entries, "build(100): only the rate fixes (%d), no spray entry", nall);
    CHECK(!memcmp(data, data_before, sizeof data), "spray time 100%%: durations untouched");

    nall = ratefix_build(RATEFIX_SPRAY_DEFAULT, 1, all, 64);
    CHECK(nall == ratefix_count + 1 && all[nall - 1].sig_va == RATEFIX_SPRAY_VA, "build(110) adds the spray entry last");
    const RateFix *sp = &all[nall - 1];
    CHECK(sp->sig_len <= RATEFIX_SIG_MAX && sp->writes[2].width == 4 && sp->writes[2].va + 4 <= sp->sig_va + sp->sig_len,
          "spray entry: three 4-byte writes inside its %d checked bytes", sp->sig_len);
    CHECK(ratefix_apply(sp, 1, 1, db, report, sizeof report) == 1, "spray time 110%%: applied");
    static const uint32_t want_rows[9] = {14300, 500, 0x3d4ccccdu, 12100, 600, 0x3d8f5c29u, 10450, 500, 0x3d958106u};
    CHECK(!memcmp(data + 0x40, want_rows, sizeof want_rows), "durations 13000/11000/9500 -> 14300/12100/10450 ms (got %u/%u/%u)",
          rd32(data + 0x40), rd32(data + 0x4c), rd32(data + 0x58));
    CHECK(!memcmp(data, data_before, 0x40) && !memcmp(data + 0x64, data_before + 0x64, sizeof data - 0x64),
          "spray time: bytes around the table untouched");
    CHECK(strstr(report, "applied: spray time 110% (0045d57c)") != NULL, "report names spray time 110%%: '%s'", report);
    CHECK(ratefix_apply(sp, 1, 1, db, report, sizeof report) == 0 && rd32(data + 0x40) == 14300,
          "spray time: second application skipped (not compounded)");
    memcpy(data, data_before, sizeof data);
    CHECK(ratefix_apply(sp, 1, 0, db, report, sizeof report) == 0 && !memcmp(data, data_before, sizeof data),
          "--no-rate-fixes: spray time off too");

    memcpy(data, data_before, sizeof data);
    CHECK(ratefix_spray_fix(200, &spray) == 1 && ratefix_apply(&spray, 1, 1, db, report, sizeof report) == 1 &&
              rd32(data + 0x40) == 26000 && rd32(data + 0x4c) == 22000 && rd32(data + 0x58) == 19000,
          "spray time 200%%: 26000/22000/19000 ms");
    memcpy(data, data_before, sizeof data);
    data[0x44] = 0xf5;   /* a penalty that differs: the row is not the shipped one */
    CHECK(ratefix_spray_fix(110, &spray) == 1 && ratefix_apply(&spray, 1, 1, db, report, sizeof report) == 0 &&
              rd32(data + 0x40) == 13000 && strstr(report, "skipped: spray time"),
          "spray time: mismatching table left untouched and skipped");

    /* World-side fixes follow --sim60. The graffiti tag update FUN_001939d0 is reached only from
     * the world update (0x16d8d0 -> 0x17c980 -> 0x179b10 -> 0x193ea0), which runs on every second
     * 1/60 s step (30 Hz) unless --sim60 is on: with the 30 Hz world the Xbox bytes are right. */
    int graffiti_entries = 0, hud_sim60_only = 0;
    for (int i = 0; i < ratefix_count; i++) {
        int graffiti = strstr(ratefix_table[i].name, "graffiti") != NULL;
        graffiti_entries += graffiti && ratefix_table[i].sim60_only;
        hud_sim60_only += !graffiti && ratefix_table[i].sim60_only;
    }
    CHECK(graffiti_entries == 2 && hud_sim60_only == 0, "graffiti cursor and off-path are sim60-only, HUD fixes are not");
    nall = ratefix_build(RATEFIX_SPRAY_DEFAULT, 0, all, 64);
    int has_graffiti = 0;
    for (int i = 0; i < nall; i++) has_graffiti += strstr(all[i].name, "graffiti") != NULL;
    CHECK(nall == ratefix_count - 2 + 1 && !has_graffiti, "build(110, 30 Hz world): HUD fixes + spray, no graffiti (%d)", nall);

    /* The whole pass on a fake image holding every checked site at its address. */
    enum { IMG_LO = 0x000e0000u, IMG_HI = 0x00460000u };
    uint8_t *img = malloc(IMG_HI - IMG_LO);
    intptr_t ib = (intptr_t)((uintptr_t)img - IMG_LO);
    for (int mode = 0; mode < 2 && img; mode++) {
        memset(img, 0xcc, IMG_HI - IMG_LO);
        for (int i = 0; i < ratefix_count; i++) memcpy(img + (ratefix_table[i].sig_va - IMG_LO), ratefix_table[i].sig, ratefix_table[i].sig_len);
        memcpy(img + (RATEFIX_SPRAY_VA - IMG_LO), rows, sizeof rows);
        static const uint8_t cursor_orig[] = {0x83, 0xf8, 0x1e, 0x72, 0x05, 0xb8, 0x1e, 0x00, 0x00, 0x00};
        static const uint8_t cursor_new[] = {0x83, 0xf8, 0x0f, 0x72, 0x05, 0xb8, 0x0f, 0x00, 0x00, 0x00};
        int n = ratefix_install_at(1, RATEFIX_SPRAY_DEFAULT, mode, ib, report, sizeof report);
        const uint8_t *cur = img + (0x00193b5au - IMG_LO), *off = img + (0x00193cc0u - IMG_LO);
        int hud = rd32(img + (0x000e0fd4u - IMG_LO)) == 0x0041bfecu && rd32(img + (0x000e0372u - IMG_LO)) == 0x00424de0u &&
                  rd32(img + (0x000e0390u - IMG_LO)) == 0x0040ffc8u && rd32(img + (RATEFIX_SPRAY_VA - IMG_LO)) == 14300;
        if (!mode) {
            CHECK(n == 4 && hud, "30 Hz world: lockpick, arrows and spray time applied (%d)", n);
            CHECK(!memcmp(cur, cursor_orig, 10) && *off == 0x04, "30 Hz world: graffiti cursor clamp and off-path bytes untouched");
            CHECK(strstr(report, "graffiti") && !strstr(strstr(report, "applied: "), "graffiti cursor (") &&
                      strstr(report, "sim60 only") != NULL,
                  "30 Hz world report names the graffiti fixes as sim60 only: '%s'", report);
            CHECK(strstr(report, "skipped: none") != NULL, "30 Hz world: nothing reported as mismatching");
        } else {
            CHECK(n == 6 && hud, "--sim60: all six applied (%d)", n);
            CHECK(!memcmp(cur, cursor_new, 10) && *off == 0x09, "--sim60: graffiti clamp 30 -> 15 and off-path 4 -> 9");
            CHECK(strstr(report, "graffiti cursor (00193b5c)") && strstr(report, "graffiti off-path (00193cc0)") &&
                      !strstr(report, "sim60 only"),
                  "--sim60 report lists the graffiti fixes as applied: '%s'", report);
        }
    }
    free(img);
    char tmp[512];
    CHECK(ratefix_install_at(0, RATEFIX_SPRAY_DEFAULT, 1, 0, tmp, sizeof tmp) == 0 && !strncmp(tmp, "[ratefix] disabled", 18),
          "--no-rate-fixes with --sim60: nothing applied");

    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
