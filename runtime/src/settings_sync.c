/* The game's side of the in-game settings sync: see settings_sync.h. */
#include "settings_sync.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void (*settings_sync_quit)(int exit_code);

static struct {
    int init;
    char ini[1024], profile[256], pending[1024], lang[8], presets[1024];
    int restart_ok;
    SmValues start, now;
} st;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void copy_env(char *dst, size_t n, const char *name) {
    const char *v = getenv(name);
    snprintf(dst, n, "%s", v ? v : "");
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}

/* The [profile NAME] section of the launcher's file into vals (read only; keys the model does not
 * know and bad values are skipped, as profile.cpp keeps their defaults). */
static void read_profile(const char *path, const char *profile, SmValues *vals) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[4200];
    int in = 0;
    while (fgets(line, sizeof line, f)) {
        char *l = trim(line);
        if (!*l || *l == '#' || *l == ';') continue;
        size_t n = strlen(l);
        if (*l == '[' && l[n - 1] == ']') {
            l[n - 1] = 0;
            char *name = trim(l + 1);
            in = !strncmp(name, "profile ", 8) && !strcmp(trim(name + 8), profile);
            continue;
        }
        char *eq = strchr(l, '=');
        if (!in || !eq) continue;
        *eq = 0;
        sm_set(vals, trim(l), trim(eq + 1));
    }
    fclose(f);
}

static void *test_thread(void *arg);

/* SM_ENV_TEST: the plan's file, read and removed (one shot), then a thread that runs it later. */
static void start_test_hook(void) {
    const char *path = getenv(SM_ENV_TEST);
    if (!path || !*path) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char text[8192];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    remove(path);
    static SettingsTestPlan plan;
    if (settings_sync_test_parse(text, &plan)) {
        printf("[settings] test hook %s: bad plan, not run\n", path);
        return;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, test_thread, &plan) == 0) {
        pthread_detach(t);
        printf("[settings] test hook: %d change(s)%s in %.1f s\n", plan.n, plan.restart ? " and a restart" : "", plan.after);
    }
}

int settings_sync_init(void) {
    pthread_mutex_lock(&lock);
    if (st.init) {
        pthread_mutex_unlock(&lock);
        return settings_sync_saved();
    }
    copy_env(st.ini, sizeof st.ini, SM_ENV_INI);
    copy_env(st.profile, sizeof st.profile, SM_ENV_PROFILE);
    copy_env(st.pending, sizeof st.pending, SM_ENV_PENDING);
    copy_env(st.lang, sizeof st.lang, SM_ENV_UI_LANGUAGE);
    copy_env(st.presets, sizeof st.presets, SM_ENV_PRESETS);
    const char *r = getenv(SM_ENV_RESTART);
    st.restart_ok = r && !strcmp(r, "1");
    if (strcmp(st.lang, "en") && strcmp(st.lang, "es")) st.lang[0] = 0;
    if (sm_values_init(&st.start)) {
        pthread_mutex_unlock(&lock);
        return 0;
    }
    if (*st.ini && *st.profile) read_profile(st.ini, st.profile, &st.start);
    if (sm_values_copy(&st.now, &st.start)) {
        sm_values_free(&st.start);
        pthread_mutex_unlock(&lock);
        return 0;
    }
    st.init = 1;
    pthread_mutex_unlock(&lock);
    if (settings_sync_saved())
        printf("[settings] in-game changes go to %s for profile '%s' (launcher %s)\n", st.pending, st.profile,
               st.restart_ok ? "relaunches on restart" : "does not wait");
    else
        printf("[settings] not started by the launcher: in-game changes last for this session\n");
    start_test_hook();
    return settings_sync_saved();
}

