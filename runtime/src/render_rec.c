/* Renderer call recorder (render_rec.h): the rr_* wrappers (render_wire_gen.h) and their hooks.
 *
 * A recording starts mid-game, so it begins with a preamble that rebuilds the renderer's state
 * in a fresh process: the last renderer_configure and renderer_init, the last value of every call
 * whose state outlives a frame (texture budget, relief/hair/skin/emission settings, gamma ramp,
 * frame-counter text, material and wall maps) and every surface made so far. The texture cache is
 * the exception: the window's first renderer_begin drops it (a lowered budget for that one call,
 * the path a full cache takes), so every texture the window draws is created inside it. What the
 * preamble cannot carry is the renderer's history (TAA, auto exposure, the previous frame, the
 * surfaces' pixels and any GPU-side accumulation): a replay's first frames can differ there.
 *
 * Inside the window the stream keeps state (render_wire.h RwEnc): in-frame
 * references, skinning program references and constant deltas, the base image only when it changed;
 * the draw-state setters are held back and written as one RW_REC_DRAW_STATE record (only the values
 * that changed) before the next call that is not one of them; queries without parameters are answered
 * from the snapshot taken after the last frame (the one a renderer process publishes). */
#define RENDER_REC_NO_ROUTE
#include "render_rec.h"
#include "render_wire.h"
#define RW_GEN_NAMES
#include "render_wire_gen.h"
#include "platform.h"
#include "mem_map.h"
#include "render_ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>   /* GetProcessTimes / GetThreadTimes (serve_cpu_ms) */
#endif

int rr_active;
/* How a loopback call with a result gets it (gen_wire.py LOOP_ASK). */
enum { RR_LOOP_WAIT, RR_LOOP_KNOWN, RR_LOOP_LATER, RR_LOOP_SKIP };

static struct {
    const char *path;
    FILE *f;
    uint32_t start, count;   /* window: renderer frames [start, start + count) */
    int traffic, pixels_every, memmap_every;
    uint32_t frame;          /* renderer_begin calls so far */
    int in_window, done, drop_pending, init_w, init_h;
    uint64_t budget;
    RwWriter w;              /* the window's records since the last frame end */
    int cur_keep;            /* 1 sticky, 2 saved list (the record being written) */
    double t0, ser_ms;       /* serialization time of the current frame */
    uint64_t t_cur;          /* the loopback: TSC at the current record's start */
    double call_ms, end_ms;  /* the current frame's time in renderer calls, in renderer_end / present */
    RwEnc *enc;              /* the window's stream state */
    uint64_t cached_cat[RW_CATS];   /* whole payloads whose bytes were sent this frame or the one before (the window) */
    uint64_t ds_calls, ds_records, ds_sent, ds_elided;   /* draw-state setter calls, records, slots sent, calls elided */
    uint64_t snap_answers, snap_stale[RW_QUERY_COUNT];   /* queries answered from the snapshot; stale answers */
    uint64_t pipe_misses, pipe_frames;   /* pipeline decisions that missed (RW_REC_PIPE_PLAN), frames with any */
    float *calls_ms, *ends_ms;
    uint8_t *sticky[RW_ID_COUNT];
    size_t sticky_n[RW_ID_COUNT];
    uint8_t *saved;          /* the surfaces' records */
    size_t saved_n, saved_cap;
    /* per-frame stats of the window */
    uint32_t nframes;
    uint64_t *bytes, (*cat)[RW_CATS];
    float *ms;
    uint64_t calls[RW_ID_COUNT], queries[RW_QUERY_COUNT];
    uint64_t file_bytes, preamble_bytes;
    uint32_t hashes, gen_hashes;
    /* the content cache (render_wire.h): on unless --renderer-cache off */
    int cache_off, cache_check;
    /* --renderer-hashes FILE: every renderer_end's frame hash (and the generated frame's) */
    const char *hashes_path;
    FILE *hashes_f;
    uint32_t hashes_frames;
    /* --render-process loopback */
    int loop, loop_open, loop_failed;
    R64Ipc *lp, *lh;          /* the channel's writer and reader views (one mapping) */
    RwDec *dec;               /* the decoder's stream state (its arena: the channel's content region) */
    uint8_t *asmb;            /* a record split by the ring's end or by a partial write */
    size_t asm_n, asm_cap;
    uint8_t *cfg_keep;        /* the last renderer_configure record: the renderer keeps its pointers */
    size_t cfg_cap;
    uint32_t *loop_out;       /* renderer_end's output (the caller's buffer) */
    const RendererConfig *loop_cfg;   /* renderer_configure's (its window callbacks) */
    double loop_got, next_frame_time;
    uint64_t fr_bytes, fr_cat[RW_CATS];   /* the frame's bytes so far (drained) */
    uint64_t loop_syncs[RW_ID_COUNT + 1];   /* waits for the decoder, by the call that waited ([RW_ID_COUNT]: queries, LOCAL calls) */
    uint64_t loop_drains, loop_bytes;
    uint64_t t_enc, t_drain, t_render;      /* TSC ticks: encoding, draining, inside the real renderer calls */
    float *enc_ms, *ovh_ms;                 /* per window frame: encoding, draining minus the renderer */
    float *hash_ms, *ds_ms;                 /* per window frame: of the encoding, content hashing and draw-state setters */
    uint64_t fr_t_enc, fr_t_drain, fr_t_render, fr_syncs, fr_t_ds, fr_drains, hash_ticks0, fr_t_ring;
    float *ring_ms;                         /* per window frame: writing the ring (of the drains) */
    float *wait_ms;                         /* per window frame: --render-process helper: waiting for replies and acks */
    uint32_t *syncs_fr, *drains_fr;
    void *host_thread;
    uint64_t other_thread;
    /* the texture-handle mirror (rr_loop_ask_renderer_texture_find) */
    uint32_t dec_begins, tm_checked;   /* renderer_begin records decoded; the frame whose renderer_begin the mirror saw */
    int tex_lost;                      /* a decoded renderer_begin freed textures (or dropped the cache) */
    uint64_t loop_mispredicted, tm_known, tm_skipped, tm_waits, tm_clears, skin_later;
} R = {.start = 1, .count = 60, .next_frame_time = -1};
static int rr_loop;        /* --render-process loopback */
static int rr_snap_check;  /* queries answered from the snapshot are compared with the live value (stale count) */

static void cached(const void *p, size_t n, int cat);
static void snap_refresh(void);
static int rr_snap_on;   /* queries are answered from the snapshot (render_wire_gen.h rr_snap) */

/* ---- --render-process helper (W6): declarations; the code is at the end ----
 * The game side is the loopback's encoder (R.loop_open, loop_host, the content cache, the draw-state records, the
 * texture-handle mirror, fire-and-forget skinned draws) writing into the CMD ring of the channel r64_input.c opened
 * with the helper (r64_shared_channel); nothing decodes here. The helper side (render_rec_serve, in the x86_64 helper)
 * is the loopback's decoder reading that ring on the helper's render thread into its renderer. What comes back on
 * the BACK ring, as R64Msg messages: the result of every record flagged RW_FLAG_REPLY (the calls the game waits for:
 * renderer_init, texture_create, an unknown texture_find, surfaces, renderer_end's readback ...), the reply of every
 * remote query (RW_REC_QUERY: the queries the snapshot does not answer, gen_wire.py LATE/CONST apart), and after
 * every frame an ack: the frame's present result and hash, the snapshot, the LATE queries' answers, the texture
 * cache's state and the frame's pipeline misses. The game waits at the end of frame N for frame N + 1 - K's ack
 * (K = --render64-frames, 2) and answers frame N + 1's snapshot queries from it, so the stream does not depend on
 * how far the helper is behind. */
static int rr_remote;    /* the game side of --render-process helper: queries and LOCAL calls go to the helper */
typedef struct { uint32_t type, len; } R64Msg;   /* a BACK ring message: len payload bytes follow */
enum { R64M_REPLY = 1, R64M_QREPLY, R64M_ACK };
/* Remote requests beyond the queries (RW_REC_QUERY's index). */
enum { RQ_CACHE_SAVE = RW_QUERY_COUNT, RQ_SNAP, RQ_TEX, RQ_ALL };
static RwWriter *rr_rq_begin(int qi, int kind);
static RwReader *rr_rq_call(int qi, int kind);
static void rr_rq_out(RwReader *r, void *dst, size_t bytes);
static const char *rr_rq_str(RwReader *r, int qi);
static void *rr_rq_scratch(int k, size_t bytes);
static size_t rr_rq_rows(void);
static uint64_t rr_ga_renderer_texture_bytes(void);
static void rr_rl_renderer_pipeline_cache_save(void);
static void rr_rl_renderer_set_vertex_check(RVertexCheck check);
static void rr_rl_renderer_set_draw_tag(uint32_t tag);
static void rr_rl_renderer_set_vertex_check_frame(int on);
static uint32_t rr_rl_renderer_pipeline_misses(uint32_t *out, uint32_t cap);
static void rr_rl_renderer_pipeline_plan(const uint32_t *misses, uint32_t n);
static double rr_rl_renderer_frame_time(void);
static void rr_rl_renderer_set_frame_time(double t_ms);
static int rw_serve_query(uint32_t qi, RwReader *rq_r, RwWriter *rq_o);
static void rw_serve_late(RwWriter *o);
static int helper_init(void);
static void helper_drain(void);
static void helper_snap(int frame_end, int present);
static double helper_result(uint32_t id);
static void helper_result_at(RwWriter *w, int kind);
static void helper_mark(int what);
static int helper_tm_usable(void);
static void helper_exit(void);
static void helper_frame_start(void);
/* --render-process helper: texture_create returns a handle of the game's own (RR_TEX_ID_BASE + n, above every handle the
 * renderer gives out), which the helper maps to its renderer's handle (serve_map); renderer handles pass unchanged. */
#define RR_TEX_ID_BASE 0x00100000u
static uint32_t helper_tex_id(size_t bytes);
static void helper_window_end(void);
static void serve_query(RwReader *r);
static void serve_before(uint32_t id);
static void serve_after(uint32_t id);
static void serve_config(RendererConfig *c, int had_surface);
static int helper_on;    /* game side: --render-process helper is running */
static uint64_t helper_fr_wait;   /* TSC ticks the game waited for the helper since the last frame end */
static void rr_rq_renderer_output_size(int *w, int *h);
static int rr_rq_renderer_frame_gen_on(void);
static int rr_rq_renderer_frame_gen_readback(uint32_t *out, int pitch_bytes);
static int serving;      /* this process is the helper (render_rec_serve) */
static uint32_t serve_flags;   /* the helper: the record being decoded's flags */
static int helper_wanted, helper_frames = 2;   /* --render-process helper, --render64-frames K */
enum { HM_END, HM_FRAME, HM_FRAME_STOP };     /* helper_mark: where the frame's records are in the stream */

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
/* The loopback's cost accounting: TSC ticks (a clock read per call would cost more than what it
 * measures), converted with a rate taken against the monotonic clock. Every record and setter call is timed;
 * with XB_LOOP_TIME_SAMPLE=1 one in RR_TIME_EVERY is, counted RR_TIME_EVERY times
 * (the TSC reads themselves cost ~0.1 ms/frame at ~20 k calls and payloads per frame, but a sampled record that
 * stalls counts 16 times: no per-frame p95). Drains, ring writes and the decoder's renderer calls are timed each. */
static uint64_t tsc(void) { return __builtin_ia32_rdtsc(); }
#define RR_TIME_EVERY 16u
static uint32_t rr_time_mask, rr_time_scale = 1, rr_tick;
static int rr_timed;   /* the record being written is timed */
static int rr_time_this(void) {
    rr_timed = !(++rr_tick & rr_time_mask);
    return rr_timed;
}
static double tsc_per_ms(void) {
    static double rate;
    static uint64_t t0;
    static double m0;
    if (!t0) { t0 = tsc(); m0 = now_ms(); return 0; }
    const double dm = now_ms() - m0;
    if (dm > 200 && !rate) rate = (double)(tsc() - t0) / dm;
    return rate;
}

static int parse_window(const char *s, uint32_t *start, uint32_t *count) {
    unsigned a, b;
    if (sscanf(s, "%u:%u", &a, &b) != 2 || !a || !b) return -1;
    *start = a;
    *count = b;
    return 0;
}
int render_rec_option(int argc, char **argv, int *i) {
    const char *o = argv[*i];
    const int has = *i + 1 < argc;
    if (!strcmp(o, "--record-renderer") && has) R.path = argv[++*i];
    else if (!strcmp(o, "--record-renderer-frames") && has) {
        if (parse_window(argv[++*i], &R.start, &R.count)) { fprintf(stderr, "--record-renderer-frames: START:COUNT, both above 0\n"); return -1; }
    } else if (!strcmp(o, "--record-renderer-pixels") && has) R.pixels_every = atoi(argv[++*i]);
    else if (!strcmp(o, "--renderer-traffic") && has) {
        R.traffic = 1;
        if (parse_window(argv[++*i], &R.start, &R.count)) { fprintf(stderr, "--renderer-traffic: START:COUNT, both above 0\n"); return -1; }
    } else if (!strcmp(o, "--mem-map-every") && has) {
        R.memmap_every = atoi(argv[++*i]);
        if (R.memmap_every <= 0) { fprintf(stderr, "--mem-map-every: a frame count above 0\n"); return -1; }
    } else if (!strcmp(o, "--render-process") && has) {
        const char *v = argv[++*i];
        if (!strcmp(v, "off")) return 1;
        if (!strcmp(v, "helper")) {   /* the x86_64 helper renders (W6, Windows W7); r64_input.c starts it with the window */
            helper_wanted = 1;
            r64_helper_render = 1;
            r64_helper_schema = RW_HELPER_SCHEMA;
        } else if (strcmp(v, "loopback")) {
            fprintf(stderr, "--render-process: off, loopback, helper-io or helper\n");
            return -1;
        }
        R.loop = rr_loop = 1;
    } else if (!strcmp(o, "--render64-frames") && has) {
        helper_frames = atoi(argv[++*i]);
        if (helper_frames < 1 || helper_frames > 4) { fprintf(stderr, "--render64-frames: 1 to 4 frames in flight\n"); return -1; }
        r64_helper_frames = (uint32_t)helper_frames;
        return 1;
    } else if (!strcmp(o, "--renderer-cache") && has) {
        const char *v = argv[++*i];
        if (!strcmp(v, "off")) R.cache_off = 1;
        else if (!strcmp(v, "check")) R.cache_check = 1;
        else if (strcmp(v, "on")) { fprintf(stderr, "--renderer-cache: on, off or check\n"); return -1; }
        return 1;
    } else if (!strcmp(o, "--renderer-check-queries")) {
        rr_snap_check = 2;   /* also in loopback (each answer then waits for the decoder) */
    } else if (!strcmp(o, "--renderer-hashes") && has) {
        R.hashes_path = argv[++*i];
        r64_helper_hash = 1;   /* with the helper presenting headless, it hashes the frames it reads back */
    } else return 0;
    if (R.path) r64_helper_hash = 1;
    rr_active = 1;
    return 1;
}

/* ---- recording ---- */
static void keep_copy(uint8_t **dst, size_t *dn, const uint8_t *p, size_t n) {
    uint8_t *c = realloc(*dst, n);
    if (!c) return;
    memcpy(c, p, n);
    RwRecordHeader h;
    memcpy(&h, c, sizeof h);
    h.flags |= RW_FLAG_PREAMBLE;
    memcpy(c, &h, sizeof h);
    *dst = c;
    *dn = n;
}
static void save_listed(const uint8_t *p, size_t n) {
    if (R.saved_n + n > R.saved_cap) {
        size_t cap = R.saved_cap ? R.saved_cap * 2 : 4096;
        while (cap < R.saved_n + n) cap *= 2;
        uint8_t *b = realloc(R.saved, cap);
        if (!b) return;
        R.saved = b;
        R.saved_cap = cap;
    }
    memcpy(R.saved + R.saved_n, p, n);
    RwRecordHeader h;
    memcpy(&h, R.saved + R.saved_n, sizeof h);
    h.flags |= RW_FLAG_PREAMBLE;
    memcpy(R.saved + R.saved_n, &h, sizeof h);
    R.saved_n += n;
}

/* Time inside the real renderer calls of the window's frames (renderer_end and renderer_present
 * apart: they wait for the GPU). */
static double rr_clock(void) { return R.in_window ? now_ms() : 0; }
static void rr_call_time(uint32_t id, double t0) {
    if (!R.in_window || t0 == 0) return;
    const double d = now_ms() - t0;
    if (id == RW_ID_END || id == RW_ID_PRESENT) R.end_ms += d;
    else R.call_ms += d;
}
/* ---- draw state (RW_REC_DRAW_STATE) ----
 * Inside the window a draw-state setter's record goes to ds_w, and its arguments become the slot's
 * pending value. The next recorded call that is not a setter (texture lookups and draw identities
 * apart: they read and write none of the setters' state) first flushes the pending values that differ
 * from the slot's last written one, in one record. The last written values stay known across draws
 * (draws change none of them); any other call forgets them (it may make a handle valid or not, which
 * the setters check when called), so a setter called after it is written again even when unchanged. */
static int is_draw(uint32_t id) {
    return id == RW_ID_DRAW || id == RW_ID_DRAW_LIT || id == RW_ID_DRAW_WORLD_LIT || id == RW_ID_DRAW_SKINNED ||
           id == RW_ID_DRAW_SKINNED_WORLD_LIT;
}
static struct { uint8_t *val, *last; size_t n, last_n, cap, last_cap; int pending, known; } ds[RW_DS_SLOTS];
static RwWriter ds_w;
/* Per RW_ID_*: 0 not asked yet, DS_NONE not a draw-state setter, a setter's slot + 1, or DS_BY_STAGE for
 * renderer_set_stage_texture (its slot is its first argument: rw_draw_state_slot). */
