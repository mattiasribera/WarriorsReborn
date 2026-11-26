/* Lua mods: --mods / --mod / --no-mods / --mods-trace (see hle_mods.h, docs/MODDING.md). */
#include "hle_mods.h"
#include "hle_scriptfix.h"
#include "hle_luagc.h"
#include "hle_diffmenu.h"
#include "hle_gore.h"
#include "hle_scars.h"
#include "hle_radar.h"
#include "hle_huspeed.h"
#include "hle_modmenu.h"
#include "colpatch.h"
#include <dirent.h>
#include "modsound.h"
#include "hle_warp.h"
#include "meshover.h"
#include "texlayer.h"
#include "modcheck.h"
#include "modassets.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STATES 8
#define MAX_ENABLE 64

typedef struct {
    ModInfo info;
    char *code[MOD_HOOK_MAX];
    size_t len[MOD_HOOK_MAX];
    int failed;
    int meshes;   /* mesh overrides loaded from its meshes folder and its asset cache (MOD_MESHES) */
    int assets;   /* output sections of its assets.ini built or found in the cache (MOD_ASSETS) */
} Mod;

static Mod *mods;
static int nmods;
static const char *opt_dir;
static char enable[MAX_ENABLE][41];
static int nenable, opt_all, opt_none, opt_trace;
static uint32_t booted[MAX_STATES];
static int nbooted;
static const char *current_id;   /* the mod whose chunk is running (ModLog) */
/* Registers ModLog in a state (title only; NULL in tests). */
static void (*register_log)(uint32_t L);
/* Binds ModSave / ModLoad to a mod (id) before its file runs, unbinds them after (id NULL; title
 * only, NULL in tests). */
static void (*bind_data)(uint32_t L, const char *id);
static char data_dir[1024];
static const char *(*data_profile)(void);
/* assets.ini (src/modassets.h): the disc and hdd folders, --rebuild-mod-assets [ID] */
static char assets_disc[1024], assets_hdd[1024];
static int opt_rebuild_all, nrebuild;
static char rebuild[MAX_ENABLE][41];

static void kept_clear(void);

void mods_reset(void) {
    kept_clear();
    for (int i = 0; i < nmods; i++)
        for (int k = 0; k < mods[i].info.nhooks; k++) free(mods[i].code[k]);
    free(mods);
    mods = NULL;
    meshover_close();
    texlayer_close();
    modsound_reset();
    nmods = nenable = opt_all = opt_none = opt_trace = nbooted = 0;
    opt_rebuild_all = nrebuild = 0;
    assets_disc[0] = assets_hdd[0] = 0;
    opt_dir = NULL;
    current_id = NULL;
}

int mods_count(void) { return nmods; }
const ModInfo *mods_get(int i) { return i >= 0 && i < nmods ? &mods[i].info : NULL; }

int mods_failed(const char *id) {
    for (int i = 0; i < nmods; i++)
        if (!strcmp(mods[i].info.id, id)) return mods[i].failed;
    return 0;
}

int mods_parse_arg(int argc, char **argv, int *i, char *err, size_t cap) {
    const char *a = argv[*i];
    if (!strcmp(a, "--no-mods")) { opt_none = 1; return 1; }
    if (!strcmp(a, "--mods-trace")) { opt_trace = 1; return 1; }
    if (!strcmp(a, "--rebuild-mod-assets")) {   /* optional ID: the next argument unless it is an option */
        if (*i + 1 >= argc || argv[*i + 1][0] == '-') { opt_rebuild_all = 1; return 1; }
        const char *v = argv[++*i];
        if (!modini_valid_id(v)) {
            snprintf(err, cap, "--rebuild-mod-assets %s: not a mod id (a-z 0-9 - _)", v);
            return -1;
        }
        if (nrebuild >= MAX_ENABLE) { snprintf(err, cap, "--rebuild-mod-assets: at most %d mods", MAX_ENABLE); return -1; }
        snprintf(rebuild[nrebuild++], sizeof rebuild[0], "%s", v);
        return 1;
    }
    if (strcmp(a, "--mods") && strcmp(a, "--mod")) return 0;
    if (*i + 1 >= argc) { snprintf(err, cap, "%s: missing value", a); return -1; }
    const char *v = argv[++*i];
    if (!strcmp(a, "--mods")) { opt_dir = v; return 1; }
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", v);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
        if (!strcmp(t, "all")) { opt_all = 1; continue; }
        if (!modini_valid_id(t)) {
            snprintf(err, cap, "--mod %s: \"%s\" is not a mod id (a-z 0-9 - _, or all)", v, t);
            return -1;
        }
        if (nenable >= MAX_ENABLE) { snprintf(err, cap, "--mod: at most %d mods", MAX_ENABLE); return -1; }
        snprintf(enable[nenable++], sizeof enable[0], "%s", t);
    }
    return 1;
}

static int enabled(const char *id) {
    if (opt_all) return 1;
    for (int i = 0; i < nenable; i++)
        if (!strcmp(enable[i], id)) return 1;
    return 0;
}

static void scan_warn(const char *msg, void *ud) { printf("[mods] %s\n", msg); }

void mods_set_assets(const char *disc, const char *hdd) {
    snprintf(assets_disc, sizeof assets_disc, "%s", disc ? disc : "");
    snprintf(assets_hdd, sizeof assets_hdd, "%s", hdd ? hdd : "");
}

static int rebuild_wanted(const char *id) {
    if (opt_rebuild_all) return 1;
    for (int i = 0; i < nrebuild; i++)
        if (!strcmp(rebuild[i], id)) return 1;
    return 0;
}

static char *read_file(const char *path, size_t *len);
/* The mod's assets.ini (src/modassets.h): built into <hdd>/mods-cache/<id>/<key> on a cache miss,
 * then loaded like a meshes folder (its group folders are the mod's groups too). */
