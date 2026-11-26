/* See r64_selftest.h. The scene and the checks follow tests/r64_render_test.c (W6), on the game's real code paths:
 * render_rec.c's wrappers in front of this process's renderer, r64_spawn and the helper's render thread. */
#include "r64_selftest.h"
#include "render_ipc.h"
#include "render_rec.h"
#include "render_wire.h"
#include "renderer.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 96
#define H 64
#define FRAMES 6
#define PRESENTS 120
#define ECHO_FRAMES 200
#define ECHO_BYTES (2u << 20)

static int failed;
static char first_fail[256];
static void check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(int ok, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("[render64] selftest: %s %s\n", ok ? "ok  " : "FAIL", msg);
    if (!ok && !failed++) snprintf(first_fail, sizeof first_fail, "%s", msg);
}
static double ms_now(void) { return (double)r64_now_ns() / 1e6; }
static int cmp_d(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
static double pct(double *v, int n, double p) {
    if (n <= 0) return 0;
    qsort(v, (size_t)n, sizeof *v, cmp_d);
    int i = (int)(p * (n - 1) + 0.5);
    return v[i < n ? i : n - 1];
}

/* ---- 2. the raw channel: the helper's echo protocol (render_ipc.h R64EchoFrame) ---- */
static int echo_stage(const char *helper) {
    R64SpawnOpts so;
    memset(&so, 0, sizeof so);
    so.helper_path = helper;
    so.schema = R64_SKELETON_SCHEMA;
    so.log_fd = -1;
    R64Ipc *x;
    char err[256];
    const double t0 = ms_now();
    if (r64_spawn(&so, &x, err, sizeof err)) {
        check(0, "the helper starts and answers the handshake: %s", err);
        return -1;
    }
    check(1, "helper %s started, handshake in %.1f ms (pid %ld)", helper, ms_now() - t0, r64_peer_pid(x));
    uint32_t *src = malloc(ECHO_BYTES + 4096 * 64);
    static double rt[ECHO_FRAMES];
    int good = 0, rc = 0, n = 0;
    if (src) {
        for (uint32_t i = 0; i < (ECHO_BYTES + 4096 * 64) / 4; i++) src[i] = i * 2654435761u ^ 0x5bd1e995u;
        const double s0 = ms_now();
        for (uint32_t f = 0; f < ECHO_FRAMES && !rc; f++) {
            const double a = ms_now();
            const R64EchoFrame h = {R64_ECHO_FRAME, f, ECHO_BYTES, 0};
            const uint8_t *p = (const uint8_t *)src + (f % 64) * 4096u;
            R64Sum s;
            r64_sum_init(&s);
            rc = r64_write(x, R64_RING_CMD, &h, sizeof h);
            for (uint32_t put = 0; !rc && put < ECHO_BYTES; put += R64_DEFAULT_CHUNK) {
                r64_sum_update(&s, p + put, R64_DEFAULT_CHUNK);
                rc = r64_write(x, R64_RING_CMD, p + put, R64_DEFAULT_CHUNK);
            }
            const uint32_t sum = r64_sum_final(&s);
            if (!rc) rc = r64_write(x, R64_RING_CMD, &sum, 4);
            if (!rc) rc = r64_flush(x, R64_RING_CMD);
            R64EchoAck k;
            if (!rc) rc = r64_read(x, R64_RING_BACK, &k, sizeof k);
            if (!rc) {
                good += k.tag == R64_ECHO_ACK && k.seq == f && k.ok && k.sum == sum;
                rt[n++] = ms_now() - a;
            }
        }
        const double secs = (ms_now() - s0) / 1e3;
        check(!rc && good == ECHO_FRAMES, "echo: %d of %d frames of %u MB with the right checksum, %.2f GB/s, round trip p50 %.3f ms, max %.3f ms",
              good, ECHO_FRAMES, ECHO_BYTES >> 20, secs > 0 ? (double)ECHO_BYTES * n / secs / 1e9 : 0.0, pct(rt, n, 0.5), pct(rt, n, 1.0));
        free(src);
    } else
        check(0, "echo: out of memory");
    int code = -1;
    const int sr = r64_shutdown(x, 1000, &code);
    check(sr == R64_OK && code == 0, "echo: the helper quit when asked (exit code %d)", code);
    return rc ? -1 : 0;
}

/* ---- 1./3. the scene ---- */
static uint32_t base[W * H], tex_a[16 * 16], tex_b[8 * 8];
static int fake_surface(void *instance, uint64_t *surface) {
    (void)instance;
    (void)surface;
    return -1;
}
static void quad(RVertex *q, float x0, float y0, float x1, float y1, uint32_t color) {
    const float xy[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int i = 0; i < 4; i++) {
        memset(&q[i], 0, sizeof q[i]);
        q[i].x = xy[i][0] * 2 - 1;
        q[i].y = 1 - xy[i][1] * 2;
        q[i].z = 0.5f;
        q[i].w = 1;
        q[i].color = color;
        for (int s = 0; s < RENDERER_STAGES; s++) q[i].tex[s][0] = uv[i][0], q[i].tex[s][1] = uv[i][1], q[i].tex[s][3] = 1;
    }
}
static void configure(int present) {
    RendererConfig c;
    memset(&c, 0, sizeof c);
    c.samples = 1;
    c.frames_in_flight = 2;
    c.no_pipeline_cache = 1;
    if (present) c.create_surface = fake_surface;   /* "the game presents": the helper's window (headless here) */
    renderer_configure(&c);
}
/* Frame f: renderer_end's result into out, or renderer_present's when out is NULL; the texture handles in h[4]. */
static int frame(int f, uint32_t *out, uint32_t *h) {
    const RViewport full = {0, 0, W, H, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    RVertex q[4];
    for (int i = 0; i < W * H; i++) base[i] = 0xFF000000u | ((uint32_t)(i * 2654435761u >> 8) ^ (uint32_t)f * 0x10101u);
    renderer_begin(base, W * 4);
    uint32_t a = renderer_texture_find(0xA0001);
    if (!a) a = renderer_texture_create(0xA0001, RENDERER_TEX_BGRA8, 16, 16, 1, tex_a);
    uint32_t b = renderer_texture_find(0xA0002);
    if (!b) b = renderer_texture_create(0xA0002, RENDERER_TEX_BGRA8, 8, 8, 1, tex_b);
    h[0] = a, h[1] = b;
    h[2] = renderer_texture_find(0xA0001);
    h[3] = renderer_texture_find(0xA0FFF);   /* never created */
    RState rs;
    renderer_state_default(&rs);
    for (int k = 0; k < 6; k++) {
        renderer_set_texture(k & 1 ? b : a);
        rs.blend_enable = k > 2;
        rs.blend_src = 0x302;
        rs.blend_dst = 0x303;
        renderer_set_state(&rs);
        const float o = 0.08f * (float)k + 0.02f * (float)f;
        quad(q, o, o, o + 0.45f, o + 0.4f, 0x80FFFFFFu - (uint32_t)k * 0x00102030u);
        renderer_draw(&full, 0, q, 4, qi, 6);
    }
    return out ? renderer_end(out, W * 4) : renderer_present();
}

int r64_selftest_run(const char *helper) {
    failed = 0;
    first_fail[0] = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[render64] selftest: %s, helper %s\n", XBRUN_VERSION, helper);
    for (int i = 0; i < 16 * 16; i++) tex_a[i] = 0xFF000000u | (uint32_t)i * 0x010307u;
    for (int i = 0; i < 8 * 8; i++) tex_b[i] = 0x80000000u | (uint32_t)i * 0x070301u;

    /* 1. the reference, in this process (before the helper mode is on) */
    static uint32_t ref[FRAMES][W * H], got[W * H];
    uint32_t ref_h[FRAMES][4], h4[4];
    configure(0);
    if (renderer_init(W, H)) {
        printf("[render64] selftest: SKIP: no Vulkan device in this process (%s): nothing to compare with\n", renderer_error());
        return 77;
    }
    printf("[render64] selftest: reference renderer: %s\n", renderer_device_name());
    for (int f = 0; f < FRAMES; f++) check(frame(f, ref[f], ref_h[f]) == 0, "reference frame %d rendered in this process", f + 1);
    renderer_shutdown();

    /* 2. the raw channel (a helper of its own: no window config, so it runs the echo protocol) */
    if (echo_stage(helper) == 0) {
        /* 3. the wire, as src/r64_input.c starts the helper for --render-process helper; SDL's dummy driver: no window */
        putenv((char *)"SDL_VIDEODRIVER=dummy");
        putenv((char *)"SDL_AUDIODRIVER=dummy");
        char *opts[] = {(char *)"--render-process", (char *)"helper", (char *)"--render64-frames", (char *)"2"};
        for (int i = 0; i < 4; i++)
            if (render_rec_option(4, opts, &i) != 1) check(0, "option %s", opts[i]);
        R64InConfig cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.flags = R64_IN_CFG_WINDOW;
        cfg.w = W, cfg.h = H;
        snprintf(cfg.title, sizeof cfg.title, "render64 selftest");
        const char *args[] = {"--wire", NULL};
        R64SpawnOpts so;
        memset(&so, 0, sizeof so);
        so.helper_path = helper;
        so.schema = r64_helper_schema;
        so.back_bytes = 4u << 20;
        so.log_fd = -1;
        so.extra_args = args;
        so.input = &cfg;
        R64Ipc *x = NULL;
        char err[256];
        if (r64_spawn(&so, &x, err, sizeof err)) {
            check(0, "the rendering helper starts: %s", err);
            x = NULL;
        }
        uint32_t in_size;
        const R64InHeader *in = x ? r64_region(x, R64_REG_INPUT, &in_size) : NULL;
        const double t_wait = ms_now();
        while (in && !__atomic_load_n(&in->helper_state, __ATOMIC_ACQUIRE) && ms_now() - t_wait < 5000 && !r64_poll(x)) {
            const struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        }
        if (in) check(in->helper_state == 1, "the helper opened SDL (dummy video driver) in %.0f ms", ms_now() - t_wait);
        if (in && in->helper_state == 1) {
            r64_shared_channel = x;
            configure(0);
            const double ti = ms_now();
            check(renderer_init(W, H) == 0, "renderer_init in the helper (%.0f ms)", ms_now() - ti);
            const char *dev = renderer_device_name();
            check(dev && dev[0], "the helper's renderer: %s", dev ? dev : "(none)");
            static double rt[FRAMES];
            for (int f = 0; f < FRAMES; f++) {
                memset(got, 0xAB, sizeof got);
                const double a = ms_now();
                const int r = frame(f, got, h4);
                rt[f] = ms_now() - a;
                const uint64_t hg = rw_frame_hash(got, W, H, W * 4), hr = rw_frame_hash(ref[f], W, H, W * 4);
                check(r == 0 && !memcmp(got, ref[f], sizeof got), "frame %d through the wire: hash %016llx, this process %016llx", f + 1,
                      (unsigned long long)hg, (unsigned long long)hr);
                check(h4[0] && h4[1] && h4[0] != h4[1] && h4[2] == h4[0] && !h4[3] && ref_h[f][2] == ref_h[f][0] && !ref_h[f][3],
                      "frame %d texture handles %x %x %x %x", f + 1, h4[0], h4[1], h4[2], h4[3]);
            }
            printf("[render64] selftest: read-back frames: round trip p50 %.2f ms, max %.2f ms\n", pct(rt, FRAMES, 0.5), pct(rt, FRAMES, 1.0));
            renderer_shutdown();
            configure(1);
            check(renderer_init(W, H) == 0 && renderer_can_present() == 1, "renderer_init again, presenting (the helper presents)");
            static double pt[PRESENTS];
            int bad = 0;
            const double p0 = ms_now();
            for (int f = 0; f < PRESENTS; f++) {
                const double a = ms_now();
                bad += frame(f % FRAMES, NULL, h4) != 0;
                pt[f] = ms_now() - a;
            }
            dev = renderer_device_name();   /* a round trip: every frame before it is acknowledged */
            const double all = ms_now() - p0;
            const double worst = pct(pt, PRESENTS, 1.0);
            check(!bad && worst < 500, "%d frames presented, game side p50 %.3f ms, p95 %.3f ms, max %.2f ms; %.0f frames/s with the helper's acks",
                  PRESENTS, pct(pt, PRESENTS, 0.5), pct(pt, PRESENTS, 0.95), worst, all > 0 ? PRESENTS * 1e3 / all : 0.0);
            renderer_shutdown();
            const R64Control *c = r64_control(x);
            printf("[render64] selftest: wire: %.1f MB through the command ring\n", c ? c->f.ring[R64_RING_CMD].head / 1048576.0 : 0.0);
            check(!r64_fault_printed && !r64_poll(x), "the helper is still running");
        }
        if (x) {
            r64_shared_channel = NULL;
            int code = -1;
            const int sr = r64_shutdown(x, 2000, &code);
            check(sr == R64_OK && code == 0, "the rendering helper quit when asked (exit code %d)", code);
        }
    }
    if (failed) printf("[render64] selftest: FAIL: %s (%d failed check%s)\n", first_fail, failed, failed == 1 ? "" : "s");
    else printf("[render64] selftest: PASS (helper %s: channel, %d frames identical through the wire, %d presented)\n", helper, FRAMES,
                PRESENTS);
    fflush(stdout);
    return failed ? 1 : 0;
}
