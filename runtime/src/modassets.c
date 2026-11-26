/* assets.ini parser, cache keys and the cache folder (format notes in modassets.h); the build
 * steps are in modassets_build.c. */
#include "modassets.h"
#include "disc.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#define ma_mkdir(p) _mkdir(p)
#else
#define ma_mkdir(p) mkdir(p, 0755)
#endif

/* stb_image_write's zlib (src/texpack_stb.c builds the implementation). */
unsigned char *stbi_zlib_compress(unsigned char *data, int data_len, int *out_len, int quality);

unsigned modassets_time_ms = MODASSETS_TIME_MS, modassets_bytes_max = MODASSETS_BYTES_MAX, modassets_files_max = MODASSETS_FILES_MAX;

/* ---- parser ---- */

static int perr(char *err, size_t cap, const char *file, int line, const char *msg, const char *arg) {
    if (err && cap) snprintf(err, cap, "%s:%d: %s%s%s", file, line, msg, arg ? " " : "", arg ? arg : "");
    return -1;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}

/* braces: a [layer] name may hold {code} and {stage} (expanded per entry and stage) */
static int valid_name(const char *s, int braces) {
    if (!*s || strlen(s) >= 64) return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_' && *s != '.' && !(braces && (*s == '{' || *s == '}'))) return 0;
    return 1;
}

static const char *const kinds[] = {"source", "texture", "mesh", "hide", "set", "layer", "sound", "collision"};
#define NKINDS (int)(sizeof kinds / sizeof kinds[0])

int ma_recipe_parse(const char *text, size_t len, const char *file, MaRecipe *r, char *err, size_t cap) {
    memset(r, 0, sizeof *r);
    snprintf(r->file, sizeof r->file, "%s", file);
    int lineno = 0, cur = -1, capl = 0, caps = 0;
    size_t i = 0;
    if (len >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) i = 3;   /* a UTF-8 byte order mark */
    while (i < len) {
        char buf[MODASSETS_LINE_MAX + 64];
        size_t n = 0;
        lineno++;
        while (i < len && text[i] != '\n') {
            if (n + 1 >= sizeof buf) {
                ma_recipe_free(r);
                return perr(err, cap, file, lineno, "line too long", NULL);
            }
            buf[n++] = text[i++];
        }
        i++;
        buf[n] = 0;
        if (memchr(buf, 0, n)) {
            ma_recipe_free(r);
            return perr(err, cap, file, lineno, "not a text file", NULL);
        }
        char *semi = strchr(buf, ';');
        if (semi) *semi = 0;
        char *s = trim(buf);
        if (!*s) continue;
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (!end || *trim(end + 1)) {
                ma_recipe_free(r);
                return perr(err, cap, file, lineno, "a section line is [kind name]", NULL);
            }
            *end = 0;
            char kind[32] = "", name[96] = "";
            if (sscanf(s + 1, "%31s %95s", kind, name) != 2) {
                ma_recipe_free(r);
                return perr(err, cap, file, lineno, "a section line is [kind name]", NULL);
            }
            int k = 0;
            while (k < NKINDS && strcmp(kind, kinds[k])) k++;
            if (k == NKINDS) {
                ma_recipe_free(r);
                return perr(err, cap, file, lineno, "unknown section kind", kind);
            }
            if (!valid_name(name, !strcmp(kind, "layer"))) {
                ma_recipe_free(r);
                return perr(err, cap, file, lineno, "a name is 1-63 of a-z A-Z 0-9 - _ . (a layer's also {code} {stage}):", name);
            }
            for (int j = 0; j < r->nsec; j++)
                if (!strcmp(r->sec[j].name, name)) {
                    ma_recipe_free(r);
                    return perr(err, cap, file, lineno, "a second section named", name);
                }
            if (r->nsec == caps) {
                caps = caps ? caps * 2 : 16;
                MaSection *ns = realloc(r->sec, sizeof *ns * (size_t)caps);
                if (!ns) {
                    ma_recipe_free(r);
                    return perr(err, cap, file, lineno, "out of memory", NULL);
                }
                r->sec = ns;
            }
            cur = r->nsec++;
            MaSection *sec = &r->sec[cur];
            memset(sec, 0, sizeof *sec);
            snprintf(sec->kind, sizeof sec->kind, "%s", kind);
            snprintf(sec->name, sizeof sec->name, "%s", name);
            sec->line = lineno;
            sec->first = r->nlines;
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) {
            ma_recipe_free(r);
            return perr(err, cap, file, lineno, "expected key = value", NULL);
        }
        *eq = 0;
        char *key = trim(s), *value = trim(eq + 1);
        if (!*key || strlen(key) >= 32 || !*value) {
            ma_recipe_free(r);
            return perr(err, cap, file, lineno, "expected key = value", NULL);
        }
        if (strlen(value) >= MODASSETS_LINE_MAX) {
            ma_recipe_free(r);
            return perr(err, cap, file, lineno, "value too long", NULL);
        }
        if (cur < 0) {
            if (strcmp(key, "format")) {
                ma_recipe_free(r);
                return perr(err, cap, file, lineno, "before the first section only `format = 1`; unknown key", key);
            }
            if (strcmp(value, "1")) {
                ma_recipe_free(r);
                return perr(err, cap, file, lineno, "this game reads assets.ini format 1, not", value);
            }
            r->format = 1;
            continue;
        }
        if (r->nlines == capl) {
            capl = capl ? capl * 2 : 64;
            MaLine *nl = realloc(r->lines, sizeof *nl * (size_t)capl);
            if (!nl) {
                ma_recipe_free(r);
                return perr(err, cap, file, lineno, "out of memory", NULL);
            }
            r->lines = nl;
        }
        MaLine *l = &r->lines[r->nlines++];
        l->line = lineno;
        snprintf(l->key, sizeof l->key, "%s", key);
        snprintf(l->value, sizeof l->value, "%s", value);
        r->sec[cur].n++;
    }
    if (!r->format) {
        ma_recipe_free(r);
        return perr(err, cap, file, 1, "no `format = 1` line", NULL);
    }
    return 0;
}

