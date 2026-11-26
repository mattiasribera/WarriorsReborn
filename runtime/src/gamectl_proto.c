/* Live game control: protocol, JSON replies, live options, request queue (see gamectl_proto.h). */
#include "gamectl_proto.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const kind_names[] = {
    [GC_NONE] = "?", [GC_PING] = "ping", [GC_STATUS] = "status", [GC_HELP] = "help", [GC_OPTS] = "opts", [GC_QUIT] = "quit",
    [GC_LUA] = "lua", [GC_INPUT] = "input", [GC_WARP] = "warp", [GC_WAIT_FRAMES] = "wait", [GC_WAIT_LEVEL] = "wait",
    [GC_CAPTURE] = "capture", [GC_STATS] = "stats", [GC_OPT] = "opt", [GC_SAVE_COPY] = "save-copy",
};

const char *gc_kind_name(GcKind k) {
    return k >= 0 && k < (int)(sizeof kind_names / sizeof kind_names[0]) && kind_names[k] ? kind_names[k] : "?";
}

static int fail(char *err, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    return -1;
}

/* Next blank-separated word of *s into w (cap bytes); *s moves past it and the blanks after it. */
static size_t word(const char **s, char *w, size_t cap) {
    const char *p = *s;
    size_t n = 0;
    while (*p && !isspace((unsigned char)*p)) {
        if (n + 1 < cap) w[n] = *p;
        n++, p++;
    }
    w[n < cap ? n : cap - 1] = 0;
    while (isspace((unsigned char)*p)) p++;
    *s = p;
    return n;
}

static int count(const char *s, uint32_t lo, uint32_t hi, uint32_t *v) {
    if (!*s || !isdigit((unsigned char)*s)) return -1;
    char *end;
    errno = 0;
    unsigned long x = strtoul(s, &end, 10);
    if (*end || errno || x < lo || x > hi) return -1;
    *v = (uint32_t)x;
    return 0;
}

static int ends_with_ci(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (n < m) return 0;
    for (size_t i = 0; i < m; i++)
        if (tolower((unsigned char)s[n - m + i]) != suffix[i]) return 0;
    return 1;
}

