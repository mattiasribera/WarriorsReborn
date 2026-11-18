/* Save backups before a game with save-changing mods, and the crash-context mods line (modsafe.h). */
#include "modsafe.h"
#include "diaglog.h"
#include "hle_mods.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define make_dir(p) _mkdir(p)
#define remove_dir(p) _rmdir(p)
#define stat_nofollow stat
#else
#include <unistd.h>
#define make_dir(p) mkdir((p), 0755)
#define remove_dir(p) rmdir(p)
#define stat_nofollow lstat   /* never follow a link out of the folders walked */
#endif

static const char PARTIAL[] = ".partial";

static uint64_t fnv(uint64_t h, const void *data, size_t n) {
    const unsigned char *b = data;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* The sorted entry names of a folder (no . and ..); -1 when it cannot be opened. */
static int list_names(const char *path, char ***out) {
    DIR *d = opendir(path);
    if (!d) return -1;
    int n = 0, cap = 16;
    char **v = malloc(sizeof *v * (size_t)cap);
    struct dirent *de;
    while (v && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (n == cap) {
            char **nv = realloc(v, sizeof *v * (size_t)(cap *= 2));
            if (!nv) break;
            v = nv;
        }
        if ((v[n] = strdup(de->d_name))) n++;
    }
    closedir(d);
    if (!v) return -1;
    qsort(v, (size_t)n, sizeof *v, cmp_names);
    *out = v;
    return n;
}

static void free_names(char **v, int n) {
    for (int i = 0; v && i < n; i++) free(v[i]);
    free(v);
}

typedef struct { int files; uint64_t bytes, sig; } Scan;

static void scan(const char *dir, const char *rel, Scan *s) {
    char **v;
    int n = list_names(dir, &v);
    for (int i = 0; i < n; i++) {
        char p[1024], r[512];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, v[i]);
        snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", v[i]);
        if (stat_nofollow(p, &st)) continue;
        s->sig = fnv(s->sig, r, strlen(r) + 1);
        if (S_ISDIR(st.st_mode)) scan(p, r, s);
        else if (S_ISREG(st.st_mode)) {
            uint64_t size = (uint64_t)st.st_size, mt = (uint64_t)st.st_mtime;
            s->files++;
            s->bytes += size;
            s->sig = fnv(fnv(s->sig, &size, sizeof size), &mt, sizeof mt);
        }
    }
    if (n >= 0) free_names(v, n);
}

static int copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : NULL;
    int ok = a && b;
    char buf[65536];
    size_t r;
    while (ok && (r = fread(buf, 1, sizeof buf, a)) > 0) ok = fwrite(buf, 1, r, b) == r;
    if (ok && ferror(a)) ok = 0;
    if (a) fclose(a);
    if (b && fclose(b)) ok = 0;
    return ok;
}

static int copy_tree(const char *from, const char *to) {
    char **v;
    if (make_dir(to)) return 0;
    int n = list_names(from, &v), ok = n >= 0;
    for (int i = 0; ok && i < n; i++) {
        char a[1024], b[1024];
        struct stat st;
        snprintf(a, sizeof a, "%s/%s", from, v[i]);
        snprintf(b, sizeof b, "%s/%s", to, v[i]);
        if (stat_nofollow(a, &st)) ok = 0;
        else if (S_ISDIR(st.st_mode)) ok = copy_tree(a, b);
        else if (S_ISREG(st.st_mode)) ok = copy_file(a, b);
    }
    if (n >= 0) free_names(v, n);
    return ok;
}

/* Only used inside <hdd>/mods-backups (rotation, unfinished copies). */
static void remove_tree(const char *path) {
    char **v;
    int n = list_names(path, &v);
    for (int i = 0; i < n; i++) {
        char p[1024];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", path, v[i]);
        if (!stat_nofollow(p, &st) && S_ISDIR(st.st_mode)) remove_tree(p);
        else remove(p);
    }
    if (n >= 0) free_names(v, n);
    remove_dir(path);
}

static int is_dir(const char *p) {
    struct stat st;
    return !stat_nofollow(p, &st) && S_ISDIR(st.st_mode);
}

static int ends_partial(const char *name) {
    size_t n = strlen(name), k = sizeof PARTIAL - 1;
    return n >= k && !strcmp(name + n - k, PARTIAL);
}

/* A finished backup: a folder with save/ and backup.txt; *sig = its signature (0 if none). */
static int finished(const char *root, const char *name, uint64_t *sig) {
    char p[1024], line[128];
    if (ends_partial(name)) return 0;
    snprintf(p, sizeof p, "%s/%s/save", root, name);
    if (!is_dir(p)) return 0;
    snprintf(p, sizeof p, "%s/%s/backup.txt", root, name);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    *sig = 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "signature = ", 12)) *sig = strtoull(line + 12, NULL, 16);
    fclose(f);
    return 1;
}