static void load_assets(const char *dir, Mod *m, int index) {
    char mdir[1100], err[700];
    snprintf(mdir, sizeof mdir, "%s/%s", dir, m->info.folder);
    MaResult res;
    int a = modassets_prepare(mdir, m->info.id, m->info.version, assets_disc, assets_hdd, rebuild_wanted(m->info.id), &res, err, sizeof err);
    if (a < 0) {
        printf("[modassets] %s: %s; the mod loads without its assets\n", m->info.id, err);
        return;
    }
    if (!a) return;
    if (res.built) printf("[modassets] %s: built %d asset%s (%d files) in %.0f ms into %s\n", m->info.id, res.outputs, res.outputs == 1 ? "" : "s", res.files, res.ms, res.cache);
    else printf("[modassets] %s: %d asset%s from the cache %s\n", m->info.id, res.outputs, res.outputs == 1 ? "" : "s", res.cache);
    int k = meshover_add_dir(res.cache, index, m->info.id);
    int t = texlayer_add_dir(res.cache, m->info.id);
    m->meshes += k > 0 ? k : 0;
    m->assets = res.outputs;
    printf("[modassets] %s: %d mesh override%s, %d texture layer file%s from the cache\n", m->info.id, k > 0 ? k : 0, k == 1 ? "" : "s", t > 0 ? t : 0,
           t == 1 ? "" : "s");
    if (res.warn[0]) printf("[modassets] %s: warning: %s\n", m->info.id, res.warn);
    /* own assets: the [sound] files (src/modsound.h) and the [collision] patches (src/colpatch.h) */
    int snds = modsound_add_dir(m->info.id, res.cache), cols = 0;
    DIR *d = opendir(res.cache);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        const size_t L = strlen(e->d_name);
        if (L < 5 || strcmp(e->d_name + L - 4, ".col")) continue;
        char path[1400];
        size_t len = 0;
        snprintf(path, sizeof path, "%s/%s", res.cache, e->d_name);
        char *text = read_file(path, &len);
        if (text && !colpatch_register(m->info.id, text, len)) cols++;
        else printf("[modassets] %s: collision %s not used\n", m->info.id, e->d_name);
        free(text);
    }
    if (d) closedir(d);
    if (snds || cols) printf("[modassets] %s: %d sound%s, %d collision patch%s from the cache\n", m->info.id, snds, snds == 1 ? "" : "s", cols, cols == 1 ? "" : "es");
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0, r;
    char *b = malloc(cap);
    while (b && (r = fread(b + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) {
            char *nb = cap < (4u << 20) ? realloc(b, cap * 2) : NULL;   /* 4 MB per file at most */
            if (!nb) { free(b); b = NULL; break; }
            b = nb;
            cap *= 2;
        }
    }
    fclose(f);
    *len = n;
    return b;
}

int mods_load(const char *default_dir) {
    const char *dir = opt_dir ? opt_dir : default_dir;
    if (opt_none) {
        if (nenable || opt_all) printf("[mods] --no-mods: no mod runs\n");
        return 0;
    }
    if (!nenable && !opt_all) return 0;
    if (!dir) { printf("[mods] no mods folder\n"); return 0; }
    ModInfo *found = calloc(MOD_MAX, sizeof *found);
    if (!found) return 0;
    int n = modini_scan(dir, found, MOD_MAX, scan_warn, NULL);
    if (n < 0) {
        printf("[mods] %s: cannot open the mods folder; no mod runs\n", dir);
        free(found);
        return 0;
    }
    mods = calloc((size_t)(n ? n : 1), sizeof *mods);
    if (!mods) { free(found); return 0; }
    for (int i = 0; i < nenable; i++) {
        int k = 0;
        while (k < n && strcmp(found[k].id, enable[i])) k++;
        if (k == n) printf("[mods] %s: not found in %s\n", enable[i], dir);
    }
    for (int i = 0; i < n; i++) {
        if (!enabled(found[i].id)) continue;
        if (!modcheck_report(dir, &found[i], scan_warn, NULL)) continue;   /* min_runtime, missing files, leftovers */
        Mod *m = &mods[nmods];
        memset(m, 0, sizeof *m);
        m->info = found[i];
        int ok = 1;
        for (int k = 0; k < m->info.nhooks && ok; k++) {
            char path[1024];
            snprintf(path, sizeof path, "%s/%s/%s", dir, m->info.folder, m->info.hooks[k].file);
            if (!(m->code[k] = read_file(path, &m->len[k]))) {
                printf("[mods] %s: error: cannot read %s; mod skipped\n", m->info.id, m->info.hooks[k].file);
                ok = 0;
            }
        }
        if (!ok) {
            for (int k = 0; k < m->info.nhooks; k++) free(m->code[k]);
            continue;
        }
        if (m->info.meshes[0]) {   /* src/meshover.h: tagged with the mod's index */
            char path[1024];
            snprintf(path, sizeof path, "%s/%s/%s", dir, m->info.folder, m->info.meshes);
            int k = meshover_add_dir(path, nmods, m->info.id);
            m->meshes = k > 0 ? k : 0;
            if (k < 0) printf("[mods] %s: optional meshes not installed (folder %s); the mod runs without them\n", m->info.id, m->info.meshes);
            else printf("[mods] %s: %d mesh override%s from %s\n", m->info.id, k, k == 1 ? "" : "s", m->info.meshes);
            int t = texlayer_add_dir(path, m->info.id);   /* src/texlayer.h: "<tex>@<label>.png" */
            if (t > 0) printf("[mods] %s: %d texture layer file%s from %s\n", m->info.id, t, t == 1 ? "" : "s", m->info.meshes);
        }
        load_assets(dir, m, nmods);
        if (m->info.collision[0]) {   /* src/colpatch.h: applied when the level's collision world appears */
            char path[1024];
            size_t len = 0;
            snprintf(path, sizeof path, "%s/%s/%s", dir, m->info.folder, m->info.collision);
            char *text = read_file(path, &len);
            if (!text || colpatch_register(m->info.id, text, len)) printf("[mods] %s: collision patch %s not used\n", m->info.id, m->info.collision);
            free(text);
        }
        nmods++;
    }
    free(found);
    char line[1024];
    size_t w = 0;
    line[0] = 0;
    for (int i = 0; i < nmods && w + 1 < sizeof line; i++) {
        int r = snprintf(line + w, sizeof line - w, "%s%s %s%s", i ? ", " : "", mods[i].info.id, mods[i].info.version,
                         mods[i].info.saves ? " (changes save data)" : "");
        if (r > 0) w += (size_t)r < sizeof line - w ? (size_t)r : sizeof line - w - 1;
    }
    printf("[mods] loaded %d%s%s (from %s)\n", nmods, nmods ? ": " : "", line, dir);
    fflush(stdout);
    return nmods;
}

/* ---- running ---- */

/* A Lua string literal of s (quotes and control characters escaped). */
static void quote(char *out, size_t cap, const char *s) {
    size_t n = 0;
    if (cap < 3) return;
    out[n++] = '"';
    for (; *s && n + 5 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 32) n += (size_t)snprintf(out + n, cap - n, "\\%03u", c);
        else out[n++] = (char)c;
    }
    out[n++] = '"';
    out[n] = 0;
}