int gc_parse(const char *line, GcCommand *c, char *err, size_t cap) {
    memset(c, 0, sizeof *c);
    if (!line) return fail(err, cap, "empty command");
    size_t len = strlen(line);
    while (len && isspace((unsigned char)line[len - 1])) len--;
    while (len && isspace((unsigned char)*line)) line++, len--;
    if (!len) return fail(err, cap, "empty command");
    if (len >= GC_LINE_MAX) return fail(err, cap, "line too long (at most %d bytes)", GC_LINE_MAX - 1);
    char buf[GC_LINE_MAX], verb[32], w[64];
    memcpy(buf, line, len);
    buf[len] = 0;
    const char *rest = buf;
    word(&rest, verb, sizeof verb);
    static const struct { const char *name; GcKind kind; } verbs[] = {
        {"ping", GC_PING}, {"status", GC_STATUS}, {"help", GC_HELP}, {"opts", GC_OPTS}, {"quit", GC_QUIT}, {"lua", GC_LUA},
        {"input", GC_INPUT}, {"warp", GC_WARP}, {"wait", GC_WAIT_FRAMES}, {"capture", GC_CAPTURE}, {"stats", GC_STATS},
        {"opt", GC_OPT}, {"save-copy", GC_SAVE_COPY},
    };
    for (size_t i = 0; i < sizeof verbs / sizeof verbs[0]; i++)
        if (!strcmp(verb, verbs[i].name)) c->kind = verbs[i].kind;
    switch (c->kind) {
    case GC_NONE:
        return fail(err, cap, "unknown command '%.31s' (help lists them)", verb);
    case GC_PING: case GC_STATUS: case GC_HELP: case GC_OPTS: case GC_QUIT:
        if (*rest) return fail(err, cap, "%s takes no argument", verb);
        return 0;
    case GC_LUA: case GC_INPUT:
        if (!*rest) return fail(err, cap, c->kind == GC_LUA ? "lua needs a chunk" : "input needs steps (FRAME:[pN/]CONTROLS[:FRAMES],...)");
        snprintf(c->text, sizeof c->text, "%s", rest);
        return 0;
    case GC_WARP:
        if (!*rest) return fail(err, cap, "warp needs a target: mN[:C], levelNN[:C], hideout[:C], coney, gym or title");
        word(&rest, c->text, sizeof c->text);
        if (*rest) return fail(err, cap, "warp takes one target");
        return 0;
    case GC_WAIT_FRAMES:
        word(&rest, w, sizeof w);
        if (!strcmp(w, "frames")) {
            word(&rest, w, sizeof w);
            if (*rest || count(w, 1, GC_WAIT_MAX, &c->n)) return fail(err, cap, "wait frames N: N is 1-%u", GC_WAIT_MAX);
            return 0;
        }
        if (!strcmp(w, "level")) {
            c->kind = GC_WAIT_LEVEL;
            c->n = GC_LEVEL_WAIT_DEFAULT;
            if (!*rest) return 0;
            word(&rest, w, sizeof w);
            if (*rest || count(w, 1, GC_WAIT_MAX, &c->n)) return fail(err, cap, "wait level [MAXFRAMES]: MAXFRAMES is 1-%u", GC_WAIT_MAX);
            return 0;
        }
        return fail(err, cap, "use wait frames N or wait level [MAXFRAMES]");
    case GC_CAPTURE:
        if (!*rest) return fail(err, cap, "capture needs a file path (.bmp or .png)");
        snprintf(c->text, sizeof c->text, "%s", rest);
        if (!ends_with_ci(c->text, ".bmp") && !ends_with_ci(c->text, ".png")) return fail(err, cap, "capture: the file must end in .bmp or .png");
        return 0;
    case GC_STATS:
        word(&rest, w, sizeof w);
        if (*rest || count(w, 1, GC_STATS_MAX, &c->n)) return fail(err, cap, "stats N: N frames, 1-%u", GC_STATS_MAX);
        return 0;
    case GC_OPT:
        if (!*rest) { c->kind = GC_OPTS; return 0; }
        word(&rest, c->name, sizeof c->name);
        if (!*rest) return fail(err, cap, "opt %s needs a value (opts lists the options)", c->name);
        snprintf(c->text, sizeof c->text, "%s", rest);
        return 0;
    case GC_SAVE_COPY:
        word(&rest, w, sizeof w);
        if (strcmp(w, "load") || !*rest) return fail(err, cap, "use save-copy load DIR");
        snprintf(c->text, sizeof c->text, "%s", rest);
        return 0;
    default:
        return fail(err, cap, "unknown command");
    }
}

/* ---- JSON ---- */

static void put(GcJson *j, const char *s, size_t n) {
    if (j->overflow || j->len + n + 3 > j->cap) { j->overflow = 1; return; }   /* room for "}\n" and the NUL */
    memcpy(j->buf + j->len, s, n);
    j->len += n;
    j->buf[j->len] = 0;
}
static void puts_(GcJson *j, const char *s) { put(j, s, strlen(s)); }
static void put_escaped(GcJson *j, const char *s) {
    put(j, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char e[8];
        if (*p == '"' || *p == '\\') e[0] = '\\', e[1] = (char)*p, e[2] = 0;
        else if (*p == '\n') strcpy(e, "\\n");
        else if (*p == '\r') strcpy(e, "\\r");
        else if (*p == '\t') strcpy(e, "\\t");
        else if (*p < 0x20) snprintf(e, sizeof e, "\\u%04x", *p);
        else e[0] = (char)*p, e[1] = 0;
        puts_(j, e);
    }
    put(j, "\"", 1);
}
static void key(GcJson *j, const char *k) {
    put(j, ",", 1);
    put_escaped(j, k);
    put(j, ":", 1);
}

