/* --rate-probe: guest memory snapshots at fixed points of game time (see rateprobe.h). */
#include "rateprobe.h"
#include <stdlib.h>
#include <string.h>

RateProbe *rateprobe_active;

struct RateProbe {
    FILE *f;
    RateProbeClock clock;
    int n;
    RateProbeRange r[RATEPROBE_MAX_RANGES];
    uint8_t *prev[RATEPROBE_MAX_RANGES];       /* last written contents */
    uint32_t prev_base[RATEPROBE_MAX_RANGES];  /* 0: none (next copy is full) */
    uint32_t *delta;                            /* scratch {offset, value} list */
    uint32_t snapshots;
    uint64_t bytes;
};

int rateprobe_parse_spec(const char *spec, char *path, size_t cap, uint32_t *interval_ms) {
    size_t len = strlen(spec), plen = len;
    uint32_t iv = RATEPROBE_DEFAULT_INTERVAL_MS;
    const char *colon = strrchr(spec, ':');
    if (colon && colon[1]) {
        const char *d = colon + 1;
        while (*d >= '0' && *d <= '9') d++;
        if (!*d) {   /* a last ':' followed only by digits: the interval */
            unsigned long v = strtoul(colon + 1, NULL, 10);
            if (v < 1 || v > 60000 || d - colon > 6) return -1;
            iv = (uint32_t)v;
            plen = (size_t)(colon - spec);
        }
    }
    if (!plen || plen >= cap) return -1;
    memcpy(path, spec, plen);
    path[plen] = 0;
    *interval_ms = iv;
    return 0;
}

/* One number (0x hex or decimal); *end must then be one of the stop characters. */
static int number(const char *s, const char **end, uint32_t *v) {
    char *e;
    if (!(*s >= '0' && *s <= '9')) return -1;
    unsigned long long x = strtoull(s, &e, 0);
    if (e == s || x > 0xffffffffull) return -1;
    *v = (uint32_t)x;
    *end = e;
    return 0;
}

int rateprobe_parse_range(const char *spec, RateProbeRange *r) {
    RateProbeRange t = {0, 0, 0, 0};
    const char *s = spec;
    if (*s == '@') { t.deref = 1; s++; }
    if (number(s, &s, &t.addr)) return -1;
    if (t.deref && *s == '+' && number(s + 1, &s, &t.off)) return -1;
    if (*s != ':' || number(s + 1, &s, &t.size) || *s) return -1;
    if (!t.size || (t.addr | t.size | t.off) & 3) return -1;
    if (!t.deref && (uint64_t)t.addr + t.size > 0x100000000ull) return -1;
    *r = t;
    return 0;
}

int rateprobe_due(RateProbeClock *c, uint32_t game_ms) {
    int due = !c->started || game_ms < c->last_ms || game_ms >= c->next_ms;
    if (due) c->next_ms = (game_ms / c->interval_ms + 1) * c->interval_ms;
    c->started = 1;
    c->last_ms = game_ms;
    return due;
}

static int put(RateProbe *p, const void *v, size_t len) {
    p->bytes += len;
    return fwrite(v, 1, len, p->f) == len ? 0 : -1;
}
static int put32(RateProbe *p, uint32_t v) { return put(p, &v, 4); }

RateProbe *rateprobe_open(FILE *f, uint32_t interval_ms, uint32_t flags, const RateProbeRange *r, int n) {
    if (!f || !interval_ms || n < 0 || n > RATEPROBE_MAX_RANGES) return NULL;
    RateProbe *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->f = f;
    p->clock.interval_ms = interval_ms;
    p->n = n;
    uint32_t maxsz = 0;
    for (int k = 0; k < n; k++) {
        p->r[k] = r[k];
        if (!(p->prev[k] = malloc(r[k].size))) { rateprobe_close(p); return NULL; }
        if (r[k].size > maxsz) maxsz = r[k].size;
    }
    if (!(p->delta = malloc(maxsz ? (size_t)maxsz * 2 : 8))) { rateprobe_close(p); return NULL; }
    int e = put32(p, RATEPROBE_MAGIC) | put32(p, RATEPROBE_VERSION) | put32(p, interval_ms) | put32(p, flags) | put32(p, (uint32_t)n);
    for (int k = 0; k < n; k++) e |= put32(p, r[k].addr) | put32(p, r[k].size) | put32(p, r[k].deref) | put32(p, r[k].off);
    if (e || fflush(f)) { rateprobe_close(p); return NULL; }
    return p;
}

