/* See r64_input.h. */
#define _GNU_SOURCE
#include "r64_input.h"
#include "input_map.h"
#include "platform.h"
#include "video.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int r64_input_helper_io;
const char *r64_input_helper_path;
const char *r64_input_inject;
uint32_t r64_input_pad_poll_us;
void (*r64_input_test_hook)(const R64InEvent *e, uint64_t now_ns);

static int source_open(int w, int h, const char *title);
static const VideoInputSource r64_source = {"helper", source_open, r64_input_pump, r64_input_close, r64_input_display_hz, 0};
/* --render-process helper (W6, Windows W7): the same input, and the helper renders and presents (render_rec.c streams to it) */
static const VideoInputSource r64_render_source = {"helper", source_open, r64_input_pump, r64_input_close, r64_input_display_hz, 1};

int r64_input_option(int argc, char **argv, int *i) {
    const char *o = argv[*i];
    const int has = *i + 1 < argc;
    if (!strcmp(o, "--render-process") && has && !strcmp(argv[*i + 1], "helper-io")) {
        r64_input_helper_io = 1;
        video_input_source = &r64_source;
        ++*i;
        return 1;
    }
    if (!strcmp(o, "--render-process") && has && !strcmp(argv[*i + 1], "helper")) {
        r64_input_helper_io = 2;   /* the window and input as helper-io; render_rec.c parses the option too */
        video_input_source = &r64_render_source;
        return 0;
    }
    if (!strcmp(o, "--render64-helper") && has) { r64_input_helper_path = argv[++*i]; return 1; }
    if (!strcmp(o, "--render64-inject") && has) { r64_input_inject = argv[++*i]; return 1; }
    if (!strcmp(o, "--render64-pad-poll-us") && has) {   /* 100-16000; 0: the helper's default (1000) */
        const unsigned long v = strtoul(argv[++*i], NULL, 0);
        r64_input_pad_poll_us = v && v < 100 ? 100 : v > 16000 ? 16000 : (uint32_t)v;
        return 1;
    }
    return 0;
}

#define LAT_MAX 65536
static struct {
    R64Ipc *x;
    void *reg;
    const R64InHeader *hdr;
    int quit, dead, captured, win_hz;
    pthread_mutex_t cmd_lock;   /* rumble comes from the guest thread, the capture release from the main thread */
    R64InputStats st;
    float *wait_ms, *origin_ms;
} G = {.cmd_lock = PTHREAD_MUTEX_INITIALIZER};

static void put_cmd(uint16_t type, uint16_t arg, const int32_t *v, uint32_t n) {
    pthread_mutex_lock(&G.cmd_lock);
    if (G.reg && !G.dead) r64_in_put(G.reg, R64_IN_CMDS, type, arg, v, n, 0);
    pthread_mutex_unlock(&G.cmd_lock);
}
static void rumble(int port, uint16_t left, uint16_t right) {
    const int32_t v[2] = {left, right};
    put_cmd(R64_IN_RUMBLE, (uint16_t)port, v, sizeof v);
    __atomic_add_fetch(&G.st.rumbles, 1, __ATOMIC_RELAXED);
}

#ifdef _WIN32
/* Windows: <game folder>\x64\WarriorsRender64.exe, with its own x64 SDL2.dll beside it (the 32-bit one next to the
 * game has the same name). */
const char *r64_input_find_helper(const char *given, char *buf, size_t n) {
    if (given) return given;
    snprintf(buf, n, "%sx64\\WarriorsRender64.exe", plat_program_dir());
    return buf;
}
#else
static int exists(const char *p) { return p && access(p, X_OK) == 0; }
const char *r64_input_find_helper(const char *given, char *buf, size_t n) {
    if (given) return given;
    const char *dir = plat_program_dir();
    snprintf(buf, n, "%sxbrun-render64", dir);
    if (exists(buf)) return buf;
    snprintf(buf, n, "%s../build64/xbrun-render64", dir);
    return buf;
}
#endif