void ma_recipe_free(MaRecipe *r) {
    free(r->sec);
    free(r->lines);
    r->sec = NULL, r->lines = NULL;
    r->nsec = r->nlines = 0;
}

/* ---- key ---- */

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

uint64_t ma_cache_key(const char *recipe, size_t len, const char *mod_version, uint64_t disc_fingerprint) {
    uint8_t fp[8];
    for (int i = 0; i < 8; i++) fp[i] = (uint8_t)(disc_fingerprint >> (8 * i));
    uint64_t h = fnv(0xcbf29ce484222325ull, "modassets " MODASSETS_ENGINE, sizeof "modassets " MODASSETS_ENGINE);
    h = fnv(h, recipe, len);
    h = fnv(h, "\0", 1);
    h = fnv(h, mod_version ? mod_version : "", mod_version ? strlen(mod_version) + 1 : 1);
    return fnv(h, fp, 8);
}

/* ---- PNG ---- */

int ma_png_write(const char *path, int w, int h, const uint8_t *rgba) {
    size_t row = (size_t)w * 4 + 1, n = row * (size_t)h;
    uint8_t *raw = malloc(n ? n : 1);
    if (!raw) return -1;
    for (int y = 0; y < h; y++) {
        raw[y * row] = 0;
        memcpy(raw + y * row + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }
    int zlen = 0;
    unsigned char *z = stbi_zlib_compress(raw, (int)n, &zlen, 8);
    free(raw);
    if (!z) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(z);
        return -1;
    }
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 6, 0, 0, 0};
    struct { const char *tag; const uint8_t *body; uint32_t len; } ch[3] = {{"IHDR", ihdr, 13}, {"IDAT", z, (uint32_t)zlen}, {"IEND", NULL, 0}};
    int ok = fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) == 8;
    for (int i = 0; i < 3 && ok; i++) {
        uint8_t be[4] = {(uint8_t)(ch[i].len >> 24), (uint8_t)(ch[i].len >> 16), (uint8_t)(ch[i].len >> 8), (uint8_t)ch[i].len};
        uint32_t crc = disc_crc32(disc_crc32(0, ch[i].tag, 4), ch[i].body, ch[i].len);
        uint8_t cb[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
        ok = fwrite(be, 1, 4, f) == 4 && fwrite(ch[i].tag, 1, 4, f) == 4 && (!ch[i].len || fwrite(ch[i].body, 1, ch[i].len, f) == ch[i].len) &&
             fwrite(cb, 1, 4, f) == 4;
    }
    free(z);
    if (fclose(f)) ok = 0;
    return ok ? 0 : -1;
}

