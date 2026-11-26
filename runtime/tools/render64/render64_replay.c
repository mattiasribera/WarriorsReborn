/* Replays a --record-renderer recording (runtime/src/render_wire.h) into this build's renderer,
 * offscreen, and checks every frame against the hash the recording process took of its own
 * renderer_end readback. Built for i386 (build/render64_replay) and x86_64
 * (build64/render64_replay): the 64-bit build is the renderer-process prototype
 *.
 *
 * usage: render64_replay FILE [--dump DIR] [--dump-every K] [--frames N] [--hashes FILE] [-v]
 *   --dump DIR      writes replayed frames as DIR/frame_<n>.bmp (every K-th, default every frame
 *                   that differs) and, for a differing frame with recorded pixels, the recorded one
 *                   as DIR/rec_<n>.bmp
 *   --frames N      stops after N frames
 *   --hashes FILE   writes "<frame> <recorded hash> <replayed hash>" lines
 *   --mem-map       the process's address space by owner at the end (src/mem_map.h)
 *   --upscaler N    replays with RendererConfig.taa_upscaler N (0 TAA, 1 FSR 3.1, 2 FSR 3.1 without its
 *                   masks) instead of the recorded one; --no-taa without temporal AA or upscaling (the
 *                   output size, no jitter): A/B comparisons of the same frames
 *   --via loopback  every renderer call of the replay goes through --render-process loopback (the content
 *                   cache, the ring, the decoder): the frames must still match
 *                   the recording's (in-process) hashes
 *   --cache off|check  with --via loopback: the content cache off, or on with its collision check
 * A --render-process helper recording replays the same way: its remote queries
 * (RW_REC_QUERY) are skipped, results the game did not wait for (RW_FLAG_LATER) are not compared.
 * Exit: 0 every frame matched, 1 some differed, 2 bad input, 77 no Vulkan device. */
#include "../../src/render_wire.h"
/* every renderer call below goes through the recorder's wrappers: pass-through, or with --via loopback
 * through the loopback (encoder, render_ipc ring, decoder: src/render_rec.c) into the renderer */
#include "../../src/render_rec.h"
#include "../../src/mem_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define RW_GEN_NAMES
#include "../../src/render_wire_gen.h"