void gc_json_begin(GcJson *j, char *buf, size_t cap, int ok, const char *cmd) {
    j->buf = buf;
    j->cap = cap;
    j->len = 0;
    j->overflow = cap < 8;
    if (cap) buf[0] = 0;
    puts_(j, ok ? "{\"ok\":true,\"cmd\":" : "{\"ok\":false,\"cmd\":");
    put_escaped(j, cmd ? cmd : "?");
}
void gc_json_str(GcJson *j, const char *k, const char *v) {
    key(j, k);
    if (v) put_escaped(j, v);
    else puts_(j, "null");
}
void gc_json_uint(GcJson *j, const char *k, uint64_t v) {
    char b[24];
    snprintf(b, sizeof b, "%llu", (unsigned long long)v);
    key(j, k);
    puts_(j, b);
}
void gc_json_num(GcJson *j, const char *k, double v) {
    char b[32];
    if (isfinite(v)) snprintf(b, sizeof b, "%.6g", v);
    else strcpy(b, "null");
    key(j, k);
    puts_(j, b);
}
void gc_json_bool(GcJson *j, const char *k, int v) {
    key(j, k);
    puts_(j, v ? "true" : "false");
}
void gc_json_lines(GcJson *j, const char *k, const char *text) {
    key(j, k);
    put(j, "[", 1);
    int first = 1;
    char line[GC_LINE_MAX];
    for (const char *p = text ? text : ""; *p;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line ? n : sizeof line - 1), p);
        if (!first) put(j, ",", 1);
        put_escaped(j, line);
        first = 0;
        p += n + (nl ? 1 : 0);
    }
    put(j, "]", 1);
}
size_t gc_json_end(GcJson *j) {
    if (j->overflow) {
        /* the command name was the first field: keep it */
        char cmd[40] = "?";
        const char *c = j->cap > 26 ? strstr(j->buf, "\"cmd\":\"") : NULL;
        if (c) sscanf(c + 7, "%39[^\"]", cmd);
        return gc_json_error(j->buf, j->cap, cmd, "reply too long");
    }
    if (j->len + 3 <= j->cap) {
        memcpy(j->buf + j->len, "}\n", 3);
        j->len += 2;
    }
    return j->len;
}
size_t gc_json_error(char *buf, size_t cap, const char *cmd, const char *error) {
    GcJson j;
    gc_json_begin(&j, buf, cap, 0, cmd);
    gc_json_str(&j, "error", error);
    if (j.overflow) {   /* last resort: a fixed reply */
        int n = snprintf(buf, cap, "{\"ok\":false,\"error\":\"reply too long\"}\n");
        return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap ? cap - 1 : 0;
    }
    return gc_json_end(&j);
}

/* ---- live options ---- */

const GcOptInfo gc_options[] = {
    {"turbo", GC_OPT_LIVE, GC_VAL_BOOL, 0, 1, "run unthrottled (the fast-boot clock: one 60 Hz period of game time per frame)"},
    {"taa-sharpness", GC_OPT_LIVE, GC_VAL_FLOAT, 0, 1, "TAA sharpening from the next frame (with --taa)"},
    {"parallax", GC_OPT_LIVE, GC_VAL_FLOAT, 0, 0.2f, "relief depth of the normal maps (with --normal-maps)"},
    {"tri-check", GC_OPT_LIVE, GC_VAL_BOOL, 0, 1, "log non-finite / exploded vertices (--tri-check)"},
    {"spin", GC_OPT_LIVE, GC_VAL_BOOL, 0, 1, "turn the left stick round (the car-theft spin)"},
    {"mash", GC_OPT_LIVE, GC_VAL_BOOL, 0, 1, "tap A 12 times a second (gym, minigames)"},
    {"right-stick", GC_OPT_LIVE, GC_VAL_PAIR, -1, 1, "hold the right stick at X Y (-1..1 each; 0 0 releases)"},
    {"idle-timeout", GC_OPT_LIVE, GC_VAL_UINT, 10, 86400, "seconds without a command before the session quits"},
    {"sharpness", GC_OPT_RESTART, GC_VAL_FLOAT, 0, 0, "--sharpen"},
    {"bloom-strength", GC_OPT_RESTART, GC_VAL_FLOAT, 0, 0, "--bloom-strength"},
    {"fps-overlay", GC_OPT_RESTART, GC_VAL_BOOL, 0, 0, "--fps-overlay"},
    {"combat-debug", GC_OPT_RESTART, GC_VAL_BOOL, 0, 0, "--combat-debug"},
    {"ai-debug", GC_OPT_RESTART, GC_VAL_BOOL, 0, 0, "--ai-debug"},
    {"stats", GC_OPT_RESTART, GC_VAL_BOOL, 0, 0, "--stats"},
    {"resolution", GC_OPT_RESTART, GC_VAL_PAIR, 0, 0, "--resolution"},
};
const int gc_option_count = (int)(sizeof gc_options / sizeof gc_options[0]);