/* ---- cache folder ---- */

static int is_dir(const char *p) {
    struct stat st;
    return !stat(p, &st) && S_ISDIR(st.st_mode);
}

static int exists(const char *p) {
    struct stat st;
    return !stat(p, &st);
}

/* Removes a file or a folder with everything in it (the cache holds two levels at most). */
static int remove_tree(const char *path, int depth) {
    if (!is_dir(path)) return remove(path);
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[1400];
        if ((size_t)snprintf(sub, sizeof sub, "%s/%s", path, e->d_name) >= sizeof sub) continue;
        if (depth < 4) remove_tree(sub, depth + 1);
    }
    if (d) closedir(d);
    return rmdir(path);
}

static int mkdirs(const char *path) {
    char p[1400];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/' || *s == '\\') {
            char c = *s;
            *s = 0;
            if (!is_dir(p) && ma_mkdir(p) && errno != EEXIST) return -1;
            *s = c;
        }
    return is_dir(p) || !ma_mkdir(p) ? 0 : -1;
}

static char *read_text(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = NULL;
    long n = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    if (n >= 0 && n <= (1 << 20) && !fseek(f, 0, SEEK_SET) && (b = malloc((size_t)n + 1))) {
        if (fread(b, 1, (size_t)n, f) != (size_t)n) {
            free(b);
            b = NULL;
        } else {
            b[n] = 0;
            *len = (size_t)n;
        }
    }
    fclose(f);
    return b;
}

