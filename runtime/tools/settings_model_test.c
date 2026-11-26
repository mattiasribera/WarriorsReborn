/* settings_model: the table's own consistency, values, expressions,
 * the command line, change classes and the pending file. The launcher's settings test checks the
 * table against profile.cpp, build_argv and apply_preset. */
#include "../src/settings_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static const char *joined(const SmArgv *a, int from) {
    static char buf[4096];
    size_t at = 0;
    buf[0] = 0;
    for (int i = from; i < a->n; i++) at += (size_t)snprintf(buf + at, sizeof buf - at, "%s%s", i > from ? "|" : "", a->a[i]);
    return buf;
}

/* The options after "<xbe> <disc>" of vals. */
static const char *opts(const SmValues *v) {
    static SmArgv a;
    sm_argv_free(&a);
    if (sm_build_argv(v, &a)) return "<oom>";
    return joined(&a, 3);
}

/* Each key named in an expression is a setting. */
static int expr_keys_known(const char *e) {
    while (*e) {
        char key[64];
        size_t n = 0;
        while (e[n] && !strchr("&|=!", e[n])) n++;
        if (n == 0 || n >= sizeof key) return 0;
        memcpy(key, e, n), key[n] = 0;
        if (sm_find(key) < 0) return 0;
        e += n;
        if (*e == '!' || *e == '=') {   /* the value list up to the next '&' or '|' */
            while (*e && *e != '&' && *e != '|') e++;
        }
        if (*e) e++;
    }
    return 1;
}

static void test_table(void) {
    int n = sm_count(), dup = 0, bad_default = 0, bad_expr = 0, menu_unlabelled = 0, menu_no_tab = 0, bad_cls = 0;
    for (int i = 0; i < n; i++) {
        const SmSetting *s = sm_setting(i);
        for (int j = 0; j < i; j++) dup += !strcmp(s->key, sm_setting(j)->key);
        if (!sm_valid(s, s->dflt)) bad_default++, printf("     default of %s not in its domain\n", s->key);
        if (!expr_keys_known(s->requires) || !expr_keys_known(s->arg_requires)) bad_expr++, printf("     bad expression in %s\n", s->key);
        if ((s->flags & SM_MENU) && !s->label) menu_unlabelled++, printf("     menu row %s has no label\n", s->key);
        if ((s->flags & SM_MENU) && s->tab == SM_TAB_NONE) menu_no_tab++;
        if (!strchr("abcd-", s->cls) || ((s->flags & SM_MENU) && s->cls == SM_BOOT)) bad_cls++, printf("     class of %s\n", s->key);
        if (sm_find(s->key) != i) dup++;
    }
    CHECK(n > 100 && !dup, "table: %d settings, keys unique", n);
    CHECK(!bad_default, "table: every default is in its domain");
    CHECK(!bad_expr, "table: every prerequisite names known settings");
    CHECK(!menu_unlabelled && !menu_no_tab, "table: every menu row has a tab and a label");
    CHECK(!bad_cls, "table: classes are a-d or '-' (never '-' in the menu)");
    CHECK(sm_find("no_such_key") < 0 && !sm_setting(-1) && !sm_setting(n), "table: unknown keys and indices");
    CHECK(sm_setting(sm_find("soft_particles_opt_in"))->arg == SM_ARG_ON, "table: soft particles under the launcher's key soft_particles_opt_in");
    CHECK(!strcmp(sm_tab_key(SM_TAB_EFFECTS), "effects") && !strcmp(sm_tab_key(SM_TAB_NONE), "none"), "table: tab keys");
}