#define MAP_BITS 16
typedef struct RwReplay {
    struct { uint32_t key, val; } map[1u << MAP_BITS];
    uint32_t *out;
    size_t out_cap;
    int out_w, out_h, got_ret, have_got;
    uint64_t got_hash, got_gen;
    uint32_t *gen;
    size_t gen_cap;
    uint32_t frames, matched, differed, unchecked, result_diffs[RW_ID_COUNT], handle_diffs;
    uint32_t gen_matched, gen_differed, gen_missing;   /* --frame-gen: the generated frames */
    uint64_t plan_misses;   /* the recording's pipeline misses handed to renderer_pipeline_plan */
    uint64_t later;         /* --via loopback: results not waited for (render_rec_result_later) */
    int in_preamble, verbose, had_surface;
    double call_ms, end_ms, t_call, frame_ms_max;
    uint64_t file_bytes;
    const char *dump;
    int dump_every;
    FILE *hashes;
    int upscaler, no_taa;   /* --upscaler N (-1: as recorded), --no-taa */
} RwReplay;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* Recorded handle -> this renderer's handle (open addressing; 0 stays 0). */
static uint32_t *slot(RwReplay *rp, uint32_t key) {
    uint32_t i = (key * 2654435761u) >> (32 - MAP_BITS);
    for (uint32_t n = 0; n < (1u << MAP_BITS); n++, i = (i + 1) & ((1u << MAP_BITS) - 1))
        if (!rp->map[i].key || rp->map[i].key == key) { rp->map[i].key = key; return &rp->map[i].val; }
    return NULL;
}
static uint32_t rw_map_handle(RwReplay *rp, uint32_t rec) {
    if (!rec) return 0;
    uint32_t *v = slot(rp, rec);
    return v && *v ? *v : rec;
}
static int rw_result_differs(RwReplay *rp, uint32_t id, int handle, double want, double got) {
    if (handle && want != 0) {
        uint32_t *v = slot(rp, (uint32_t)want);
        if (v) *v = (uint32_t)got;
    }
    return want != got;
}
static void rw_replay_config(RwReplay *rp, RendererConfig *c, int had_surface) {
    c->pipeline_cache_dir = NULL;   /* in-memory pipeline cache: the pipelines are the same */
    if (rp->upscaler >= 0) c->taa_upscaler = rp->upscaler;
    if (rp->no_taa) c->taa = c->taa_up_w = c->taa_up_h = c->taa_upscaler = 0;
    rp->had_surface = had_surface;
}
static int rw_replay_init(RwReplay *rp, int w, int h) {
    const int r = renderer_init(w, h);
    if (r) {
        fprintf(stderr, "render64_replay: renderer_init %dx%d failed: %s\n", w, h, renderer_error());
        exit(77);
    }
    printf("[replay] renderer_init %dx%d on %s (%d-bit)%s\n", w, h, renderer_device_name(), (int)(sizeof(void *) * 8),
           rp->had_surface ? "; the recording presented to a window, this replay reads back" : "");
    return r;
}
static int rw_replay_end(RwReplay *rp, int pitch_bytes) {
    int w = 0, h = 0;
    renderer_output_size(&w, &h);
    const size_t need = (size_t)w * h;
    if (need > rp->out_cap) {
        free(rp->out);
        rp->out = malloc(need * 4);
        rp->out_cap = rp->out ? need : 0;
    }
    const double t = now_ms();
    const int r = rp->out ? renderer_end(rp->out, w * 4) : -1;
    rp->end_ms += now_ms() - t;
    rp->out_w = w;
    rp->out_h = h;
    rp->got_ret = r;
    rp->got_hash = r ? 0 : rw_frame_hash(rp->out, w, h, w * 4);
    rp->got_gen = 0;
    if (!r && renderer_frame_gen_on()) {
        if (need > rp->gen_cap) {
            free(rp->gen);
            rp->gen = malloc(need * 4);
            rp->gen_cap = rp->gen ? need : 0;
        }
        if (rp->gen && renderer_frame_gen_readback(rp->gen, w * 4)) rp->got_gen = rw_frame_hash(rp->gen, w, h, w * 4);
    }
    rp->have_got = 1;
    return r;
}
/* renderer_present: offscreen here, the frame is read back and checked like renderer_end's */
static int rw_replay_present(RwReplay *rp) { return rw_replay_end(rp, 0); }
static uint32_t rec_flags;   /* the record being replayed's flags (RW_FLAG_LATER: its result was not waited for) */
static void rw_replay_before(RwReplay *rp, uint32_t id) { rp->t_call = now_ms(); }
static void rw_replay_after(RwReplay *rp, uint32_t id, int handle, int differs) {
    rp->call_ms += now_ms() - rp->t_call;
    if (!differs) return;
    if (render_rec_result_later(id) || rec_flags & RW_FLAG_LATER) {   /* not waited for (the placeholder differs) */
        rp->later++;
        return;
    }
    if (handle) rp->handle_diffs++;
    if (rp->result_diffs[id]++ < 3 && rp->verbose)
        printf("[replay] frame %u: %s returned a different value than recorded\n", rp->frames + 1, rw_id_name[id]);
}
#define RW_GEN_REPLAY
#include "../../src/render_wire_gen.h"

