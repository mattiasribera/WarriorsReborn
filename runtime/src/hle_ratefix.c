/* Rate fixes for the 60 Hz presentation (see hle_ratefix.h). */
#include "hle_ratefix.h"
#include <stdio.h>
#include <string.h>

/* Title fixes. Each entry: what it steps, checked bytes (disassembly), operands and new values.
 * .text VA = file offset + 0x10000; .rdata VA 0x40fb20 = file offset 0x402000. Constants named
 * by address are existing read-only floats in .rdata. */
const RateFix ratefix_table[] = {
    /* Lockpick widget (vtable 0x4243e8), update = slot 5 at 0xe0fd0: angle(+0x3ac) +=
     * speed[i](1/2/3) * [0x41b658] (0.1f rad) per call, wrapped at 2*pi. movss xmm0,[0x41b658]
     * -> [0x41bfec] (0.05f). */
    {"lockpick", 0x000e0fd0u, {0xf3, 0x0f, 0x10, 0x05, 0x58, 0xb6, 0x41, 0x00}, 8, {{0x000e0fd4u, 4, 0x0041bfecu}}, 0},
    /* Instruction arrow (HUDEnableInstArrow; vtable 0x424260 at HUD+0xf4ec), update = slot 5 at
     * 0xe0330: offset(+0xe4) += step(+0xe0) per call, step = [0x410170] (-0.5f) at the top and
     * [0x410414] (2.0f) at the bottom. -> [0x424de0] (-0.25f) and [0x40ffc8] (1.0f). */
    {"arrow fall", 0x000e036eu, {0xf3, 0x0f, 0x10, 0x05, 0x70, 0x01, 0x41, 0x00}, 8, {{0x000e0372u, 4, 0x00424de0u}}, 0},
    {"arrow rise", 0x000e038cu, {0xf3, 0x0f, 0x10, 0x05, 0x14, 0x04, 0x41, 0x00}, 8, {{0x000e0390u, 4, 0x0040ffc8u}}, 0},
    /* Graffiti cursor (CNSPlayerTag update FUN_001939d0, once per world update): cursor += stick *
     * speed (0.05-0.073 /ms) * min(now - last, 30) ms. The tag update is reached only from the
     * world update (0x16d8d0 -> 0x17c980 -> 0x179b10 -> 0x193ea0), 30 Hz by default: every
     * update (33 ms) is clamped to 30 ms, as on the Xbox, so the bytes stay. With --sim60 an
     * update is 16-17 ms, 11% faster per second: cmp eax,0x1e; jb; mov eax,0x1e -> 15.
     * sim60 only. */
    {"graffiti cursor", 0x00193b5au, {0x83, 0xf8, 0x1e, 0x72, 0x05, 0xb8, 0x1e, 0x00, 0x00, 0x00}, 10,
     {{0x00193b5cu, 1, 0x0fu}, {0x00193b60u, 4, 0x0fu}}, 1},
    /* Graffiti off-path budget: +0x4fc counts world updates with the stick moved off the outline
     * (never reset on returning); above 4 the cursor snaps back and input freezes for 500-600 ms
     * while the can keeps emptying. With --sim60: cmp dword [esi+0x4fc],4 -> 9 (the same 167 ms
     * at 60 updates per second). sim60 only, as the cursor. */
    {"graffiti off-path", 0x00193cbau, {0x83, 0xbe, 0xfc, 0x04, 0x00, 0x00, 0x04, 0x0f, 0x86}, 9,
     {{0x00193cc0u, 1, 0x09u}}, 1},
};
const int ratefix_count = (int)(sizeof ratefix_table / sizeof ratefix_table[0]);

/* Appends "s" to the report, keeping it NUL-terminated and within cap. */
static void put(char *report, size_t cap, size_t *len, const char *s) {
    int n = snprintf(report + *len, cap - *len, "%s", s);
    if (n > 0) *len += (size_t)n < cap - *len ? (size_t)n : cap - *len - 1;
}

/* The fix is well formed: 1-2 writes of width 1 or 4, each inside the checked bytes. */
static int fix_valid(const RateFix *f) {
    if (f->sig_len > RATEFIX_SIG_MAX || !f->writes[0].width) return 0;
    for (int w = 0; w < RATEFIX_WRITES_MAX && f->writes[w].width; w++) {
        const RateFixWrite *x = &f->writes[w];
        if ((x->width != 1 && x->width != 4) || x->va < f->sig_va || x->va + x->width > f->sig_va + f->sig_len) return 0;
    }
    return 1;
}