int r64_input_open(const R64InputOpen *o, char *err, size_t n) {
    char path[4200];
    R64InConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.flags = R64_IN_CFG_WINDOW | (o->fullscreen ? R64_IN_CFG_FULLSCREEN : 0) | (o->refresh_match ? 0 : R64_IN_CFG_NO_REFRESH_MATCH) |
                (r64_helper_render && video_want_vulkan ? R64_IN_CFG_VULKAN : 0);
    cfg.w = (uint32_t)o->w;
    cfg.h = (uint32_t)o->h;
    cfg.poll_us = o->poll_us;
    snprintf(cfg.title, sizeof cfg.title, "%s", o->title ? o->title : "WarriorsReborn");
    /* the player's controller choices go to the helper, which opens the controllers */
    const char *args[2 * INPUT_PORTS + 10];
    int na = 0;
    for (int p = 1; p <= INPUT_PORTS; p++)
        if (input_pad_slot_spec(p)) args[na++] = "--pad-slot", args[na++] = input_pad_slot_spec(p);
    if (video_pad_mappings) args[na++] = "--pad-mappings", args[na++] = video_pad_mappings;
    if (o->inject) args[na++] = "--test-inject", args[na++] = o->inject;
    if (r64_helper_render) {   /* --render-process helper: the helper renders the stream (render_rec.c) */
        args[na++] = "--wire";
        if (r64_helper_hash) args[na++] = "--hash-frames";
    }
    args[na] = NULL;
    R64SpawnOpts so;
    memset(&so, 0, sizeof so);
    so.helper_path = r64_input_find_helper(o->helper_path, path, sizeof path);
    if (r64_helper_render) {   /* the regions of master plan 3.1; the BACK ring carries readbacks too */
        so.schema = r64_helper_schema;
        so.back_bytes = 4u << 20;
    } else {
        so.schema = R64_SKELETON_SCHEMA;
        so.cmd_bytes = 1u << 16;   /* no wire stream: small rings, the input region at its default 64 KB */
        so.back_bytes = 1u << 16;
        so.content_bytes = 4096;
        so.chunk_bytes = 4096;
    }
    so.log_fd = -1;
    so.extra_args = args;
    so.input = &cfg;
    R64Ipc *x;
    if (r64_spawn(&so, &x, err, n)) return -1;
    uint32_t size;
    void *reg = r64_region(x, R64_REG_INPUT, &size);
    if (r64_in_check(reg, size, err, n)) {
        r64_shutdown(x, 1000, NULL);
        return -1;
    }
    const R64InHeader *h = reg;
    const uint64_t t0 = r64_now_ns();
    while (!__atomic_load_n(&h->helper_state, __ATOMIC_ACQUIRE)) {
        if (r64_poll(x) || r64_now_ns() - t0 > 5000000000ull) {
            char st[32];
            snprintf(err, n, "helper %s: no window (%s)", so.helper_path, r64_poll(x) ? r64_peer_exit(x, st, sizeof st) : "5 s timeout");
            r64_shutdown(x, 1000, NULL);
            return -1;
        }
        nanosleep(&(struct timespec){0, 1000000}, NULL);
    }
    if (h->helper_state != 1) {
        snprintf(err, n, "helper %s could not open its window", so.helper_path);
        r64_shutdown(x, 1000, NULL);
        return -1;
    }
    G.wait_ms = calloc(LAT_MAX, sizeof *G.wait_ms);
    G.origin_ms = calloc(LAT_MAX, sizeof *G.origin_ms);
    memset(&G.st, 0, sizeof G.st);
    G.quit = G.dead = G.captured = G.win_hz = 0;
    G.hdr = h;
    G.reg = reg;
    G.x = x;
    input_rumble_hook = rumble;
    if (r64_helper_render) r64_shared_channel = x;   /* render_rec.c streams into it */
    r64_log("helper-io: helper pid %ld owns the window (%ux%u%s) and the input; %s", r64_peer_pid(x), cfg.w, cfg.h,
            o->fullscreen ? ", fullscreen" : "", r64_helper_render ? "it renders too (--render-process helper)" : "rendering stays in this process");
    return 0;
}
int r64_input_active(void) { return G.x != NULL; }
long r64_input_helper_pid(void) { return G.x ? r64_peer_pid(G.x) : 0; }
int r64_input_last_exit = -1;
int r64_input_display_hz(void) { return G.win_hz; }

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static uint64_t t64(uint32_t lo, uint32_t hi) { return (uint64_t)hi << 32 | lo; }

static void apply(const R64InEvent *e) {
    switch (e->type) {
    case R64_IN_RESYNC:
        G.st.resyncs++;
        printf("[render64] input: %d records lost (ring full): keys and buttons released\n", e->v[0]);
        input_release_all();
        break;
    case R64_IN_KEY: G.st.keys++; input_key_event(e->v[0], e->arg, e->v[1]); break;
    case R64_IN_MOTION: input_mouse_motion(e->v[0], e->v[1]); break;
    case R64_IN_WHEEL: input_mouse_wheel(e->v[0]); break;
    case R64_IN_RELEASE_ALL: input_release_all(); break;
    case R64_IN_CAPTURE:
        G.captured = e->arg;
        input_mouse_reset();
        break;
    case R64_IN_PAD: {
        G.st.pads++;
        const int port = e->arg;
        if (port < 0 || port >= INPUT_PORTS) break;
        if (!e->v[0]) { input_set_port_controller(port, NULL); break; }
        XPad p;
        memcpy(&p, &e->v[1], sizeof p);
        input_pad_apply(&p, &p);   /* the player's remap lives in the game's key map */
        input_set_port_controller(port, &p);
        break;
    }
    case R64_IN_ESCAPE:
        if (video_escape_hook && video_escape_hook()) break;   /* a menu took it */
        if (!e->arg) G.quit = 1;
        else put_cmd(R64_IN_SET_CAPTURE, 0, NULL, 0);
        break;
    case R64_IN_QUIT: G.quit = 1; break;
    case R64_IN_WINDOW: G.win_hz = e->v[3]; break;
    default: break;
    }
}

