/* End-to-end test of --render-process helper: this i386 program is the game side
 * (src/render_rec.c's wrappers, as hle_d3d.c calls them) and starts the x86_64 helper (build64/xbrun-render64
 * --wire, on SDL's dummy video driver: no Vulkan window, so the helper renders offscreen). A small scene (a base
 * image, two textures, textured quads, draw-state changes) is drawn for a few frames:
 *   1. in this process, without the helper (the reference readbacks and their hashes);
 *   2. through the helper with renderer_end readbacks: the pixels that come back must be the same bytes, texture
 *      handles the same as in-process (texture_create waits for its handle, repeated lookups are answered by the
 *      game side's mirror and checked by the helper), the snapshot, remote and late queries answered;
 *   3. through the helper presenting headless (the configuration has a window callback): renderer_can_present is 1,
 *      renderer_present does not wait, at most K frames are unacknowledged, and the helper's hashes of the frames it
 *      read back (--renderer-hashes) are the in-process ones;
 *   2b. temporal upscaling configured: renderer_taa_upscale (a LATE query) asked right after renderer_init, before
 *      any frame was acknowledged, answers as in-process (--dynres starts there: hle_d3d.c dynres_start);
 *   4. the helper killed mid-stream: one FAULT line, every later call returns at once, the helper is reaped.
 * Exits 77 (skip) when this machine has no Vulkan device or the helper cannot start (no x86_64 SDL2). */
#define _GNU_SOURCE
#include "../src/render_rec.h"
#include "../src/render_ipc.h"
#include "../src/render_wire.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

#define W 96
#define H 64
#define FRAMES 4
static const char *helper = "build64/xbrun-render64";
static uint32_t base[W * H], tex_a[16 * 16], tex_b[8 * 8];
static int fake_surface(void *instance, uint64_t *surface) { (void)instance; (void)surface; return -1; }

