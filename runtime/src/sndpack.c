/* Sound replacement packs (format and keying in sndpack.h). */
#include "sndpack.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

size_t sndpack_budget = 256u << 20;

static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

uint32_t sndpack_crc32(const char *s) {
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++) c = t[(c ^ (uint8_t)*s) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

int sndpack_cue_name(const char *rel, char *out, size_t n) {
    size_t len = strlen(rel);
    if (len <= 4 || strcasecmp(rel + len - 4, ".wav")) return -1;
    len -= 4;
    int pre = strncasecmp(rel, "vags/", 5) && strncasecmp(rel, "vags\\", 5);
    if ((pre ? 5 : 0) + len + 1 > n) return -1;
    size_t o = 0;
    if (pre) { memcpy(out, "vags/", 5); o = 5; }
    for (size_t i = 0; i < len; i++) out[o++] = rel[i] == '\\' ? '/' : (char)tolower((unsigned char)rel[i]);
    out[o] = 0;
    return 0;
}

/* ---- WAV ---- */

static int fail(char *err, size_t errlen, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s", msg);
    return -1;
}

int sndpack_wav_info(const uint8_t *d, size_t n, SndWav *w, char *err, size_t errlen) {
    memset(w, 0, sizeof *w);
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return fail(err, errlen, "not a RIFF WAVE file");
    int have_fmt = 0, tag = 0, align = 0;
    for (size_t o = 12; o + 8 <= n;) {
        uint32_t len = rd32(d + o + 4);
        size_t body = o + 8;
        if (!memcmp(d + o, "fmt ", 4) && len >= 16 && body + 16 <= n) {
            tag = (int)rd16(d + body);
            w->channels = (int)rd16(d + body + 2);
            w->rate = rd32(d + body + 4);
            align = (int)rd16(d + body + 12);
            w->bits = (int)rd16(d + body + 14);
            if (tag == 0xFFFE && len >= 40 && body + 26 <= n) tag = (int)rd16(d + body + 24);   /* sub-format GUID */
            have_fmt = 1;
        } else if (!memcmp(d + o, "data", 4)) {
            if (!have_fmt) return fail(err, errlen, "data before fmt");
            if (tag != 1 && tag != 3) return fail(err, errlen, "compressed WAV (only PCM or float)");
            if (tag == 3 && w->bits != 32) return fail(err, errlen, "float WAV must be 32-bit");
            if (tag == 1 && w->bits != 8 && w->bits != 16 && w->bits != 24 && w->bits != 32)
                return fail(err, errlen, "PCM WAV must be 8, 16, 24 or 32-bit");
            if (w->channels != 1 && w->channels != 2) return fail(err, errlen, "only mono or stereo");
            if (!w->rate || w->rate > 384000) return fail(err, errlen, "bad sample rate");
            int frame = w->channels * w->bits / 8;
            if (align && align != frame) return fail(err, errlen, "bad block align");
            size_t avail = n - body < len ? n - body : len;   /* a data length past the end is clamped */
            w->is_float = tag == 3;
            w->data = body;
            w->frames = (uint32_t)(avail / (size_t)frame);
            return 0;
        }
        o = body + len + (len & 1);
        if (o < body) break;
    }
    return fail(err, errlen, have_fmt ? "no data chunk" : "no fmt chunk");
}

int16_t *sndpack_wav_pcm16(const uint8_t *d, size_t n, const SndWav *w) {
    (void)n;
    size_t count = (size_t)w->frames * w->channels;
    int16_t *out = malloc(count ? count * sizeof *out : 1);
    if (!out) return NULL;
    const uint8_t *p = d + w->data;
    for (size_t i = 0; i < count; i++) {
        int32_t v;
        if (w->is_float) {
            uint32_t u = rd32(p + 4 * i);
            float f;
            memcpy(&f, &u, 4);
            f *= 32768.0f;
            v = f >= 32767.0f ? 32767 : f <= -32768.0f ? -32768 : (int32_t)f;
        } else if (w->bits == 8) v = ((int)p[i] - 128) * 256;
        else if (w->bits == 16) v = (int16_t)rd16(p + 2 * i);
        else if (w->bits == 24) v = (int16_t)rd16(p + 3 * i + 1);
        else v = (int16_t)rd16(p + 4 * i + 2);
        out[i] = (int16_t)v;
    }
    return out;
}

/* ---- name table ---- */

int sndpack_names_parse(const uint8_t *glr, size_t n, SndName **out) {
    *out = NULL;
    if (n < 16) return -1;
    uint32_t items = rd32(glr);
    size_t o = 16;
    for (uint32_t i = 0; i < items && o + 16 <= n; i++) {
        uint32_t type = rd32(glr + o), size = rd32(glr + o + 4);
        size_t body = o + 16;
        if (size > n - body) return -1;
        if (type == 0x29) {
            if (size < 4) return -1;
            uint32_t count = rd32(glr + body);
            if (count > (size - 4) / 12) return -1;
            SndName *t = malloc(count ? count * sizeof *t : 1);
            if (!t) return -1;
            for (uint32_t k = 0; k < count; k++) {
                t[k].crc = rd32(glr + body + 4 + 12 * k);
                t[k].cue = rd32(glr + body + 8 + 12 * k);
            }
            *out = t;
            return (int)count;
        }
        o = body + size;
    }
    return -1;
}

static uint8_t *read_range(const char *path, long off, size_t len, size_t *got) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (len == (size_t)-1) {
        fseek(f, 0, SEEK_END);
        long end = ftell(f);
        len = end > off ? (size_t)(end - off) : 0;
    }
    uint8_t *d = malloc(len ? len : 1);
    if (d && (fseek(f, off, SEEK_SET) || fread(d, 1, len, f) != len)) { free(d); d = NULL; }
    fclose(f);
    if (d && got) *got = len;
    return d;
}