int settings_sync_saved(void) { return st.init && st.pending[0] && st.profile[0]; }
int settings_sync_restart_ok(void) { return settings_sync_saved() && st.restart_ok; }
const char *settings_sync_ui_language(void) { return st.lang; }
const char *settings_sync_presets_path(void) { return st.presets; }
const char *settings_sync_profile(void) { return st.profile; }
const SmValues *settings_sync_start(void) { return &st.start; }
const SmValues *settings_sync_now(void) { return &st.now; }

/* Under lock. */
static int write_pending(void) {
    if (!settings_sync_saved()) return 1;
    size_t cap = 4096;
    char *buf = NULL;
    int n;
    for (;;) {   /* grows until the text fits */
        char *nb = realloc(buf, cap);
        if (!nb) return free(buf), -1;
        buf = nb;
        n = sm_pending_format(buf, cap, st.profile, &st.start, &st.now);
        if (n < 0) return free(buf), -1;
        if ((size_t)n < cap) break;
        cap = (size_t)n + 1;
    }
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", st.pending);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(buf, 1, (size_t)n, f) == (size_t)n;
    if (f && fclose(f)) ok = 0;
    free(buf);
    if (!ok || plat_replace_file(tmp, st.pending)) {
        remove(tmp);
        return -1;
    }
    return 0;
}

int settings_sync_set(const char *key, const char *value) {
    pthread_mutex_lock(&lock);
    int rc = !st.init ? -4 : sm_set(&st.now, key, value);
    if (rc == 0 && write_pending() < 0) rc = -3;
    pthread_mutex_unlock(&lock);
    return rc;
}

int settings_sync_write(void) {
    pthread_mutex_lock(&lock);
    int rc = st.init ? write_pending() : -1;
    pthread_mutex_unlock(&lock);
    return rc;
}

void settings_sync_restart(void) {
    if (settings_sync_write() < 0) printf("[settings] restart: the pending file %s could not be written\n", st.pending);
    printf("[settings] restart requested (exit code %d)%s\n", SM_EXIT_RESTART, settings_sync_restart_ok() ? "" : ": the launcher is not waiting");
    if (settings_sync_quit) settings_sync_quit(SM_EXIT_RESTART);
}

int settings_sync_test_parse(const char *text, SettingsTestPlan *plan) {
    memset(plan, 0, sizeof *plan);
    char line[512];
    for (const char *p = text; *p;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n >= sizeof line) return -1;
        memcpy(line, p, n), line[n] = 0;
        p = nl ? nl + 1 : p + n;
        char *l = trim(line);
        if (!*l || *l == '#') continue;
        char *eq = strchr(l, '='), *end;
        if (!strncmp(l, "after ", 6)) {
            plan->after = strtod(l + 6, &end);
            if (end == l + 6 || *trim(end) || plan->after < 0) return -1;
        } else if (!strcmp(l, "restart")) {
            plan->restart = 1;
        } else if (eq && plan->n < SETTINGS_TEST_MAX) {
            *eq = 0;
            snprintf(plan->key[plan->n], sizeof plan->key[0], "%s", trim(l));
            snprintf(plan->value[plan->n], sizeof plan->value[0], "%s", trim(eq + 1));
            plan->n++;
        } else {
            return -1;
        }
    }
    return 0;
}

void settings_sync_test_run(const SettingsTestPlan *plan) {
    for (int i = 0; i < plan->n; i++)
        printf("[settings] test hook: %s = %s (%d)\n", plan->key[i], plan->value[i], settings_sync_set(plan->key[i], plan->value[i]));
    if (plan->restart) settings_sync_restart();
}

static void *test_thread(void *arg) {
    const SettingsTestPlan *plan = arg;
    struct timespec t = {(time_t)plan->after, (long)((plan->after - (double)(time_t)plan->after) * 1e9)};
    nanosleep(&t, NULL);
    settings_sync_test_run(plan);
    return NULL;
}

void settings_sync_reset(void) {
    pthread_mutex_lock(&lock);
    sm_values_free(&st.start);
    sm_values_free(&st.now);
    memset(&st, 0, sizeof st);
    pthread_mutex_unlock(&lock);
}
