/* Mod safety checks: min_runtime and the folder manifest (see modcheck.h). */
#include "modcheck.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <strings.h>
#define name_eq(a, b) (!strcasecmp((a), (b)))   /* the Windows file system ignores case */
#define name_prefix(a, p, n) (!strncasecmp((a), (p), (n)))
#else
#define name_eq(a, b) (!strcmp((a), (b)))
#define name_prefix(a, p, n) (!strncmp((a), (p), (n)))
#endif

int modcheck_version_cmp(const char *a, const char *b) {
    while (*a || *b) {
        unsigned long x = strtoul(a, (char **)&a, 10), y = strtoul(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        else if (*a) return 0;   /* not a number: stop comparing */
        if (*b == '.') b++;
        else if (*b) return 0;
    }
    return 0;
}

int modcheck_runtime_ok(const ModInfo *m) {
    return !m->min_runtime[0] || modcheck_version_cmp(MOD_RUNTIME_VERSION, m->min_runtime) >= 0;
}

/* Calls fn(name, prefix, ud) for each `files` entry (prefix = 1 for a trailing *). */
static void each_listed(const ModInfo *m, void (*fn)(const char *name, int prefix, void *ud), void *ud) {
    char list[sizeof m->files];
    snprintf(list, sizeof list, "%s", m->files);
    for (char *t = strtok(list, ","); t; t = strtok(NULL, ",")) {
        while (*t == ' ' || *t == '\t') t++;
        size_t n = strlen(t);
        while (n && (t[n - 1] == ' ' || t[n - 1] == '\t')) t[--n] = 0;
        int prefix = n && t[n - 1] == '*';
        if (prefix) t[--n] = 0;
        if (n) fn(t, prefix, ud);
    }
}

typedef struct { const char *name; int hit; } Match;
static void match_listed(const char *name, int prefix, void *ud) {
    Match *mt = ud;
    if (prefix ? name_prefix(mt->name, name, strlen(name)) : name_eq(mt->name, name)) mt->hit = 1;
}

static int expected(const ModInfo *m, const char *name) {
    if (name[0] == '.' || name_eq(name, "mod.ini") || name_eq(name, "Thumbs.db") || name_eq(name, "desktop.ini")) return 1;
    if (m->meshes[0] && name_eq(name, m->meshes)) return 1;
    if (name_eq(name, "assets.ini") || name_eq(name, "own")) return 1;   /* src/modassets.h: built from the disc; own assets */
    if (m->collision[0] && name_eq(name, m->collision)) return 1;
    for (int k = 0; k < m->nhooks; k++)
        if (name_eq(name, m->hooks[k].file)) return 1;
    Match mt = {name, 0};
    each_listed(m, match_listed, &mt);
    return mt.hit;
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

typedef struct {
    char **names;
    int n;
    void (*fn)(int kind, const char *name, void *ud);
    void *ud;
    int problems;
} Listing;

static int present(const Listing *l, const char *name) {
    for (int i = 0; i < l->n; i++)
        if (name_eq(l->names[i], name)) return 1;
    return 0;
}

static void missing_listed(const char *name, int prefix, void *ud) {
    Listing *l = ud;
    if (prefix || present(l, name)) return;
    if (l->fn) l->fn(MODCHECK_MISSING_FILE, name, l->ud);
    l->problems++;
}

int modcheck_files(const char *dir, const ModInfo *m, void (*fn)(int kind, const char *name, void *ud), void *ud) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, m->folder);
    DIR *d = opendir(path);
    if (!d) return -1;
    char *names[256];
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < 256) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if ((names[n] = strdup(de->d_name))) n++;
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], cmp_names);
    Listing l = {names, n, fn, ud, 0};
    for (int i = 0; i < n; i++)
        if (!expected(m, names[i])) {
            if (fn) fn(MODCHECK_LEFTOVER, names[i], ud);
            l.problems++;
        }
    for (int k = 0; k < m->nhooks; k++) {
        int dup = 0;   /* one file can serve several hooks */
        for (int j = 0; j < k; j++) dup |= name_eq(m->hooks[j].file, m->hooks[k].file);
        if (dup || present(&l, m->hooks[k].file)) continue;
        if (fn) fn(MODCHECK_MISSING_LUA, m->hooks[k].file, ud);
        l.problems++;
    }
    if (m->collision[0]) missing_listed(m->collision, 0, &l);
    each_listed(m, missing_listed, &l);
    for (int i = 0; i < n; i++) free(names[i]);
    return l.problems;
}

typedef struct {
    const ModInfo *m;
    void (*warn)(const char *msg, void *ud);
    void *ud;
    int refuse;
} Report;

static void report_one(int kind, const char *name, void *ud) {
    Report *r = ud;
    char msg[512];
    if (kind == MODCHECK_LEFTOVER)
        snprintf(msg, sizeof msg, "%s: \"%s\" in its folder is not part of version %s (left from an older version?); "
                 "delete it or reinstall the mod. The mod still loads", r->m->id, name, r->m->version);
    else if (kind == MODCHECK_MISSING_LUA) {
        snprintf(msg, sizeof msg, "%s: missing file %s (named in its mod.ini); mod not loaded: reinstall it", r->m->id, name);
        r->refuse = 1;
    } else
        snprintf(msg, sizeof msg, "%s: missing file %s (listed in its mod.ini files); reinstall the mod", r->m->id, name);
    if (r->warn) r->warn(msg, r->ud);
}

int modcheck_report(const char *dir, const ModInfo *m, void (*warn)(const char *msg, void *ud), void *ud) {
    char msg[256];
    if (!modcheck_runtime_ok(m)) {
        snprintf(msg, sizeof msg, "%s: needs mod runtime %s or newer (this game has %s); mod not loaded: update the game",
                 m->id, m->min_runtime, MOD_RUNTIME_VERSION);
        if (warn) warn(msg, ud);
        return 0;
    }
    Report r = {m, warn, ud, 0};
    if (modcheck_files(dir, m, report_one, &r) < 0) {
        snprintf(msg, sizeof msg, "%s: cannot open its folder %s; mod not loaded", m->id, m->folder);
        if (warn) warn(msg, ud);
        return 0;
    }
    return !r.refuse;
}