static int fail(char *err, size_t cap, const char *fmt, const char *a, const char *b) {
    if (err && cap) snprintf(err, cap, fmt, a, b);
    return -1;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int modassets_current(const char *mod_dir, const char *id, const char *version, const char *disc, const char *hdd) {
    char path[1400];
    snprintf(path, sizeof path, "%s/assets.ini", mod_dir);
    size_t len = 0;
    char *text = read_text(path, &len);
    if (!text) return exists(path) ? 0 : -1;
    Disc d;
    if (!disc || !*disc || !hdd || !*hdd || disc_open(&d, disc)) {
        free(text);
        return 0;
    }
    uint64_t key = ma_cache_key(text, len, version, d.fingerprint);
    disc_close(&d);
    free(text);
    if (ma_own_key(mod_dir, &key, NULL, 0)) return 0;
    snprintf(path, sizeof path, "%s/mods-cache/%s/%016llx/done", hdd, id, (unsigned long long)key);
    return exists(path) ? 1 : 0;
}

int modassets_prepare(const char *mod_dir, const char *id, const char *version, const char *disc, const char *hdd, int rebuild,
                      MaResult *res, char *err, size_t cap) {
    memset(res, 0, sizeof *res);
    if (err && cap) err[0] = 0;
    char path[1400];
    snprintf(path, sizeof path, "%s/assets.ini", mod_dir);
    size_t len = 0;
    char *text = read_text(path, &len);
    if (!text) return exists(path) ? fail(err, cap, "assets.ini: cannot read it%s%s", "", "") : 0;
    MaRecipe r;
    if (ma_recipe_parse(text, len, "assets.ini", &r, err, cap)) {
        free(text);
        return -1;
    }
    if (!hdd || !*hdd || !disc || !*disc) {
        free(text);
        ma_recipe_free(&r);
        return fail(err, cap, "no %s folder to build from%s", !disc || !*disc ? "disc" : "hdd", "");
    }
    Disc d;
    if (disc_open(&d, disc)) {
        free(text);
        ma_recipe_free(&r);
        return fail(err, cap, "no game files: %s/XBoxWad.idx cannot be read%s", disc, "");
    }
    const uint64_t fingerprint = d.fingerprint;
    disc_close(&d);
    res->key = ma_cache_key(text, len, version, fingerprint);
    if (ma_own_key(mod_dir, &res->key, err, cap)) {
        free(text);
        ma_recipe_free(&r);
        return -1;
    }
    snprintf(r.dir, sizeof r.dir, "%s", mod_dir);
    {
        char ip[1300];
        snprintf(ip, sizeof ip, "%s/mods-cache/disc-%016llx.idx", hdd, (unsigned long long)fingerprint);
        ma_set_index_path(ip);
    }
    free(text);
    char base[1200], partial[1300];
    snprintf(base, sizeof base, "%s/mods-cache/%s", hdd, id);
    snprintf(res->cache, sizeof res->cache, "%s/%016llx", base, (unsigned long long)res->key);
    snprintf(partial, sizeof partial, "%s.partial", res->cache);
    snprintf(path, sizeof path, "%s/done", res->cache);
    if (!rebuild && exists(path)) {
        int outs = 0;
        for (int i = 0; i < r.nsec; i++)
            outs += !strcmp(r.sec[i].kind, "mesh") || !strcmp(r.sec[i].kind, "hide") || !strcmp(r.sec[i].kind, "layer") || !strcmp(r.sec[i].kind, "sound") ||
                    !strcmp(r.sec[i].kind, "collision");
        res->outputs = outs;
        ma_recipe_free(&r);
        return 1;
    }
    if (mkdirs(base)) {
        ma_recipe_free(&r);
        res->cache[0] = 0;
        return fail(err, cap, "cannot create the cache folder %s%s", base, "");
    }
    remove_tree(partial, 0);   /* an interrupted build */
    if (ma_mkdir(partial)) {
        ma_recipe_free(&r);
        res->cache[0] = 0;
        return fail(err, cap, "cannot create %s%s", partial, "");
    }
    double t0 = now_ms();
    uint64_t bytes = 0;
    int outs = ma_build(&r, disc, partial, &res->files, &bytes, err, cap);
    snprintf(res->warn, sizeof res->warn, "%s", r.warn);
    ma_recipe_free(&r);
    res->ms = now_ms() - t0;
    if (outs < 0) {
        remove_tree(partial, 0);
        res->cache[0] = 0;
        return -1;
    }
    snprintf(path, sizeof path, "%s/build.json", partial);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "{\"id\": \"%s\", \"version\": \"%s\", \"engine\": \"%s\", \"key\": \"%016llx\", \"outputs\": %d, \"files\": %d, \"ms\": %.0f}\n", id,
                version ? version : "", MODASSETS_ENGINE, (unsigned long long)res->key, outs, res->files, res->ms);
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/done", partial);
    f = fopen(path, "wb");
    int ok = f && !fclose(f);
    if (ok) {
        remove_tree(res->cache, 0);   /* --rebuild-mod-assets: the old build of this key */
        ok = !rename(partial, res->cache);
    }
    if (!ok) {
        remove_tree(partial, 0);
        res->cache[0] = 0;
        return fail(err, cap, "cannot finish the cache folder %s%s", partial, "");
    }
    DIR *dd = opendir(base);   /* the mod's other keys are stale now */
    struct dirent *e;
    char keep[32];
    snprintf(keep, sizeof keep, "%016llx", (unsigned long long)res->key);
    while (dd && (e = readdir(dd))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, keep)) continue;
        char sub[1400];
        if ((size_t)snprintf(sub, sizeof sub, "%s/%s", base, e->d_name) < sizeof sub) remove_tree(sub, 0);
    }
    if (dd) closedir(dd);
    res->built = 1;
    res->outputs = outs;
    return 1;
}