static void test_values(void) {
    SmValues v;
    CHECK(!sm_values_init(&v), "values: init");
    CHECK(!strcmp(sm_get(&v, "bloom"), "false") && !strcmp(sm_get(&v, "sharp_fonts"), "true") &&
          !strcmp(sm_get(&v, "bloom_strength"), "-1") && !strcmp(sm_get(&v, "spray_time"), "110") &&
          !strcmp(sm_get(&v, "present"), "auto") && !strcmp(sm_get(&v, "resolution"), "") && !sm_get(&v, "nope"),
          "values: Profile() defaults");
    CHECK(!sm_set(&v, "bloom", "on") && !strcmp(sm_get(&v, "bloom"), "true") && !sm_set(&v, "bloom", "0") &&
          !strcmp(sm_get(&v, "bloom"), "false"), "values: bools normalized (on -> true, 0 -> false)");
    CHECK(sm_set(&v, "bloom", "maybe") == -2 && !strcmp(sm_get(&v, "bloom"), "false"), "values: a bad bool keeps the value");
    CHECK(!sm_set(&v, "msaa", "4") && sm_set(&v, "msaa", "3") == -2 && !strcmp(sm_get(&v, "msaa"), "4"), "values: int choices");
    CHECK(!sm_set(&v, "spray_time", "150") && sm_set(&v, "spray_time", "99") == -2 && sm_set(&v, "spray_time", "x") == -2,
          "values: int range");
    CHECK(!sm_set(&v, "bloom_strength", "1.50") && !strcmp(sm_get(&v, "bloom_strength"), "1.5") &&
          !sm_set(&v, "bloom_strength", "-1") && sm_set(&v, "bloom_strength", "9") == -2, "values: floats %%.3g, -1 = left out");
    CHECK(!sm_set(&v, "taa_upscale", "quality") && sm_set(&v, "taa_upscale", "ultra2") == -2, "values: enum choices");
    CHECK(!sm_set(&v, "volfog_quality", "") && !sm_set(&v, "volfog_quality", "low") && sm_set(&v, "volfog_quality", "max") == -2,
          "values: an enum with an empty choice");
    CHECK(sm_set(&v, "future_key", "1") == -1, "values: unknown key");
    CHECK(!sm_set(&v, "texture_pack", "C:\\Packs\\v3") && !strcmp(sm_get(&v, "texture_pack"), "C:\\Packs\\v3"), "values: paths kept as typed");
    SmValues c;
    CHECK(!sm_values_copy(&c, &v) && !strcmp(sm_get(&c, "texture_pack"), "C:\\Packs\\v3"), "values: copy");
    sm_values_free(&c);
    sm_values_free(&v);
}

static void test_expr(void) {
    SmValues v;
    sm_values_init(&v);
    CHECK(sm_expr(&v, "") && !sm_expr(&v, "rain_hd") && !sm_expr(&v, "rain_hd|damp_streets"), "expr: empty holds, false bools");
    sm_set(&v, "damp_streets", "true");
    CHECK(sm_expr(&v, "rain_hd|damp_streets") && !sm_expr(&v, "rain_hd&damp_streets"), "expr: or / and");
    CHECK(sm_expr(&v, "resolution=") && !sm_expr(&v, "texture_pack!=") && sm_expr(&v, "taa_upscale=off"), "expr: empty and equals");
    sm_set(&v, "hud_style", "hd-glass");
    CHECK(sm_expr(&v, "hud_style=hd/hd-glass") && !sm_expr(&v, "hud_style!=hd/hd-glass"), "expr: value lists");
    sm_set(&v, "ssao", "true");
    CHECK(!sm_expr(&v, "ssao&ssao_mode=gtao") && (sm_set(&v, "ssao_mode", "gtao"), sm_expr(&v, "ssao&ssao_mode=gtao")),
          "expr: gtao quality's prerequisite");
    CHECK(!sm_expr(&v, "nope") && !sm_expr(&v, "nope=1"), "expr: unknown keys are false");
    CHECK(sm_truthy("1") && sm_truthy("quality") && !sm_truthy("off") && !sm_truthy("0") && !sm_truthy("") && !sm_truthy("false"),
          "expr: truthy values");
    int hair = sm_find("hair");
    CHECK(!sm_available(&v, hair) && (sm_set(&v, "char_lighting", "true"), sm_available(&v, hair)), "expr: hair needs character lighting");
    sm_values_free(&v);
}