static int write_bmp(const char *path, const uint32_t *px, int w, int h, size_t pitch_px) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const uint32_t row = (uint32_t)((w * 3 + 3) & ~3), size = 54 + row * (uint32_t)h;
    uint8_t hd[54] = {'B', 'M'};
    memcpy(hd + 2, &size, 4);
    hd[10] = 54;
    hd[14] = 40;
    memcpy(hd + 18, &w, 4);
    memcpy(hd + 22, &h, 4);
    hd[26] = 1;
    hd[28] = 24;
    fwrite(hd, 1, 54, f);
    uint8_t *line = calloc(row, 1);
    for (int y = h - 1; y >= 0 && line; y--) {
        for (int x = 0; x < w; x++) {
            const uint32_t p = px[(size_t)y * pitch_px + x];
            line[x * 3] = (uint8_t)p, line[x * 3 + 1] = (uint8_t)(p >> 8), line[x * 3 + 2] = (uint8_t)(p >> 16);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    return fclose(f);
}

/* The recorder's RW_REC_FRAME after a renderer_end: compares it with this replay's frame. */
static void frame_check(RwReplay *rp, RwReader *r) {
    const uint32_t frame = rw_ru32(r);
    const int ret = rw_ri32(r), w = rw_ri32(r), h = rw_ri32(r);
    const uint64_t hash = rw_ru64(r), gen = rw_ru64(r);
    const int pixels = (int)rw_ru32(r);
    rp->frames++;
    if (gen && rp->have_got) {   /* the recording built a generated frame here */
        if (!rp->got_gen) rp->gen_missing++;
        else if (gen == rp->got_gen) rp->gen_matched++;
        else if (++rp->gen_differed <= 20 || rp->verbose) printf("[replay] frame %u: the generated frame differs\n", frame);
    } else if (rp->got_gen && rp->have_got) rp->gen_missing++;
    if (!rp->have_got || ret || !hash) {
        rp->unchecked++;
    } else if (w != rp->out_w || h != rp->out_h) {
        rp->differed++;
        printf("[replay] frame %u: size %dx%d, recorded %dx%d\n", frame, rp->out_w, rp->out_h, w, h);
    } else if (hash == rp->got_hash) {
        rp->matched++;
    } else {
        rp->differed++;
        char line[160] = "";
        if (pixels) {   /* the recorded frame is in the record: count the pixels that differ */
            uint64_t n = 0, maxd = 0;
            uint32_t *rec = rp->dump ? malloc((size_t)w * h * 4) : NULL;
            for (int y = 0; y < h; y++) {
                const uint32_t *row = rw_rblob(r, NULL);
                if (!row) break;
                if (rec) memcpy(rec + (size_t)y * w, row, (size_t)w * 4);
                for (int x = 0; x < w; x++) {
                    const uint32_t a = row[x], b = rp->out[(size_t)y * w + x];
                    if ((a ^ b) & 0xFFFFFFu) {
                        n++;
                        for (int c = 0; c < 24; c += 8) {
                            const int d = abs((int)((a >> c) & 255) - (int)((b >> c) & 255));
                            if ((uint64_t)d > maxd) maxd = (uint64_t)d;
                        }
                    }
                }
            }
            snprintf(line, sizeof line, ": %llu of %d pixels differ (%.3f%%), largest channel difference %llu", (unsigned long long)n, w * h,
                     100.0 * n / ((double)w * h), (unsigned long long)maxd);
            if (rec && rp->dump) {
                char path[512];
                snprintf(path, sizeof path, "%s/rec_%05u.bmp", rp->dump, frame);
                write_bmp(path, rec, w, h, (size_t)w);
            }
            free(rec);
        }
        if (rp->differed <= 20 || rp->verbose) printf("[replay] frame %u differs%s\n", frame, line);
    }
    if (rp->hashes) fprintf(rp->hashes, "%u %016llx %016llx\n", frame, (unsigned long long)hash, (unsigned long long)rp->got_hash);
    const int dump = rp->dump && rp->have_got && !rp->got_ret &&
                     (rp->dump_every > 0 ? rp->frames % (uint32_t)rp->dump_every == 0 : hash != rp->got_hash);
    if (dump) {
        char path[512];
        snprintf(path, sizeof path, "%s/frame_%05u.bmp", rp->dump, frame);
        write_bmp(path, rp->out, rp->out_w, rp->out_h, (size_t)rp->out_w);
    }
    rp->have_got = 0;
}

/* RW_REC_DRAW_STATE: each slot's setter, in slot order (render_wire.h). */
static int draw_state(RwReplay *rp, RwReader *r) {
    const uint32_t mask = rw_ru32(r);
    for (int s = 0; s < RW_DS_SLOTS && !r->bad; s++) {
        if (!(mask >> s & 1)) continue;
        size_t n = 0;
        const uint8_t *args = rw_rblob_plain(r, &n);
        if (!args) return -1;
        RwReader a;
        rw_reader(&a, args, n);   /* the setter's own record payload: aligned as it was */
        if (rw_replay_call(rp, rw_draw_state_id(s), &a)) return -1;
    }
    return r->bad || mask >> RW_DS_SLOTS ? -1 : 0;
}

typedef struct {
    uint8_t *block[64];
    size_t size[64], used[64];
    int n, cur;
} Arena;
static void arena_reset(Arena *a) {
    for (int i = 0; i < a->n; i++) a->used[i] = 0;
    a->cur = 0;
}
static uint8_t *arena_take(Arena *a, size_t n) {
    n = (n + RW_ALIGN - 1) & ~(size_t)(RW_ALIGN - 1);
    for (; a->cur < a->n; a->cur++)
        if (a->size[a->cur] - a->used[a->cur] >= n) {
            uint8_t *p = a->block[a->cur] + a->used[a->cur];
            a->used[a->cur] += n;
            return p;
        }
    if (a->n == 64) return NULL;
    const size_t size = n > (64u << 20) ? n : (64u << 20);
    a->block[a->n] = malloc(size);   /* malloc is at least 8-aligned: RW_ALIGN */
    if (!a->block[a->n]) return NULL;
    a->size[a->n] = size;
    a->used[a->n] = n;
    a->cur = a->n++;
    return a->block[a->cur];
}

int main(int argc, char **argv) {
    static RwReplay rp;
    rp.upscaler = -1;
    const char *path = NULL, *hashes = NULL;
    uint32_t max_frames = 0;
    int mem_map = 0, via = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dump") && i + 1 < argc) rp.dump = argv[++i];
        else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc) rp.dump_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hashes") && i + 1 < argc) hashes = argv[++i];
        else if (!strcmp(argv[i], "-v")) rp.verbose = 1;
        else if (!strcmp(argv[i], "--mem-map")) mem_map = 1;
        else if (!strcmp(argv[i], "--upscaler") && i + 1 < argc) rp.upscaler = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-taa")) rp.no_taa = 1;
        else if (argv[i][0] != '-' && !path) path = argv[i];
        else if (!strcmp(argv[i], "--via") && i + 1 < argc && !strcmp(argv[i + 1], "loopback")) {
            char *o[] = {"--render-process", "loopback"};
            int k = 0;
            render_rec_option(2, o, &k);
            i++;
            via = 1;
        } else if (!strcmp(argv[i], "--cache") && i + 1 < argc) {
            char *o[] = {"--renderer-cache", argv[++i]};
            int k = 0;
            if (render_rec_option(2, o, &k) != 1) return 2;
        }
        else if (argv[i][0] != '-' && !path) path = argv[i];
        else { fprintf(stderr, "usage: render64_replay FILE [--dump DIR] [--dump-every K] [--frames N] [--hashes FILE] [--mem-map] [--upscaler N] [--no-taa] [--via loopback] [--cache off|check] [-v]\n"); return 2; }
    }
    if (!path) { fprintf(stderr, "usage: render64_replay FILE [--dump DIR] [--dump-every K] [--frames N] [--hashes FILE] [--mem-map] [--upscaler N] [--no-taa] [--via loopback] [--cache off|check] [-v]\n"); return 2; }
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "render64_replay: cannot open %s\n", path); return 2; }
    RwFileHeader h;
    char why[160] = "shorter than its header";
    if (fread(&h, sizeof h, 1, f) != 1 || rw_check_header(&h, why, sizeof why)) {
        fprintf(stderr, "render64_replay: %s: %s\n", path, why);
        return 2;
    }
    if (hashes) rp.hashes = fopen(hashes, "w");
    printf("[replay] %s: schema %08x, written by a %u-bit process; replaying in a %d-bit one\n", path, h.schema, h.ptr_bytes * 8,
           (int)(sizeof(void *) * 8));
    /* Every record of a frame keeps its own address until the frame's renderer_begin is replayed
     * again: the renderer keys some per-frame copies by pointer (GPU skinning's programs and
     * constants: the recording process passes one pointer for repeated data), so reusing one
     * buffer would hand it the same pointer for different data. Blocks of 64 MB, never moved. */
    Arena arena = {0};
    RwDec *dec = rw_dec_new();   /* the stream's references, constants and base image (render_wire.h) */
    if (!dec) { fprintf(stderr, "render64_replay: out of memory\n"); return 2; }
    uint64_t records = 0, ds_records = 0;
    const double t0 = now_ms();
    double frame_t0 = t0, frame_call0 = 0, frame_end0 = 0;
    for (;;) {
        RwRecordHeader rh;
        if (fread(&rh, sizeof rh, 1, f) != 1) break;
        if (rh.id == RW_ID_BEGIN) {   /* a frame chunk starts */
            arena_reset(&arena);
            rw_dec_frame(dec);
        }
        uint8_t *buf = arena_take(&arena, sizeof rh + rh.len);
        if (!buf) { fprintf(stderr, "render64_replay: out of memory\n"); return 2; }
        memcpy(buf, &rh, sizeof rh);
        if (rh.len && fread(buf + sizeof rh, 1, rh.len, f) != rh.len) { fprintf(stderr, "render64_replay: truncated record\n"); break; }
        rp.file_bytes += sizeof rh + rh.len;
        records++;
        RwReader r;
        rw_reader(&r, buf + sizeof rh, rh.len);
        r.dec = rh.flags & RW_FLAG_PREAMBLE ? NULL : dec;   /* preamble records stand alone */
        rec_flags = rh.flags;
        if (rh.id == RW_REC_PREAMBLE_BEGIN) rp.in_preamble = 1;
        else if (rh.id == RW_REC_PREAMBLE_END) {
            rp.in_preamble = 0;
            printf("[replay] preamble: %.1f MB, %llu records\n", rp.file_bytes / 1048576.0, (unsigned long long)records);
        } else if (rh.id == RW_REC_PIPE_PLAN) {   /* the recording's pipeline misses for the next renderer_end/present */
            const uint32_t n = rw_ru32(&r);
            if (r.bad || n > (rh.len - 4) / 4) { fprintf(stderr, "render64_replay: bad pipeline plan\n"); return 2; }
            renderer_pipeline_plan((const uint32_t *)r.p, n);
            rp.plan_misses += n;
        } else if (rh.id == RW_REC_QUERY) {   /* a --render-process helper recording's remote query: no renderer state */
        } else if (rh.id == RW_REC_DRAW_STATE) {
            if (draw_state(&rp, &r)) { fprintf(stderr, "render64_replay: bad draw-state record\n"); return 2; }
            ds_records++;
        } else if (rh.id == RW_REC_FRAME) {
            frame_check(&rp, &r);
            rw_dec_frame(dec);   /* the next chunk starts after the frame record */
            const double t = now_ms(), fm = (rp.call_ms - frame_call0) - (rp.end_ms - frame_end0);
            if (rp.frames > 1 && fm > rp.frame_ms_max) rp.frame_ms_max = fm;
            frame_t0 = t, frame_call0 = rp.call_ms, frame_end0 = rp.end_ms;
            if (max_frames && rp.frames >= max_frames) break;
        } else if (rh.id >= RW_ID_COUNT || rw_replay_call(&rp, rh.id, &r)) {
            fprintf(stderr, "render64_replay: bad record %u (%u bytes)\n", rh.id, rh.len);
            return 2;
        }
    }
    (void)frame_t0;
    (void)ds_records;
    rw_dec_free(dec);
    fclose(f);
    if (rp.hashes) fclose(rp.hashes);
    const double total = now_ms() - t0;
    printf("[replay] %u frames: %u match the recording byte for byte, %u differ, %u not checked (no readback)\n", rp.frames, rp.matched,
           rp.differed, rp.unchecked);
    if (rp.gen_matched || rp.gen_differed || rp.gen_missing)
        printf("[replay] generated frames (--frame-gen): %u match, %u differ, %u built on one side only\n", rp.gen_matched, rp.gen_differed,
               rp.gen_missing);
    uint32_t rd = 0;
    for (int i = 0; i < RW_ID_COUNT; i++) rd += rp.result_diffs[i];
    printf("[replay] results different from the recording: %u (%u handles)\n", rd, rp.handle_diffs);
    if (rp.later) printf("[replay] results not waited for through the loopback (fire-and-forget, not compared): %llu\n", (unsigned long long)rp.later);
    if (rp.plan_misses) printf("[replay] pipeline misses of the recording reproduced: %llu\n", (unsigned long long)rp.plan_misses);
    for (int i = 0; i < RW_ID_COUNT; i++)
        if (rp.result_diffs[i]) printf("[replay]   %s: %u\n", rw_id_name[i], rp.result_diffs[i]);
    printf("[replay] %.1f MB in %.0f ms; renderer calls %.1f ms (renderer_end %.1f ms) = %.2f ms/frame besides renderer_end, "
           "the slowest %.2f ms\n", rp.file_bytes / 1048576.0, total, rp.call_ms, rp.end_ms,
           rp.frames ? (rp.call_ms - rp.end_ms) / rp.frames : 0.0, rp.frame_ms_max);
    if (mem_map) mem_map_report("replay end", 0);   /* the renderer and its driver without the game */
    renderer_shutdown();
    if (via) render_rec_exit();   /* the loopback's report */
    return rp.differed || rp.gen_differed || rp.gen_missing ? 1 : 0;
}