#define DS_NONE ((int8_t)-1)
#define DS_BY_STAGE ((int8_t)100)
static int8_t ds_slot_of[RW_ID_COUNT];
static int ds_slot(uint32_t id, const uint8_t *args, size_t n);
static int keep_bytes(uint8_t **b, size_t *cap, const uint8_t *p, size_t n) {
    if (n > *cap) {
        uint8_t *c = realloc(*b, n);
        if (!c) return -1;
        *b = c;
        *cap = n;
    }
    memcpy(*b, p, n);
    return 0;
}
static void ds_forget(void) {
    for (int s = 0; s < RW_DS_SLOTS; s++) ds[s].known = 0;
}
/* What a non-setter call may have changed for the setters (W2 L8 issue 2): a draw nothing; a texture or
 * movie plane made (or the textures it evicted) only which handles renderer_set_stage_texture accepts
 * (the only setter that checks textures: emission, relief, hair and skin masks are stored as given);
 * anything else everything (surfaces, configuration, budget, frame boundaries). */
static void ds_forget_for(uint32_t id) {
    if (is_draw(id)) return;
    if (id == RW_ID_TEXTURE_CREATE || id == RW_ID_MOVIE_PLANE) {
        for (int s = 0; s < RENDERER_STAGES; s++) ds[s].known = 0;
        return;
    }
    ds_forget();
}
static uint32_t ds_pending;   /* the slots with a pending value (each differs from the slot's last written one, if known) */
static void ds_flush(void) {
    const uint32_t mask = ds_pending;
    if (!mask) return;
    ds_pending = 0;
    for (int s = 0; s < RW_DS_SLOTS; s++) ds[s].pending = 0;
    rw_begin_record(&R.w, RW_REC_DRAW_STATE, 0);
    rw_u32(&R.w, mask);
    for (int s = 0; s < RW_DS_SLOTS; s++) {
        if (!(mask >> s & 1)) continue;
        rw_blob_plain(&R.w, ds[s].val, ds[s].n);
        ds[s].known = !keep_bytes(&ds[s].last, &ds[s].last_cap, ds[s].val, ds[s].n);
        ds[s].last_n = ds[s].n;
        R.ds_sent++;
    }
    rw_end_record(&R.w);
    R.ds_records++;
}
/* Before renderer_end/present's record: the frame's pipeline misses (renderer_pipeline_misses; empty
 * without pipeline_async), which the replay hands to renderer_pipeline_plan before the same call. */
static void pipe_plan_record(void) {
    static uint32_t *buf;
    static uint32_t cap;
    uint32_t n = renderer_pipeline_misses(buf, cap);
    if (n > cap) {
        uint32_t *b = realloc(buf, n * sizeof *b);
        if (!b) { R.w.bad = 1; return; }
        buf = b;
        cap = n;
        n = renderer_pipeline_misses(buf, cap);
    }
    rw_begin_record(&R.w, RW_REC_PIPE_PLAN, 0);
    rw_u32(&R.w, n);
    for (uint32_t i = 0; i < n; i++) rw_u32(&R.w, buf[i]);
    rw_end_record(&R.w);
    R.pipe_misses += n;
    R.pipe_frames += n > 0;
}
/* The stream is on: inside a recording or traffic window, or always with the loopback. */
static int streaming(void) { return R.in_window || R.loop_open > 0; }
static __thread int loop_host;   /* this thread opened the loopback (the renderer's thread) */
static RwWriter *rr_start(uint32_t id, int keep) {
    if (R.loop_open > 0 && !loop_host) return NULL;   /* another thread's direct call: not part of the stream */
    if (!streaming() && (R.done || !keep)) return NULL;
    if (streaming()) {
        if (R.loop_open > 0 && rr_time_this()) R.t_cur = tsc();
        R.w.timed = rr_timed;
        if (R.in_window) {
            if (R.loop_open <= 0) R.t0 = now_ms();   /* the loopback counts TSC ticks instead (no clock read per call) */
            R.calls[id]++;
        }
        if (!ds_slot_of[id]) ds_slot(id, NULL, 0);
        if (ds_slot_of[id] > 0) {   /* held back (the slot is known once its arguments are) */
            ds_w.n = ds_w.rec = 0;
            ds_w.bad = 0;
            const RwRecordHeader h = {(uint16_t)id, 0, 0};
            rw_put_fast(&ds_w, &h, sizeof h);
            R.cur_keep = 0;
            return &ds_w;
        }
        if (id != RW_ID_TEXTURE_FIND && id != RW_ID_SET_DRAW_IDENTITY) {
            ds_flush();
            ds_forget_for(id);
        }
        R.w.no_refs = keep != 0;   /* a kept record must stand alone in a later preamble */
        /* the loopback's renderer makes its own pipeline decisions (it is this process's renderer) */
        if ((id == RW_ID_END || id == RW_ID_PRESENT) && !R.loop_open) pipe_plan_record();
        if ((id == RW_ID_END || id == RW_ID_PRESENT) && helper_on) helper_mark(HM_END);   /* the helper's misses go before it */
    }
    R.cur_keep = keep;
    rw_begin_record(&R.w, id, 0);
    return &R.w;
}
/* A setter's payload (its record's bytes after the header) into its slot: the slot's pending value, or
 * nothing when the call changes nothing (the same as the pending value, or no pending value and the same as
 * the last written one, which ds_flush would elide). s < 0 (a stage past the last): a record as it came. */
static void ds_store(uint32_t id, int s, const uint8_t *args, size_t n) {
    if (s < 0) {
        ds_flush();
        ds_forget_for(id);
        rw_begin_record(&R.w, id, 0);
        rw_bytes(&R.w, args, n);
        rw_end_record(&R.w);
        return;
    }
    R.ds_calls++;
    if (ds[s].known && ds[s].last_n == n && !memcmp(ds[s].last, args, n)) {   /* back to the last written value: nothing to send */
        ds[s].pending = 0;
        ds_pending &= ~(1u << s);
        R.ds_elided++;
        return;
    }
    if (ds[s].pending && ds[s].n == n && !memcmp(ds[s].val, args, n)) return;
    if (n > ds[s].cap && keep_bytes(&ds[s].val, &ds[s].cap, args, n)) {
        R.w.bad = 1;
        return;
    }
    memcpy(ds[s].val, args, n);
    ds[s].n = n;
    ds[s].pending = 1;
    ds_pending |= 1u << s;
}
static int ds_slot(uint32_t id, const uint8_t *args, size_t n) {
    if (!ds_slot_of[id]) {
        const int sl = rw_draw_state_slot(id, 0);
        ds_slot_of[id] = id == RW_ID_SET_STAGE_TEXTURE ? DS_BY_STAGE : sl >= 0 ? (int8_t)(sl + 1) : DS_NONE;
    }
    if (ds_slot_of[id] != DS_BY_STAGE) return ds_slot_of[id] - 1;
    uint32_t first = 0;
    if (n >= 4) memcpy(&first, args, 4);
    return rw_draw_state_slot(id, first);
}
static void ds_finish(void) {
    rw_end_record(&ds_w);
    if (!ds_w.bad) {
        RwRecordHeader h;
        memcpy(&h, ds_w.buf, sizeof h);
        const uint8_t *args = ds_w.buf + sizeof h;
        const size_t n = ds_w.n - sizeof h;
        ds_store(h.id, ds_slot(h.id, args, n), args, n);
    }
    if (R.loop_open > 0 && rr_timed) {
        const uint64_t d = (tsc() - R.t_cur) * rr_time_scale;
        R.fr_t_enc += d;
        R.fr_t_ds += d;
    }
}
/* The loopback's draw-state setters (gen_wire.py DRAW_STATE): the arguments packed as their record's payload
 * (rr_ds_put scalars, rr_ds_blob a plain struct pointer: rw_blob's whole form), no record writer. */
#define RR_DS_MAX 512
static inline void rr_ds_put(uint8_t *a, size_t *n, const void *p, size_t k) {
    memcpy(a + *n, p, k);
    *n += k;
}
static inline void rr_ds_pad(uint8_t *a, size_t *n) {   /* to 8 from the record start (the header is 8 bytes) */
    while (*n % RW_ALIGN) a[(*n)++] = 0;
}
static inline void rr_ds_blob(uint8_t *a, size_t *n, const void *p, size_t k) {
    const uint32_t len = p ? (uint32_t)k : RW_NULL;
    rr_ds_put(a, n, &len, 4);
    if (!p) return;
    rr_ds_pad(a, n);
    rr_ds_put(a, n, p, k);
    rr_ds_pad(a, n);
}
static void rr_ds_call(uint32_t id, uint8_t *a, size_t n) {
    const int timed = rr_time_this();
    const uint64_t t0 = timed ? tsc() : 0;
    rr_ds_pad(a, &n);
    if (R.in_window) R.calls[id]++;
    ds_store(id, ds_slot(id, a, n), a, n);
    if (timed) {
        const uint64_t d = (tsc() - t0) * rr_time_scale;
        R.fr_t_enc += d;
        R.fr_t_ds += d;
    }
}
static void rr_finish(RwWriter *w) {
    if (w == &ds_w) {
        ds_finish();
        return;
    }
    rw_end_record(w);
    if (!w->bad) {
        const uint8_t *p = w->buf + w->rec;
        const size_t n = w->n - w->rec;
        RwRecordHeader h;
        memcpy(&h, p, sizeof h);
        if (R.cur_keep == 1) keep_copy(&R.sticky[h.id], &R.sticky_n[h.id], p, n);
        else if (R.cur_keep == 2) save_listed(p, n);
    }
    if (R.loop_open > 0) {
        if (rr_timed) R.fr_t_enc += (tsc() - R.t_cur) * rr_time_scale;
    } else if (R.in_window) R.ser_ms += now_ms() - R.t0;
    else w->n = w->rec;   /* outside the window only the copies are kept */
}
static void rr_query(int q) {
    if (R.in_window) R.queries[q]++;
}

static void put_file(const void *p, size_t n) {
    if (R.f && n && fwrite(p, 1, n, R.f) != n) {
        printf("[render64] %s: write failed: recording stopped\n", R.path);
        fclose(R.f);
        R.f = NULL;
    }
    R.file_bytes += n;
}
static void marker(uint32_t id) {
    RwWriter m = {0};
    rw_begin_record(&m, id, RW_FLAG_PREAMBLE);
    rw_end_record(&m);
    put_file(m.buf, m.n);
    rw_free(&m);
}
static int stream_cache(RwEnc *e, uint32_t bytes) {
    if (R.cache_off) return 0;
    if (rw_enc_cache(e, bytes)) { printf("[render64] content cache: out of memory: off\n"); return -1; }
    rw_enc_cache_verify(e, R.cache_check);
    return 0;
}
static void open_window(void) {
    R.in_window = 1;
    R.bytes = calloc(R.count, sizeof *R.bytes);
    R.cat = calloc(R.count, sizeof *R.cat);
    R.ms = calloc(R.count, sizeof *R.ms);
    R.calls_ms = calloc(R.count, sizeof *R.calls_ms);
    R.ends_ms = calloc(R.count, sizeof *R.ends_ms);
    R.enc_ms = calloc(R.count, sizeof *R.enc_ms);
    R.ovh_ms = calloc(R.count, sizeof *R.ovh_ms);
    R.syncs_fr = calloc(R.count, sizeof *R.syncs_fr);
    R.hash_ms = calloc(R.count, sizeof *R.hash_ms);
    R.ds_ms = calloc(R.count, sizeof *R.ds_ms);
    R.drains_fr = calloc(R.count, sizeof *R.drains_fr);
    R.ring_ms = calloc(R.count, sizeof *R.ring_ms);
    R.wait_ms = calloc(R.count, sizeof *R.wait_ms);
    if (R.loop_open > 0) {   /* the stream is already on: the window only measures (the file was opened with it) */
        if (R.traffic) rw_enc_on_whole(R.enc, cached);
        return;
    }
    rw_reset(&R.w);
    if (!R.enc) R.enc = rw_enc_new();
    if (!R.enc) {
        printf("[render64] out of memory: no recording\n");
        R.in_window = 0;
        R.done = 1;
        return;
    }
    stream_cache(R.enc, RW_CACHE_BYTES);
    rw_enc_stream(R.enc);
    if (R.traffic) rw_enc_on_whole(R.enc, cached);
    R.w.enc = R.enc;
    for (int s = 0; s < RW_DS_SLOTS; s++) ds[s].pending = ds[s].known = 0;
    ds_pending = 0;
    snap_refresh();
    if (R.path) {
        R.f = fopen(R.path, "wb");
        if (!R.f) printf("[render64] %s: cannot create the recording\n", R.path);
        RwFileHeader h;
        rw_file_header(&h);
        put_file(&h, sizeof h);
        marker(RW_REC_PREAMBLE_BEGIN);
        if (R.sticky[RW_ID_CONFIGURE]) put_file(R.sticky[RW_ID_CONFIGURE], R.sticky_n[RW_ID_CONFIGURE]);
        if (R.sticky[RW_ID_INIT]) put_file(R.sticky[RW_ID_INIT], R.sticky_n[RW_ID_INIT]);
        for (int id = 1; id < RW_ID_COUNT; id++)
            if (R.sticky[id] && id != RW_ID_CONFIGURE && id != RW_ID_INIT) put_file(R.sticky[id], R.sticky_n[id]);
        put_file(R.saved, R.saved_n);
        marker(RW_REC_PREAMBLE_END);
        R.preamble_bytes = R.file_bytes;
    }
    /* a recording creates every texture it draws: the cache is dropped at its first frame (the
     * traffic measurement alone leaves the cache as it is) */
    if (R.path) {
        renderer_set_texture_budget(1);
        R.drop_pending = 1;
    }
}