const GcOptInfo *gc_opt_find(const char *name) {
    for (int i = 0; i < gc_option_count; i++)
        if (!strcmp(gc_options[i].name, name)) return &gc_options[i];
    return NULL;
}

static int number(const char *s, float *v) {
    char *end;
    errno = 0;
    double x = strtod(s, &end);
    if (end == s || *end || errno || !isfinite(x)) return -1;
    *v = (float)x;
    return 0;
}

int gc_opt_value(const GcOptInfo *o, const char *value, float out[2], char *err, size_t cap) {
    out[0] = out[1] = 0;
    if (!o) return fail(err, cap, "unknown option (opts lists them)");
    if (o->cls != GC_OPT_LIVE) return fail(err, cap, "%s cannot change in a running game: start the session with %s", o->name, o->what);
    char a[64] = "", b[64] = "";
    const char *s = value;
    word(&s, a, sizeof a);
    word(&s, b, sizeof b);
    if (o->val == GC_VAL_BOOL) {
        if (*b || *s) return fail(err, cap, "%s: 0/1 (on/off)", o->name);
        if (!strcmp(a, "1") || !strcmp(a, "on") || !strcmp(a, "true")) out[0] = 1;
        else if (!strcmp(a, "0") || !strcmp(a, "off") || !strcmp(a, "false")) out[0] = 0;
        else return fail(err, cap, "%s: 0/1 (on/off)", o->name);
        return 0;
    }
    if (o->val == GC_VAL_PAIR) {
        if (!*b || *s || number(a, &out[0]) || number(b, &out[1]) || out[0] < o->lo || out[0] > o->hi || out[1] < o->lo || out[1] > o->hi)
            return fail(err, cap, "%s: two numbers %g-%g", o->name, o->lo, o->hi);
        return 0;
    }
    if (*b || *s || number(a, &out[0]) || out[0] < o->lo || out[0] > o->hi ||
        (o->val == GC_VAL_UINT && out[0] != floorf(out[0])))
        return fail(err, cap, o->val == GC_VAL_UINT ? "%s: a whole number %g-%g" : "%s: a number %g-%g", o->name, o->lo, o->hi);
    return 0;
}

/* ---- queue ---- */

void gcq_init(GcQueue *q) {
    memset(q, 0, sizeof *q);
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->done_cv, NULL);
}

static void reply_closed(GcRequest *r) {
    GcCommand c;
    char err[8];
    gc_parse(r->line, &c, err, sizeof err);
    r->reply_len = gc_json_error(r->reply, sizeof r->reply, gc_kind_name(c.kind), "the game is exiting");
    r->done = 1;
}

int gcq_submit(GcQueue *q, GcRequest *r) {
    r->done = 0;
    r->next = NULL;
    r->reply[0] = 0;
    r->reply_len = 0;
    pthread_mutex_lock(&q->lock);
    if (q->closed) {
        pthread_mutex_unlock(&q->lock);
        reply_closed(r);
        return -1;
    }
    if (q->tail) q->tail->next = r;
    else q->head = r;
    q->tail = r;
    while (!r->done) pthread_cond_wait(&q->done_cv, &q->lock);
    int closed = q->closed && strstr(r->reply, "the game is exiting");
    pthread_mutex_unlock(&q->lock);
    return closed ? -1 : 0;
}

GcRequest *gcq_peek(GcQueue *q) {
    pthread_mutex_lock(&q->lock);
    GcRequest *r = q->head;
    pthread_mutex_unlock(&q->lock);
    return r;
}

void gcq_finish(GcQueue *q, GcRequest *r, const char *reply, size_t len) {
    pthread_mutex_lock(&q->lock);
    if (q->head == r) {
        q->head = r->next;
        if (!q->head) q->tail = NULL;
        if (len >= sizeof r->reply) len = sizeof r->reply - 1;
        memcpy(r->reply, reply, len);
        r->reply[len] = 0;
        r->reply_len = len;
        r->done = 1;
        pthread_cond_broadcast(&q->done_cv);
    }
    pthread_mutex_unlock(&q->lock);
}