static void run_hook(const DevLuaApi *api, uint32_t L, Mod *m, int k, const char *script) {
    char pre[256], qid[64], qs[160], chunk[160], err[512];
    int t;
    quote(qid, sizeof qid, m->info.id);
    quote(qs, sizeof qs, script);
    int n = snprintf(pre, sizeof pre, "MOD_ID = %s MOD_SCRIPT = %s MOD_MESHES = %d MOD_ASSETS = %d", qid, qs, m->meshes, m->assets);
    int base = api->gettop(L);
    devlua_host_chunks++;
    api->dobuffer(L, pre, (size_t)n, "=mods");
    devlua_host_chunks--;
    api->settop(L, base);
    if (bind_data) bind_data(L, m->info.id);
    snprintf(chunk, sizeof chunk, "=%s/%s", m->info.id, m->info.hooks[k].file);
    current_id = m->info.id;
    if (devlua_run_ex(api, L, m->code[k], m->len[k], chunk, NULL, &t, err, sizeof err)) {
        m->failed = 1;
        printf("[mods] %s: error in %s (%s%s): %s; mod turned off for this run\n", m->info.id, m->info.hooks[k].file,
               m->info.hooks[k].script[0] ? "after " : "boot, after ", script, err);
    } else if (opt_trace) {
        printf("[mods] %s: ran %s after %s\n", m->info.id, m->info.hooks[k].file, script);
    }
    current_id = NULL;
    if (bind_data) bind_data(L, NULL);
    fflush(stdout);
}

void mods_on_script(const DevLuaApi *api, uint32_t L, const char *name) {
    static int busy;
    if (busy || !L || !name) return;
    char script[64];   /* "=level95.lua" / "@x/level95.lua" -> "level95" */
    if (!scriptfix_script_name(name, script, sizeof script)) return;
    busy = 1;
    if (opt_trace) printf("[mods] script %s (Lua state %08x)\n", script, L);
    int is_new = 1;
    for (int i = 0; i < nbooted; i++) is_new &= booted[i] != L;
    char peak[128];
    if (luagc_on_script(L, script, is_new, peak, sizeof peak)) printf("%s\n", peak);
    if (is_new) {
        if (nbooted == MAX_STATES) memmove(booted, booted + 1, sizeof booted[0] * (MAX_STATES - 1)), nbooted--;
        booted[nbooted++] = L;
        meshover_points_clear();   /* ModMeshPoint keys are handles of the old state */
        modsound_new_state();      /* a level load: the mods' own sounds stop */
        hle_modmenu_new_state();   /* a level load: the menu closes, a mod's music stops */
        if (register_log) register_log(L);
        for (int i = 0; i < nmods; i++)
            for (int k = 0; k < mods[i].info.nhooks && !mods[i].failed; k++)
                if (!mods[i].info.hooks[k].script[0]) run_hook(api, L, &mods[i], k, script);
    }
    for (int i = 0; i < nmods; i++)
        for (int k = 0; k < mods[i].info.nhooks && !mods[i].failed; k++)
            if (mods[i].info.hooks[k].script[0] && modini_match(mods[i].info.hooks[k].script, script))
                run_hook(api, L, &mods[i], k, script);
    if (opt_trace) fflush(stdout);
    busy = 0;
}

int mods_mesh_set(const char *group, int on) {
    int was = meshover_group_on(group), n = meshover_group_set(group, on);
    if (n < 0) {
        static int warned;
        if (warned++ < 8) printf("[mods] ModMeshSet: no mesh group \"%.100s\"\n", group ? group : "?");
    } else if (was != (on != 0)) {
        printf("[mods] mesh group %s %s (%d override%s)\n", group, on ? "on" : "off", n, n == 1 ? "" : "s");
    }
    fflush(stdout);
    return n;
}

int mods_mesh_point(const char *group, const char *key, const float *p) {
    const int n = meshover_point_set(group, key, p);
    if (n < 0) {
        static int warned;
        if (warned++ < 8)
            printf("[mods] ModMeshPoint: %s \"%.100s\"\n", n == -1 ? "no mesh group" : "too many points (64) in", group ? group : "?");
        fflush(stdout);
    }
    return n;
}

/* ---- mod data (ModSave / ModLoad) ---- */

typedef struct {
    char prof[MODS_KEY_MAX + 1], key[MODS_KEY_MAX + 1];
    char *val;
} DataEnt;

typedef struct {
    DataEnt *e;
    int n, cap;
} DataSet;

void mods_set_storage(const char *hdd_dir, const char *(*profile_fn)(void)) {
    snprintf(data_dir, sizeof data_dir, "%s", hdd_dir ? hdd_dir : "");
    data_profile = profile_fn;
}