static void test_argv(void) {
    SmValues v;
    sm_values_init(&v);
    SmArgv a = {0};
    sm_build_argv(&v, &a);
    CHECK(a.n == 3 && !strcmp(a.a[0], "") && !strcmp(a.a[1], "default.xbe") && !strcmp(a.a[2], ""), "argv: defaults = the bare command");
    sm_argv_free(&a);
    sm_set(&v, "xbrun", "/g/xbrun");
    sm_set(&v, "disc", "/g/disc//");
    sm_build_argv(&v, &a);
    CHECK(!strcmp(joined(&a, 0), "/g/xbrun|/g/disc/default.xbe|/g/disc"), "argv: head, separators stripped (%s)", joined(&a, 0));
    sm_argv_free(&a);
    sm_set(&v, "disc", "C:\\");
    sm_build_argv(&v, &a);
    CHECK(!strcmp(joined(&a, 1), "C:\\default.xbe|C:\\"), "argv: a Windows drive root (%s)", joined(&a, 1));
    sm_argv_free(&a);

    /* bounce lighting: --ssgi <quality> only with GTAO on, the switch has no flag of its own */
    SmValues g;
    sm_values_init(&g);
    sm_set(&g, "ssgi", "true");
    sm_set(&g, "ssgi_strength", "1.5");
    sm_set(&g, "ssao", "true");
    CHECK(!strcmp(opts(&g), "--ssao") && !sm_available(&g, sm_find("ssgi")) && !sm_available(&g, sm_find("ssgi_quality")),
          "argv: ssgi left out and greyed without GTAO (%s)", opts(&g));
    sm_set(&g, "ssao_mode", "gtao");
    CHECK(!strcmp(opts(&g), "--ssao|--ssao-mode|gtao|--ssgi|medium|--ssgi-strength|1.5") && sm_available(&g, sm_find("ssgi_strength")),
          "argv: ssgi's default quality is written with GTAO (%s)", opts(&g));
    sm_set(&g, "ssgi", "false");
    CHECK(!strcmp(opts(&g), "--ssao|--ssao-mode|gtao") && sm_available(&g, sm_find("ssgi")) && !sm_available(&g, sm_find("ssgi_quality")),
          "argv: ssgi off passes nothing (%s)", opts(&g));
    sm_values_free(&g);

    sm_set(&v, "rain_puddles", "true");
    CHECK(!strcmp(opts(&v), ""), "argv: puddles left out without better rain");
    sm_set(&v, "rain_hd", "true");
    CHECK(!strcmp(opts(&v), "--rain-hd|--rain-hd-puddles"), "argv: puddles after better rain (%s)", opts(&v));
    sm_set(&v, "rain_hd", "false"), sm_set(&v, "rain_puddles", "false");
    sm_set(&v, "volfog", "true");
    CHECK(!strcmp(opts(&v), "--volfog|--volfog-quality|high"), "argv: volfog quality high when empty (%s)", opts(&v));
    sm_set(&v, "volfog_quality", "medium");
    CHECK(!strcmp(opts(&v), "--volfog|--volfog-quality|medium"), "argv: volfog quality medium");
    sm_set(&v, "volfog", "false");
    sm_set(&v, "fps_overlay", "true");
    CHECK(!strcmp(opts(&v), "--fps-overlay"), "argv: fps overlay, fps + ms is the default");
    sm_set(&v, "fps_overlay_items", "gpu,fps");
    sm_set(&v, "fps_overlay_corner", "br");
    CHECK(!strcmp(opts(&v), "--fps-overlay|--fps-overlay-items|fps,gpu|--fps-overlay-corner|br"), "argv: overlay items in order, corner (%s)", opts(&v));
    sm_set(&v, "fps_overlay_items", "bogus");
    CHECK(!strcmp(opts(&v), "--fps-overlay|--fps-overlay-items|fps|--fps-overlay-corner|br"), "argv: unknown items -> fps (%s)", opts(&v));
    sm_set(&v, "fps_overlay", "false");
    sm_set(&v, "draw_distance", "3");
    CHECK(!strcmp(opts(&v), "--draw-distance|2|--prop-detail|4"), "argv: draw distance 3 (%s)", opts(&v));
    sm_set(&v, "draw_distance", "1");
    sm_set(&v, "button_icons", "ps-white");
    sm_set(&v, "glyph_packs", "C:\\Warriors\\glyph packs\\");
    sm_set(&v, "texture_pack", "/p");
    CHECK(!strcmp(opts(&v), "--texture-pack|C:\\Warriors\\glyph packs\\ps-white|--texture-pack|/p"), "argv: glyph pack before the texture pack (%s)", opts(&v));
    sm_set(&v, "button_icons", "kbm");
    CHECK(!strcmp(opts(&v), "--texture-pack|/p|--button-icons|kbm"), "argv: kbm icons, no glyph pack (%s)", opts(&v));
    sm_set(&v, "button_icons", "xbox"), sm_set(&v, "texture_pack", "");
    sm_set(&v, "dynres", "true");
    CHECK(!strcmp(opts(&v), ""), "argv: dynres needs temporal upscaling");
    sm_set(&v, "taa_upscale", "quality");
    sm_set(&v, "upscaler", "fsr3");
    sm_set(&v, "taa_sharpness", "0.5");
    CHECK(!strcmp(opts(&v), "--taa-upscale|quality|--upscaler|fsr3|--taa-sharpness|0.5|--dynres|auto"), "argv: temporal upscaling group (%s)", opts(&v));
    sm_values_free(&v);
    /* saturation 1 and HDR off are left out; peak and paper white only with HDR, when not the default */
    sm_values_init(&v);
    CHECK(!strcmp(sm_get(&v, "saturation"), "1") && !strcmp(sm_get(&v, "hdr"), "off") && !strcmp(sm_get(&v, "hdr_peak"), "1000") &&
              !strcmp(sm_get(&v, "hdr_paper_white"), "200") && !strcmp(opts(&v), ""),
          "argv: saturation 1, HDR off, 1000 / 200 nits by default, nothing passed (%s)", opts(&v));
    sm_set(&v, "hdr_peak", "600");
    CHECK(!strcmp(opts(&v), ""), "argv: the peak needs HDR (%s)", opts(&v));
    sm_set(&v, "saturation", "1.25");
    sm_set(&v, "hdr", "auto");
    CHECK(!strcmp(opts(&v), "--saturation|1.25|--hdr|auto|--hdr-peak|600"), "argv: saturation, HDR and its peak (%s)", opts(&v));
    sm_set(&v, "hdr_paper_white", "250");
    sm_set(&v, "hdr_peak", "1000");
    CHECK(!strcmp(opts(&v), "--saturation|1.25|--hdr|auto|--hdr-paper-white|250"), "argv: paper white, the default peak left out (%s)", opts(&v));
    CHECK(sm_set(&v, "saturation", "2.5") == -2 && sm_set(&v, "hdr", "maybe") == -2 && sm_set(&v, "hdr_peak", "300") == -2,
          "values: saturation 2.5, hdr maybe and a 300-nit peak refused");
    sm_values_free(&v);
    sm_values_init(&v);
    sm_set(&v, "sharpen", "100");
    sm_set(&v, "spray_time", "150");
    sm_set(&v, "music_volume", "-6");
    sm_set(&v, "gpu_vertex_programs", "off");
    sm_set(&v, "extra_args", "--log-limit 5 \"--x y\"");
    CHECK(!strcmp(opts(&v), "--no-gpu-vertex-programs|--sharpen|100|--spray-time|150|--music-volume|-6|--log-limit|5|--x y"), "argv: ints, map, extra args (%s)", opts(&v));
    sm_values_free(&v);
    sm_argv_free(&a);
}