int sndpack_names_load(const char *disc, SndName **out) {
    *out = NULL;
    char path[1024];
    snprintf(path, sizeof path, "%s/XBoxWad.idx", disc);
    size_t n = 0;
    uint8_t *idx = read_range(path, 0, (size_t)-1, &n);
    if (!idx) return -1;
    int r = -1;
    uint32_t want = sndpack_crc32("ee_files\\warriors.glr");
    if (n >= 20) {
        uint32_t wads = rd32(idx), files = rd32(idx + 4), wad_table = rd32(idx + 12), file_table = rd32(idx + 16);
        if (wads <= 256 && wad_table <= n && (n - wad_table) / 36 >= wads && file_table <= n && (n - file_table) / 16 >= files)
            for (uint32_t i = 0; i < files; i++) {
                const uint8_t *f = idx + file_table + 16 * i;
                uint32_t loc = rd32(f), size = rd32(f + 4);
                if (rd32(f + 12) != want || loc >> 24 >= wads || size > (64u << 20)) continue;
                char wad[33];
                memcpy(wad, idx + wad_table + 36 * (loc >> 24), 32);
                wad[32] = 0;
                snprintf(path, sizeof path, "%s/%s", disc, wad);
                uint8_t *glr = read_range(path, (long)(loc & 0xFFFFFF) << 10, size, NULL);
                if (glr) r = sndpack_names_parse(glr, size, out);
                free(glr);
                break;
            }
    }
    free(idx);
    return r;
}

/* ---- pack ---- */

typedef struct {
    char *path;                  /* relative to the pack */
    uint32_t cue;
    int16_t *pcm;
    XwbBank wb;
    XwbEntry e;
} Override;
typedef struct { char bank[17]; int wave, ov; } Key;

static Override *ovs;
static int novs;
static Key *keys;
static int nkeys;

static char **files;
static int nfiles, capfiles;