static int data_name_ok(const char *s, size_t n) {
    if (n < 1 || n > MODS_KEY_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

static const char *data_prof(void) {
    const char *p = data_profile ? data_profile() : NULL;
    return p && data_name_ok(p, strlen(p)) ? p : "none";
}

static void data_free(DataSet *d) {
    for (int i = 0; i < d->n; i++) free(d->e[i].val);
    free(d->e);
    memset(d, 0, sizeof *d);
}

static int data_add(DataSet *d, const char *prof, const char *key, const char *val) {
    if (d->n == d->cap) {
        int cap = d->cap ? d->cap * 2 : 16;
        DataEnt *e = realloc(d->e, sizeof *e * (size_t)cap);
        if (!e) return -1;
        d->e = e;
        d->cap = cap;
    }
    DataEnt *x = &d->e[d->n];
    snprintf(x->prof, sizeof x->prof, "%s", prof);
    snprintf(x->key, sizeof x->key, "%s", key);
    if (!(x->val = strdup(val))) return -1;
    d->n++;
    return 0;
}

/* A minimal JSON reader for the files the runtime writes: an object of objects of strings. */
typedef struct { const char *p, *end; } Js;

static void js_ws(Js *j) { while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++; }
static int js_ch(Js *j, char c) {
    js_ws(j);
    if (j->p < j->end && *j->p == c) { j->p++; return 1; }
    return 0;
}
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
/* A string into out (cap bytes, NUL-terminated). 0, or -1 (syntax, too long). */
static int js_str(Js *j, char *out, size_t cap) {
    size_t n = 0;
    if (!js_ch(j, '"')) return -1;
    while (j->p < j->end && *j->p != '"') {
        unsigned c = (unsigned char)*j->p++;
        if (c < 0x20) return -1;
        if (c == '\\') {
            if (j->p >= j->end) return -1;
            char e = *j->p++;
            switch (e) {
            case '"': case '\\': case '/': c = (unsigned char)e; break;
            case 'b': c = 8; break;
            case 'f': c = 12; break;
            case 'n': c = 10; break;
            case 'r': c = 13; break;
            case 't': c = 9; break;
            case 'u': {
                if (j->end - j->p < 4) return -1;
                c = 0;
                for (int i = 0; i < 4; i++) {
                    int h = hexv(*j->p++);
                    if (h < 0) return -1;
                    c = c << 4 | (unsigned)h;
                }
                if (!c) return -1;
                if (c >= 0x80) {   /* UTF-8 (the runtime writes \u only for control characters) */
                    char u[3];
                    int k = 0;
                    if (c < 0x800) u[k++] = (char)(0xc0 | c >> 6);
                    else u[k++] = (char)(0xe0 | c >> 12), u[k++] = (char)(0x80 | (c >> 6 & 0x3f));
                    u[k++] = (char)(0x80 | (c & 0x3f));
                    if (n + (size_t)k >= cap) return -1;
                    memcpy(out + n, u, (size_t)k);
                    n += (size_t)k;
                    continue;
                }
                break;
            }
            default: return -1;
            }
        }
        if (n + 1 >= cap) return -1;
        out[n++] = (char)c;
    }
    if (j->p >= j->end) return -1;
    j->p++;
    out[n] = 0;
    return 0;
}

/* Reads path into d: 0 (a missing file is empty), -1 unreadable (err). */
static int data_read(const char *path, DataSet *d, char *err, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char *b = malloc(MODS_DATA_MAX + 1);
    size_t n = b ? fread(b, 1, MODS_DATA_MAX + 1, f) : 0;
    fclose(f);
    if (!b) { snprintf(err, cap, "out of memory"); return -1; }
    if (n > MODS_DATA_MAX) { free(b); snprintf(err, cap, "%s is larger than %d bytes; left as it is", path, MODS_DATA_MAX); return -1; }
    Js j = {b, b + n};
    char prof[MODS_KEY_MAX + 1], key[MODS_KEY_MAX + 1], val[MODS_VALUE_MAX + 1];
    int ok = js_ch(&j, '{');
    if (ok && !js_ch(&j, '}')) {
        do {
            ok = !js_str(&j, prof, sizeof prof) && data_name_ok(prof, strlen(prof)) && js_ch(&j, ':') && js_ch(&j, '{');
            if (ok && !js_ch(&j, '}')) {
                do {
                    ok = !js_str(&j, key, sizeof key) && data_name_ok(key, strlen(key)) && js_ch(&j, ':') &&
                         !js_str(&j, val, sizeof val) && !data_add(d, prof, key, val);
                } while (ok && js_ch(&j, ','));
                ok = ok && js_ch(&j, '}');
            }
        } while (ok && js_ch(&j, ','));
        ok = ok && js_ch(&j, '}');
    }
    js_ws(&j);
    ok = ok && j.p == j.end;
    free(b);
    if (!ok) {
        data_free(d);
        snprintf(err, cap, "%s is not a mod data file; left as it is", path);
        return -1;
    }
    return 0;
}

/* Appends s as a JSON string; returns the new length (> cap when it did not fit). */
static size_t js_put(char *out, size_t n, size_t cap, const char *s) {
    if (n < cap) out[n] = '"';
    n++;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char tmp[8];
        int k;
        if (c == '"' || c == '\\') k = snprintf(tmp, sizeof tmp, "\\%c", c);
        else if (c < 0x20) k = snprintf(tmp, sizeof tmp, "\\u%04x", c);
        else tmp[0] = (char)c, k = 1;
        for (int i = 0; i < k; i++, n++)
            if (n < cap) out[n] = tmp[i];
    }
    if (n < cap) out[n] = '"';
    return n + 1;
}
static size_t js_raw(char *out, size_t n, size_t cap, const char *s) {
    for (; *s; s++, n++)
        if (n < cap) out[n] = *s;
    return n;
}

/* The file for d, grouped by profile in first-seen order: its length (> cap: too large). */
static size_t data_format(const DataSet *d, char *out, size_t cap) {
    size_t n = js_raw(out, 0, cap, "{");
    int groups = 0;
    for (int i = 0; i < d->n; i++) {
        int seen = 0;
        for (int k = 0; k < i && !seen; k++) seen = !strcmp(d->e[k].prof, d->e[i].prof);
        if (seen) continue;
        n = js_raw(out, n, cap, groups++ ? ",\n  " : "\n  ");
        n = js_put(out, n, cap, d->e[i].prof);
        n = js_raw(out, n, cap, ": {");
        int keys = 0;
        for (int k = i; k < d->n; k++) {
            if (strcmp(d->e[k].prof, d->e[i].prof)) continue;
            n = js_raw(out, n, cap, keys++ ? ",\n    " : "\n    ");
            n = js_put(out, n, cap, d->e[k].key);
            n = js_raw(out, n, cap, ": ");
            n = js_put(out, n, cap, d->e[k].val);
        }
        n = js_raw(out, n, cap, "\n  }");
    }
    return js_raw(out, n, cap, groups ? "\n}\n" : "}\n");
}

static int data_path(const char *id, char *path, size_t cap, char *err, size_t errcap) {
    if (!id || !modini_valid_id(id)) { snprintf(err, errcap, "not a mod id"); return -1; }
    if (!data_dir[0]) { snprintf(err, errcap, "no --hdd folder for mod data"); return -1; }
    snprintf(path, cap, "%s/mods-data/%s.json", data_dir, id);
    return 0;
}

int mods_data_load(const char *id, const char *key, char *out, size_t cap, char *err, size_t errcap) {
    char path[1200];
    DataSet d = {0};
    if (!key || !data_name_ok(key, strlen(key))) { snprintf(err, errcap, "the key must be 1-%d of a-z A-Z 0-9 . _ -", MODS_KEY_MAX); return -1; }
    if (data_path(id, path, sizeof path, err, errcap) || data_read(path, &d, err, errcap)) return -1;
    const char *prof = data_prof();
    int found = 0;
    for (int i = 0; i < d.n && !found; i++)
        if (!strcmp(d.e[i].prof, prof) && !strcmp(d.e[i].key, key)) {
            snprintf(out, cap, "%s", d.e[i].val);
            found = 1;
        }
    data_free(&d);
    return found;
}

int mods_data_save(const char *id, const char *key, const char *value, char *err, size_t cap) {
    char path[1200], tmp[1210];
    DataSet d = {0};
    if (!key || !data_name_ok(key, strlen(key))) { snprintf(err, cap, "the key must be 1-%d of a-z A-Z 0-9 . _ -", MODS_KEY_MAX); return -1; }
    if (value && strlen(value) > MODS_VALUE_MAX) { snprintf(err, cap, "the value is longer than %d bytes", MODS_VALUE_MAX); return -1; }
    if (data_path(id, path, sizeof path, err, cap) || data_read(path, &d, err, cap)) return -1;
    const char *prof = data_prof();
    int at = -1;
    for (int i = 0; i < d.n && at < 0; i++)
        if (!strcmp(d.e[i].prof, prof) && !strcmp(d.e[i].key, key)) at = i;
    int rc = 0;
    if (at >= 0 && value) {
        char *v = strdup(value);
        if (!v) rc = -1;
        else free(d.e[at].val), d.e[at].val = v;
    } else if (at >= 0) {
        free(d.e[at].val);
        memmove(&d.e[at], &d.e[at + 1], sizeof d.e[0] * (size_t)(d.n - at - 1));
        d.n--;
    } else if (value) {
        rc = data_add(&d, prof, key, value);
    } else {
        data_free(&d);
        return 0;   /* removing a key that is not there */
    }
    char *buf = rc ? NULL : malloc(MODS_DATA_MAX + 1);
    if (!buf) { data_free(&d); snprintf(err, cap, "out of memory"); return -1; }
    size_t n = data_format(&d, buf, MODS_DATA_MAX + 1);
    data_free(&d);
    if (n > MODS_DATA_MAX) { free(buf); snprintf(err, cap, "%s would be larger than %d bytes; not written", path, MODS_DATA_MAX); return -1; }
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/mods-data", data_dir);
    plat_mkdir(dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(buf, 1, n, f) == n;
    ok = f && !fclose(f) && ok;
    free(buf);
    if (!ok || plat_replace_file(tmp, path)) {
        remove(tmp);
        snprintf(err, cap, "cannot write %s", path);
        return -1;
    }
    return 0;
}

/* ---- run memory (ModKeep / ModKept) ---- */

typedef struct {
    char id[41], key[MODS_KEY_MAX + 1];
    char *val;
} Kept;
static Kept kept[MODS_KEPT_MAX];
static int nkept;

int mods_keep(const char *id, const char *key, const char *value, char *err, size_t cap) {
    if (!id) { snprintf(err, cap, "no mod"); return -1; }
    if (!key || !data_name_ok(key, strlen(key))) { snprintf(err, cap, "the key must be 1-%d of a-z A-Z 0-9 . _ -", MODS_KEY_MAX); return -1; }
    if (value && strlen(value) > MODS_VALUE_MAX) { snprintf(err, cap, "the value is longer than %d bytes", MODS_VALUE_MAX); return -1; }
    int at = -1;
    for (int i = 0; i < nkept && at < 0; i++)
        if (!strcmp(kept[i].id, id) && !strcmp(kept[i].key, key)) at = i;
    if (!value) {
        if (at >= 0) free(kept[at].val), kept[at] = kept[--nkept];
        return 0;
    }
    char *v = strdup(value);
    if (!v) { snprintf(err, cap, "out of memory"); return -1; }
    if (at < 0) {
        if (nkept == MODS_KEPT_MAX) { free(v); snprintf(err, cap, "more than %d kept values", MODS_KEPT_MAX); return -1; }
        at = nkept++;
        snprintf(kept[at].id, sizeof kept[at].id, "%s", id);
        snprintf(kept[at].key, sizeof kept[at].key, "%s", key);
        kept[at].val = NULL;
    }
    free(kept[at].val);
    kept[at].val = v;
    return 0;
}

static void kept_clear(void) {
    for (int i = 0; i < nkept; i++) free(kept[i].val);
    nkept = 0;
}

const char *mods_kept(const char *id, const char *key) {
    for (int i = 0; id && key && i < nkept; i++)
        if (!strcmp(kept[i].id, id) && !strcmp(kept[i].key, key)) return kept[i].val;
    return NULL;
}

/* ---- the Rumble menu's gangs (ModRumbleGangs) ---- */

static const WarpSig rumble_sigs[] = {
    {"GetRumbleModeData copy", 0x0011cbc4u, 5, {0xb9, 0x84, 0xbd, 0x4f, 0x00}},                     /* mov ecx, 0x4fbd84 */
    {"rumble intro gang line", 0x001244d5u, 8, {0x0f, 0xb7, 0x14, 0x4d, 0x8a, 0xbd, 0x4f, 0x00}},   /* movzx edx, word [ecx*2+0x4fbd8a] */
};

int mods_rumble_gangs(intptr_t bias, int gang1, int gang2, char *err, size_t cap) {
    if (gang1 < 1 || gang1 > MODS_RUMBLE_GANG_MAX || gang2 < 1 || gang2 > MODS_RUMBLE_GANG_MAX) {
        snprintf(err, cap, "gang ids must be 1-%d (got %d, %d)", MODS_RUMBLE_GANG_MAX, gang1, gang2);
        return -1;
    }
    char bad[128];
    if (warp_check(rumble_sigs, (int)(sizeof rumble_sigs / sizeof *rumble_sigs), bias, bad, sizeof bad)) {
        snprintf(err, cap, "the title's rumble code differs at %s: nothing written", bad);
        return -1;
    }
    uint16_t *g = (uint16_t *)(uintptr_t)((intptr_t)MODS_RUMBLE_DATA + 6 + bias);
    g[0] = (uint16_t)(gang1 - 1);
    g[1] = (uint16_t)(gang2 - 1);
    return 0;
}

/* ---- title ---- */

typedef int(__attribute__((cdecl)) * LuaCFn)(uint32_t L);
static void (*title_pushcclosure)(uint32_t L, LuaCFn fn, int n);

/* ModLog(text): `[mods] <id>: text` (the id of the mod whose chunk runs, else "log"). Called by the
 * game's VM, so it realigns the stack (the title keeps it 4-byte aligned). */
int mods_tex_layer(const char *layer, int on) {
    int was = texlayer_on(layer), n = texlayer_set(layer, on);
    if (n < 0) {
        static int warned;
        if (warned++ < 8) printf("[mods] ModTexLayer: no texture layer \"%.100s\"\n", layer ? layer : "?");
    } else if (was != (on != 0)) {
        printf("[mods] texture layer %s %s (%d texture%s)\n", layer, on ? "on" : "off", n, n == 1 ? "" : "s");
    }
    fflush(stdout);
    return n;
}

__attribute__((force_align_arg_pointer, cdecl)) static int mod_log(uint32_t L) {
    const char *s = devlua_title_api.type(L, 1) == 1 ? "nil" : devlua_title_api.tostring(L, 1);
    printf("[mods] %s: %.400s\n", current_id ? current_id : "log", s ? s : "?");
    fflush(stdout);
    return 0;
}

/* ModMeshSet(group, on): mods_mesh_set; on is off for nil or 0. */
__attribute__((force_align_arg_pointer, cdecl)) static int mod_mesh_set(uint32_t L) {
    const DevLuaApi *a = &devlua_title_api;
    const char *g = a->type(L, 1) == 3 ? a->tostring(L, 1) : NULL;
    const int t = a->type(L, 2);
    const char *v = t == 2 ? a->tostring(L, 2) : NULL;
    mods_mesh_set(g, t != 1 && t != -1 && !(v && !strcmp(v, "0")));
    return 0;
}

static void (*title_pushnil)(uint32_t L);
static void (*title_pushnumber)(uint32_t L, double n);

/* ModTexLayer(layer, on): mods_tex_layer; on is off for nil or 0. Returns the number of textures
 * the layer covers, nil when there is no such layer. */
__attribute__((force_align_arg_pointer, cdecl)) static int mod_tex_layer(uint32_t L) {
    const DevLuaApi *a = &devlua_title_api;
    const char *g = a->type(L, 1) == 3 ? a->tostring(L, 1) : NULL;
    const int t = a->type(L, 2);
    const char *v = t == 2 ? a->tostring(L, 2) : NULL;
    int n = mods_tex_layer(g, t != 1 && t != -1 && !(v && !strcmp(v, "0")));
    if (n < 0) title_pushnil(L);
    else title_pushnumber(L, n);
    return 1;
}
static void (*title_pushstring)(uint32_t L, const char *s);

/* ModMeshPoint(group, key, x, y, z): mods_mesh_point; x, y, z all numbers sets the point, else it
 * is removed. Returns the group's number of points, or nil. */
__attribute__((force_align_arg_pointer, cdecl)) static int mod_mesh_point(uint32_t L) {
    const DevLuaApi *a = &devlua_title_api;
    const char *g = a->type(L, 1) == 3 ? a->tostring(L, 1) : NULL;
    const char *key = a->type(L, 2) == 2 || a->type(L, 2) == 3 ? a->tostring(L, 2) : NULL;
    float p[3];
    int have = 1;
    for (int i = 0; i < 3; i++) {
        const char *v = a->type(L, 3 + i) == 2 ? a->tostring(L, 3 + i) : NULL;
        if (v) p[i] = strtof(v, NULL);
        else have = 0;
    }
    const int n = key ? mods_mesh_point(g, key, have ? p : NULL) : -1;
    if (n < 0) title_pushnil(L);
    else title_pushnumber(L, n);
    return 1;
}

/* ModSave / ModLoad: C closures whose one upvalue is the mod id (Lua 4 passes upvalues after the
 * arguments, so the id is the last stack slot). The unbound globals (outside a mod's file) refuse. */
static const char *data_args(uint32_t L, int *nargs) {
    const DevLuaApi *a = &devlua_title_api;
    int top = a->gettop(L);
    *nargs = top - 1;
    return top >= 1 && a->type(L, top) == 3 ? a->tostring(L, top) : NULL;
}
static const char *str_arg(uint32_t L, int i, int nargs) {
    const DevLuaApi *a = &devlua_title_api;
    int t = i <= nargs ? a->type(L, i) : 1;
    return t == 2 || t == 3 ? a->tostring(L, i) : NULL;
}

__attribute__((force_align_arg_pointer, cdecl)) static int mod_save(uint32_t L) {
    int nargs, t = 1;
    const char *id = data_args(L, &nargs), *key = str_arg(L, 1, nargs), *val = str_arg(L, 2, nargs);
    char err[300];
    if (nargs >= 2) t = devlua_title_api.type(L, 2);
    if (t != 1 && t != 2 && t != 3) snprintf(err, sizeof err, "the value must be a string, a number or nil");
    else if (!mods_data_save(id, key, val, err, sizeof err)) {
        title_pushnumber(L, 1);
        return 1;
    }
    printf("[mods] %s: ModSave(%.80s): %s\n", id ? id : "?", key ? key : "?", err);
    fflush(stdout);
    title_pushnil(L);
    return 1;
}

__attribute__((force_align_arg_pointer, cdecl)) static int mod_load(uint32_t L) {
    int nargs;
    const char *id = data_args(L, &nargs), *key = str_arg(L, 1, nargs);
    char val[MODS_VALUE_MAX + 1], err[300];
    int r = mods_data_load(id, key, val, sizeof val, err, sizeof err);
    if (r < 0) {
        printf("[mods] %s: ModLoad(%.80s): %s\n", id ? id : "?", key ? key : "?", err);
        fflush(stdout);
    }
    if (r == 1) title_pushstring(L, val);
    else title_pushnil(L);
    return 1;
}

/* ModOption(name): the value of the mod's own mod.ini key `name` (modini_option), or nil. */
const char *mods_option(const char *id, const char *name) {
    for (int i = 0; id && i < nmods; i++)
        if (!strcmp(mods[i].info.id, id)) return modini_option(&mods[i].info, name);
    return NULL;
}

__attribute__((force_align_arg_pointer, cdecl)) static int mod_option(uint32_t L) {
    int nargs;
    const char *id = data_args(L, &nargs), *name = str_arg(L, 1, nargs);
    const char *v = mods_option(id, name);
    if (v) title_pushstring(L, v);
    else title_pushnil(L);
    return 1;
}

__attribute__((force_align_arg_pointer, cdecl)) static int mod_keep(uint32_t L) {
    int nargs, t = 1;
    const char *id = data_args(L, &nargs), *key = str_arg(L, 1, nargs), *val = str_arg(L, 2, nargs);
    char err[200];
    if (nargs >= 2) t = devlua_title_api.type(L, 2);
    if (t != 1 && t != 2 && t != 3) snprintf(err, sizeof err, "the value must be a string, a number or nil");
    else if (!mods_keep(id, key, val, err, sizeof err)) {
        title_pushnumber(L, 1);
        return 1;
    }
    printf("[mods] %s: ModKeep(%.80s): %s\n", id ? id : "?", key ? key : "?", err);
    fflush(stdout);
    title_pushnil(L);
    return 1;
}

__attribute__((force_align_arg_pointer, cdecl)) static int mod_kept(uint32_t L) {
    int nargs;
    const char *id = data_args(L, &nargs), *v = mods_kept(id, str_arg(L, 1, nargs));
    if (v) title_pushstring(L, v);
    else title_pushnil(L);
    return 1;
}


/* ModSound(name [, x, y, z] [, volume [, loop]]), ModSoundMove(h, x, y, z), ModSoundStop(h)
 * (src/modsound.h): closures bound to the mod id like ModSave. */
static int num_arg(uint32_t L, int i, int nargs, float *v) {
    const char *s = i <= nargs && devlua_title_api.type(L, i) == 2 ? devlua_title_api.tostring(L, i) : NULL;
    if (s) *v = strtof(s, NULL);
    return s != NULL;
}
static void sound_warn(const char *id, const char *what, const char *why) {
    static int warned;
    if (warned++ < 16) {
        printf("[mods] %s: %s: %s\n", id ? id : "?", what, why);
        fflush(stdout);
    }
}
__attribute__((force_align_arg_pointer, cdecl)) static int mod_sound(uint32_t L) {
    int nargs;
    const char *id = data_args(L, &nargs), *name = str_arg(L, 1, nargs);
    float v[5];
    int k = 0;
    while (k < 5 && num_arg(L, 2 + k, nargs, &v[k])) k++;
    const float *pos = k >= 3 ? v : NULL;
    const int o = k >= 3 ? 3 : 0;
    const float vol = k > o ? v[o] : 1.0f;
    const int loop = k > o + 1 && v[o + 1] != 0;
    char err[200], what[120];
    const int h = modsound_play(id, name, pos, vol, loop, err, sizeof err);
    if (h > 0) {
        static int logged;
        if (logged++ < 32) {
            if (pos) printf("[mods] %s: sound %s at %.2f %.2f %.2f (volume %.2f%s)\n", id, name, pos[0], pos[1], pos[2], vol, loop ? ", looping" : "");
            else printf("[mods] %s: sound %s (volume %.2f%s)\n", id, name, vol, loop ? ", looping" : "");
            fflush(stdout);
        }
        title_pushnumber(L, h);
    } else {
        snprintf(what, sizeof what, "ModSound(%.60s)", name ? name : "?");
        sound_warn(id, what, err);
        title_pushnil(L);
    }
    return 1;
}
__attribute__((force_align_arg_pointer, cdecl)) static int mod_sound_move(uint32_t L) {
    int nargs;
    const char *id = data_args(L, &nargs);
    float h, p[3];
    int ok = num_arg(L, 1, nargs, &h) && num_arg(L, 2, nargs, &p[0]) && num_arg(L, 3, nargs, &p[1]) && num_arg(L, 4, nargs, &p[2]);
    if (ok && !modsound_move(id, (int)h, p)) title_pushnumber(L, 1);
    else title_pushnil(L);
    return 1;
}
__attribute__((force_align_arg_pointer, cdecl)) static int mod_sound_stop(uint32_t L) {
    int nargs;
    const char *id = data_args(L, &nargs);
    float h;
    if (num_arg(L, 1, nargs, &h) && !modsound_stop(id, (int)h)) title_pushnumber(L, 1);
    else title_pushnil(L);
    return 1;
}

/* ModRumbleGangs(gang1, gang2): mods_rumble_gangs on the title; 1, or nil (the reason logged). */
__attribute__((force_align_arg_pointer, cdecl)) static int mod_rumble_gangs(uint32_t L) {
    const DevLuaApi *a = &devlua_title_api;
    const char *s1 = a->type(L, 1) == 2 ? a->tostring(L, 1) : NULL, *s2 = a->type(L, 2) == 2 ? a->tostring(L, 2) : NULL;
    const double g1 = s1 ? strtod(s1, NULL) : 0, g2 = s2 ? strtod(s2, NULL) : 0;
    char err[200];
    int ok = 0;
    if (!s1 || !s2) snprintf(err, sizeof err, "two gang ids wanted");
    else if (!(g1 >= 1 && g1 <= MODS_RUMBLE_GANG_MAX && g2 >= 1 && g2 <= MODS_RUMBLE_GANG_MAX) || g1 != (int)g1 || g2 != (int)g2)
        snprintf(err, sizeof err, "gang ids must be whole numbers 1-%d", MODS_RUMBLE_GANG_MAX);
    else ok = !mods_rumble_gangs(0, (int)g1, (int)g2, err, sizeof err);
    static int logged;
    if (logged++ < 32) {
        if (ok) printf("[mods] %s: rumble gangs set: %d vs %d (the DJ says dj_gang_%d, dj_gang_%d)\n", current_id ? current_id : "log", (int)g1, (int)g2, (int)g1, (int)g2);
        else printf("[mods] %s: ModRumbleGangs: %s\n", current_id ? current_id : "log", err);
        fflush(stdout);
    }
    if (ok) title_pushnumber(L, 1);
    else title_pushnil(L);
    return 1;
}

__attribute__((force_align_arg_pointer, cdecl)) static int mod_data_unbound(uint32_t L) {
    static int warned;
    if (warned++ < 8) {
        printf("[mods] ModSave / ModLoad / ModSound called outside a mod's file: keep the copy the file sees (MyMod.Save = ModSave)\n");
        fflush(stdout);
    }
    title_pushnil(L);
    return 1;
}

static void title_bind_data(uint32_t L, const char *id) {
    const DevLuaApi *a = &devlua_title_api;
    if (id) title_pushstring(L, id);
    title_pushcclosure(L, id ? mod_save : mod_data_unbound, id ? 1 : 0);
    a->setglobal(L, "ModSave");
    if (id) title_pushstring(L, id);
    title_pushcclosure(L, id ? mod_load : mod_data_unbound, id ? 1 : 0);
    a->setglobal(L, "ModLoad");
    if (id) title_pushstring(L, id);
    title_pushcclosure(L, id ? mod_option : mod_data_unbound, id ? 1 : 0);
    a->setglobal(L, "ModOption");
    if (id) title_pushstring(L, id);
    title_pushcclosure(L, id ? mod_keep : mod_data_unbound, id ? 1 : 0);
    a->setglobal(L, "ModKeep");
    if (id) title_pushstring(L, id);
    title_pushcclosure(L, id ? mod_kept : mod_data_unbound, id ? 1 : 0);
    a->setglobal(L, "ModKept");
    if (id) title_pushstring(L, id);
    title_pushcclosure(L, id ? mod_sound : mod_data_unbound, id ? 1 : 0);
    a->setglobal(L, "ModSound");
    if (id) title_pushstring(L, id);
    title_pushcclosure(L, id ? mod_sound_move : mod_data_unbound, id ? 1 : 0);
    a->setglobal(L, "ModSoundMove");
    if (id) title_pushstring(L, id);
    title_pushcclosure(L, id ? mod_sound_stop : mod_data_unbound, id ? 1 : 0);
    a->setglobal(L, "ModSoundStop");
}

static void title_register_log(uint32_t L) {
    title_pushcclosure(L, mod_log, 0);
    devlua_title_api.setglobal(L, "ModLog");
    title_pushcclosure(L, mod_mesh_set, 0);
    devlua_title_api.setglobal(L, "ModMeshSet");
    title_pushcclosure(L, mod_mesh_point, 0);
    devlua_title_api.setglobal(L, "ModMeshPoint");
    title_pushcclosure(L, mod_tex_layer, 0);
    devlua_title_api.setglobal(L, "ModTexLayer");
    title_bind_data(L, NULL);
    title_pushcclosure(L, hle_gore_lua, 0);
    devlua_title_api.setglobal(L, "ModBlood");
    title_pushcclosure(L, hle_scars_lua, 0);
    devlua_title_api.setglobal(L, "ModScar");
    title_pushcclosure(L, hle_huspeed_lua, 0);
    devlua_title_api.setglobal(L, "ModSpeed");
    title_pushcclosure(L, mod_rumble_gangs, 0);
    devlua_title_api.setglobal(L, "ModRumbleGangs");
    title_pushcclosure(L, hle_husprint_lua, 0);
    devlua_title_api.setglobal(L, "ModSprint");
    title_pushcclosure(L, hle_combo_slowmo_lua, 0);
    devlua_title_api.setglobal(L, "ModComboSlowMo");
    title_pushcclosure(L, hle_radar_lua, 0);
    devlua_title_api.setglobal(L, "ModRadarColor");
    title_pushcclosure(L, hle_diffmenu_lua_option, 0);
    devlua_title_api.setglobal(L, "ModDifficultyOption");
    title_pushcclosure(L, hle_diffmenu_lua_chosen, 0);
    devlua_title_api.setglobal(L, "ModDifficultyChosen");
    static const struct { LuaCFn fn; const char *name; } menu_fns[] = {
        {hle_modmenu_lua_open, "ModMenuOpen"},         {hle_modmenu_lua_state, "ModMenuState"},
        {hle_modmenu_lua_close, "ModMenuClose"},       {hle_modmenu_lua_music_play, "ModMusicPlay"},
        {hle_modmenu_lua_music_playing, "ModMusicPlaying"}, {hle_modmenu_lua_music_mute, "ModMusicMute"},
        {hle_modmenu_lua_values, "ModMenuValues"},     {hle_modmenu_lua_key, "ModMenuKey"},
    };
    for (size_t i = 0; i < sizeof menu_fns / sizeof *menu_fns; i++) {
        title_pushcclosure(L, menu_fns[i].fn, 0);
        devlua_title_api.setglobal(L, menu_fns[i].name);
    }
}

/* ScriptFixLog(text): `[scriptfix] text` (hle_scriptfix.h). */
__attribute__((force_align_arg_pointer, cdecl)) static int scriptfix_log(uint32_t L) {
    const char *s = devlua_title_api.type(L, 1) == 1 ? "nil" : devlua_title_api.tostring(L, 1);
    printf("[scriptfix] %.400s\n", s ? s : "?");
    fflush(stdout);
    return 0;
}

static void scriptfix_bind(uint32_t L) {
    title_pushcclosure(L, scriptfix_log, 0);
    devlua_title_api.setglobal(L, "ScriptFixLog");
}

static int mods_hooked;   /* mods or --mods-trace: mods_on_script runs; else only the script fixes */

static void on_dobuffer(WarpFrame *f) {
    const uint32_t *args = (const uint32_t *)(uintptr_t)f->esp;   /* ret, L, buf, size, name */
    if (f->eax != 0) return;                                      /* the script failed: no hooks */
    const char *name = (const char *)(uintptr_t)args[4];
    if (!name) return;
    if (devlua_host_chunks) return;                              /* a --lua or mod chunk itself */
    /* The fixes first: a mod's hook for the same script sees the title's functions as fixed. */
    scriptfix_on_script(&devlua_title_api, args[1], name, scriptfix_bind);
    if (mods_hooked) mods_on_script(&devlua_title_api, args[1], name);
}

static const WarpSig push_sigs[] = {
    {"lua_pushcclosure", MODS_PUSHCCLOSURE, 5, {0xe9, 0xbb, 0xde, 0x00, 0x00}},
    {"lua_pushnil", MODS_PUSHNIL, 13, {0x56, 0x8b, 0x74, 0x24, 0x08, 0x8b, 0x06, 0xc7, 0x00, 0x01, 0x00, 0x00, 0x00}},
    {"lua_pushnumber", MODS_PUSHNUMBER, 14, {0xdd, 0x44, 0x24, 0x08, 0x56, 0x8b, 0x74, 0x24, 0x08, 0x8b, 0x06, 0xdd, 0x58, 0x08}},
    {"lua_pushstring", MODS_PUSHSTRING, 12, {0x8b, 0x54, 0x24, 0x08, 0x85, 0xd2, 0x56, 0x75, 0x0e, 0x8b, 0x74, 0x24}},
};

int hle_mods_install(void) {
    const int want_mods = nmods || opt_trace;
    if (!want_mods && !scriptfix_enabled()) return 0;
    char bad[256];
    if (devlua_check_api(0, bad, sizeof bad) || warp_check(push_sigs, (int)(sizeof push_sigs / sizeof push_sigs[0]), 0, bad, sizeof bad)) {
        if (want_mods) printf("[mods] not available: signature mismatch at %s (nothing written; no mod runs)\n", bad);
        if (scriptfix_enabled()) printf("[scriptfix] not available: signature mismatch at %s (nothing written)\n", bad);
        return 0;
    }
    uint8_t *stub = plat_alloc_exec(WARP_AFTER_STUB_MAX);
    if (!stub) return 0;
    title_pushcclosure = (void (*)(uint32_t, LuaCFn, int))(uintptr_t)MODS_PUSHCCLOSURE;
    title_pushnil = (void (*)(uint32_t))(uintptr_t)MODS_PUSHNIL;
    title_pushnumber = (void (*)(uint32_t, double))(uintptr_t)MODS_PUSHNUMBER;
    title_pushstring = (void (*)(uint32_t, const char *))(uintptr_t)MODS_PUSHSTRING;
    if (!want_mods) {   /* only the script fixes: the hook, none of the mod layer */
        warp_build_after_stub(stub, (uint32_t)(uintptr_t)on_dobuffer, devlua_dobuffer_orig(), MODS_DOBUFFER_LEN,
                              DEVLUA_DOBUFFER + MODS_DOBUFFER_LEN, 4);
        warp_write_jmp((uint8_t *)(uintptr_t)DEVLUA_DOBUFFER, DEVLUA_DOBUFFER, (uint32_t)(uintptr_t)stub, MODS_DOBUFFER_LEN);
        printf("[scriptfix] hook after lua_dobuffer (%08x)\n", DEVLUA_DOBUFFER);
        fflush(stdout);
        return 0;
    }
    mods_hooked = 1;
    huspeed_lua_init(title_pushnumber, title_pushnil);
    radar_lua_init(title_pushnumber, title_pushnil);
    diffmenu_lua_init(title_pushnumber, title_pushnil);
    hle_modmenu_lua_init(title_pushnumber, title_pushnil, title_pushstring);
    register_log = title_register_log;
    bind_data = title_bind_data;
    hle_gore_trace = opt_trace;
    hle_scars_trace = opt_trace;
    hle_radar_trace = opt_trace;
    /* Mods grow the level states past the title's Lua collection cap: without this a collection
     * nests inside another and frees live tables (hle_luagc.h). */
    hle_luagc_install();
    warp_build_after_stub(stub, (uint32_t)(uintptr_t)on_dobuffer, devlua_dobuffer_orig(), MODS_DOBUFFER_LEN,
                          DEVLUA_DOBUFFER + MODS_DOBUFFER_LEN, 4);
    warp_write_jmp((uint8_t *)(uintptr_t)DEVLUA_DOBUFFER, DEVLUA_DOBUFFER, (uint32_t)(uintptr_t)stub, MODS_DOBUFFER_LEN);
    printf("[mods] hook after lua_dobuffer (%08x)%s\n", DEVLUA_DOBUFFER, opt_trace ? ", tracing script loads" : "");
    fflush(stdout);
    return 1;
}