int r64_input_pump(void) {
    if (!G.x) return 1;
    if (G.dead) return 0;
    if (r64_poll(G.x)) {
        char st[32];
        if (!__atomic_exchange_n(&r64_fault_printed, 1, __ATOMIC_ACQ_REL)) {   /* render_rec.c may have seen it first */
            printf("=== FAULT (render64 helper exited: %s)\n", r64_peer_exit(G.x, st, sizeof st));
            fflush(stdout);
        }
        G.dead = 1;
        return 0;
    }
    const int closed = __atomic_load_n(&G.hdr->quit, __ATOMIC_ACQUIRE);   /* before the drain: its QUIT record is then in it */
    R64InEvent e;
    while (r64_in_get(G.reg, R64_IN_EVENTS, &e)) {
        const uint64_t now = r64_now_ns(), t = t64(e.t_lo, e.t_hi), o = t64(e.o_lo, e.o_hi);
        G.st.events++;
        if (G.st.n_wait < LAT_MAX && G.wait_ms) G.wait_ms[G.st.n_wait++] = (float)((double)(now - t) / 1e6);
        if (o && G.st.n_origin < LAT_MAX && G.origin_ms) G.origin_ms[G.st.n_origin++] = (float)((double)(now - o) / 1e6);
        if (r64_input_test_hook) r64_input_test_hook(&e, now);
        apply(&e);
    }
    if (G.captured) input_mouse_frame(now_s());
    return !(G.quit || closed);
}

static int cmp_f(const void *a, const void *b) {
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}
static void pcts(float *v, uint32_t n, double *p50, double *p95, double *mx) {
    *p50 = *p95 = *mx = 0;
    if (!n) return;
    qsort(v, n, sizeof *v, cmp_f);
    *p50 = v[n / 2];
    *p95 = v[(size_t)n * 95 / 100 < n ? (size_t)n * 95 / 100 : n - 1];
    *mx = v[n - 1];
}
void r64_input_stats(R64InputStats *s) {
    *s = G.st;
    if (G.reg) s->dropped = r64_in_dropped(G.reg, R64_IN_EVENTS);
    if (G.wait_ms) {
        float *w = malloc((size_t)(s->n_wait + 1) * sizeof *w), *o = malloc((size_t)(s->n_origin + 1) * sizeof *o);
        if (w && o) {
            memcpy(w, G.wait_ms, s->n_wait * sizeof *w);
            memcpy(o, G.origin_ms, s->n_origin * sizeof *o);
            pcts(w, s->n_wait, &s->wait_p50, &s->wait_p95, &s->wait_max);
            pcts(o, s->n_origin, &s->origin_p50, &s->origin_p95, &s->origin_max);
        }
        free(w);
        free(o);
    }
}

void r64_input_close(void) {
    if (!G.x) return;
    R64InputStats s;
    r64_input_stats(&s);
    printf("[render64] input: %u records (%u keys, %u pad states, %u rumbles sent), %u dropped, %u resyncs; helper -> game "
           "p50 %.3f p95 %.3f max %.3f ms\n",
           s.events, s.keys, s.pads, s.rumbles, s.dropped, s.resyncs, s.wait_p50, s.wait_p95, s.wait_max);
    if (s.n_origin)
        printf("[render64] input: from the input to the game: p50 %.3f p95 %.3f max %.3f ms (%u timed records)\n", s.origin_p50, s.origin_p95,
               s.origin_max, s.n_origin);
    input_rumble_hook = NULL;
    pthread_mutex_lock(&G.cmd_lock);
    R64Ipc *x = G.x;
    G.x = NULL;
    if (r64_shared_channel == x) r64_shared_channel = NULL;
    G.reg = NULL;
    pthread_mutex_unlock(&G.cmd_lock);
    int code = -1;
    const int rc = G.dead ? R64_ERR_PEER_DEAD : r64_shutdown(x, 1000, &code);
    r64_input_last_exit = G.dead ? -1 : code;
    if (!G.dead) printf("[render64] helper-io: helper %s (exit code %d)\n", rc == R64_OK ? "exited" : "killed after 1 s", code);
    else r64_shutdown(x, 0, NULL);
    G.hdr = NULL;
    free(G.wait_ms);
    free(G.origin_ms);
    G.wait_ms = G.origin_ms = NULL;
}

/* video.c's external backend (video_input_source). */
static int source_open(int w, int h, const char *title) {
    char err[256] = "";
    const R64InputOpen o = {r64_input_helper_path, w, h, video_fullscreen, video_refresh_match, title, r64_input_inject, r64_input_pad_poll_us};
    if (r64_input_open(&o, err, sizeof err) == 0) return 0;
    printf("[render64] helper-io: %s: the game's own window and input\n", err);
    return -1;
}