int ratefix_apply(const RateFix *t, int n, int enabled, intptr_t bias, char *report, size_t cap) {
    size_t len = 0;
    if (!cap) return 0;
    report[0] = 0;
    if (!enabled) {
        put(report, cap, &len, "[ratefix] disabled (--no-rate-fixes): title runs its per-frame HUD steps and spray time unchanged");
        return 0;
    }
    int applied = 0;
    char done[384] = "", skipped[384] = "";
    size_t dlen = 0, slen = 0;
    for (int i = 0; i < n; i++) {
        const RateFix *f = &t[i];
        const uint8_t *sig_at = (const uint8_t *)(uintptr_t)(f->sig_va + bias);
        char item[64];
        if (!fix_valid(f) || memcmp(sig_at, f->sig, f->sig_len)) {
            snprintf(item, sizeof item, "%s%s (%08x)", slen ? ", " : "", f->name, f->sig_va);
            put(skipped, sizeof skipped, &slen, item);
            continue;
        }
        for (int w = 0; w < RATEFIX_WRITES_MAX && f->writes[w].width; w++)
            memcpy((uint8_t *)(uintptr_t)(f->writes[w].va + bias), &f->writes[w].value, f->writes[w].width);
        snprintf(item, sizeof item, "%s%s (%08x)", dlen ? ", " : "", f->name, f->writes[0].va);
        put(done, sizeof done, &dlen, item);
        applied++;
    }
    put(report, cap, &len, "[ratefix] applied: ");
    put(report, cap, &len, dlen ? done : "none");
    put(report, cap, &len, "; skipped: ");
    put(report, cap, &len, slen ? skipped : "none");
    return applied;
}

int ratefix_parse_arg(const char *arg, int *enabled) {
    if (!strcmp(arg, "--no-rate-fixes")) { *enabled = 0; return 1; }
    if (!strcmp(arg, "--rate-fixes")) { *enabled = 1; return 1; }
    return 0;
}

int ratefix_parse_spray(const char *s, int *percent) {
    int v = 0, n = 0;
    for (; s[n]; n++) {
        if (s[n] < '0' || s[n] > '9' || n >= 3) return 0;
        v = v * 10 + (s[n] - '0');
    }
    if (!n || v < RATEFIX_SPRAY_MIN || v > RATEFIX_SPRAY_MAX) return 0;
    *percent = v;
    return 1;
}

/* Graffiti difficulty rows (.data 0x45d57c, file offset 0x4504fc), picked by FUN_00192f30 as
 * 0x45d570 + 12 * difficulty (1..3): {spray can ms, input freeze after a snap-back ms, cursor
 * speed per ms}. The can (FUN_00193300) empties over that many ms of the title clock
 * [[0x4aca1c]+0x40], then the next can in the inventory starts or the tag fails. */
static const uint32_t spray_rows[9] = {13000, 500, 0x3d4ccccdu /* 0.05f */, 11000, 600, 0x3d8f5c29u /* 0.07f */,
                                       9500, 500, 0x3d958106u /* 0.073f */};

int ratefix_spray_fix(int percent, RateFix *out) {
    static char name[32];
    if (percent == 100 || percent < RATEFIX_SPRAY_MIN || percent > RATEFIX_SPRAY_MAX) return 0;
    memset(out, 0, sizeof *out);
    snprintf(name, sizeof name, "spray time %d%%", percent);
    out->name = name;
    out->sig_va = RATEFIX_SPRAY_VA;
    out->sig_len = (uint8_t)sizeof spray_rows;
    memcpy(out->sig, spray_rows, sizeof spray_rows);
    for (int r = 0; r < 3; r++) {
        uint32_t ms = spray_rows[3 * r];
        out->writes[r] = (RateFixWrite){RATEFIX_SPRAY_VA + 12u * (uint32_t)r, 4, (ms * (uint32_t)percent + 50u) / 100u};
    }
    return 1;
}

int ratefix_build(int spray_percent, int sim60, RateFix *out, int cap) {
    int n = 0;
    for (int i = 0; i < ratefix_count && n < cap; i++)
        if (sim60 || !ratefix_table[i].sim60_only) out[n++] = ratefix_table[i];
    if (n < cap && ratefix_spray_fix(spray_percent, &out[n])) n++;
    return n;
}

int ratefix_install_at(int enabled, int spray_percent, int sim60, intptr_t bias, char *report, size_t cap) {
    RateFix fixes[32];
    int count = ratefix_build(spray_percent, sim60, fixes, (int)(sizeof fixes / sizeof fixes[0]));
    int n = ratefix_apply(fixes, count, enabled, bias, report, cap);
    if (!enabled || sim60) return n;
    /* 30 Hz world: name the world-side fixes left out (the title's bytes are right there). */
    size_t len = strlen(report);
    int first = 1;
    put(report, cap, &len, "; sim60 only (30 Hz world, title bytes kept): ");
    for (int i = 0; i < ratefix_count; i++)
        if (ratefix_table[i].sim60_only) {
            if (!first) put(report, cap, &len, ", ");
            put(report, cap, &len, ratefix_table[i].name);
            first = 0;
        }
    return n;
}

int hle_ratefix_install(int enabled, int spray_percent, int sim60) {
    char report[1024];
    int n = ratefix_install_at(enabled, spray_percent, sim60, 0, report, sizeof report);
    printf("%s\n", report);
    return n;
}