void gcq_close(GcQueue *q) {
    pthread_mutex_lock(&q->lock);
    q->closed = 1;
    for (GcRequest *r = q->head; r; r = r->next) reply_closed(r);
    q->head = q->tail = NULL;
    pthread_cond_broadcast(&q->done_cv);
    pthread_mutex_unlock(&q->lock);
}

int gcq_pending(GcQueue *q) {
    pthread_mutex_lock(&q->lock);
    int n = 0;
    for (GcRequest *r = q->head; r; r = r->next) n++;
    pthread_mutex_unlock(&q->lock);
    return n;
}

/* ---- captures ---- */

static void le32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8), p[2] = (unsigned char)(v >> 16), p[3] = (unsigned char)(v >> 24); }
static void be32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)(v >> 24), p[1] = (unsigned char)(v >> 16), p[2] = (unsigned char)(v >> 8), p[3] = (unsigned char)v; }

static uint32_t crc_table[256];
static void crc_init(void) {
    if (crc_table[1]) return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc_update(uint32_t crc, const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return crc;
}

typedef struct {
    FILE *f;
    uint32_t crc;
    int bad;
} Chunk;
static void chunk_begin(Chunk *c, FILE *f, const char *type, uint32_t len) {
    unsigned char h[8];
    be32(h, len);
    memcpy(h + 4, type, 4);
    c->f = f;
    c->bad = fwrite(h, 1, 8, f) != 8;
    c->crc = crc_update(0xffffffffu, h + 4, 4);
}
static void chunk_data(Chunk *c, const void *p, size_t n) {
    if (!n) return;
    c->bad |= fwrite(p, 1, n, c->f) != n;
    c->crc = crc_update(c->crc, p, n);
}
static int chunk_end(Chunk *c) {
    unsigned char t[4];
    be32(t, c->crc ^ 0xffffffffu);
    return c->bad | (fwrite(t, 1, 4, c->f) != 4);
}

static int write_png(FILE *f, const uint32_t *px, int w, int h, int pitch) {
    crc_init();
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    int bad = fwrite(sig, 1, 8, f) != 8;
    unsigned char ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = ihdr[11] = ihdr[12] = 0;
    Chunk c;
    chunk_begin(&c, f, "IHDR", 13);
    chunk_data(&c, ihdr, 13);
    bad |= chunk_end(&c);
    /* zlib stream of stored blocks: each row (filter 0 + RGB) is fed through; blocks of <= 65535 bytes */
    const size_t row = (size_t)w * 3 + 1, raw = row * (size_t)h;
    const size_t nblocks = raw ? (raw + 65534) / 65535 : 1;
    const size_t zlen = 2 + nblocks * 5 + raw + 4;
    if (zlen > 0x7fffffffu) return -1;
    chunk_begin(&c, f, "IDAT", (uint32_t)zlen);
    static const unsigned char zh[2] = {0x78, 0x01};
    chunk_data(&c, zh, 2);
    unsigned char *line = malloc(row);
    if (!line) return -1;
    uint32_t a1 = 1, a2 = 0;
    size_t left_in_block = 0, total_left = raw;
    for (int y = 0; y < h; y++) {
        line[0] = 0;
        for (int x = 0; x < w; x++) {
            uint32_t p = px[(size_t)y * (size_t)pitch + (size_t)x];
            line[1 + x * 3] = (unsigned char)(p >> 16), line[2 + x * 3] = (unsigned char)(p >> 8), line[3 + x * 3] = (unsigned char)p;
        }
        for (size_t i = 0; i < row; i++) a1 = (a1 + line[i]) % 65521u, a2 = (a2 + a1) % 65521u;
        size_t off = 0;
        while (off < row) {
            if (!left_in_block) {
                size_t n = total_left < 65535 ? total_left : 65535;
                unsigned char bh[5] = {(unsigned char)(total_left <= 65535), (unsigned char)n, (unsigned char)(n >> 8),
                                       (unsigned char)~n, (unsigned char)(~n >> 8)};
                chunk_data(&c, bh, 5);
                left_in_block = n;
            }
            size_t n = row - off < left_in_block ? row - off : left_in_block;
            chunk_data(&c, line + off, n);
            off += n, left_in_block -= n, total_left -= n;
        }
    }
    if (!raw) {
        static const unsigned char empty[5] = {1, 0, 0, 0xff, 0xff};
        chunk_data(&c, empty, 5);
    }
    free(line);
    unsigned char ad[4];
    be32(ad, a2 << 16 | a1);
    chunk_data(&c, ad, 4);
    bad |= chunk_end(&c);
    chunk_begin(&c, f, "IEND", 0);
    bad |= chunk_end(&c);
    return bad ? -1 : 0;
}

static int write_bmp(FILE *f, const uint32_t *px, int w, int h, int pitch) {
    const uint32_t stride = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + stride * (uint32_t)h;
    unsigned char hd[54] = {'B', 'M'};
    le32(hd + 2, size);
    le32(hd + 10, 54);
    le32(hd + 14, 40);
    le32(hd + 18, (uint32_t)w);
    le32(hd + 22, (uint32_t)h);
    hd[26] = 1, hd[28] = 24;
    le32(hd + 34, stride * (uint32_t)h);
    int bad = fwrite(hd, 1, 54, f) != 54;
    unsigned char *line = calloc(1, stride ? stride : 1);
    if (!line) return -1;
    for (int y = h - 1; y >= 0 && !bad; y--) {
        for (int x = 0; x < w; x++) {
            uint32_t p = px[(size_t)y * (size_t)pitch + (size_t)x];
            line[x * 3] = (unsigned char)p, line[x * 3 + 1] = (unsigned char)(p >> 8), line[x * 3 + 2] = (unsigned char)(p >> 16);
        }
        bad = fwrite(line, 1, stride, f) != stride;
    }
    free(line);
    return bad ? -1 : 0;
}

int gc_write_image(const char *path, const uint32_t *px, int w, int h, int pitch, char *err, size_t cap) {
    int png = ends_with_ci(path, ".png");
    if (!png && !ends_with_ci(path, ".bmp")) return fail(err, cap, "%s: the file must end in .bmp or .png", path);
    if (w <= 0 || h <= 0 || pitch < w || !px) return fail(err, cap, "no frame to write");
    FILE *f = fopen(path, "wb");
    if (!f) return fail(err, cap, "cannot create %s: %s", path, strerror(errno));
    int r = png ? write_png(f, px, w, h, pitch) : write_bmp(f, px, w, h, pitch);
    if (fclose(f)) r = -1;
    if (r) return fail(err, cap, "cannot write %s", path);
    return 0;
}

uint64_t gc_frame_hash(const uint32_t *px, int w, int h, int pitch) {
    uint64_t x = 0xcbf29ce484222325ull;
    for (int y = 0; y < h; y++)
        for (int i = 0; i < w; i++) {
            uint32_t p = px[(size_t)y * (size_t)pitch + (size_t)i] & 0xffffffu;
            for (int b = 0; b < 3; b++) x = (x ^ ((p >> (8 * b)) & 0xff)) * 0x100000001b3ull;
        }
    return x;
}

uint32_t gc_script_end(const char *spec) {
    uint32_t last = 0;
    for (const char *p = spec; p && *p;) {
        char *end;
        unsigned long f = strtoul(p, &end, 10);
        if (end == p || *end != ':') return 0;
        const char *step_end = strchr(end, ',');
        if (!step_end) step_end = end + strlen(end);
        unsigned long dur = 6;
        const char *d = memchr(end + 1, ':', (size_t)(step_end - end - 1));
        if (d) dur = strtoul(d + 1, NULL, 10);
        const char *names = end + 1;
        size_t nl = (size_t)((d ? d : step_end) - names);
        if (nl >= 4 && (!strncmp(names + nl - 4, "plug", 4))) dur = 0;   /* pN/plug, pN/unplug */
        if (f + dur > last) last = (uint32_t)(f + dur);
        p = *step_end ? step_end + 1 : step_end;
    }
    return last;
}
