/* mod.ini parsing and the mods folder scan (see modini.h). */
#include "modini.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int modini_valid_id(const char *id) {
    size_t n = strlen(id);
    if (n < 1 || n > 40) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

/* A plain file name inside the mod folder. */
static int valid_file(const char *f) {
    if (!*f || strstr(f, "..") || strchr(f, '/') || strchr(f, '\\') || strchr(f, ':')) return 0;
    return strlen(f) < 64;
}

/* A plain folder name inside the mod's folder (no separators, no "..", not "." ). */
static int valid_folder(const char *f) {
    if (!*f || !strcmp(f, ".") || strstr(f, "..") || strchr(f, '/') || strchr(f, '\\') || strchr(f, ':')) return 0;
    return strlen(f) < 64;
}

int modini_match(const char *pattern, const char *script) {
    size_t n = strlen(pattern);
    if (n && pattern[n - 1] == '*') return !strncmp(pattern, script, n - 1);
    return !strcmp(pattern, script);
}

static void trim(char **s, char **e) {
    while (*s < *e && (**s == ' ' || **s == '\t')) (*s)++;
    while (*e > *s && ((*e)[-1] == ' ' || (*e)[-1] == '\t' || (*e)[-1] == '\r')) (*e)--;
}

static void copy(char *dst, size_t cap, const char *s, size_t n) {
    if (n >= cap) n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = 0;
}

int modini_parse(const char *text, size_t len, ModInfo *m, char *err, size_t cap) {
    memset(m, 0, sizeof *m);
    const char *p = text, *end = text + len;
    if (len >= 3 && (unsigned char)p[0] == 0xef && (unsigned char)p[1] == 0xbb && (unsigned char)p[2] == 0xbf) p += 3;
    int line = 0;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        line++;
        char *s = (char *)p, *e = (char *)le;
        p = nl ? nl + 1 : end;
        trim(&s, &e);
        if (s == e || *s == ';' || *s == '#' || *s == '[') continue;
        const char *eq = memchr(s, '=', (size_t)(e - s));
        if (!eq) { snprintf(err, cap, "line %d: expected key = value", line); return -1; }
        char *ks = s, *ke = (char *)eq, *vs = (char *)eq + 1, *ve = e;
        trim(&ks, &ke);
        trim(&vs, &ve);
        char key[80], val[400];
        copy(key, sizeof key, ks, (size_t)(ke - ks));
        copy(val, sizeof val, vs, (size_t)(ve - vs));
        if (!strcmp(key, "id")) copy(m->id, sizeof m->id, val, strlen(val));
        else if (!strcmp(key, "name")) copy(m->name, sizeof m->name, val, strlen(val));
        else if (!strcmp(key, "name.es")) copy(m->name_es, sizeof m->name_es, val, strlen(val));
        else if (!strcmp(key, "description")) copy(m->description, sizeof m->description, val, strlen(val));
        else if (!strcmp(key, "description.es")) copy(m->description_es, sizeof m->description_es, val, strlen(val));
        else if (!strcmp(key, "version")) copy(m->version, sizeof m->version, val, strlen(val));
        else if (!strcmp(key, "author")) copy(m->author, sizeof m->author, val, strlen(val));
        else if (!strcmp(key, "saves")) {
            if (!strcmp(val, "yes")) m->saves = 1;
            else if (!strcmp(val, "no")) m->saves = 0;
            else { snprintf(err, cap, "line %d: saves must be yes or no", line); return -1; }
        } else if (!strcmp(key, "meshes")) {
            if (!valid_folder(val)) {
                snprintf(err, cap, "line %d: meshes: \"%s\" must be a folder name inside the mod's folder", line, val);
                return -1;
            }
            copy(m->meshes, sizeof m->meshes, val, strlen(val));
        } else if (!strcmp(key, "collision")) {
            if (!valid_folder(val)) {
                snprintf(err, cap, "line %d: collision: \"%s\" must be a file name inside the mod's folder", line, val);
                return -1;
            }
            copy(m->collision, sizeof m->collision, val, strlen(val));
        } else if (!strcmp(key, "min_runtime")) {
            int ok = val[0] >= '0' && val[0] <= '9' && strlen(val) < sizeof m->min_runtime;
            for (const char *c = val; ok && *c; c++) ok = (*c >= '0' && *c <= '9') || (*c == '.' && c[1] && c[1] != '.');
            if (!ok) { snprintf(err, cap, "line %d: min_runtime: \"%s\" must be a version like 1.1", line, val); return -1; }
            copy(m->min_runtime, sizeof m->min_runtime, val, strlen(val));
        } else if (!strcmp(key, "files")) {
            if (strlen(val) >= sizeof m->files) { snprintf(err, cap, "line %d: files: at most %d characters", line, (int)sizeof m->files - 1); return -1; }
            char list[sizeof m->files];
            copy(list, sizeof list, val, strlen(val));
            for (char *t = strtok(list, ","); t; t = strtok(NULL, ",")) {
                char *ts = t, *te = t + strlen(t);
                trim(&ts, &te);
                *te = 0;
                size_t tn = strlen(ts);
                char name[64];
                copy(name, sizeof name, ts, tn && ts[tn - 1] == '*' ? tn - 1 : tn);
                if (!valid_folder(name)) {
                    snprintf(err, cap, "line %d: files: \"%s\" must be a name inside the mod's folder", line, ts);
                    return -1;
                }
            }
            copy(m->files, sizeof m->files, val, strlen(val));
        } else if (!strcmp(key, "boot") || !strncmp(key, "after.", 6)) {
            const char *script = key[0] == 'b' ? "" : key + 6;
            if (key[0] == 'a' && (!*script || strlen(script) >= sizeof m->hooks[0].script)) {
                snprintf(err, cap, "line %d: after. needs a script name (at most 63 characters)", line);
                return -1;
            }
            if (!valid_file(val)) {
                snprintf(err, cap, "line %d: %s: \"%s\" must be a .lua file name inside the mod's folder", line, key, val);
                return -1;
            }
            if (m->nhooks >= MOD_HOOK_MAX) { snprintf(err, cap, "line %d: more than %d hooks", line, MOD_HOOK_MAX); return -1; }
            ModHook *h = &m->hooks[m->nhooks++];
            copy(h->script, sizeof h->script, script, strlen(script));
            copy(h->file, sizeof h->file, val, strlen(val));
        }
        /* other keys: ignored (newer mod.ini versions) */
    }
    if (!m->id[0] || !modini_valid_id(m->id)) {
        snprintf(err, cap, "id \"%s\": required, 1-40 characters of a-z 0-9 - _", m->id);
        return -1;
    }
    if (!m->name[0]) { snprintf(err, cap, "name: required"); return -1; }
    if (!m->version[0]) { snprintf(err, cap, "version: required"); return -1; }
    return 0;
}