/* The finished backups, newest first (names are timestamps); returns the count (*out may be NULL). */
static int backups(const char *root, char ***out, uint64_t *newest_sig) {
    char **v;
    int n = list_names(root, &v), k = 0;
    *out = NULL;
    if (n < 0) return 0;
    char **keep = malloc(sizeof *keep * (size_t)(n ? n : 1));
    for (int i = n - 1; keep && i >= 0; i--) {   /* reverse name order = newest first */
        uint64_t sig;
        if (finished(root, v[i], &sig)) {
            if (!k && newest_sig) *newest_sig = sig;
            keep[k++] = v[i];
            v[i] = NULL;
        }
    }
    free_names(v, n);   /* the names not kept (free(NULL) for the kept ones) */
    *out = keep;
    return keep ? k : 0;
}

int modsafe_count(const char *hdd) {
    char root[1024], **v;
    snprintf(root, sizeof root, "%s/mods-backups", hdd);
    int n = backups(root, &v, NULL);
    free_names(v, n);
    return n;
}

int modsafe_backup(const char *hdd, const char *stamp, int keep, char *out, size_t cap) {
    char save[1024], root[1024], name[128], tmp[1200], dest[1200], p[1300];
    out[0] = 0;
    snprintf(save, sizeof save, "%s/partition1/UDATA/%s", hdd, MODSAFE_TITLE);
    snprintf(root, sizeof root, "%s/mods-backups", hdd);
    Scan sc = {0, 0, 1469598103934665603ull};
    if (!is_dir(save)) return MODSAFE_NOSAVE;
    scan(save, "", &sc);
    if (!sc.files) return MODSAFE_NOSAVE;
    if (!is_dir(root) && make_dir(root)) {
        snprintf(out, cap, "cannot create %s", root);
        return MODSAFE_FAILED;
    }
    char **v;
    int n = list_names(root, &v);
    for (int i = 0; i < n; i++)   /* left by a game that was closed mid-copy */
        if (ends_partial(v[i])) snprintf(p, sizeof p, "%s/%s", root, v[i]), remove_tree(p);
    if (n >= 0) free_names(v, n);
    uint64_t newest = 0;
    n = backups(root, &v, &newest);
    if (n > 0 && newest == sc.sig) {
        snprintf(out, cap, "%s/%s", root, v[0]);
        free_names(v, n);
        return MODSAFE_UNCHANGED;
    }
    free_names(v, n);
    snprintf(name, sizeof name, "%s", stamp);
    for (int k = 2; snprintf(dest, sizeof dest, "%s/%s", root, name), is_dir(dest); k++)
        snprintf(name, sizeof name, "%s-%d", stamp, k);
    snprintf(tmp, sizeof tmp, "%s%s", dest, PARTIAL);
    snprintf(p, sizeof p, "%s/save", tmp);
    int ok = !make_dir(tmp) && copy_tree(save, p);
    if (ok) {
        snprintf(p, sizeof p, "%s/backup.txt", tmp);
        FILE *f = fopen(p, "wb");
        ok = f && fprintf(f, "kind = mods\nfiles = %d\nbytes = %llu\nsignature = %016llx\n", sc.files,
                          (unsigned long long)sc.bytes, (unsigned long long)sc.sig) > 0;
        if (f && fclose(f)) ok = 0;
    }
    if (!ok || rename(tmp, dest)) {
        remove_tree(tmp);
        snprintf(out, cap, "cannot copy the save to %s", dest);
        return MODSAFE_FAILED;
    }
    snprintf(out, cap, "%s", dest);
    if (keep > 0) {
        n = backups(root, &v, NULL);
        for (int i = keep; i < n; i++) snprintf(p, sizeof p, "%s/%s", root, v[i]), remove_tree(p);
        free_names(v, n);
    }
    return MODSAFE_DONE;
}

void mods_safety_start(const char *hdd) {
    char line[512], savers[256];
    size_t w = 0, sw = 0;
    line[0] = savers[0] = 0;
    for (int i = 0; i < mods_count(); i++) {
        const ModInfo *m = mods_get(i);
        int r = snprintf(line + w, sizeof line - w, "%s%s %s", i ? ", " : "", m->id, m->version);
        if (r > 0) w += (size_t)r < sizeof line - w ? (size_t)r : sizeof line - w - 1;
        if (m->saves && (r = snprintf(savers + sw, sizeof savers - sw, "%s%s", sw ? ", " : "", m->id)) > 0)
            sw += (size_t)r < sizeof savers - sw ? (size_t)r : sizeof savers - sw - 1;
    }
    dlog_set_mods(line);
    if (!sw || !hdd) return;
    char stamp[32], out[1300];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (!tm || !strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", tm)) snprintf(stamp, sizeof stamp, "backup");
    switch (modsafe_backup(hdd, stamp, MODSAFE_KEEP, out, sizeof out)) {
    case MODSAFE_DONE: printf("[mods] save backed up to %s (before %s, which change%s save data)\n", out, savers, strchr(savers, ',') ? "" : "s"); break;
    case MODSAFE_UNCHANGED: printf("[mods] save unchanged since the backup %s; no new copy\n", out); break;
    case MODSAFE_NOSAVE: printf("[mods] no save in %s yet; nothing to back up\n", hdd); break;
    default: printf("[mods] save backup failed: %s; the game starts anyway\n", out); break;
    }
    fflush(stdout);
}