static void test_classes(void) {
    SmValues start;
    sm_values_init(&start);
    int bloom = sm_find("bloom"), ssao = sm_find("ssao"), hud = sm_find("hud_style"), up = sm_find("taa_upscale"), neon = sm_find("neon_glow");
    CHECK(sm_change_class(&start, bloom, "true") == SM_REINIT, "class: bloom as the first colour reader is a re-init");
    CHECK(sm_change_class(&start, ssao, "true") == SM_REINIT, "class: ssao as the first depth reader is a re-init");
    CHECK(sm_change_class(&start, hud, "hd") == SM_PASS && sm_change_class(&start, hud, "glass") == SM_REINIT, "class: a glass HUD reads the colour target");
    CHECK(sm_change_class(&start, bloom, "false") == SM_LIVE, "class: an unchanged value is live");
    sm_set(&start, "taa", "true");
    sm_set(&start, "hud_style", "hd-glass");
    CHECK(sm_change_class(&start, ssao, "true") == SM_PASS && sm_change_class(&start, bloom, "true") == SM_PASS,
          "class: with TAA and a glass HUD at start both are a pass");
    CHECK(sm_change_class(&start, up, "quality") == SM_REINIT, "class: temporal upscaling on is a re-init");
    sm_set(&start, "taa_upscale", "ultra");
    CHECK(sm_change_class(&start, up, "performance") == SM_LIVE && sm_change_class(&start, up, "off") == SM_REINIT,
          "class: an upscaling mode while on is live, off is a re-init");
    CHECK(sm_change_class(&start, neon, "true") == SM_RESTART, "class: neon glow on needs the mask pack layered at start");
    sm_set(&start, "neon_glow", "true");
    CHECK(sm_change_class(&start, neon, "false") == SM_PASS, "class: neon glow off again is a pass");
    CHECK(sm_change_class(&start, sm_find("parallax"), "true") == SM_LIVE && sm_change_class(&start, sm_find("mods"), "x") == SM_RESTART,
          "class: parallax live, mods on restart");
    SmValues now;
    sm_values_copy(&now, &start);
    CHECK(sm_changes_class(&start, &now) == SM_LIVE, "class: no change");
    sm_set(&now, "parallax", "true");
    sm_set(&now, "ssao", "true");
    CHECK(sm_changes_class(&start, &now) == SM_PASS, "class: the heaviest change");
    sm_set(&now, "language", "es");
    CHECK(sm_changes_class(&start, &now) == SM_RESTART && sm_class_rank('d') == 3 && sm_class_rank('-') == 4, "class: ranks");
    sm_values_free(&now);
    sm_values_free(&start);
}