static void walk(const char *root, const char *rel, int depth) {
    char path[1024];
    snprintf(path, sizeof path, "%s%s%s", root, *rel ? "/" : "", rel);
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sub[1024], full[1100];
        if ((size_t)snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", e->d_name) >= sizeof sub) continue;
        snprintf(full, sizeof full, "%s/%s", root, sub);
        struct stat st;
        if (stat(full, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 16) walk(root, sub, depth + 1);
        } else if (nfiles < 1 << 20) {
            if (nfiles == capfiles) {
                capfiles = capfiles ? capfiles * 2 : 64;
                char **f = realloc(files, (size_t)capfiles * sizeof *f);
                if (!f) break;
                files = f;
            }
            files[nfiles++] = strdup(sub);
        }
    }
    closedir(d);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int cmp_name(const void *a, const void *b) {
    uint32_t x = ((const SndName *)a)->crc, y = ((const SndName *)b)->crc;
    return x < y ? -1 : x > y;
}

void sndpack_close(void) {
    for (int i = 0; i < novs; i++) { free(ovs[i].path); free(ovs[i].pcm); }
    free(ovs);
    free(keys);
    ovs = NULL;
    keys = NULL;
    novs = nkeys = 0;
}

int sndpack_open_table(const char *dir, const SndName *names, int n) {
    sndpack_close();
    DIR *probe = opendir(dir);
    if (!probe) return -1;
    closedir(probe);
    walk(dir, "", 0);
    qsort(files, (size_t)nfiles, sizeof *files, cmp_str);
    SndName *sorted = malloc((n > 0 ? (size_t)n : 1) * sizeof *sorted);
    if (sorted && n > 0) { memcpy(sorted, names, (size_t)n * sizeof *sorted); qsort(sorted, (size_t)n, sizeof *sorted, cmp_name); }
    ovs = calloc(nfiles ? (size_t)nfiles : 1, sizeof *ovs);
    size_t used = 0;
    for (int i = 0; i < nfiles && sorted && ovs; i++) {
        char cue_name[512], path[1200], err[96];
        if (sndpack_cue_name(files[i], cue_name, sizeof cue_name)) continue;
        SndName key = {sndpack_crc32(cue_name), 0};
        const SndName *hit = bsearch(&key, sorted, (size_t)n, sizeof *sorted, cmp_name);
        if (!hit) { printf("[audio] sound pack: %s: no cue named %s\n", files[i], cue_name); continue; }
        int dup = 0;
        for (int k = 0; k < novs && !dup; k++) dup = ovs[k].cue == hit->cue;
        if (dup) { printf("[audio] sound pack: %s: cue %s already replaced\n", files[i], cue_name); continue; }
        snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        size_t len = 0;
        uint8_t *d = read_range(path, 0, (size_t)-1, &len);
        SndWav w;
        if (!d || sndpack_wav_info(d, len, &w, err, sizeof err)) {
            printf("[audio] sound pack: %s: %s\n", files[i], d ? err : "cannot read");
            free(d);
            continue;
        }
        size_t bytes = (size_t)w.frames * w.channels * 2;
        if (bytes > sndpack_budget - used) {
            printf("[audio] sound pack: %s: over the %zu MiB budget, skipped\n", files[i], sndpack_budget >> 20);
            free(d);
            continue;
        }
        int16_t *pcm = sndpack_wav_pcm16(d, len, &w);
        free(d);
        if (!pcm) continue;
        used += bytes;
        Override *o = &ovs[novs++];
        o->path = strdup(files[i]);
        o->cue = hit->cue;
        o->pcm = pcm;
        snprintf(o->wb.name, sizeof o->wb.name, "sndpack");
        o->wb.count = 1;
        o->wb.entries = &o->e;
        o->wb.mem = (const uint8_t *)pcm;
        o->wb.fd = -1;
        o->e.codec = XWB_PCM;
        o->e.channels = w.channels;
        o->e.bits = 16;
        o->e.rate = w.rate;
        o->e.length = (uint32_t)bytes;
    }
    for (int i = 0; i < nfiles; i++) free(files[i]);
    free(files);
    files = NULL;
    nfiles = capfiles = 0;
    free(sorted);
    if (!novs) { free(ovs); ovs = NULL; }
    return novs;
}

int sndpack_open(const char *dir, const char *disc) {
    DIR *probe = opendir(dir);
    if (!probe) return -1;
    closedir(probe);
    SndName *names = NULL;
    int n = sndpack_names_load(disc, &names);
    if (n < 0) return -2;
    int r = sndpack_open_table(dir, names, n);
    free(names);
    return r;
}

int sndpack_active(void) { return novs > 0; }

int sndpack_bind(const XsbBank *sb) {
    int added = 0;
    for (int i = 0; i < novs; i++) {
        uint32_t rng = 1;
        XsbSound s;
        if (xsb_cue_sound(sb, ovs[i].cue, &rng, &s)) continue;
        for (int w = 0; w < s.nwaves; w++) {
            Key *k = realloc(keys, (size_t)(nkeys + 1) * sizeof *k);
            if (!k) return added;
            keys = k;
            xsb_bank_name(sb, s.w[w].bank, keys[nkeys].bank);
            keys[nkeys].wave = s.w[w].wave;
            keys[nkeys++].ov = i;
            added++;
        }
    }
    return added;
}

const XwbEntry *sndpack_find(const char *bank, int wave, const XwbBank **wb, const char **path) {
    for (int i = 0; i < nkeys; i++)
        if (keys[i].wave == wave && !strcasecmp(keys[i].bank, bank)) {
            Override *o = &ovs[keys[i].ov];
            if (wb) *wb = &o->wb;
            if (path) *path = o->path;
            return &o->e;
        }
    return NULL;
}

uint32_t sndpack_ms(const char *bank, int wave) {
    const XwbEntry *e = sndpack_find(bank, wave, NULL, NULL);
    return e ? xwb_entry_ms(e) : XSB_NO_MS;
}