/* Writes one range: unreadable, full copy or the dwords changed since the last copy. */
static int write_range(RateProbe *p, int k, RateProbeRead rd, void *ctx) {
    const RateProbeRange *r = &p->r[k];
    uint32_t base = r->addr;
    const uint8_t *cur = NULL;
    if (r->deref) {
        const uint32_t *ptr = rd(r->addr, 4, ctx);
        base = ptr && *ptr ? *ptr + r->off : 0;
    }
    if (base) cur = rd(base, r->size, ctx);
    if (!cur) {
        p->prev_base[k] = 0;
        return put32(p, 0) | put32(p, RATEPROBE_NONE) | put32(p, 0);
    }
    uint32_t n = 0, limit = r->size / 8;   /* a list at least as big as the copy is not worth it */
    if (p->prev_base[k] == base) {
        const uint32_t *a = (const uint32_t *)cur, *b = (const uint32_t *)p->prev[k];
        for (uint32_t i = 0; i < r->size / 4 && n <= limit; i++)
            if (a[i] != b[i]) { p->delta[2 * n] = i * 4; p->delta[2 * n + 1] = a[i]; n++; }
    }
    int e;
    if (p->prev_base[k] == base && n <= limit && n * 8 < r->size)
        e = put32(p, base) | put32(p, RATEPROBE_DELTA) | put32(p, n) | put(p, p->delta, (size_t)n * 8);
    else
        e = put32(p, base) | put32(p, RATEPROBE_FULL) | put32(p, r->size) | put(p, cur, r->size);
    memcpy(p->prev[k], cur, r->size);
    p->prev_base[k] = base;
    return e;
}

int rateprobe_frame(RateProbe *p, uint32_t frame, uint32_t game_ms, RateProbeRead rd, void *ctx) {
    if (!p || !rateprobe_due(&p->clock, game_ms)) return 0;
    int e = put32(p, RATEPROBE_SNAP) | put32(p, frame) | put32(p, game_ms) | put32(p, (uint32_t)p->n);
    for (int k = 0; k < p->n; k++) e |= write_range(p, k, rd, ctx);
    if (e || fflush(p->f)) return -1;   /* flushed: the process may exit at any later frame */
    p->snapshots++;
    return 1;
}

void rateprobe_counts(const RateProbe *p, uint32_t *snapshots, uint64_t *bytes) {
    *snapshots = p ? p->snapshots : 0;
    *bytes = p ? p->bytes : 0;
}

void rateprobe_close(RateProbe *p) {
    if (!p) return;
    if (p->f) fclose(p->f);
    for (int k = 0; k < p->n; k++) free(p->prev[k]);
    free(p->delta);
    free(p);
}

static int hexbytes(const char *s, size_t n, uint8_t *out) {
    if (!n || n & 1 || n / 2 > RATEPROBE_POKE_MAX) return -1;
    for (size_t j = 0; j < n; j++) {
        char c = s[j];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        if (j & 1) out[j / 2] = (uint8_t)(out[j / 2] | d);
        else out[j / 2] = (uint8_t)(d << 4);
    }
    return (int)(n / 2);
}

int rateprobe_parse_poke(const char *spec, RateProbePoke *k) {
    RateProbePoke t;
    memset(&t, 0, sizeof t);
    const char *s;
    if (number(spec, &s, &t.addr) || *s != ':') return -1;
    s++;
    const char *slash = strchr(s, '/');
    int n = hexbytes(s, slash ? (size_t)(slash - s) : strlen(s), t.bytes);
    if (n < 0) return -1;
    t.len = (uint32_t)n;
    if (slash) {
        if (hexbytes(slash + 1, strlen(slash + 1), t.expect) != n) return -1;
        t.has_expect = 1;
    }
    if ((uint64_t)t.addr + t.len > 0x100000000ull) return -1;
    *k = t;
    return 0;
}

int rateprobe_apply_poke(const RateProbePoke *k, uint8_t *mem) {
    if (k->has_expect && memcmp(mem, k->expect, k->len)) return -1;
    memcpy(mem, k->bytes, k->len);
    return 0;
}

int rateprobe_parse_arg(int argc, char **argv, int *i, RateProbeOptions *o) {
    const char *a = argv[*i];
    int spec = !strcmp(a, "--rate-probe"), range = !strcmp(a, "--rate-probe-range"), poke = !strcmp(a, "--poke");
    if (!spec && !range && !poke) return 0;
    if (*i + 1 >= argc) return -1;
    const char *v = argv[++*i];
    if (poke) {
        if (o->npokes >= RATEPROBE_MAX_POKES || rateprobe_parse_poke(v, &o->pokes[o->npokes])) return -1;
        o->npokes++;
        return 1;
    }
    if (spec) return rateprobe_parse_spec(v, o->path, sizeof o->path, &o->interval_ms) ? -1 : 1;
    if (o->nranges >= RATEPROBE_MAX_RANGES - 1) return -1;   /* one slot is kept for the .data section */
    return rateprobe_parse_range(v, &o->ranges[o->nranges]) ? -1 : (o->nranges++, 1);
}
