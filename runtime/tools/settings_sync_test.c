/* settings_sync: the environment the launcher passes, the start
 * values read from its profiles file, the pending file (only the changed keys, atomically replaced),
 * the restart request and the test hook's plan. Scratch files in a mkdtemp folder under $TMPDIR. */
#include "../src/settings_sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static int quit_code = -1, quits;
static void stub_quit(int code) { quit_code = code, quits++; }

static char dir[512], ini[600], pending[600];

static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f) fputs(text, f), fclose(f);
}

static const char *slurp(const char *path) {
    static char buf[4096];
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

static void env(const char *profile) {
    setenv(SM_ENV_INI, ini, 1);
    setenv(SM_ENV_PROFILE, profile, 1);
    setenv(SM_ENV_PENDING, pending, 1);
    setenv(SM_ENV_UI_LANGUAGE, "es", 1);
    setenv(SM_ENV_PRESETS, "/w/x.ini.presets", 1);
    setenv(SM_ENV_RESTART, "1", 1);
}

static void unenv(void) {
    const char *names[] = {SM_ENV_INI, SM_ENV_PROFILE, SM_ENV_PENDING, SM_ENV_UI_LANGUAGE, SM_ENV_PRESETS, SM_ENV_RESTART, SM_ENV_TEST};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) unsetenv(names[i]);
}

static void test_by_hand(void) {
    unenv();
    settings_sync_reset();
    CHECK(settings_sync_init() == 0 && !settings_sync_saved() && !settings_sync_restart_ok() && !*settings_sync_ui_language(),
          "by hand: no environment, changes not saved");
    CHECK(settings_sync_set("bloom", "true") == 0 && !strcmp(sm_get(settings_sync_now(), "bloom"), "true") && settings_sync_write() == 1,
          "by hand: a change kept for the session, no file");
}

static void test_launcher(void) {
    put(ini, "# warriors-launcher profiles\n[launcher]\nselected = main\n\n[profile other]\nbloom = false\n\n"
             "[profile main]\nbloom = true\nmsaa = 4\nmusic_volume = -3\nfuture_key = x\nbad line\nssao = maybe\n\n[future]\nbloom = false\n");
    unlink(pending);
    env("main");
    settings_sync_reset();
    CHECK(settings_sync_init() == 1 && settings_sync_saved() && settings_sync_restart_ok(), "launcher: environment read");
    CHECK(!strcmp(settings_sync_ui_language(), "es") && !strcmp(settings_sync_presets_path(), "/w/x.ini.presets") &&
          !strcmp(settings_sync_profile(), "main"), "launcher: language, presets, profile");
    const SmValues *s = settings_sync_start();
    CHECK(!strcmp(sm_get(s, "bloom"), "true") && !strcmp(sm_get(s, "msaa"), "4") && !strcmp(sm_get(s, "music_volume"), "-3") &&
          !strcmp(sm_get(s, "ssao"), "false"), "launcher: start values from its profile section only (bad lines skipped)");
    CHECK(access(pending, F_OK) != 0, "launcher: no pending file before a change");
    CHECK(settings_sync_set("bloom", "false") == 0, "launcher: set");
    const char *t = slurp(pending);
    CHECK(t && !strcmp(t, SM_PENDING_MAGIC "\n[profile main]\nbloom = false\n"), "launcher: the pending file holds the one change (%s)", t ? t : "none");
    settings_sync_set("music_volume", "-3");
    settings_sync_set("msaa", "8");
    t = slurp(pending);
    CHECK(t && !strcmp(t, SM_PENDING_MAGIC "\n[profile main]\nmsaa = 8\nbloom = false\n"), "launcher: a value equal to the start is not written");
    settings_sync_set("bloom", "true");
    settings_sync_set("xbrun", "/elsewhere");   /* not a menu setting: kept but never written */
    t = slurp(pending);
    CHECK(t && !strcmp(t, SM_PENDING_MAGIC "\n[profile main]\nmsaa = 8\n"), "launcher: a change back drops the key; non-menu keys never written");
    CHECK(settings_sync_set("nope", "1") == -1 && settings_sync_set("msaa", "3") == -2 && !strcmp(sm_get(settings_sync_now(), "msaa"), "8"),
          "launcher: unknown key and bad value refused");
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.tmp", pending);
    CHECK(access(tmp, F_OK) != 0, "launcher: no temporary file left (atomic replace)");
    quits = 0;
    settings_sync_quit = stub_quit;
    settings_sync_set("ssao", "true");
    unlink(pending);
    settings_sync_restart();
    t = slurp(pending);
    CHECK(quits == 1 && quit_code == SM_EXIT_RESTART && t && strstr(t, "ssao = true"), "launcher: restart writes the file and quits with %d", SM_EXIT_RESTART);
}

static void test_missing_profile(void) {
    env("nobody");
    settings_sync_reset();
    CHECK(settings_sync_init() == 1 && !strcmp(sm_get(settings_sync_start(), "msaa"), "0"), "missing profile: defaults as start values");
    setenv(SM_ENV_INI, "/no/such/file.ini", 1);
    settings_sync_reset();
    CHECK(settings_sync_init() == 1 && !strcmp(sm_get(settings_sync_start(), "bloom"), "false"), "unreadable profiles file: defaults");
    setenv(SM_ENV_PENDING, "/no/such/dir/x.pending", 1);
    settings_sync_reset();
    settings_sync_init();
    CHECK(settings_sync_set("bloom", "true") == -3 && !strcmp(sm_get(settings_sync_now(), "bloom"), "true"),
          "unwritable pending file: -3, the value kept");
}

static void test_plan(void) {
    SettingsTestPlan p;
    CHECK(!settings_sync_test_parse("# hook\nafter 2.5\nbloom = true\n music_volume = -6 \nrestart\n", &p) && p.after == 2.5 && p.restart &&
          p.n == 2 && !strcmp(p.key[1], "music_volume") && !strcmp(p.value[1], "-6"), "plan: parsed");
    CHECK(settings_sync_test_parse("after x\n", &p) == -1 && settings_sync_test_parse("bloom\n", &p) == -1, "plan: bad lines");
    env("main");
    unlink(pending);
    settings_sync_reset();
    settings_sync_init();
    quits = 0;
    settings_sync_test_parse("after 0\nsky_hd = true\nrestart\n", &p);
    settings_sync_test_run(&p);
    const char *t = slurp(pending);
    CHECK(quits == 1 && t && strstr(t, "sky_hd = true"), "plan: run sets the keys and restarts");
    /* the hook file is read and removed at init (one shot) */
    char hook[700];
    snprintf(hook, sizeof hook, "%s/hook.txt", dir);
    put(hook, "after 3600\nbloom = false\n");
    setenv(SM_ENV_TEST, hook, 1);
    settings_sync_reset();
    settings_sync_init();
    CHECK(access(hook, F_OK) != 0, "plan: the hook file is consumed at start");
    unsetenv(SM_ENV_TEST);
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/settings_sync_test.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(dir)) return perror(dir), 2;
    snprintf(ini, sizeof ini, "%s/profiles.ini", dir);
    snprintf(pending, sizeof pending, "%s/profiles.ini.pending", dir);
    test_by_hand();
    test_launcher();
    test_missing_profile();
    test_plan();
    unlink(ini), unlink(pending);
    rmdir(dir);
    if (failures) printf("%d FAILED\n", failures);
    else printf("ALL OK\n");
    return failures ? 1 : 0;
}