static double ms_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
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
    if (present) c.create_surface = fake_surface;   /* "the game presents" (the helper's window) */
    renderer_configure(&c);
}
/* Temporal upscaling to W x H from TAA_W x TAA_H (hle_d3d.c's --taa-upscale configuration). */
#define TAA_W 64
#define TAA_H 43
static void configure_taa(void) {
    RendererConfig c;
    memset(&c, 0, sizeof c);
    c.samples = 1;
    c.frames_in_flight = 2;
    c.no_pipeline_cache = 1;
    c.taa = 1;
    c.taa_up_w = TAA_W, c.taa_up_h = TAA_H;
    renderer_configure(&c);
}
/* One frame of the scene (frame f): returns renderer_end's result (out != NULL) or renderer_present's. */
static int frame(int f, uint32_t *out, uint32_t *handles) {
    const RViewport full = {0, 0, W, H, 0, 1};
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    RVertex q[4];
    for (int i = 0; i < W * H; i++) base[i] = 0xFF000000u | ((uint32_t)(i * 2654435761u >> 8) ^ (uint32_t)f * 0x10101u);
    renderer_begin(base, W * 4);
    uint32_t a = renderer_texture_find(0xA0001);
    if (!a) a = renderer_texture_create(0xA0001, RENDERER_TEX_BGRA8, 16, 16, 1, tex_a);
    uint32_t b = renderer_texture_find(0xA0002);
    if (!b) b = renderer_texture_create(0xA0002, RENDERER_TEX_BGRA8, 8, 8, 1, tex_b);
    handles[0] = a, handles[1] = b;
    handles[2] = renderer_texture_find(0xA0001);   /* again in the frame: the mirror, no record */
    handles[3] = renderer_texture_find(0xA0FFF);   /* never created */
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
static uint64_t read_hash(const char *path, uint32_t want_frame) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned fr;
    unsigned long long h, g;
    uint64_t found = 0;
    while (fscanf(f, "%u %llx %llx", &fr, &h, &g) == 3)
        if (fr == want_frame) found = h;
    fclose(f);
    return found;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--helper") && i + 1 < argc) helper = argv[++i];
    for (int i = 0; i < 16 * 16; i++) tex_a[i] = 0xFF000000u | (uint32_t)i * 0x010307u;
    for (int i = 0; i < 8 * 8; i++) tex_b[i] = 0x80000000u | (uint32_t)i * 0x070301u;

    /* 1. in-process */
    static uint32_t ref[FRAMES][W * H], got[W * H];
    uint32_t ref_h[FRAMES][4], h4[4];
    configure(0);
    if (renderer_init(W, H)) {
        printf("SKIP: no Vulkan device (%s)\n", renderer_error());
        return 77;
    }
    for (int f = 0; f < FRAMES; f++) {
        const int r = frame(f, ref[f], ref_h[f]);
        CHECK(r == 0, "in-process frame %d rendered (hash %016llx)", f + 1, (unsigned long long)rw_frame_hash(ref[f], W, H, W * 4));
    }
    renderer_shutdown();
    configure_taa();
    int ref_up = -1, ref_uw = 0, ref_uh = 0;
    if (!renderer_init(W, H)) ref_up = renderer_taa_upscale(&ref_uw, &ref_uh);
    CHECK(ref_up >= 0, "in-process temporal upscaling: %d, render %dx%d", ref_up, ref_uw, ref_uh);
    renderer_shutdown();

    /* the helper, as src/r64_input.c starts it for --render-process helper */
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    char hashes[] = "/tmp/r64_render_test_XXXXXX";
    const int hfd = mkstemp(hashes);
    if (hfd >= 0) close(hfd);
    char *opts[] = {"--render-process", "helper", "--renderer-hashes", hashes, "--render64-frames", "2"};
    for (int i = 0; i < 6; i++) {
        if (render_rec_option(6, opts, &i) != 1) { printf("FAIL option %s\n", opts[i]); return 1; }
    }
    R64InConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.flags = R64_IN_CFG_WINDOW;
    cfg.w = W, cfg.h = H;
    snprintf(cfg.title, sizeof cfg.title, "r64_render_test");
    const char *args[] = {"--wire", "--hash-frames", NULL};
    R64SpawnOpts so;
    memset(&so, 0, sizeof so);
    so.helper_path = helper;
    so.schema = r64_helper_schema;
    so.back_bytes = 4u << 20;
    so.log_fd = -1;
    so.extra_args = args;
    so.input = &cfg;
    R64Ipc *x;
    char err[256];
    if (r64_spawn(&so, &x, err, sizeof err)) {
        printf("SKIP: %s\n", err);
        return 77;
    }
    uint32_t in_size;
    const R64InHeader *in = r64_region(x, R64_REG_INPUT, &in_size);
    const double t_wait = ms_now();
    while (!__atomic_load_n(&in->helper_state, __ATOMIC_ACQUIRE) && ms_now() - t_wait < 5000 && !r64_poll(x)) usleep(1000);
    if (in->helper_state != 1) {
        printf("SKIP: the helper opened no window (no x86_64 SDL2?)\n");
        r64_shutdown(x, 1000, NULL);
        return 77;
    }
    r64_shared_channel = x;
    const long pid = r64_peer_pid(x);

    /* 2. readback through the helper */
    configure(0);
    CHECK(renderer_init(W, H) == 0, "renderer_init in the helper");
    int ow = 0, oh = 0;
    renderer_output_size(&ow, &oh);
    CHECK(ow == W && oh == H, "renderer_output_size from the helper: %dx%d", ow, oh);
    const char *dev = renderer_device_name();
    CHECK(dev && dev[0], "renderer_device_name from the helper: %s", dev ? dev : "(null)");
    CHECK(renderer_samples() == 1, "renderer_samples (snapshot) %d", renderer_samples());
    CHECK(renderer_can_present() == 0, "renderer_can_present 0 without a window callback");
    for (int f = 0; f < FRAMES; f++) {
        memset(got, 0xAB, sizeof got);
        const int r = frame(f, got, h4);
        CHECK(r == 0 && !memcmp(got, ref[f], sizeof got), "frame %d read back through the helper: the in-process bytes (%016llx)", f + 1,
              (unsigned long long)rw_frame_hash(got, W, H, W * 4));
        /* the game side's own handles (texture_create waits for nothing): stable, distinct, the repeat and the
         * absent key as in-process; the helper maps them to its renderer's (its contradictions: below) */
        static uint32_t first[4];
        if (!f) memcpy(first, h4, sizeof first);
        CHECK(h4[0] && h4[1] && h4[0] != h4[1] && h4[2] == h4[0] && !h4[3] && !memcmp(h4, first, sizeof h4) && (ref_h[f][2] == ref_h[f][0]) && !ref_h[f][3],
              "frame %d texture handles %x %x %x %x (in-process %u %u %u %u)", f + 1, h4[0], h4[1], h4[2], h4[3], ref_h[f][0], ref_h[f][1], ref_h[f][2],
              ref_h[f][3]);
    }
    RendererTiming t = {-1, -1, -1, -1, -1, -1, -1};
    renderer_last_timing(&t);
    CHECK(t.record_ms >= 0 && t.record_ms < 1000, "renderer_last_timing from an ack: record %.3f ms", t.record_ms);
    CHECK(renderer_texture_count() >= 2, "renderer_texture_count (snapshot) %u", renderer_texture_count());
    CHECK(renderer_texture_bytes() >= (16 * 16 + 8 * 8) * 4, "renderer_texture_bytes (game side) %llu", (unsigned long long)renderer_texture_bytes());
    renderer_shutdown();

    /* 2b. temporal upscaling: the LATE renderer_taa_upscale before any frame of this renderer_init is acknowledged */
    configure_taa();
    CHECK(renderer_init(W, H) == 0, "renderer_init in the helper (temporal upscaling)");
    int up_w = 0, up_h = 0;
    const int up = renderer_taa_upscale(&up_w, &up_h);
    CHECK(up == ref_up && up_w == ref_uw && up_h == ref_uh, "renderer_taa_upscale right after renderer_init: %d, %dx%d (in-process %d, %dx%d)", up,
          up_w, up_h, ref_up, ref_uw, ref_uh);
    renderer_shutdown();

    /* 3. presenting headless: frames in flight */
    configure(1);
    CHECK(renderer_init(W, H) == 0, "renderer_init again (presenting)");
    CHECK(renderer_can_present() == 1, "renderer_can_present 1: the helper presents (headless: read back there)");
    double worst = 0;
    for (int f = 0; f < FRAMES; f++) {
        const double t0 = ms_now();
        const int r = frame(f, NULL, h4);
        const double d = ms_now() - t0;
        if (d > worst) worst = d;
        CHECK(r == 0, "frame %d presented (%.2f ms on the game side)", f + 1, d);
    }
    CHECK(worst < 500, "renderer_present does not wait for the frame (slowest %.2f ms)", worst);
    renderer_shutdown();
    dev = renderer_device_name();   /* a round trip: every frame before it is acknowledged */

    /* 4. the helper killed: FAULT, no waiting */
    kill((pid_t)pid, SIGKILL);
    usleep(50000);
    const double t1 = ms_now();
    configure(0);
    const int ir = renderer_init(W, H);
    const int fr = frame(0, got, h4);
    const double d1 = ms_now() - t1;
    CHECK(r64_fault_printed == 1, "the dead helper gave one FAULT line");
    CHECK(d1 < 1000, "calls after the helper died return at once (%.1f ms; init %d, end %d)", d1, ir, fr);
    render_rec_exit();   /* the hashes file is closed */
    for (int f = 0; f < FRAMES; f++) {
        const uint64_t hh = read_hash(hashes, (uint32_t)(FRAMES + 1 + f)), want = rw_frame_hash(ref[f], W, H, W * 4);
        CHECK(hh == want, "frame %d presented headless: the helper's hash %016llx is the in-process one %016llx", f + 1, (unsigned long long)hh,
              (unsigned long long)want);
    }
    unlink(hashes);
    r64_shutdown(x, 0, NULL);
    int st;
    CHECK(waitpid(-1, &st, WNOHANG) <= 0, "no child process left");
    printf("%s: %d failure%s\n", failures ? "FAILED" : "ALL OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