static int read_text(const char *path, char **out, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char *b = malloc(65536);
    size_t n = b ? fread(b, 1, 65535, f) : 0;
    fclose(f);
    if (!b) return -1;
    b[n] = 0;
    *out = b;
    *len = n;
    return 0;
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int modini_scan(const char *dir, ModInfo *out, int max, void (*warn)(const char *msg, void *ud), void *ud) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    char *names[256];
    int nn = 0;
    struct dirent *de;
    while ((de = readdir(d)) && nn < 256) {
        if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof out->folder) continue;
        names[nn] = strdup(de->d_name);
        if (names[nn]) nn++;
    }
    closedir(d);
    qsort(names, (size_t)nn, sizeof names[0], cmp_names);
    int n = 0;
    for (int i = 0; i < nn; i++) {
        char path[1024], msg[512], err[256];
        char *text;
        size_t len;
        snprintf(path, sizeof path, "%s/%s/mod.ini", dir, names[i]);
        if (n < max && read_text(path, &text, &len) == 0) {
            ModInfo *m = &out[n];
            int r = modini_parse(text, len, m, err, sizeof err);
            free(text);
            int dup = -1;
            for (int k = 0; r == 0 && k < n; k++)
                if (!strcmp(out[k].id, m->id)) dup = k;
            if (r) snprintf(msg, sizeof msg, "%s/mod.ini: %s (skipped)", names[i], err);
            else if (dup >= 0) snprintf(msg, sizeof msg, "%s: id %s is already used by %s (skipped)", names[i], m->id, out[dup].folder);
            if (r || dup >= 0) {
                if (warn) warn(msg, ud);
            } else {
                snprintf(m->folder, sizeof m->folder, "%s", names[i]);
                n++;
            }
        }
        free(names[i]);
    }
    return n;
}