static void test_pending(void) {
    SmValues start, now;
    sm_values_init(&start);
    sm_set(&start, "bloom", "true");
    sm_values_copy(&now, &start);
    char buf[512];
    int n = sm_pending_format(buf, sizeof buf, "my profile", &start, &now);
    CHECK(n > 0 && !strcmp(buf, SM_PENDING_MAGIC "\n[profile my profile]\n"), "pending: no change, header only");
    sm_set(&now, "bloom", "false");
    sm_set(&now, "ssao", "on");
    sm_set(&now, "xbrun", "/elsewhere");   /* not a menu setting: never written */
    sm_set(&now, "bloom_strength", "1.25");
    n = sm_pending_format(buf, sizeof buf, "my profile", &start, &now);
    CHECK(n > 0 && !strcmp(buf, SM_PENDING_MAGIC "\n[profile my profile]\nbloom = false\nbloom_strength = 1.25\nssao = true\n"),
          "pending: only the changed menu keys, in table order (%s)", buf);
    CHECK(sm_pending_format(buf, 20, "my profile", &start, &now) >= 20 && strlen(buf) < 20, "pending: truncation reported");
    CHECK(sm_pending_format(buf, sizeof buf, "bad]name", &start, &now) == -1 && sm_pending_format(buf, sizeof buf, "a\nb", &start, &now) == -1,
          "pending: a profile name the ini cannot hold");
    sm_values_free(&now);
    sm_values_free(&start);
}

int main(void) {
    test_table();
    test_values();
    test_expr();
    test_argv();
    test_classes();
    test_pending();
    if (failures) printf("%d FAILED\n", failures);
    else printf("ALL OK\n");
    return failures ? 1 : 0;
}