static int cmp_u64(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static int cmp_f(const void *a, const void *b) {
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}
/* The window's traffic: per frame (the first frame, which refills the texture cache, is
 * reported apart from the rest), by category, the calls and the queries. */
static void report(void) {
    const uint32_t n = R.nframes;
    if (!n) return;
    printf("[render64] window: renderer frames %u-%u (%u recorded)%s%s\n", R.start, R.start + n - 1, n, R.path ? ", file " : "",
           R.path ? R.path : "");
    if (R.path)
        printf("[render64] file: %.1f MB (preamble %.1f MB), %u frame hashes, %u generated-frame hashes\n", R.file_bytes / 1048576.0,
               R.preamble_bytes / 1048576.0, R.hashes, R.gen_hashes);
    printf("[render64] first frame%s: %.2f MB, textures %.2f MB\n", R.path ? " (texture cache refill)" : "", R.bytes[0] / 1048576.0,
           R.cat[0][RW_CAT_TEXTURES] / 1048576.0);
    const uint32_t from = n > 1 ? 1 : 0, m = n - from;
    uint64_t *sorted = malloc(m * sizeof *sorted);
    float *ms = malloc(m * sizeof *ms);
    double cat[RW_CATS] = {0}, sum = 0, msum = 0;
    if (!sorted || !ms) { free(sorted); free(ms); return; }
    for (uint32_t i = 0; i < m; i++) {
        sorted[i] = R.bytes[from + i];
        ms[i] = R.ms[from + i];
        sum += (double)sorted[i];
        msum += ms[i];
        for (int c = 0; c < RW_CATS; c++) cat[c] += (double)R.cat[from + i][c];
    }
    qsort(sorted, m, sizeof *sorted, cmp_u64);
    qsort(ms, m, sizeof *ms, cmp_f);
    printf("[render64] per frame (frames 2-%u): mean %.2f MB, p50 %.2f, p95 %.2f, max %.2f MB; %.0f MB/s at 60 fps\n", n,
           sum / m / 1048576.0, sorted[m / 2] / 1048576.0, sorted[m * 95 / 100] / 1048576.0, sorted[m - 1] / 1048576.0,
           sum / m * 60 / 1048576.0);
    char line[1024];
    int k = 0;
    for (int c = 0; c < RW_CATS; c++)
        if (cat[c] > 0) k += snprintf(line + k, sizeof line - (size_t)k, "%s %s %.3f", k ? "," : "", rw_cat_name[c], cat[c] / m / 1048576.0);
    printf("[render64] by category (MB/frame):%s\n", line);
    /* the stream state's savings (render_wire.h), per frame over the whole window */
    const RwEncStats *st = rw_enc_stats(R.enc);
    double refs = 0;
    k = 0;
    for (int c = 0; c < RW_CATS; c++) {
        refs += (double)st->ref_bytes[c];
        if (st->ref_bytes[c]) k += snprintf(line + k, sizeof line - (size_t)k, "%s %s %.3f", k ? "," : "", rw_cat_name[c], st->ref_bytes[c] / (double)n / 1048576.0);
    }
    printf("[render64] in-frame references: %.2f MB/frame not sent (%.0f references/frame, %llu same pointer and size with other bytes);%s\n",
           refs / n / 1048576.0, (double)st->refs / n, (unsigned long long)st->stale, line);
    printf("[render64] skinning: %.1f programs/frame by reference (%llu with other bytes now: the renderer keys them by pointer); "
           "constants %.1f draws/frame, %.1f registers of 192 sent per draw, %.0f%% the last draw's pointer, %.2f MB/frame not sent\n",
           (double)st->prog_refs / n, (unsigned long long)st->prog_stale, (double)st->consts_draws / n,
           st->consts_draws ? (double)st->consts_regs / st->consts_draws : 0.0, st->consts_draws ? 100.0 * st->consts_same_ptr / st->consts_draws : 0.0,
           st->consts_saved / (double)n / 1048576.0);
    printf("[render64] base image: sent in %llu of %u frames (%.2f MB/frame not sent)\n", (unsigned long long)st->base_sent, n,
           st->base_saved / (double)n / 1048576.0);
    if (!R.cache_off) {   /* the content cache (render_wire.h); the counters cover the stream so far */
        double held = 0;
        k = 0;
        for (int c = 0; c < RW_CATS; c++) {
            held += (double)st->cache_hit_bytes[c];
            if (st->cache_hit_bytes[c]) k += snprintf(line + k, sizeof line - (size_t)k, "%s %s %.3f", k ? "," : "", rw_cat_name[c], st->cache_hit_bytes[c] / (double)n / 1048576.0);
        }
        printf("[render64] content cache: %.2f MB/frame the reader already held (%.0f hits, %.0f moves per frame);%s; new %.2f MB/frame; "
               "%llu evicted, %llu sent whole (chunk budget), %llu collisions; hashed %.2f MB/frame\n", held / n / 1048576.0,
               (double)st->cache_hits / n, (double)st->cache_moves / n, line, st->cache_new_bytes / (double)n / 1048576.0,
               (unsigned long long)st->cache_evicted, (unsigned long long)st->cache_refused, (unsigned long long)st->cache_collisions,
               st->cache_hashed / (double)n / 1048576.0);
    }
    printf("[render64] draw state: %.1f setter calls/frame -> %.1f records/frame carrying %.1f values; %.1f calls/frame elided (unchanged)\n",
           (double)R.ds_calls / n, (double)R.ds_records / n, (double)R.ds_sent / n, (double)R.ds_elided / n);
    printf("[render64] pipeline misses (pipeline_async: drawn with the base variant or left out, RW_REC_PIPE_PLAN): %llu in %llu of %u frames\n",
           (unsigned long long)R.pipe_misses, (unsigned long long)R.pipe_frames, n);
    if (R.traffic) {
        double cached = 0;
        k = 0;
        for (int c = 0; c < RW_CATS; c++) {
            cached += (double)R.cached_cat[c];
            if (R.cached_cat[c]) k += snprintf(line + k, sizeof line - (size_t)k, "%s %s %.3f", k ? "," : "", rw_cat_name[c], R.cached_cat[c] / (double)m / 1048576.0);
        }
        printf("[render64] whole payloads whose bytes were sent this frame or the frame before (a content cache would do): %.2f MB/frame;%s\n",
               cached / m / 1048576.0, line);
    }
    printf("[render64] serialization (copy into the frame buffer, this process): mean %.3f ms, p50 %.3f, p95 %.3f, max %.3f ms\n",
           msum / m, ms[m / 2], ms[m * 95 / 100], ms[m - 1]);
    if (R.loop_open > 0) {   /* the loopback's added cost on the renderer thread (TSC; no clock reads per call) */
        float *a = malloc(m * sizeof *a);
        double es = 0, os = 0, ss = 0, hs = 0, ds_ = 0, dr = 0, rs = 0;
        float *g = a ? malloc(m * sizeof *g) : NULL;
        if (a) {
            for (uint32_t i = 0; i < m; i++) {
                a[i] = R.enc_ms[from + i] + R.ovh_ms[from + i];
                es += R.enc_ms[from + i], os += R.ovh_ms[from + i], ss += R.syncs_fr[from + i];
                hs += R.hash_ms[from + i], ds_ += R.ds_ms[from + i], dr += R.drains_fr[from + i], rs += R.ring_ms[from + i];
                if (g) g[i] = R.enc_ms[from + i] + R.ring_ms[from + i];
            }
            qsort(a, m, sizeof *a, cmp_f);
            printf("[render64] loopback added cost: mean %.3f ms/frame (encoding %.3f, ring + decoding %.3f), p50 %.3f, p95 %.3f, max %.3f; "
                   "%.1f waits/frame for the decoder\n", (es + os) / m, es / m, os / m, a[m / 2], a[m * 95 / 100], a[m - 1], ss / m);
            printf("[render64] loopback encoding by part (ms/frame): content hashing %.3f, draw-state setters %.3f, the rest %.3f; %.1f drains/frame\n",
                   hs / m, ds_ / m, (es - hs - ds_) / m, dr / m);
            if (g) {   /* what stays on this thread when the decoder is another process: encoding + ring writes */
                qsort(g, m, sizeof *g, cmp_f);
                printf("[render64] %s game side (encoding + ring writes; decoding is the helper's): mean %.3f ms/frame (ring writes %.3f), "
                       "p50 %.3f, p95 %.3f, max %.3f\n", helper_on ? "helper" : "loopback", (es + rs) / m, rs / m, g[m / 2], g[m * 95 / 100], g[m - 1]);
            }
            if (helper_on && R.wait_ms && g) {   /* the game thread blocked on the helper: replies (texture_create ...) and acks */
                double wsum = 0;
                for (uint32_t i = 0; i < m; i++) wsum += g[i] = R.wait_ms[from + i];
                qsort(g, m, sizeof *g, cmp_f);
                printf("[render64] helper waits on the game thread (replies + frames in flight): mean %.3f ms/frame, p50 %.3f, p95 %.3f, max %.3f\n",
                       wsum / m, g[m / 2], g[m * 95 / 100], g[m - 1]);
            }
            free(a);
            free(g);
        }
    }
    double cs = 0, es = 0;
    for (uint32_t i = 0; i < m; i++) cs += R.calls_ms[from + i], es += R.ends_ms[from + i];
    for (uint32_t i = 0; i < m; i++) ms[i] = R.calls_ms[from + i];
    qsort(ms, m, sizeof *ms, cmp_f);
    printf("[render64] inside renderer calls (moves to a renderer process): mean %.3f ms, p50 %.3f, p95 %.3f ms; "
           "renderer_end/present (GPU wait, readback) mean %.3f ms\n", cs / m, ms[m / 2], ms[m * 95 / 100], es / m);
    /* calls and queries per frame over the whole window, the most frequent first */
    for (int pass = 0; pass < 2; pass++) {
        const int count = pass ? RW_QUERY_COUNT : RW_ID_COUNT;
        const uint64_t *v = pass ? R.queries : R.calls;
        uint64_t total = 0;
        for (int i = 0; i < count; i++) total += v[i];
        k = 0;
        for (int shown = 0; shown < 10; shown++) {
            int best = -1;
            for (int i = 0; i < count; i++)
                if (v[i] && (best < 0 || v[i] > v[best])) best = i;
            if (best < 0) break;
            k += snprintf(line + k, sizeof line - (size_t)k, "%s %s %.1f", shown ? "," : "", (pass ? rw_query_name : rw_id_name)[best] + 9,
                          (double)v[best] / n);
            ((uint64_t *)v)[best] = 0;   /* reported once (the window is over) */
        }
        printf("[render64] %s per frame: %.1f;%s\n", pass ? "queries (round trips a renderer process answers or caches)" : "calls",
               (double)total / n, line);
    }
    uint64_t stale = 0;
    k = 0;
    for (int q = 0; q < RW_QUERY_COUNT; q++) {
        stale += R.snap_stale[q];
        if (R.snap_stale[q] && k < (int)sizeof line - 64)
            k += snprintf(line + k, sizeof line - (size_t)k, "%s %s %llu", k ? "," : "", rw_query_name[q] + 9, (unsigned long long)R.snap_stale[q]);
    }
    printf("[render64] queries answered from the snapshot: %.1f/frame; stale answers %llu%s%s\n", (double)R.snap_answers / n,
           (unsigned long long)stale, k ? ":" : "", k ? line : "");
    free(sorted);
    free(ms);
}
static void close_window(void) {
    if (helper_on) helper_window_end();   /* the window's last frames acknowledged: the recording has them all */
    R.in_window = 0;
    R.done = 1;
    if (R.loop_open <= 0) {   /* the loopback's stream goes on */
        R.w.enc = NULL;
        rr_snap_on = 0;
    }
    if (R.f) fclose(R.f);
    R.f = NULL;
    report();
}
static void loop_drain(void);
/* verify: the bytes of the frame's verification record (hash, pixels), not counted as traffic */
static void frame_done(size_t verify) {
    if (R.loop_open > 0) loop_drain();   /* the frame record reaches the decoder: its chunk ends there too */
    const uint64_t total = R.fr_bytes + R.w.n;
    if (R.in_window && R.nframes < R.count) {
        R.bytes[R.nframes] = total - verify;
        uint64_t counted = 0;
        for (int c = 1; c < RW_CATS; c++) {
            R.cat[R.nframes][c] = R.fr_cat[c] + R.w.cat[c];
            counted += R.cat[R.nframes][c];
        }
        R.cat[R.nframes][RW_CAT_ARGS] = total - verify - counted;
        R.ms[R.nframes] = (float)R.ser_ms;
        R.calls_ms[R.nframes] = (float)R.call_ms;
        R.ends_ms[R.nframes] = (float)R.end_ms;
        const double rate = tsc_per_ms();
        if (R.loop_open > 0 && rate > 0) {
            R.enc_ms[R.nframes] = (float)(R.fr_t_enc / rate);
            R.ms[R.nframes] = R.enc_ms[R.nframes];   /* the serialization line: the encoding's TSC time */
            R.ovh_ms[R.nframes] = (float)((double)(R.fr_t_drain - R.fr_t_render) / rate);
            R.syncs_fr[R.nframes] = (uint32_t)R.fr_syncs;
            R.hash_ms[R.nframes] = (float)((double)(rw_enc_stats(R.enc)->cache_hash_ticks - R.hash_ticks0) * rr_time_scale / rate);
            R.ds_ms[R.nframes] = (float)(R.fr_t_ds / rate);
            R.drains_fr[R.nframes] = (uint32_t)R.fr_drains;
            R.ring_ms[R.nframes] = (float)(R.fr_t_ring / rate);
            if (R.wait_ms) R.wait_ms[R.nframes] = (float)(helper_fr_wait / rate);
        }
        R.nframes++;
    }
    if (R.w.bad) printf("[render64] frame %u: out of memory, records lost\n", R.frame);
    if (R.loop_open <= 0) {
        if (R.path) put_file(R.w.buf, R.w.n);
        rw_reset(&R.w);
    }
    R.fr_bytes = 0;
    memset(R.fr_cat, 0, sizeof R.fr_cat);
    R.t_enc += R.fr_t_enc, R.t_drain += R.fr_t_drain, R.t_render += R.fr_t_render;
    R.fr_t_enc = R.fr_t_drain = R.fr_t_render = R.fr_syncs = R.fr_t_ds = R.fr_drains = R.fr_t_ring = 0;
    helper_fr_wait = 0;
    if (R.enc) R.hash_ticks0 = rw_enc_stats(R.enc)->cache_hash_ticks;
    rw_enc_frame(R.enc);   /* the next chunk starts after RW_REC_FRAME */
    R.ser_ms = R.call_ms = R.end_ms = 0;
    if (R.in_window && R.frame >= R.start + R.count - 1) close_window();
}

/* Whole payloads whose bytes were already sent this frame or the frame before (by content: a 64-bit
 * hash and the size), what a content cache in a renderer process (master plan 3.3 items 3-4) would
 * still keep from the stream. --renderer-traffic only; called by the encoder (rw_enc_on_whole). */
#define SEEN_BITS 15
typedef struct { uint64_t h; size_t n; } Content;
static Content content[2][1u << SEEN_BITS];   /* [cur] this frame, [!cur] the frame before */
static uint32_t content_used[2];
static int content_cur;
static uint64_t bytes_hash(const void *p, size_t n) {
    const uint8_t *b = p;
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, b + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h ^ (h >> 31);
}
static int content_find(int which, uint64_t h, size_t n, int add) {
    uint32_t i = (uint32_t)(h >> (64 - SEEN_BITS));
    for (;; i = (i + 1) & ((1u << SEEN_BITS) - 1)) {
        if (content[which][i].n == n && content[which][i].h == h) return 1;
        if (!content[which][i].n) break;
    }
    if (add && content_used[which] * 2 < (1u << SEEN_BITS)) {
        content[which][i].h = h;
        content[which][i].n = n;
        content_used[which]++;
    }
    return 0;
}
static void cached(const void *p, size_t n, int cat) {
    if (!R.traffic || !R.in_window || !p || !n) return;
    const int timed = R.loop_open <= 0;   /* a measurement: not serialization time */
    const double t0 = timed ? now_ms() : 0;
    const uint64_t t1 = timed || !rr_timed ? 0 : tsc();
    const uint64_t h = bytes_hash(p, n);
    const int here = content_find(content_cur, h, n, 1);
    if ((here || content_find(!content_cur, h, n, 0)) && R.nframes) R.cached_cat[cat] += n;
    if (timed) R.t0 += now_ms() - t0;
    else if (rr_timed) R.t_cur += tsc() - t1;
}
static void seen_clear(void) {
    content_cur = !content_cur;   /* this frame's set becomes the frame before's */
    if (content_used[content_cur]) memset(content[content_cur], 0, sizeof content[content_cur]);
    content_used[content_cur] = 0;
}

/* ---- the query snapshot (render_wire_gen.h rr_snap_take) ---- */
static void rr_snap_take(void);
static void rr_snap_answer(int q, int stale) {
    R.snap_answers++;
    if (stale) R.snap_stale[q]++;
}
static int snap_end_kind;   /* snap_refresh's caller: 0 renderer_init, 1 renderer_end, 2 renderer_present */
static void snap_refresh(void) {
    rr_snap_on = 0;
    if (!streaming()) return;
    if (helper_on) {   /* from the helper: its ack of an earlier frame, or (after renderer_init) a request */
        helper_snap(snap_end_kind != 0, snap_end_kind == 2);
        snap_end_kind = 0;
        return;
    }
    rr_snap_take();
    rr_snap_on = 1;
}

static size_t rw_base_bytes(int pitch_bytes);

/* ---- hooks of the generated wrappers ---- */
static void rr_pre_renderer_begin(const uint32_t *base, int pitch_bytes) {
    R.frame++;
    seen_clear();
    if (R.memmap_every && R.frame % (uint32_t)R.memmap_every == 0) {
        char why[32];
        snprintf(why, sizeof why, "frame %u", R.frame);
        mem_map_report(why, 1);
    }
    if ((R.path || R.traffic) && !R.done && !R.in_window && R.frame == R.start) open_window();
    if (streaming()) rw_enc_frame(R.enc);   /* a frame chunk starts (render_wire.h) */
    if (helper_on) helper_frame_start();
}
static void rr_post_renderer_begin(const uint32_t *base, int pitch_bytes) {
    if (!R.drop_pending) return;
    R.drop_pending = 0;
    renderer_set_texture_budget(R.budget);
}
/* The frame's hash and the generated frame's (--frame-gen; 0: none), once per renderer_end. */
static struct { uint32_t frame; int w, h; uint64_t hash, gen; } fh;
static void frame_hashes(int ret, const uint32_t *out, int pitch_bytes) {
    if (fh.frame == R.frame) return;
    fh.frame = R.frame;
    fh.w = fh.h = 0;
    if (out && helper_on) rr_rq_renderer_output_size(&fh.w, &fh.h);   /* the renderer is the helper's */
    else if (out) renderer_output_size(&fh.w, &fh.h);
    fh.hash = out && !ret ? rw_frame_hash(out, fh.w, fh.h, pitch_bytes) : 0;
    fh.gen = 0;
    static uint32_t *gen_px;
    static size_t gen_cap;
    if (fh.hash && (helper_on ? rr_rq_renderer_frame_gen_on() : renderer_frame_gen_on())) {
        const size_t need = (size_t)fh.w * fh.h;
        if (need > gen_cap) {
            free(gen_px);
            gen_px = malloc(need * 4);
            gen_cap = gen_px ? need : 0;
        }
        if (gen_px && (helper_on ? rr_rq_renderer_frame_gen_readback(gen_px, fh.w * 4) : renderer_frame_gen_readback(gen_px, fh.w * 4)))
            fh.gen = rw_frame_hash(gen_px, fh.w, fh.h, fh.w * 4);
    }
}
static void frame_record(int ret, const uint32_t *out, int pitch_bytes) {
    /* the loopback without a recording: the record only ends the chunk (no readback hash) */
    const int check = !R.loop_open || R.f || R.hashes_path;
    if (check) frame_hashes(ret, out, pitch_bytes);
    const int w = check ? fh.w : 0, h = check ? fh.h : 0;
    const uint64_t hash = check ? fh.hash : 0, gen = check ? fh.gen : 0;
    const int pixels = out && !ret && R.pixels_every > 0 && (R.frame - R.start) % (uint32_t)R.pixels_every == 0;
    R.w.no_refs = 1;   /* the verification record: whole */
    if (helper_on) helper_mark(HM_FRAME);   /* the helper's hash may go into it */
    rw_begin_record(&R.w, RW_REC_FRAME, 0);
    rw_u32(&R.w, R.frame);
    rw_i32(&R.w, ret);
    rw_i32(&R.w, w);
    rw_i32(&R.w, h);
    rw_u64(&R.w, hash);
    rw_u64(&R.w, gen);
    rw_u32(&R.w, (uint32_t)pixels);
    if (pixels)
        for (int y = 0; y < h; y++) rw_blob(&R.w, (const uint8_t *)out + (size_t)y * pitch_bytes, (size_t)w * 4, 1, RW_CAT_ARGS);
    rw_end_record(&R.w);
    if (helper_on) helper_mark(HM_FRAME_STOP);
    R.hashes += out && !ret;
    R.gen_hashes += gen != 0;
}
/* --renderer-hashes: "<renderer frame> <hash> <generated-frame hash>" per renderer_end with a readback. */
static void hashes_line(int ret, const uint32_t *out, int pitch_bytes) {
    if (!out || ret) return;
    if (!R.hashes_f) {
        R.hashes_f = fopen(R.hashes_path, "w");
        if (!R.hashes_f) {
            printf("[render64] %s: cannot write the frame hashes\n", R.hashes_path);
            R.hashes_path = NULL;
            return;
        }
    }
    frame_hashes(ret, out, pitch_bytes);
    fprintf(R.hashes_f, "%u %016llx %016llx\n", R.frame, (unsigned long long)fh.hash, (unsigned long long)fh.gen);
    R.hashes_frames++;
}
static void rr_post_renderer_end(uint32_t *out, int pitch_bytes, int ret) {
    if (R.hashes_path) hashes_line(ret, out, pitch_bytes);
    if (!streaming()) return;
    const size_t before = R.w.n;
    frame_record(ret, out, pitch_bytes);
    frame_done(R.w.n - before);
    snap_end_kind = 1;
    snap_refresh();
}
static void rr_post_renderer_present(int ret) {
    if (!streaming()) return;
    const size_t before = R.w.n;
    frame_record(ret, NULL, 0);
    frame_done(R.w.n - before);
    snap_end_kind = 2;
    snap_refresh();
}
static void rr_post_renderer_lamp_wall_map_region(const RLampWallMap *m, int x0, int y0, int x1, int y1, int ret) {
    /* the loaded map now holds these cells: the preamble's wall map is m as it is now */
    if (ret || !m) return;
    RwWriter s = {0};
    rw_begin_record(&s, RW_ID_LAMP_WALL_MAP, 0);
    rw_put_map(&s, (const RMaterialMap *)m);
    rw_end_record(&s);
    if (!s.bad) keep_copy(&R.sticky[RW_ID_LAMP_WALL_MAP], &R.sticky_n[RW_ID_LAMP_WALL_MAP], s.buf, s.n);
    rw_free(&s);
}
static void tm_clear(void);
static void rr_post_renderer_shutdown(void) {
    if (R.loop_open > 0) tm_clear();
    rr_snap_on = 0;
    if (R.loop_open > 0) loop_drain();
    if (R.in_window) close_window();
}
static void rr_post_renderer_configure(const RendererConfig *c) {
    if (R.loop_open > 0) tm_clear();
    rr_snap_on = 0;
}
static void rr_post_renderer_init(int w, int h, int ret) {
    if (R.loop_open > 0) tm_clear();
    R.init_w = w;
    R.init_h = h;
    snap_refresh();
}
static void rr_post_renderer_set_texture_budget(uint64_t bytes) { R.budget = bytes; }
/* renderer_begin's base image: rows of the back buffer (renderer_init's size) pitch bytes apart. */
static size_t rw_base_bytes(int pitch_bytes) {
    return R.init_h > 0 && pitch_bytes > 0 ? (size_t)pitch_bytes * (size_t)(R.init_h - 1) + (size_t)R.init_w * 4 : 0;
}

static void loop_exit(void);
void render_rec_exit(void) {
    if (!rr_active) return;
    if (helper_on) {   /* every frame acknowledged, the recording written, the report; r64_input.c closes the channel */
        helper_exit();
        if (R.in_window) close_window();
    } else {
        if (R.loop_open > 0) loop_drain();
        if (R.in_window) close_window();
        if (R.loop_open > 0) loop_exit();
    }
    if (R.hashes_f) {
        fclose(R.hashes_f);
        printf("[render64] frame hashes: %u frames in %s\n", R.hashes_frames, R.hashes_path);
        R.hashes_f = NULL;
    }
    if (R.memmap_every) mem_map_report("exit", 1);
}

/* ---- the loopback (--render-process loopback) ----
 * Every renderer.h call is encoded as in a recording (the stream state, the content cache, the draw-state
 * records, the query snapshot), written into render_ipc's command ring, read back from it and decoded by
 * the replay's generated decoder (rw_replay_call) into the real renderer, all in this process and on the
 * renderer's thread: the stream is drained when a call needs its result (rr_loop_result), before a query
 * or a LOCAL call (rr_loop_sync), at the end of every frame and whenever 256 KB are pending. Handles come
 * back as results (the same values as in-process), renderer_end reads back into the caller's buffer, the
 * configuration keeps its window callbacks. The content cache's arena is the channel's content region. */
#define LOOP_CHUNK (256u << 10)
#define LOOP_RING (8u << 20)
typedef struct RwReplay { int unused; } RwReplay;
static RwReplay loop_rp;
static uint64_t loop_t_call;
static int loop_draining;
/* The result fields (offsets in R.w) of the records with a result since the last drain, in stream order:
 * the decoder fills them in for the recording, in the same order (rw_result_differs), and checks a value
 * the game side already knew (RR_LOOP_KNOWN: the texture-handle mirror). */
static struct { uint32_t off; uint8_t known; } *loop_res;
static uint32_t loop_res_n, loop_res_cap, loop_res_next;
static int loop_res_kind = RR_LOOP_WAIT;   /* of the record being written (rr_loop_ask_*) */
static void rr_loop_result_at(RwWriter *w) {
    if (w != &R.w) return;
    if (helper_on) {   /* flags the record: a reply wanted, or the result not waited for */
        helper_result_at(w, loop_res_kind);
        loop_res_kind = RR_LOOP_WAIT;
        return;
    }
    if (loop_res_n == loop_res_cap) {
        const uint32_t cap = loop_res_cap ? loop_res_cap * 2 : 1024;
        void *b = realloc(loop_res, cap * sizeof *loop_res);
        if (!b) { w->bad = 1; return; }
        loop_res = b;
        loop_res_cap = cap;
    }
    loop_res[loop_res_n].off = (uint32_t)w->n;
    loop_res[loop_res_n++].known = loop_res_kind == RR_LOOP_KNOWN;
    loop_res_kind = RR_LOOP_WAIT;
}
static uint32_t serve_map(uint32_t v);
static void serve_map_set(uint32_t id, uint32_t handle);
static uint32_t rw_map_handle(RwReplay *rp, uint32_t v) { return serving ? serve_map(v) : v; }
static uint64_t serve_mispredicted;   /* the helper: results the game side knew and the renderer contradicts */
static int rw_result_differs(RwReplay *rp, uint32_t id, int handle, double want, double got) {
    R.loop_got = got;
    if (serving) {   /* the reply goes in serve_after (with the texture bytes a create added) */
        if (id == RW_ID_TEXTURE_CREATE && (uint32_t)want >= RR_TEX_ID_BASE) {   /* the game's own handle for it */
            serve_map_set((uint32_t)want, (uint32_t)got);
            return 0;
        }
        if (!(serve_flags & (RW_FLAG_REPLY | RW_FLAG_LATER)) && serve_map((uint32_t)want) != (uint32_t)got && serve_mispredicted++ < 10)
            r64_log("%s answered %.0f on the game side, the renderer says %.0f (renderer frame %u)", rw_id_name[id], want, got, R.dec_begins);
        return 0;
    }
    if (loop_res_next < loop_res_n) {
        const uint32_t off = loop_res[loop_res_next].off;
        if (loop_res[loop_res_next++].known && want != got && R.loop_mispredicted++ < 10)
            printf("[render64] loopback: %s answered %.0f on the game side, the renderer says %.0f (renderer frame %u)\n", rw_id_name[id], want,
                   got, R.frame);
        const uint32_t v = (uint32_t)(int64_t)got;
        if (off + 4 <= R.w.n) memcpy(R.w.buf + off, &v, 4);
    }
    return 0;
}
static void rw_replay_config(RwReplay *rp, RendererConfig *c, int had_surface) {
    if (serving) serve_config(c, had_surface);   /* the helper's own window (or none) */
    const RendererConfig *src = R.loop_cfg;   /* what the wire leaves out: this process's window */
    if (!src) return;
    c->instance_ext = src->instance_ext;
    c->instance_ext_count = src->instance_ext_count;
    c->create_surface = src->create_surface;
    c->drawable_size = src->drawable_size;
}
static int rw_replay_init(RwReplay *rp, int w, int h) { return renderer_init(w, h); }
static int serve_end(int pitch_bytes);
static int serve_present(void);
static int rw_replay_end(RwReplay *rp, int pitch_bytes) { return serving ? serve_end(pitch_bytes) : renderer_end(R.loop_out, pitch_bytes); }
static int rw_replay_present(RwReplay *rp) { return serving ? serve_present() : renderer_present(); }
static uint32_t loop_tex_n, loop_tex_drops;
static void rw_replay_before(RwReplay *rp, uint32_t id) {
    if (serving) serve_before(id);
    if (id == RW_ID_BEGIN) {   /* the texture-handle mirror: did this renderer_begin free textures? */
        loop_tex_n = renderer_texture_count();
        loop_tex_drops = renderer_texture_drop_count();
    }
    loop_t_call = tsc();
}
static void rw_replay_after(RwReplay *rp, uint32_t id, int handle, int differs) {
    R.fr_t_render += tsc() - loop_t_call;
    if (id == RW_ID_BEGIN) {
        R.dec_begins++;
        if (renderer_texture_count() < loop_tex_n || renderer_texture_drop_count() != loop_tex_drops) R.tex_lost = 1;
    }
    if (serving) serve_after(id);
}
#define RW_GEN_REPLAY
#include "render_wire_gen.h"
#undef RW_GEN_REPLAY

static void loop_fatal(const char *what, uint32_t id) {
    if (serving) printf("[r64] === FAULT (render64 helper: %s, record %u, renderer frame %u)\n", what, id, R.dec_begins);
    else printf("=== FAULT (render64 %s: %s, record %u, renderer frame %u)\n", helper_on ? "helper" : "loopback", what, id, R.frame);
    fflush(stdout);
    abort();
}
/* RW_REC_DRAW_STATE: each slot's setter, in slot order (render_wire.h). */
static int loop_draw_state(RwReader *r) {
    const uint32_t mask = rw_ru32(r);
    for (int s = 0; s < RW_DS_SLOTS && !r->bad; s++) {
        if (!(mask >> s & 1)) continue;
        size_t n = 0;
        const uint8_t *args = rw_rblob_plain(r, &n);
        if (!args) return -1;
        RwReader a;
        rw_reader(&a, args, n);
        if (rw_replay_call(&loop_rp, rw_draw_state_id(s), &a)) return -1;
    }
    return r->bad || mask >> RW_DS_SLOTS ? -1 : 0;
}
static void loop_record(const uint8_t *rec) {
    RwRecordHeader h;
    memcpy(&h, rec, sizeof h);
    if (h.id == RW_ID_CONFIGURE) {   /* the renderer keeps the configuration's pointers: a copy of its own */
        if (keep_bytes(&R.cfg_keep, &R.cfg_cap, rec, sizeof h + h.len)) loop_fatal("out of memory", h.id);
        rec = R.cfg_keep;
    }
    RwReader r;
    rw_reader(&r, rec + sizeof h, h.len);
    r.dec = R.dec;
    int bad;
    serve_flags = h.flags;
    if (serving && h.id == RW_REC_QUERY) {   /* the helper: a remote query (its reply on the BACK ring) */
        serve_query(&r);
        return;
    }
    if (h.id == RW_REC_FRAME) {   /* the chunk ends (the hash is the recording's business) */
        rw_dec_frame(R.dec);
        return;
    }
    if (h.id == RW_ID_BEGIN) rw_dec_frame(R.dec);   /* a chunk starts */
    if (h.id == RW_REC_DRAW_STATE) bad = loop_draw_state(&r);
    else bad = h.id >= RW_ID_COUNT || rw_replay_call(&loop_rp, h.id, &r);
    if (bad) loop_fatal("bad record", h.id);
}
/* Reads every complete record in the ring and decodes it (a record split by the ring's end, or not
 * fully written yet, is put together in asmb). */
static int ring_consume(R64Ipc *v, int block) {
    const uint32_t hn = (uint32_t)sizeof(RwRecordHeader);
    for (;;) {
        if (!block && !r64_readable(v, R64_RING_CMD)) return 0;
        const void *sp;
        uint32_t sn;
        const int rc = r64_peek(v, R64_RING_CMD, &sp, &sn);
        if (rc && block) return rc;   /* the helper: the game quit or is gone */
        if (rc) loop_fatal("ring", 0);
        const uint8_t *b = sp;
        if (!R.asm_n && sn >= hn) {
            RwRecordHeader h;
            memcpy(&h, b, sizeof h);
            if (sn - hn >= h.len) {   /* in place */
                loop_record(b);
                r64_consume(v, R64_RING_CMD, hn + h.len);
                continue;
            }
        }
        size_t want = hn - R.asm_n;
        if (R.asm_n >= hn) {
            RwRecordHeader h;
            memcpy(&h, R.asmb, sizeof h);
            want = hn + h.len - R.asm_n;
        }
        const uint32_t take = want < sn ? (uint32_t)want : sn;
        if (R.asm_n + take > R.asm_cap) {
            size_t cap = R.asm_cap ? R.asm_cap : 1u << 16;
            while (cap < R.asm_n + take) cap *= 2;
            uint8_t *nb = realloc(R.asmb, cap);
            if (!nb) loop_fatal("out of memory", 0);
            R.asmb = nb;
            R.asm_cap = cap;
        }
        memcpy(R.asmb + R.asm_n, b, take);
        R.asm_n += take;
        r64_consume(v, R64_RING_CMD, take);
        if (R.asm_n >= hn) {
            RwRecordHeader h;
            memcpy(&h, R.asmb, sizeof h);
            if (R.asm_n == hn + h.len) {
                R.asm_n = 0;
                loop_record(R.asmb);
            }
        }
    }
}
static void loop_consume(void) { ring_consume(R.lh, 0); }
/* Everything encoded so far: through the ring into the renderer. */
static void loop_drain(void) {
    if (helper_on) {   /* into the helper's CMD ring: nothing decodes here */
        helper_drain();
        return;
    }
    if (!R.w.n) return;
    if (loop_draining) loop_fatal("a renderer call while the stream is decoded", 0);
    loop_draining = 1;
    const uint64_t t0 = tsc();
    if (R.w.bad) loop_fatal("out of memory: records lost", 0);
    R.fr_bytes += R.w.n;
    for (int c = 0; c < RW_CATS; c++) R.fr_cat[c] += R.w.cat[c];
    R.loop_bytes += R.w.n;
    R.loop_drains++;
    R.fr_drains++;
    const uint8_t *p = R.w.buf;
    size_t n = R.w.n;
    while (n) {
        uint32_t m = r64_writable(R.lp, R64_RING_CMD);
        if (m > n) m = (uint32_t)n;
        const uint64_t tw = tsc();
        if (!m || r64_write(R.lp, R64_RING_CMD, p, m) || r64_flush(R.lp, R64_RING_CMD)) loop_fatal("ring", 0);
        R.fr_t_ring += tsc() - tw;   /* the game side's share of a drain in a two-process setup (the decoder is the helper's) */
        p += m, n -= m;
        loop_consume();
    }
    /* the recording is a tee of the ring, with the results the decoder returned (rw_result_differs) */
    if (loop_res_next != loop_res_n) loop_fatal("results out of step with the stream", 0);
    loop_res_n = loop_res_next = 0;
    if (R.f) put_file(R.w.buf, R.w.n);
    rw_reset(&R.w);
    rw_enc_writer_reset(R.enc);
    R.fr_t_drain += tsc() - t0;
    loop_draining = 0;
}
static int loop_init(void) {
    R.loop_open = -1;   /* tried once */
    if (helper_wanted) return helper_init();
    R64SpawnOpts o;
    memset(&o, 0, sizeof o);
    o.schema = RW_SCHEMA;
    o.cmd_bytes = LOOP_RING;
    char err[200] = "";
    if (r64_loopback(&o, &R.lp, &R.lh, err, sizeof err)) {
        printf("[render64] loopback: %s: the in-process renderer\n", err);
        return 0;
    }
    uint32_t an = 0;
    void *arena = r64_region(R.lh, R64_REG_CONTENT, &an);
    if (!R.enc) R.enc = rw_enc_new();
    R.dec = rw_dec_new();
    if (!R.enc || !R.dec) {
        printf("[render64] loopback: out of memory: the in-process renderer\n");
        r64_shutdown(R.lp, 0, NULL);
        return 0;
    }
    stream_cache(R.enc, an);
    rw_enc_stream(R.enc);
    R.w.enc = R.enc;
    rw_reset(&R.w);
    rw_dec_arena(R.dec, arena, an);
    rw_dec_copy_programs(R.dec, 1);
    rw_dec_stream(R.dec);
    loop_host = 1;
    tsc_per_ms();
    if (getenv("XB_LOOP_TIME_SAMPLE")) rr_time_mask = RR_TIME_EVERY - 1, rr_time_scale = RR_TIME_EVERY;
    R.loop_open = 1;
    if (R.path) {   /* the recording starts with the stream: renderer frames 1 to the window's last */
        if (R.start != 1) printf("[render64] loopback: the recording starts at the stream's start (frame 1), not %u\n", R.start);
        R.f = fopen(R.path, "wb");
        if (!R.f) printf("[render64] %s: cannot create the recording\n", R.path);
        RwFileHeader h;
        rw_file_header(&h);
        put_file(&h, sizeof h);
    }
    printf("[render64] loopback: every renderer call through the wire (ring %u MB, content cache %s, %u MB; encoding timed in 1 call of %u)\n",
           LOOP_RING >> 20, R.cache_off ? "off" : R.cache_check ? "on with the collision check" : "on", an >> 20, rr_time_scale);
    return 1;
}
static int rr_loop_on(void) {
    if (R.loop_open > 0) {
        if (loop_host) return 1;
        if (!R.other_thread++)
            printf(helper_on ? "[render64] helper: a renderer call from another thread: made in this process, which has no renderer running\n"
                             : "[render64] loopback: a renderer call from another thread: made directly\n");
        return 0;
    }
    if (R.loop_open < 0 || !loop_init()) {
        rr_loop = 0;
        return 0;
    }
    return 1;
}
static void rr_loop_sync(void) {
    if (R.loop_open <= 0 || !loop_host || loop_draining || helper_on) return;   /* the helper: queries are requests */
    R.loop_syncs[RW_ID_COUNT]++;
    R.fr_syncs++;
    loop_drain();
}
static void rr_loop_output(uint32_t *out) { R.loop_out = out; }
static void rr_loop_config(const RendererConfig *c) { R.loop_cfg = c; }
static double rr_loop_result(uint32_t id) {
    if (helper_on) return helper_result(id);
    R.loop_syncs[id]++;
    R.fr_syncs++;
    loop_drain();
    return R.loop_got;
}
static void rr_loop_after(uint32_t id) {
    loop_res_kind = RR_LOOP_WAIT;
    /* the configuration and shutdown at once (the caller's configuration may not outlive the call) */
    if (R.w.n >= LOOP_CHUNK || id == RW_ID_CONFIGURE || id == RW_ID_SHUTDOWN) loop_drain();
}
static double rr_loop_frame_time(void) {
    const double t = R.next_frame_time >= 0 ? R.next_frame_time : now_ms();
    R.next_frame_time = -1;
    return t;
}
static void rr_pre_renderer_set_frame_time(double t_ms) { R.next_frame_time = t_ms; }
/* ---- the texture-handle mirror ----
 * The game side keeps what the renderer's texture cache answers for each key it looked up or created, so
 * renderer_texture_find needs no round trip: the renderer's cache only changes in renderer_texture_create
 * (which still waits for its handle), in renderer_begin (least recently used textures freed, or the whole
 * cache dropped: the decoder sees it from the texture count and the drop count around the call, and the
 * mirror is then emptied) and in configure/init/shutdown (emptied too). A lookup marks the texture used in
 * the current frame (the renderer's LRU), so the first lookup of a key in a frame still travels, without
 * waiting (RR_LOOP_KNOWN: the decoder checks the handle); later lookups of it in the same frame, and lookups
 * of a key the renderer does not hold, have no effect and send nothing (RR_LOOP_SKIP). */
#define TM_BITS 17
typedef struct { uint64_t key; uint32_t handle, frame, epoch; } TexMirror;
static TexMirror *tm;
static uint32_t tm_epoch = 1, tm_n;
static int tm_off = -1;   /* -1 not decided yet; 1: every lookup waits (XB_LOOP_TEXTURE_WAIT, or calls from other threads) */
static void tm_clear(void) {
    tm_epoch++;
    tm_n = 0;
    R.tm_clears++;
}
static TexMirror *tm_slot(uint64_t key) {
    uint32_t i = (uint32_t)((key ^ key >> 29) * 0x9E3779B97F4A7C15ull >> (64 - TM_BITS));
    for (;; i = (i + 1) & ((1u << TM_BITS) - 1))
        if (tm[i].epoch != tm_epoch || tm[i].key == key) return &tm[i];
}
static int tm_usable(void) {
    if (tm_off < 0) {
        tm = calloc(1u << TM_BITS, sizeof *tm);
        tm_off = !tm || getenv("XB_LOOP_TEXTURE_WAIT") != NULL;
    }
    if (tm_off || R.other_thread) return 0;   /* another thread's direct create would change the cache behind the mirror */
    if (helper_on) return helper_tm_usable();
    if (R.tm_checked != R.frame) {   /* this frame's renderer_begin must be decoded first: did it free textures? */
        if (R.dec_begins < R.frame) {
            R.loop_syncs[RW_ID_BEGIN]++;
            R.fr_syncs++;
            loop_drain();
        }
        if (R.tex_lost) tm_clear();
        R.tex_lost = 0;
        R.tm_checked = R.frame;
    }
    return 1;
}
static void tm_set(uint64_t key, uint32_t handle) {
    if (tm_off) return;
    TexMirror *e = tm_slot(key);
    if (e->epoch != tm_epoch) {
        if (tm_n >= (1u << TM_BITS) / 2) {   /* full: start again */
            tm_clear();
            e = tm_slot(key);
        }
        tm_n++;
    }
    *e = (TexMirror){key, handle, R.frame, tm_epoch};
}
static int rr_loop_ask_renderer_texture_find(uint64_t key, uint32_t *ret) {
    if (!tm_usable()) return RR_LOOP_WAIT;
    TexMirror *e = tm_slot(key);
    if (e->epoch != tm_epoch) {
        R.tm_waits++;
        return RR_LOOP_WAIT;
    }
    *ret = e->handle;
    if (!e->handle || e->frame == R.frame) {
        R.tm_skipped++;
        return RR_LOOP_SKIP;
    }
    e->frame = R.frame;
    R.tm_known++;
    loop_res_kind = RR_LOOP_KNOWN;
    return RR_LOOP_KNOWN;
}
static void rr_loop_told_renderer_texture_find(uint64_t key, uint32_t ret) {
    if (tm_off == 0 && !R.other_thread) tm_set(key, ret);
}
static int rr_loop_ask_renderer_texture_create(uint64_t key, uint32_t format, uint32_t w, uint32_t h, uint32_t levels, const void *data,
                                               uint32_t *ret) {
    if (tm_off == 0 && !R.other_thread) tm_usable();   /* the mirror is this frame's before it learns the new handle */
    if (helper_on && !R.other_thread && !getenv("XB_HELPER_TEXTURE_WAIT")) {   /* a handle of the game's own: no round trip */
        *ret = helper_tex_id(rw_texture_bytes(format, w, h, levels));
        loop_res_kind = RR_LOOP_KNOWN;
        return RR_LOOP_KNOWN;
    }
    return RR_LOOP_WAIT;
}
static void rr_loop_told_renderer_texture_create(uint64_t key, uint32_t format, uint32_t w, uint32_t h, uint32_t levels, const void *data,
                                                 uint32_t ret) {
    rr_loop_told_renderer_texture_find(key, ret);
}
/* GPU-skinned draws: the result (the draw's first vertex) is only read by XB_GSK_CHECK's frame check, so
 * the draw does not wait for it (the caller gets -1: "nothing to check"); with XB_GSK_CHECK it waits. */
static int skin_wait = -1;
static int skin_ask(int32_t *ret) {
    if (skin_wait < 0) skin_wait = getenv("XB_GSK_CHECK") != NULL;
    if (skin_wait) return RR_LOOP_WAIT;
    *ret = -1;
    R.skin_later++;
    loop_res_kind = RR_LOOP_LATER;   /* the helper flags the record (RW_FLAG_LATER) */
    return RR_LOOP_LATER;
}
static int rr_loop_ask_renderer_draw_skinned(const RViewport *vp, uint32_t flags, const RSkinJob *job, const uint32_t (*ins)[4],
                                             const float (*consts)[4], const uint8_t *data, uint32_t data_bytes, const uint32_t *idx,
                                             uint32_t ni, const RSkinNormals *nrm, int32_t *ret) {
    return skin_ask(ret);
}
static void rr_loop_told_renderer_draw_skinned(const RViewport *vp, uint32_t flags, const RSkinJob *job, const uint32_t (*ins)[4],
                                               const float (*consts)[4], const uint8_t *data, uint32_t data_bytes, const uint32_t *idx,
                                               uint32_t ni, const RSkinNormals *nrm, int32_t ret) {}
static int rr_loop_ask_renderer_draw_skinned_world_lit(const RViewport *vp, uint32_t flags, const RSkinJob *job, const uint32_t (*ins)[4],
                                                       const float (*consts)[4], const uint8_t *data, uint32_t data_bytes,
                                                       const uint32_t *idx, uint32_t ni, const float *normals, uint32_t lights, int32_t *ret) {
    return skin_ask(ret);
}
static void rr_loop_told_renderer_draw_skinned_world_lit(const RViewport *vp, uint32_t flags, const RSkinJob *job, const uint32_t (*ins)[4],
                                                         const float (*consts)[4], const uint8_t *data, uint32_t data_bytes,
                                                         const uint32_t *idx, uint32_t ni, const float *normals, uint32_t lights,
                                                         int32_t ret) {}

/* ---- hash-ahead (render_rec.h render_rec_prehash) ----
 * The caller queues the ranges it will pass as payloads in this frame (from data it no longer changes); a
 * worker thread hashes them in queue order while the renderer thread works through the frame, and the
 * encoder takes a finished hash (rw_enc_hash_hook) instead of reading the bytes itself. A range the
 * worker has not reached yet is hashed by the encoder as before. The worker starts once the first job (the
 * base image, needed at once by renderer_begin) is queued and takes the jobs queued after it in order until
 * the queue is closed (render_rec_prehash_start: waking the worker after the
 * whole queue cost the encoder ~0.12 ms/frame of waiting for the base image). The renderer thread appends a
 * job, then publishes it by advancing ph.njob (release); the worker writes a job's result, then publishes it
 * by advancing ph.done (release; the encoder reads it with acquire). */
#define PH_JOBS (1u << 14)
#define PH_CTAB 2048u   /* constants blocks per frame at most half of it (16 KB: no cache misses) */
/* A job hashes n bytes at p (h: the hash), or (n == PH_CONSTS) compares the skinning-constant block p with
 * the block queued before it, prev (mask: the registers that differ; rw_enc_consts_hook). */
#define PH_CONSTS 1u
typedef struct { const void *p, *prev; uint32_t n; uint64_t h; uint32_t mask[6]; } PhJob;
static struct {
    PhJob *job;
    uint32_t njob, cap;
    struct { const void *p; uint32_t job, gen; } ctab[PH_CTAB];   /* constants block -> its job, this frame's (cgen) */
    uint32_t cgen, nconsts;
    uint32_t done, cancel, busy, go, closed;
    int state;   /* 0 not started, 1 running, -1 off */
    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint64_t queued, used;   /* bytes queued, bytes whose hash the encoder took */
    const void *consts_last; /* the last skinning-constant block queued this frame */
    uint64_t consts_used;    /* register deltas the encoder took */
} ph;
/* The registers of the 192 x 16-byte block cur that differ from prev's (as rw_put_skin_consts compares them). */
static void consts_delta(const void *cur, const void *prev, uint32_t mask[6]) {
    memset(mask, 0, 6 * sizeof *mask);
    if (cur == prev) return;
    const uint8_t *a = cur, *b = prev;
    for (uint32_t i = 0; i < 192; i++)
        if (memcmp(a + i * 16, b + i * 16, 16)) mask[i >> 5] |= 1u << (i & 31);
}
static void *ph_worker(void *arg) {
    pthread_mutex_lock(&ph.mu);
    for (;;) {
        while (!ph.go) pthread_cond_wait(&ph.cv, &ph.mu);
        ph.go = 0;
        pthread_mutex_unlock(&ph.mu);
        for (uint32_t i = 0; !__atomic_load_n(&ph.cancel, __ATOMIC_ACQUIRE);) {
            if (i == __atomic_load_n(&ph.njob, __ATOMIC_ACQUIRE)) {   /* caught up: more to come unless closed */
                if (__atomic_load_n(&ph.closed, __ATOMIC_ACQUIRE) && i == __atomic_load_n(&ph.njob, __ATOMIC_ACQUIRE)) break;
                __builtin_ia32_pause();
                continue;
            }
            PhJob *j = &ph.job[i];
            if (j->n == PH_CONSTS) {
                if (j->prev) consts_delta(j->p, j->prev, j->mask);   /* the frame's first block has no delta */
            }
            else j->h = rw_hash64(j->p, j->n);
            __atomic_store_n(&ph.done, ++i, __ATOMIC_RELEASE);
        }
        pthread_mutex_lock(&ph.mu);
        ph.busy = 0;
        pthread_cond_broadcast(&ph.cv);
    }
    return arg;
}
/* The encoder takes the payloads in about the order they were queued: the job after the last one taken, else
 * one of the next PH_AHEAD (a payload the caller did not queue, or queued far from where it is used, is
 * hashed by the encoder). No table: a lookup reads the job list in order (a
 * (pointer, size) table cost a cache miss per payload, ~0.2 ms/frame). */
#define PH_AHEAD 16u
static uint32_t ph_next;   /* the job after the last payload taken */
static int ph_known(const void *p, size_t n, uint64_t *h) {
    if (!ph.njob || n > UINT32_MAX) return 0;
    uint32_t k = ph_next;
    const uint32_t end = k + PH_AHEAD < ph.njob ? k + PH_AHEAD : ph.njob;
    while (k < end && (ph.job[k].p != p || ph.job[k].n != n)) k++;
    if (k == end) return 0;
    uint32_t done = __atomic_load_n(&ph.done, __ATOMIC_ACQUIRE);
    /* the first job (renderer_begin's image, render_rec_prehash_base) is needed at once: waiting for the worker
     * costs less than comparing the image's bytes here */
    if (!k && !done && ph.busy) {   /* at most ~0.15 ms (a worker without a core then is not waited for) */
        const double rate = tsc_per_ms();
        const uint64_t until = tsc() + (uint64_t)(rate > 0 ? rate * 0.15 : 300000);
        while (!(done = __atomic_load_n(&ph.done, __ATOMIC_ACQUIRE)) && tsc() < until) __builtin_ia32_pause();
    }
    if (k >= done) return 0;
    ph_next = k + 1;
    *h = ph.job[k].h;
    ph.used += n;
    /* the next payloads' cache lookups: their bucket, then their entry, ahead of time (queue order ~ draw order) */
    if (k + 4 < done && ph.job[k + 4].n != PH_CONSTS) rw_enc_cache_prefetch(R.enc, ph.job[k + 4].h, 1);
    if (k + 10 < done && ph.job[k + 10].n != PH_CONSTS) rw_enc_cache_prefetch(R.enc, ph.job[k + 10].h, 0);
    return 1;
}
/* rw_enc_consts_hook: the worker's delta between exactly these two blocks (a small table by block: the
 * encoder may take them out of queue order, e.g. a block drawn again in another pass); a queued block passed
 * again right after itself has no delta. */
static uint32_t ph_cslot(const void *p) {
    uint32_t i = (uint32_t)((uintptr_t)p >> 4) * 2654435761u >> 21;   /* 11 bits: PH_CTAB */
    for (;; i = (i + 1) & (PH_CTAB - 1))
        if (ph.ctab[i].gen != ph.cgen || ph.ctab[i].p == p) return i;
}
static int ph_consts(const void *cur, const void *prev, uint32_t mask[6]) {
    if (!ph.njob) return 0;
    const uint32_t i = ph_cslot(cur);
    if (ph.ctab[i].gen != ph.cgen) return 0;   /* not a queued block: its bytes may have changed */
    if (cur == prev) {   /* the block the reader has: no delta */
        memset(mask, 0, 6 * sizeof *mask);
        ph.consts_used++;
        return 1;
    }
    const uint32_t k = ph.ctab[i].job;
    if (ph.job[k].prev != prev || k >= __atomic_load_n(&ph.done, __ATOMIC_ACQUIRE)) return 0;
    memcpy(mask, ph.job[k].mask, 6 * sizeof *mask);
    ph.consts_used++;
    return 1;
}
int render_rec_prehash_on(void) {
    if (R.loop_open <= 0 || !loop_host || R.cache_off || ph.state < 0) return 0;
    if (!ph.state) {
        ph.job = malloc(PH_JOBS * sizeof *ph.job);
        ph.cap = PH_JOBS;
        ph.cgen = 1;
        pthread_mutex_init(&ph.mu, NULL);
        pthread_cond_init(&ph.cv, NULL);
        if (!ph.job || getenv("XB_LOOP_NO_PREHASH") || pthread_create(&ph.th, NULL, ph_worker, NULL)) {
            ph.state = -1;
            return 0;
        }
        ph.state = 1;
        rw_enc_hash_hook(R.enc, ph_known);
        rw_enc_consts_hook(R.enc, ph_consts);
    }
    return 1;
}
void render_rec_prehash(const void *p, size_t n) {
    if (ph.state <= 0 || ph.closed || !p || n < RW_CACHE_MIN || n > UINT32_MAX || ph.njob == ph.cap) return;
    ph.job[ph.njob] = (PhJob){p, NULL, (uint32_t)n, 0, {0}};
    __atomic_store_n(&ph.njob, ph.njob + 1, __ATOMIC_RELEASE);
    ph.queued += n;
}
void render_rec_prehash_consts(const void *cur) {
    if (ph.state <= 0 || ph.closed || !cur || ph.njob == ph.cap) return;
    if (ph.nconsts == PH_CTAB / 2) return;
    const uint32_t i = ph_cslot(cur);
    if (ph.ctab[i].gen == ph.cgen) return;   /* queued already (the same block again: no delta from itself) */
    ph.ctab[i].p = cur;
    ph.ctab[i].job = ph.njob;
    ph.ctab[i].gen = ph.cgen;
    ph.nconsts++;
    ph.job[ph.njob] = (PhJob){cur, ph.consts_last, PH_CONSTS, 0, {0}};
    __atomic_store_n(&ph.njob, ph.njob + 1, __ATOMIC_RELEASE);
    ph.consts_last = cur;
}
static void ph_wake(void) {
    if (ph.state <= 0 || ph.busy || ph.closed) return;
    pthread_mutex_lock(&ph.mu);
    ph.busy = ph.go = 1;
    pthread_cond_broadcast(&ph.cv);
    pthread_mutex_unlock(&ph.mu);
}
void render_rec_prehash_base(const uint32_t *base, int pitch_bytes) {
    if (base) render_rec_prehash(base, rw_base_bytes(pitch_bytes));
    ph_wake();   /* the worker starts on it while the rest is queued */
}
void render_rec_prehash_start(void) {
    if (ph.state <= 0) return;
    ph_wake();   /* (no base image queued) */
    __atomic_store_n(&ph.closed, 1, __ATOMIC_RELEASE);
}
void render_rec_prehash_end(void) {
    if (ph.state <= 0) return;
    __atomic_store_n(&ph.cancel, 1, __ATOMIC_RELEASE);
    pthread_mutex_lock(&ph.mu);
    while (ph.busy) pthread_cond_wait(&ph.cv, &ph.mu);
    pthread_mutex_unlock(&ph.mu);
    ph.cancel = ph.closed = 0;
    ph.done = ph.njob = ph_next = ph.nconsts = 0;
    ph.consts_last = NULL;
    if (++ph.cgen == 0) {   /* wrapped: every table entry is stale */
        memset(ph.ctab, 0, sizeof ph.ctab);
        ph.cgen = 1;
    }
}
int render_rec_result_later(uint32_t id) {
    if (R.loop_open <= 0 || (id != RW_ID_DRAW_SKINNED && id != RW_ID_DRAW_SKINNED_WORLD_LIT)) return 0;
    if (skin_wait < 0) skin_wait = getenv("XB_GSK_CHECK") != NULL;
    return !skin_wait;
}
static void loop_exit(void) {
    const double rate = tsc_per_ms();
    const uint32_t n = R.frame ? R.frame : 1;
    uint64_t sync = 0;
    for (int i = 0; i <= RW_ID_COUNT; i++) sync += R.loop_syncs[i];
    printf("[render64] loopback: %u renderer frames, %.1f MB through the ring (%.2f MB/frame), %llu drains, %.1f waits/frame for the decoder\n",
           R.frame, R.loop_bytes / 1048576.0, R.loop_bytes / 1048576.0 / n, (unsigned long long)R.loop_drains, (double)sync / n);
    char line[1024];
    int k = 0;
    for (int shown = 0; shown < 8; shown++) {
        int best = -1;
        for (int i = 0; i <= RW_ID_COUNT; i++)
            if (R.loop_syncs[i] && (best < 0 || R.loop_syncs[i] > R.loop_syncs[best])) best = i;
        if (best < 0) break;
        k += snprintf(line + k, sizeof line - (size_t)k, "%s %s %.1f", shown ? "," : "", best == RW_ID_COUNT ? "queries/local" : rw_id_name[best] + 9,
                      (double)R.loop_syncs[best] / n);
        R.loop_syncs[best] = 0;
    }
    printf("[render64] loopback waits per frame:%s\n", line);
    printf("[render64] loopback texture lookups: %.1f/frame known on the game side (sent, not waited for), %.1f/frame without a record, "
           "%.1f/frame waited (key not seen yet); mirror emptied %llu times; %llu answers the renderer contradicted; "
           "%.1f skinned draws/frame not waited for\n", (double)R.tm_known / n, (double)R.tm_skipped / n, (double)R.tm_waits / n,
           (unsigned long long)R.tm_clears, (unsigned long long)R.loop_mispredicted, (double)R.skin_later / n);
    if (rate > 0)
        printf("[render64] loopback cost (renderer thread, whole run): encoding %.3f ms/frame, ring + decoding %.3f ms/frame (renderer calls %.3f ms/frame apart)\n",
               R.t_enc / rate / n, (double)(R.t_drain - R.t_render) / rate / n, R.t_render / rate / n);
    const RwEncStats *st = rw_enc_stats(R.enc);
    if (!R.cache_off) {
        double held = 0;
        for (int c = 0; c < RW_CATS; c++) held += (double)st->cache_hit_bytes[c];
        printf("[render64] content cache (whole run): %.2f MB/frame held by the reader, %.2f MB/frame new, %.0f moves/frame, %llu evicted, %llu sent whole "
               "(budget), %llu collisions\n", held / 1048576.0 / n, st->cache_new_bytes / 1048576.0 / n, (double)st->cache_moves / n,
               (unsigned long long)st->cache_evicted, (unsigned long long)st->cache_refused, (unsigned long long)st->cache_collisions);
    }
    if (R.other_thread) printf("[render64] loopback: %llu renderer calls from other threads (made directly)\n", (unsigned long long)R.other_thread);
    if (ph.state > 0)
        printf("[render64] hash-ahead worker: %.2f MB/frame queued, %.2f MB/frame of payload hashes taken from it, %.2f MB/frame hashed by the encoder\n",
               ph.queued / 1048576.0 / n, ph.used / 1048576.0 / n, st->cache_hashed / 1048576.0 / n);
    if (ph.state > 0)
        printf("[render64] skinning constants: %.1f draws/frame with the register delta from the worker, %.1f compared here; %llu deltas contradicted (check mode)\n",
               (double)st->consts_delta_known / n, (double)(st->consts_draws - st->consts_delta_known) / n, (unsigned long long)st->consts_delta_wrong);
    if (R.f) {
        fclose(R.f);
        R.f = NULL;
    }
    r64_shutdown(R.lp, 0, NULL);
    R.lp = R.lh = NULL;
    R.loop_open = 0;
    rr_loop = 0;
}

#define RW_GEN_WRAP
#include "render_wire_gen.h"

/* ==== --render-process helper (W6; the overview is with the declarations at the top) ==== */

/* ---- both sides ---- */
static void *rr_rq_scratch(int k, size_t bytes) {   /* the helper: a remote query's output buffers */
    static void *buf[8];
    static size_t cap[8];
    static uint64_t none[8];
    if (k < 0 || k >= 8) loop_fatal("a query with too many outputs", RW_REC_QUERY);
    if (!bytes) return none;
    if (bytes > cap[k]) {
        void *b = realloc(buf[k], bytes);
        if (!b) loop_fatal("out of memory", RW_REC_QUERY);
        buf[k] = b;
        cap[k] = bytes;
    }
    memset(buf[k], 0, bytes);
    return buf[k];
}

/* ---- the game side ---- */
#define H_ACKS 16u
#define ACK_TEX_LOST 1u   /* the frame's renderer_begin freed textures (or dropped the cache) */
#define ACK_HASHED 2u     /* presented headless: read back and hashed by the helper */
typedef struct {
    uint32_t frame, flags, tex_count, nmiss, nlate;
    int32_t ret, w, h;
    uint64_t hash, gen, tex_bytes, present_ns;
    float srv_ms;
    uint8_t *buf;    /* the ack's payload as it came */
    size_t n, cap, snap_off, miss_off;
    struct { uint32_t qi; size_t off, n; } late[RW_LATE_COUNT];
} HAck;
static struct {
    R64Ipc *x;
    int dead, closed, rq_open, tri_warned;
    uint8_t *bk;   /* the last message read from the BACK ring */
    size_t bk_n, bk_cap;
    uint32_t bk_type;
    HAck ack[H_ACKS];
    uint32_t acked, used, ended;   /* the last frame acknowledged, the one answering queries, frames ended (sent) */
    int32_t present_ret;
    uint64_t created[H_ACKS];      /* texture bytes the helper's renderer added, per frame (frame % H_ACKS) */
    uint32_t creates[H_ACKS];
    uint32_t create_failed, tex_synced;
    uint64_t rq_n[RQ_ALL], rq_late[RW_QUERY_COUNT];
    uint8_t *cst[RW_QUERY_COUNT];  /* CONST queries' first replies */
    size_t cst_n[RW_QUERY_COUNT];
    char *str[RW_QUERY_COUNT];     /* string results (the last one per query) */
    size_t rows;                   /* the helper renderer's output height */
    uint64_t present_call[H_ACKS]; /* when the game sent frame N's renderer_present */
    /* the recording: a tee of the CMD stream, written once each frame's ack is in (its hash, present result and
     * pipeline misses are the helper's) */
    uint8_t *tee;
    size_t tee_n, tee_cap;
    uint64_t tee_base;             /* stream offset of tee[0] */
    struct { uint64_t end, res, rec, stop; int has_end, has_res, set; } tf[H_ACKS];
    int64_t mark_end, mark_rec, mark_stop, mark_res;   /* offsets in R.w of the frame's records (-1 none) */
    uint64_t *pend;                /* stream offsets of the result fields waiting for a reply (the tee patches them) */
    uint32_t pend_n, pend_cap, pend_head;
    /* statistics */
    uint64_t wait_id[RW_ID_COUNT + 1];   /* reply wait ticks by the call that waited ([RW_ID_COUNT]: remote queries) */
    uint64_t replies, reply_ticks, ack_waits, ack_ticks, tex_syncs, pressure_frames, late_losses, contradicted, tb_stale, tb_asked;
    float *delay, *ackw;           /* per frame: game's present call -> helper's present returned; the ack wait (ms) */
    uint32_t ndelay, nackw;
    double srv_ms_sum;
    uint32_t srv_n;
} H = {.mark_end = -1, .mark_rec = -1, .mark_stop = -1, .mark_res = -1};
#define H_SAMPLES (1u << 16)

static uint64_t rr_rq_renderer_texture_bytes(void);

static void helper_dead(void) {
    if (H.dead) return;
    H.dead = 1;
    char st[32];
    if (!__atomic_exchange_n(&r64_fault_printed, 1, __ATOMIC_ACQ_REL)) {
        printf("=== FAULT (render64 helper exited: %s)\n", r64_peer_exit(H.x, st, sizeof st));
        fflush(stdout);
    }
}
static void tee_put(const void *p, size_t n) {
    if (H.tee_n + n > H.tee_cap) {
        size_t cap = H.tee_cap ? H.tee_cap : 1u << 20;
        while (cap < H.tee_n + n) cap *= 2;
        uint8_t *b = realloc(H.tee, cap);
        if (!b) loop_fatal("out of memory: the recording", 0);
        H.tee = b;
        H.tee_cap = cap;
    }
    memcpy(H.tee + H.tee_n, p, n);
    H.tee_n += n;
}
static void tee_patch(uint64_t at, const void *v, size_t n) {
    if (at >= H.tee_base && at + n <= H.tee_base + H.tee_n) memcpy(H.tee + (at - H.tee_base), v, n);
}
static void tee_write_to(uint64_t to) {   /* writes the tee up to stream offset `to` and drops it */
    if (to <= H.tee_base) return;
    const size_t n = (size_t)(to - H.tee_base);
    put_file(H.tee, n);
    memmove(H.tee, H.tee + n, H.tee_n - n);
    H.tee_n -= n;
    H.tee_base = to;
}
/* Frame a->frame's ack: its present result, hash and pipeline misses into the recording, then it is written. */
static void tee_frame_done(const HAck *a) {
    typeof(H.tf[0]) *t = &H.tf[a->frame % H_ACKS];
    if (!t->set) return;
    t->set = 0;
    if (t->has_res) tee_patch(t->res, &a->ret, 4);
    if (a->hash) {   /* the game read nothing back: the frame record gets the helper's size and hashes */
        tee_patch(t->rec + 8 + 4, &a->ret, 4);
        tee_patch(t->rec + 8 + 8, &a->w, 4);
        tee_patch(t->rec + 8 + 12, &a->h, 4);
        tee_patch(t->rec + 8 + 16, &a->hash, 8);
        tee_patch(t->rec + 8 + 24, &a->gen, 8);
    }
    {   /* before the frame's renderer_end / renderer_present, as an in-process recording has it (also when empty: the
         * replay then builds every pipeline before drawing, as the helper had them) */
        tee_write_to(t->has_end ? t->end : t->rec);
        RwWriter m = {0};
        rw_begin_record(&m, RW_REC_PIPE_PLAN, 0);
        rw_u32(&m, a->nmiss);
        rw_bytes(&m, a->buf + a->miss_off, (size_t)a->nmiss * 4);
        rw_end_record(&m);
        put_file(m.buf, m.n);
        rw_free(&m);
        R.pipe_misses += a->nmiss;
        R.pipe_frames++;
    }
    tee_write_to(t->stop);
}
static void ack_take(void) {
    RwReader r;
    rw_reader(&r, H.bk, H.bk_n);
    const uint32_t frame = rw_ru32(&r);
    HAck *a = &H.ack[frame % H_ACKS];
    if (H.bk_n > a->cap) {
        uint8_t *b = realloc(a->buf, H.bk_n);
        if (!b) loop_fatal("out of memory", 0);
        a->buf = b;
        a->cap = H.bk_n;
    }
    memcpy(a->buf, H.bk, H.bk_n);
    a->n = H.bk_n;
    rw_reader(&r, a->buf, a->n);
    a->frame = rw_ru32(&r);
    a->ret = rw_ri32(&r);
    a->flags = rw_ru32(&r);
    a->w = rw_ri32(&r);
    a->h = rw_ri32(&r);
    a->tex_count = rw_ru32(&r);
    a->hash = rw_ru64(&r);
    a->gen = rw_ru64(&r);
    a->tex_bytes = rw_ru64(&r);
    a->present_ns = rw_ru64(&r);
    const uint32_t contradicted = rw_ru32(&r);
    a->srv_ms = rw_rf32(&r);
    size_t n = 0;
    const uint8_t *p = rw_rblob_plain(&r, &n);
    a->snap_off = p && n == sizeof rr_snap ? (size_t)(p - a->buf) : 0;
    a->nlate = 0;
    for (uint32_t i = 0; i < RW_LATE_COUNT && !r.bad; i++) {
        const uint32_t qi = rw_ru32(&r);
        if (qi == RW_NULL) continue;
        p = rw_rblob_plain(&r, &n);
        if (!p || qi >= RW_QUERY_COUNT) continue;
        a->late[a->nlate].qi = qi;
        a->late[a->nlate].off = (size_t)(p - a->buf);
        a->late[a->nlate++].n = n;
    }
    a->nmiss = rw_ru32(&r);
    a->miss_off = (size_t)(r.p - a->buf);
    if (r.bad || (size_t)(a->buf + a->n - r.p) < (size_t)a->nmiss * 4) loop_fatal("a short ack from the helper", 0);
    if (a->frame != H.acked + 1) printf("[render64] helper: ack of frame %u after %u\n", a->frame, H.acked);
    H.acked = a->frame;
    if (contradicted > H.contradicted) H.contradicted = contradicted;
    if (a->flags & ACK_TEX_LOST && a->frame > H.tex_synced) {   /* not foreseen: the mirror is emptied late */
        tm_clear();
        H.late_losses++;
    }
    const uint64_t call = H.present_call[a->frame % H_ACKS];
    if (call && a->present_ns > call && H.delay && H.ndelay < H_SAMPLES) H.delay[H.ndelay++] = (float)((double)(a->present_ns - call) / 1e6);
    H.present_call[a->frame % H_ACKS] = 0;
    H.srv_ms_sum += a->srv_ms;
    H.srv_n++;
    if (R.f) tee_frame_done(a);
    if (R.hashes_path && a->flags & ACK_HASHED) {   /* the helper's hashes of the frames it presented headless */
        if (!R.hashes_f && !(R.hashes_f = fopen(R.hashes_path, "w"))) {
            printf("[render64] %s: cannot write the frame hashes\n", R.hashes_path);
            R.hashes_path = NULL;
        } else {
            fprintf(R.hashes_f, "%u %016llx %016llx\n", a->frame, (unsigned long long)a->hash, (unsigned long long)a->gen);
            R.hashes_frames++;
        }
    }
}
/* One message from the BACK ring into H.bk (acks are taken at once). 0, or -1 once the helper is gone. */
static int back_next(void) {
    if (H.dead) return -1;
    R64Msg m;
    if (r64_read(H.x, R64_RING_BACK, &m, sizeof m)) { helper_dead(); return -1; }
    if (m.len > H.bk_cap) {
        uint8_t *b = realloc(H.bk, m.len);
        if (!b) loop_fatal("out of memory", 0);
        H.bk = b;
        H.bk_cap = m.len;
    }
    if (m.len && r64_read(H.x, R64_RING_BACK, H.bk, m.len)) { helper_dead(); return -1; }
    H.bk_type = m.type;
    H.bk_n = m.len;
    if (m.type == R64M_ACK) ack_take();
    return 0;
}
static int back_until(uint32_t type) {
    for (;;) {
        if (back_next()) return -1;
        if (H.bk_type == type) return 0;
    }
}
static void back_poll(void) {
    while (!H.dead && r64_readable(H.x, R64_RING_BACK) >= sizeof(R64Msg)) back_next();
}

static int helper_init(void) {
    R64Ipc *x = r64_shared_channel;
    if (!x) {
        printf("[render64] helper: the helper did not start (see the [render64] helper-io line): the in-process renderer\n");
        return 0;
    }
    uint32_t an = 0, cn = 0;
    r64_region(x, R64_REG_CONTENT, &an);
    r64_region(x, R64_REG_CMD, &cn);
    if (!R.enc) R.enc = rw_enc_new();
    if (!R.enc) {
        printf("[render64] helper: out of memory: the in-process renderer\n");
        return 0;
    }
    stream_cache(R.enc, an);
    rw_enc_stream(R.enc);
    R.w.enc = R.enc;
    rw_reset(&R.w);
    H.x = x;
    R.lp = x;
    helper_on = rr_remote = 1;
    loop_host = 1;
    tsc_per_ms();
    if (getenv("XB_LOOP_TIME_SAMPLE")) rr_time_mask = RR_TIME_EVERY - 1, rr_time_scale = RR_TIME_EVERY;
    R.loop_open = 1;
    H.delay = calloc(H_SAMPLES, sizeof *H.delay);
    H.ackw = calloc(H_SAMPLES, sizeof *H.ackw);
    if (R.path) {   /* the recording starts with the stream: renderer frames 1 to the window's last */
        if (R.start != 1) printf("[render64] helper: the recording starts at the stream's start (frame 1), not %u\n", R.start);
        R.f = fopen(R.path, "wb");
        if (!R.f) printf("[render64] %s: cannot create the recording\n", R.path);
        RwFileHeader h;
        rw_file_header(&h);
        put_file(&h, sizeof h);
        H.tee_base = 0;
    }
    printf("[render64] helper: every renderer call goes to the x86_64 helper (pid %ld): CMD ring %u MB, content cache %s (%u MB), "
           "%d frame%s in flight\n", r64_peer_pid(x), cn >> 20, R.cache_off ? "off" : R.cache_check ? "on with the collision check" : "on", an >> 20,
           helper_frames, helper_frames == 1 ? "" : "s");
    return 1;
}
static uint32_t helper_next_tex = RR_TEX_ID_BASE;
static uint32_t helper_tex_id(size_t bytes) {
    /* the device bytes come with the ack; until then an over-estimate (alignment, the renderer's own rounding) */
    H.created[R.frame % H_ACKS] += bytes + bytes / 4 + 4096;
    H.creates[R.frame % H_ACKS]++;
    if (helper_next_tex == 0xFFFFFFFFu) helper_next_tex = RR_TEX_ID_BASE;   /* 4 billion creates later */
    return helper_next_tex++;
}
static void helper_frame_start(void) {
    H.created[R.frame % H_ACKS] = 0;
    H.creates[R.frame % H_ACKS] = 0;
    H.tf[R.frame % H_ACKS].set = H.tf[R.frame % H_ACKS].has_end = H.tf[R.frame % H_ACKS].has_res = 0;
}
static void helper_mark(int what) {
    if (!R.f) return;
    if (what == HM_END) H.mark_end = (int64_t)R.w.n;
    else if (what == HM_FRAME) H.mark_rec = (int64_t)R.w.n;
    else H.mark_stop = (int64_t)R.w.n;
}
static void helper_result_at(RwWriter *w, int kind) {
    RwRecordHeader h;
    memcpy(&h, w->buf + w->rec, sizeof h);
    if (h.id == RW_ID_PRESENT) {   /* not waited for: its result comes with the frame's ack */
        if (R.f) H.mark_res = (int64_t)w->n;
    } else if (kind == RR_LOOP_LATER) h.flags |= RW_FLAG_LATER;
    else if (kind == RR_LOOP_WAIT) {
        h.flags |= RW_FLAG_REPLY;
        if (R.f) {   /* where the reply goes in the recording */
            if (loop_res_n == loop_res_cap) {
                const uint32_t cap = loop_res_cap ? loop_res_cap * 2 : 1024;
                void *b = realloc(loop_res, cap * sizeof *loop_res);
                if (!b) { w->bad = 1; return; }
                loop_res = b;
                loop_res_cap = cap;
            }
            loop_res[loop_res_n].off = (uint32_t)w->n;
            loop_res[loop_res_n++].known = 0;
        }
    }
    memcpy(w->buf + w->rec, &h, sizeof h);
}
static void helper_drain(void) {
    if (!R.w.n) return;
    const uint64_t t0 = tsc();
    if (R.w.bad) loop_fatal("out of memory: records lost", 0);
    R.fr_bytes += R.w.n;
    for (int c = 0; c < RW_CATS; c++) R.fr_cat[c] += R.w.cat[c];
    R.loop_bytes += R.w.n;
    R.loop_drains++;
    R.fr_drains++;
    if (!H.dead && !H.closed) {
        const uint64_t tw = tsc();
        if (r64_write(H.x, R64_RING_CMD, R.w.buf, (uint32_t)R.w.n) || r64_flush(H.x, R64_RING_CMD)) helper_dead();
        R.fr_t_ring += tsc() - tw;
    }
    if (R.f) {
        const uint64_t base = H.tee_base + H.tee_n;
        for (uint32_t i = 0; i < loop_res_n; i++) {
            if (H.pend_n == H.pend_cap) {
                const uint32_t cap = H.pend_cap ? H.pend_cap * 2 : 256;
                uint64_t *b = realloc(H.pend, cap * sizeof *b);
                if (!b) loop_fatal("out of memory", 0);
                H.pend = b;
                H.pend_cap = cap;
            }
            H.pend[H.pend_n++] = base + loop_res[i].off;
        }
        typeof(H.tf[0]) *t = &H.tf[R.frame % H_ACKS];
        if (H.mark_end >= 0) t->end = base + (uint64_t)H.mark_end, t->has_end = 1;
        if (H.mark_res >= 0) t->res = base + (uint64_t)H.mark_res, t->has_res = 1;
        if (H.mark_rec >= 0) t->rec = base + (uint64_t)H.mark_rec;
        if (H.mark_stop >= 0) t->stop = base + (uint64_t)H.mark_stop, t->set = 1;
        H.mark_end = H.mark_res = H.mark_rec = H.mark_stop = -1;
        tee_put(R.w.buf, R.w.n);
    }
    loop_res_n = loop_res_next = 0;
    rw_reset(&R.w);
    rw_enc_writer_reset(R.enc);
    R.fr_t_drain += tsc() - t0;
}
/* A record flagged RW_FLAG_REPLY was written: its result (and renderer_end's pixels) from the helper. */
static double helper_result(uint32_t id) {
    if (id == RW_ID_PRESENT) {   /* not waited for: the result of the frame whose ack answers the queries */
        H.present_call[R.frame % H_ACKS] = r64_now_ns();
        loop_drain();
        return H.present_ret;
    }
    R.loop_syncs[id]++;
    R.fr_syncs++;
    loop_drain();
    if (H.dead || H.closed) return id == RW_ID_INIT || id == RW_ID_END ? -1 : 0;   /* no renderer any more */
    const uint64_t t0 = tsc();
    if (back_until(R64M_REPLY)) return 0;
    H.reply_ticks += tsc() - t0;
    helper_fr_wait += tsc() - t0;
    H.wait_id[id < RW_ID_COUNT ? id : RW_ID_COUNT] += tsc() - t0;
    H.replies++;
    RwReader r;
    rw_reader(&r, H.bk, H.bk_n);
    const int32_t v = rw_ri32(&r);
    const uint32_t px = rw_ru32(&r);
    const uint64_t aux = rw_ru64(&r);
    if (r.bad || (size_t)(r.end - r.p) < px) loop_fatal("a short reply from the helper", id);
    if (id == RW_ID_END && px && R.loop_out) memcpy(R.loop_out, r.p, px);
    if (id == RW_ID_TEXTURE_CREATE) {
        H.created[R.frame % H_ACKS] += aux;
        H.creates[R.frame % H_ACKS]++;
        if (!v) H.create_failed = R.frame ? R.frame : 1;
    }
    if (R.f && H.pend_head < H.pend_n) tee_patch(H.pend[H.pend_head++], &v, 4);
    if (H.pend_head == H.pend_n) H.pend_head = H.pend_n = 0;
    R.loop_got = v;
    return v;
}
/* After renderer_init (a request: the helper's snapshot now) or at the end of frame N (frame N + 1 - K's ack, K =
 * --render64-frames; frame N's own with a read-back frame, which waited anyway): the snapshot and the LATE answers. */
static void helper_snap(int frame_end, int present) {
    rr_snap_on = 1;
    if (!frame_end) {
        H.used = 0;   /* an earlier renderer_init's acks answer no LATE query of this one */
        if (rr_rq_begin(RQ_SNAP, 0)) {
            RwReader *r = rr_rq_call(RQ_SNAP, 0);
            size_t n = 0;
            const void *p = rw_rblob_plain(r, &n);
            if (p && n == sizeof rr_snap) memcpy(rr_snap, p, n);
        }
        int w = 0, h = 0;
        rr_rq_renderer_output_size(&w, &h);
        H.rows = h > 0 ? (size_t)h : 0;
        return;
    }
    H.ended = R.frame;
    const uint32_t k = (uint32_t)helper_frames, target = present ? (R.frame + 1 > k ? R.frame + 1 - k : 0) : R.frame;
    if (H.acked < target) {
        const uint64_t t0 = tsc();
        while (!H.dead && H.acked < target)
            if (back_next()) break;
        H.ack_ticks += tsc() - t0;
        helper_fr_wait += tsc() - t0;
        H.ack_waits++;
        const double rate = tsc_per_ms();
        if (H.ackw && H.nackw < H_SAMPLES && rate > 0) H.ackw[H.nackw++] = (float)((double)(tsc() - t0) / rate);
    } else if (H.ackw && H.nackw < H_SAMPLES) H.ackw[H.nackw++] = 0;
    back_poll();
    if (!target || H.acked < target) return;
    const HAck *a = &H.ack[target % H_ACKS];
    if (a->frame != target) return;
    if (a->snap_off) memcpy(rr_snap, a->buf + a->snap_off, sizeof rr_snap);
    H.used = target;
    H.present_ret = a->ret;
}
/* The texture bytes and textures the helper's renderer may hold now: its last ack plus everything created since. */
static uint64_t helper_tex_bound(uint32_t *live) {
    const HAck *a = H.acked ? &H.ack[H.acked % H_ACKS] : NULL;
    uint64_t b = a ? a->tex_bytes : 0;
    uint32_t n = a ? a->tex_count : 0;
    /* frame a's own creates too: those after its renderer_end are not in the ack */
    for (uint32_t f = a ? a->frame : 0; f <= R.frame; f++)
        if (f + H_ACKS > R.frame) b += H.created[f % H_ACKS], n += H.creates[f % H_ACKS];
    *live = n;
    return b;
}
/* Whether the helper's renderer_begin of this frame may have freed textures: it frees the least recently used ones only
 * when the cache is over its byte budget (renderer_set_texture_budget, 512 MiB by default) or its 4096 entries, or
 * after a failed create (src/renderer/vk_renderer.cpp evict_textures). */
static int helper_pressure(void) {
    uint32_t live;
    const uint64_t b = helper_tex_bound(&live), budget = R.budget ? R.budget : (512ull << 20);
    return R.frame - H.acked + 1 >= H_ACKS || b + b / 16 >= budget || live + 64 >= 4096 || (H.create_failed && H.create_failed + 4 > R.frame);
}
/* The texture-handle mirror with the helper: usable at once unless the cache may have been trimmed at this frame's
 * renderer_begin; then the first lookup of the frame asks the helper (after that renderer_begin in the stream). */
static int helper_tm_usable(void) {
    if (R.tm_checked == R.frame) return 1;
    R.tm_checked = R.frame;
    if (!helper_pressure()) return 1;
    H.pressure_frames++;
    if (rr_rq_begin(RQ_TEX, 0)) {
        RwReader *r = rr_rq_call(RQ_TEX, 0);
        if (rw_ru32(r)) tm_clear();
        H.tex_syncs++;
    }
    H.tex_synced = R.frame;
    return 1;
}
static uint64_t rr_ga_renderer_texture_bytes(void) {
    if (!helper_on || !loop_host) return 0;
    uint32_t live;
    const uint64_t v = helper_tex_bound(&live);
    if (rr_snap_check) {   /* check mode: the live value (a round trip) */
        H.tb_asked++;
        if (rr_rq_renderer_texture_bytes() != v) H.tb_stale++;
    }
    return v;
}

static RwReader rq_rd;
static RwWriter *rr_rq_begin(int qi, int kind) {
    if (!helper_on || !loop_host || H.dead || H.closed || loop_draining || H.rq_open) return NULL;
    /* LATE: from the acks, once one of this renderer_init's frames was used; before that (--dynres and the [taa] /
     * [fsr] lines ask right after renderer_init) a round trip, else every answer would be 0 */
    if ((kind == 1 && H.used) || (kind == 2 && qi < RW_QUERY_COUNT && H.cst[qi])) return NULL;
    rw_begin_record(&R.w, RW_REC_QUERY, RW_FLAG_REPLY);
    rw_u32(&R.w, (uint32_t)qi);
    H.rq_open = 1;
    return &R.w;
}
static RwReader *rr_rq_call(int qi, int kind) {
    rw_reader(&rq_rd, "", 0);   /* nothing to answer with: every read is 0 */
    if (H.rq_open) {
        H.rq_open = 0;
        rw_end_record(&R.w);
        if (qi >= 0 && qi < RQ_ALL) H.rq_n[qi]++;
        R.loop_syncs[RW_ID_COUNT]++;
        R.fr_syncs++;
        loop_drain();
        const uint64_t t0 = tsc();
        if (back_until(R64M_QREPLY)) return &rq_rd;
        H.reply_ticks += tsc() - t0;
        helper_fr_wait += tsc() - t0;
        H.wait_id[RW_ID_COUNT] += tsc() - t0;
        H.replies++;
        if (kind == 2 && qi < RW_QUERY_COUNT && (H.cst[qi] = malloc(H.bk_n ? H.bk_n : 1))) {
            memcpy(H.cst[qi], H.bk, H.bk_n);
            H.cst_n[qi] = H.bk_n;
        }
        rw_reader(&rq_rd, H.bk, H.bk_n);
        return &rq_rd;
    }
    if (kind == 1 && H.used && qi < RW_QUERY_COUNT) {   /* LATE: from the ack in use */
        const HAck *a = &H.ack[H.used % H_ACKS];
        for (uint32_t i = 0; i < a->nlate; i++)
            if (a->late[i].qi == (uint32_t)qi) {
                rw_reader(&rq_rd, a->buf + a->late[i].off, a->late[i].n);
                H.rq_late[qi]++;
                break;
            }
    } else if (kind == 2 && qi < RW_QUERY_COUNT && H.cst[qi]) rw_reader(&rq_rd, H.cst[qi], H.cst_n[qi]);
    return &rq_rd;
}
static void rr_rq_out(RwReader *r, void *dst, size_t bytes) {
    size_t n = 0;
    const void *p = rw_rblob(r, &n);
    if (dst && p && bytes) memcpy(dst, p, n < bytes ? n : bytes);
}
static const char *rr_rq_str(RwReader *r, int qi) {
    size_t n = 0;
    const char *p = rw_rblob(r, &n);
    if (qi < 0 || qi >= RW_QUERY_COUNT) return "";
    char *s = realloc(H.str[qi], n + 1);
    if (!s) return "";
    if (p) memcpy(s, p, n);
    s[n] = 0;
    H.str[qi] = s;
    return s;
}
static size_t rr_rq_rows(void) {
    if (serving) {
        int w = 0, h = 0;
        renderer_output_size(&w, &h);
        return h > 0 ? (size_t)h : 0;
    }
    return H.rows;
}
static void rr_rl_renderer_pipeline_cache_save(void) {
    if (rr_rq_begin(RQ_CACHE_SAVE, 0)) rr_rq_call(RQ_CACHE_SAVE, 0);
}
static void rr_rl_renderer_set_vertex_check(RVertexCheck check) {
    if (check && !H.tri_warned++) printf("[render64] helper: --tri-check runs with the in-process renderer only (--render-process off)\n");
}
static void rr_rl_renderer_set_draw_tag(uint32_t tag) { (void)tag; }
static void rr_rl_renderer_set_vertex_check_frame(int on) { (void)on; }
static uint32_t rr_rl_renderer_pipeline_misses(uint32_t *out, uint32_t cap) { (void)out; (void)cap; return 0; }
static void rr_rl_renderer_pipeline_plan(const uint32_t *misses, uint32_t n) { (void)misses; (void)n; }
static double rr_rl_renderer_frame_time(void) { return now_ms(); }
static void rr_rl_renderer_set_frame_time(double t_ms) { (void)t_ms; }   /* rr_pre_renderer_set_frame_time keeps it */

static double pct_f(float *v, uint32_t n, double q) {
    if (!n) return 0;
    qsort(v, n, sizeof *v, cmp_f);
    uint32_t i = (uint32_t)(q * n);
    return v[i < n ? i : n - 1];
}
static void helper_window_end(void) {   /* from frame_done: this frame is sent, its ack is the last one the recording needs */
    loop_drain();
    while (!H.dead && H.acked < R.frame)
        if (back_next()) break;
    if (R.f) tee_write_to(H.tee_base + H.tee_n);
}
static void helper_exit(void) {
    if (H.closed) return;
    loop_drain();
    while (!H.dead && H.acked < H.ended)   /* every frame sent is acknowledged: the recording gets them */
        if (back_next()) break;
    if (!H.dead) back_poll();
    if (R.f) {
        tee_write_to(H.tee_base + H.tee_n);
        fclose(R.f);
        R.f = NULL;
    }
    H.closed = 1;
    const uint32_t n = R.frame ? R.frame : 1;
    const double rate = tsc_per_ms();
    printf("[render64] helper: %u renderer frames, %u acknowledged, %.1f MB through the CMD ring (%.2f MB/frame), %llu drains\n", R.frame, H.acked,
           R.loop_bytes / 1048576.0, R.loop_bytes / 1048576.0 / n, (unsigned long long)R.loop_drains);
    if (rate > 0)
        printf("[render64] helper waits (game thread): %.2f replies/frame (%.3f ms/frame), acks %.3f ms/frame (p50 %.3f, p95 %.3f, %llu frames waited); "
               "the helper's frame work %.2f ms/frame\n", (double)H.replies / n, H.reply_ticks / rate / n, H.ack_ticks / rate / n,
               pct_f(H.ackw, H.nackw, 0.5), pct_f(H.ackw, H.nackw, 0.95), (unsigned long long)H.ack_waits, H.srv_n ? H.srv_ms_sum / H.srv_n : 0.0);
    if (H.ndelay)
        printf("[render64] helper present: the game's renderer_present -> the helper's present returned: p50 %.3f ms, p95 %.3f ms, max %.3f ms (%u frames)\n",
               pct_f(H.delay, H.ndelay, 0.5), pct_f(H.delay, H.ndelay, 0.95), pct_f(H.delay, H.ndelay, 1.0), H.ndelay);
    char line[1024];
    int k = 0;
    for (int shown = 0; shown < 6 && rate > 0; shown++) {   /* the round trips by the call that waited */
        int best = -1;
        for (int i = 0; i <= RW_ID_COUNT; i++)
            if (H.wait_id[i] && (best < 0 || H.wait_id[i] > H.wait_id[best])) best = i;
        if (best < 0) break;
        k += snprintf(line + k, sizeof line - (size_t)k, "%s %s %.2f/frame %.3f ms/frame", shown ? "," : "", best == RW_ID_COUNT ? "queries" : rw_id_name[best] + 9,
                      (double)R.loop_syncs[best] / n, H.wait_id[best] / rate / n);
        H.wait_id[best] = 0;
    }
    if (k) printf("[render64] helper replies waited for:%s\n", line);
    k = 0;
    for (int shown = 0; shown < 8; shown++) {
        int best = -1;
        for (int i = 0; i < RQ_ALL; i++)
            if (H.rq_n[i] && (best < 0 || H.rq_n[i] > H.rq_n[best])) best = i;
        if (best < 0) break;
        const char *nm = best < RW_QUERY_COUNT ? rw_query_name[best] + 9 : best == RQ_CACHE_SAVE ? "pipeline_cache_save" : best == RQ_SNAP ? "snapshot" : "texture_state";
        k += snprintf(line + k, sizeof line - (size_t)k, "%s %s %llu", shown ? "," : "", nm, (unsigned long long)H.rq_n[best]);
        H.rq_n[best] = 0;
    }
    uint64_t late = 0;
    for (int i = 0; i < RW_QUERY_COUNT; i++) late += H.rq_late[i];
    printf("[render64] helper remote queries (round trips):%s; %.1f/frame answered from the acks (late), %llu snapshot answers\n", k ? line : " none",
           (double)late / n, (unsigned long long)R.snap_answers);
    printf("[render64] helper texture lookups: %.1f/frame known on the game side, %.1f/frame without a record, %.1f/frame waited; "
           "%llu frames under cache pressure (%llu asked the helper), %llu losses seen only in an ack, %llu answers the helper's renderer contradicted; "
           "%.1f skinned draws/frame not waited for\n", (double)R.tm_known / n, (double)R.tm_skipped / n, (double)R.tm_waits / n,
           (unsigned long long)H.pressure_frames, (unsigned long long)H.tex_syncs, (unsigned long long)H.late_losses,
           (unsigned long long)H.contradicted, (double)R.skin_later / n);
    if (R.pipe_misses) printf("[render64] helper pipeline misses: %llu in %llu frames (in the recording)\n", (unsigned long long)R.pipe_misses,
                              (unsigned long long)R.pipe_frames);
    if (H.tb_asked) printf("[render64] helper renderer_texture_bytes: %llu of %llu game-side answers differ from the helper's value (check mode)\n",
                           (unsigned long long)H.tb_stale, (unsigned long long)H.tb_asked);
    if (rr_snap_check) {
        uint64_t stale = 0;
        for (int i = 0; i < RW_QUERY_COUNT; i++) stale += R.snap_stale[i];
        printf("[render64] helper snapshot answers: %llu, %llu differ from the helper's live value (the ack is %d frame%s old by design)\n",
               (unsigned long long)R.snap_answers, (unsigned long long)stale, helper_frames - 1, helper_frames == 2 ? "" : "s");
        for (int i = 0; i < RW_QUERY_COUNT; i++)
            if (R.snap_stale[i]) printf("[render64]   %s: %llu\n", rw_query_name[i], (unsigned long long)R.snap_stale[i]);
    }
    if (R.other_thread) printf("[render64] helper: %llu renderer calls from other threads (made in this process)\n", (unsigned long long)R.other_thread);
    const RwEncStats *st = rw_enc_stats(R.enc);
    if (!R.cache_off) {
        double held = 0;
        for (int c = 0; c < RW_CATS; c++) held += (double)st->cache_hit_bytes[c];
        printf("[render64] content cache (whole run): %.2f MB/frame held by the helper, %.2f MB/frame new, %.0f moves/frame, %llu evicted, %llu sent whole "
               "(budget), %llu collisions\n", held / 1048576.0 / n, st->cache_new_bytes / 1048576.0 / n, (double)st->cache_moves / n,
               (unsigned long long)st->cache_evicted, (unsigned long long)st->cache_refused, (unsigned long long)st->cache_collisions);
    }
    if (rate > 0)
        printf("[render64] helper game side (whole run): encoding %.3f ms/frame, ring writes %.3f ms/frame\n", R.t_enc / rate / n,
               (double)(R.t_drain - R.t_render) / rate / n);
}

/* ---- the helper side (render_rec_serve) ---- */
static uint32_t *serve_ids;   /* the game's texture handle - RR_TEX_ID_BASE -> this renderer's handle */
static size_t serve_ids_n;
static uint64_t serve_id_failed;
static uint32_t serve_map(uint32_t v) {
    if (v < RR_TEX_ID_BASE) return v;
    return v - RR_TEX_ID_BASE < serve_ids_n ? serve_ids[v - RR_TEX_ID_BASE] : 0;
}
static void serve_map_set(uint32_t id, uint32_t handle) {
    const size_t i = id - RR_TEX_ID_BASE;
    if (i >= serve_ids_n) {
        size_t n = serve_ids_n ? serve_ids_n : 4096;
        while (n <= i) n *= 2;
        uint32_t *b = realloc(serve_ids, n * sizeof *b);
        if (!b) loop_fatal("out of memory", RW_ID_TEXTURE_CREATE);
        memset(b + serve_ids_n, 0, (n - serve_ids_n) * sizeof *b);
        serve_ids = b;
        serve_ids_n = n;
    }
    serve_ids[i] = handle;
    if (!handle && serve_id_failed++ < 10) r64_log("texture_create failed for the game's handle %u (drawn without it)", id);
}
static struct {
    R64Ipc *x;
    RenderServe o;
    RendererConfig win;    /* its window callbacks (rw_replay_config) */
    int headless, dead, qi_can_present;
    uint32_t *px, *gen;
    size_t px_cap, gen_cap, px_bytes;
    RwWriter q, a;         /* a query reply, an ack */
    int32_t ret;
    int w, h, hashed;
    uint64_t hash, ghash, tex0, tex_add, t_begin;
    uint32_t frame_lost, lost_since_ask;
    uint64_t replies, queries, acks, frames_shown;
} S = {.qi_can_present = -1};
static void serve_send(uint32_t type, const void *a, size_t an, const void *b, size_t bn) {
    if (S.dead) return;
    const R64Msg m = {type, (uint32_t)(an + bn)};
    if (r64_write(S.x, R64_RING_BACK, &m, sizeof m) || (an && r64_write(S.x, R64_RING_BACK, a, (uint32_t)an)) ||
        (bn && r64_write(S.x, R64_RING_BACK, b, (uint32_t)bn)) || r64_flush(S.x, R64_RING_BACK))
        S.dead = 1;
}
static uint32_t *serve_buf(uint32_t **b, size_t *cap, size_t bytes) {
    if (bytes > *cap) {
        free(*b);
        *b = malloc(bytes);
        *cap = *b ? bytes : 0;
        if (!*b) loop_fatal("out of memory", RW_ID_END);
    }
    return *b;
}
static void serve_config(RendererConfig *c, int had_surface) {
    /* the game presents (its window is this one): with a Vulkan window here the renderer presents into it, without
     * one (headless: SDL's dummy driver) it renders offscreen and every frame is read back and dropped */
    S.headless = had_surface && !S.o.create_surface;
    R.loop_cfg = had_surface && S.o.create_surface ? &S.win : NULL;
    (void)c;
}
static int serve_end(int pitch_bytes) {
    int w = 0, h = 0;
    renderer_output_size(&w, &h);
    const size_t need = (size_t)(pitch_bytes > 0 ? pitch_bytes : 0) * (size_t)(h > 0 ? h : 0);
    serve_buf(&S.px, &S.px_cap, need ? need : 4);
    S.ret = renderer_end(S.px, pitch_bytes);
    S.w = w, S.h = h;
    S.px_bytes = S.ret ? 0 : need;
    return S.ret;
}
static int serve_present(void) {
    if (!S.headless) {
        S.ret = renderer_present();
        if (!S.ret && S.o.presented) __atomic_add_fetch(S.o.presented, 1, __ATOMIC_RELEASE);
        S.frames_shown++;
        return S.ret;
    }
    int w = 0, h = 0;   /* headless: read back (as the replay of this PRESENT does), hash when asked, drop */
    renderer_output_size(&w, &h);
    const size_t need = (size_t)(w > 0 ? w : 0) * (size_t)(h > 0 ? h : 0) * 4;
    serve_buf(&S.px, &S.px_cap, need ? need : 4);
    S.ret = renderer_end(S.px, w * 4);
    S.w = w, S.h = h;
    if (!S.ret && S.o.hash) {
        S.hash = rw_frame_hash(S.px, w, h, w * 4);
        S.ghash = 0;
        if (renderer_frame_gen_on() && renderer_frame_gen_readback(serve_buf(&S.gen, &S.gen_cap, need ? need : 4), w * 4))
            S.ghash = rw_frame_hash(S.gen, w, h, w * 4);
        S.hashed = 1;
    }
    return S.ret;
}
static void serve_before(uint32_t id) {
    if (id == RW_ID_TEXTURE_CREATE) S.tex0 = renderer_texture_bytes();
    else if (id == RW_ID_BEGIN) S.t_begin = r64_now_ns();
}
static void serve_ack(void) {
    static uint32_t *miss;
    static uint32_t miss_cap;
    RwWriter *a = &S.a;
    rw_reset(a);
    rw_u32(a, R.dec_begins);
    rw_i32(a, S.ret);
    rw_u32(a, (S.frame_lost ? ACK_TEX_LOST : 0u) | (S.hashed ? ACK_HASHED : 0u));
    rw_i32(a, S.w);
    rw_i32(a, S.h);
    rw_u32(a, renderer_texture_count());
    rw_u64(a, S.hashed ? S.hash : 0);
    rw_u64(a, S.hashed ? S.ghash : 0);
    rw_u64(a, renderer_texture_bytes());
    rw_u64(a, r64_now_ns());
    rw_u32(a, (uint32_t)serve_mispredicted);
    rw_f32(a, S.t_begin ? (float)((double)(r64_now_ns() - S.t_begin) / 1e6) : 0.0f);
    rr_snap_take();
    if (S.headless && S.qi_can_present >= 0) rr_snap[S.qi_can_present].i = 1;
    rw_blob_plain(a, rr_snap, sizeof rr_snap);
    rw_serve_late(a);
    uint32_t n = renderer_pipeline_misses(miss, miss_cap);
    if (n > miss_cap) {
        uint32_t *b = realloc(miss, n * sizeof *b);
        if (!b) loop_fatal("out of memory", RW_ID_END);
        miss = b;
        miss_cap = n;
        n = renderer_pipeline_misses(miss, miss_cap);
    }
    rw_u32(a, n);
    rw_bytes(a, miss, (size_t)n * 4);
    if (a->bad) loop_fatal("out of memory: an ack", RW_ID_END);
    serve_send(R64M_ACK, a->buf, a->n, NULL, 0);
    S.acks++;
    S.frame_lost = 0;
    S.hashed = 0;
    S.hash = S.ghash = 0;
}
static void serve_after(uint32_t id) {
    if (id == RW_ID_BEGIN) {
        if (R.tex_lost) S.frame_lost = S.lost_since_ask = 1;
        R.tex_lost = 0;
    }
    if (serve_flags & RW_FLAG_REPLY) {
        struct { int32_t v; uint32_t px; uint64_t aux; } rp = {(int32_t)(int64_t)R.loop_got, 0, 0};
        const void *pixels = NULL;
        if (id == RW_ID_TEXTURE_CREATE) rp.aux = renderer_texture_bytes() - S.tex0;
        if (id == RW_ID_END && S.px_bytes) {
            rp.px = (uint32_t)S.px_bytes;
            pixels = S.px;
        }
        serve_send(R64M_REPLY, &rp, sizeof rp, pixels, rp.px);
        S.replies++;
    }
    if (id == RW_ID_END || id == RW_ID_PRESENT) serve_ack();
}
static void serve_query(RwReader *r) {
    const uint32_t qi = rw_ru32(r);
    RwWriter *o = &S.q;
    rw_reset(o);
    if (qi < RW_QUERY_COUNT) {
        if (rw_serve_query(qi, r, o)) r64_log("a bad remote query %u", qi);
        if ((int)qi == S.qi_can_present && S.headless) {   /* the headless present: the game presents, frames are read back here */
            rw_reset(o);
            rw_i32(o, 1);
        }
    } else if (qi == RQ_CACHE_SAVE) renderer_pipeline_cache_save();
    else if (qi == RQ_SNAP) {
        rr_snap_take();
        if (S.qi_can_present >= 0 && R.loop_cfg == NULL && S.headless) rr_snap[S.qi_can_present].i = 1;
        rw_blob_plain(o, rr_snap, sizeof rr_snap);
    } else if (qi == RQ_TEX) {
        rw_u32(o, S.lost_since_ask);
        S.lost_since_ask = 0;
        rw_u64(o, renderer_texture_bytes());
        rw_u32(o, renderer_texture_count());
    } else r64_log("an unknown remote request %u", qi);
    if (o->bad) loop_fatal("out of memory: a reply", RW_REC_QUERY);
    serve_send(R64M_QREPLY, o->buf, o->n, NULL, 0);
    S.queries++;
}
/* CPU time of this process (0) or of the calling thread (1), ms. */
static double serve_cpu_ms(int thread) {
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!(thread ? GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u) : GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))) return 0;
    const uint64_t kk = (uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime, uu = (uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime;
    return (double)(kk + uu) / 1e4;   /* 100 ns units */
#else
    struct timespec t;
    if (clock_gettime(thread ? CLOCK_THREAD_CPUTIME_ID : CLOCK_PROCESS_CPUTIME_ID, &t)) return 0;
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
#endif
}
int render_rec_serve(struct R64Ipc *x, const RenderServe *o) {
    serving = 1;
    S.x = x;
    S.o = *o;
    memset(&S.win, 0, sizeof S.win);
    S.win.instance_ext = o->instance_ext;
    S.win.instance_ext_count = o->instance_ext_count;
    S.win.create_surface = o->create_surface;
    S.win.drawable_size = o->drawable_size;
    for (int i = 0; i < RW_QUERY_COUNT; i++)
        if (!strcmp(rw_query_name[i], "renderer_can_present")) S.qi_can_present = i;
    uint32_t an = 0;
    void *arena = r64_region(x, R64_REG_CONTENT, &an);
    R.dec = rw_dec_new();
    if (!R.dec) return 5;
    rw_dec_arena(R.dec, arena, an);
    rw_dec_copy_programs(R.dec, 1);
    rw_dec_stream(R.dec);
    R.lh = x;
    const uint64_t t0 = r64_now_ns();
    const double cpu0 = serve_cpu_ms(0), thr0 = serve_cpu_ms(1);
    const int rc = ring_consume(x, 1);
    const double secs = (double)(r64_now_ns() - t0) / 1e9, cpu = serve_cpu_ms(0) - cpu0, thr = serve_cpu_ms(1) - thr0;
    const uint32_t nf = R.dec_begins ? R.dec_begins : 1;
    r64_log("render: CPU %.2f ms/frame for the helper process (%.1f%% of one core), %.2f ms/frame on the render thread", cpu / nf,
            secs > 0 ? cpu / (secs * 10) : 0.0, thr / nf);
    r64_log("render: %u frames in %.1f s (%llu shown in the window), %llu replies, %llu remote queries, %llu acks; %llu results the game side "
            "knew that the renderer contradicts; %s", R.dec_begins, secs, (unsigned long long)S.frames_shown, (unsigned long long)S.replies,
            (unsigned long long)S.queries, (unsigned long long)S.acks, (unsigned long long)serve_mispredicted,
            rc == R64_ERR_QUIT ? "the game quit" : rc == R64_ERR_PEER_DEAD ? "the game is gone" : "stopped");
    renderer_shutdown();   /* the swapchain before the main thread closes the window; no-op when the game shut it down */
    serving = 0;
    return rc == R64_ERR_QUIT ? 0 : rc == R64_ERR_PEER_DEAD || S.dead ? 4 : 5;
}
