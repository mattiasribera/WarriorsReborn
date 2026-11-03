/* HLE Direct3D 8 boundary: host replacements for the D3D8 library entry points that title code
 * calls in the statically linked D3D8 library (see hle_d3d.h).
 *
 * Boundary: the title calls 80 D3D8-section entry points directly
 * (no indirect calls, no device vtable). Argument bytes come from each function's own "ret N",
 * following a leading jmp/call to the code that actually returns (the census misread three of
 * those: SetTile, BlockUntilNotBusy, BlockUntilIdle). Entries with 0 bytes are register-argument
 * helpers (SetRenderState_Simple: ecx/edx) or take no arguments. Names ending in '?' are inferred
 * from the code, not from symbols.
 *
 * Objects use the Xbox D3DResource layout so title code and the CPU-only XGRAPHICS/D3DX code can
 * read them: Common (refcount | type | flags), Data (physical address for GPU memory), Lock,
 * Format, Size, then Parent for surfaces. Headers live in guest memory, pixel and vertex data in
 * the contiguous window (VA = 0x80000000 | Data). Nothing is ever freed: the title may still hold
 * pointers to released resources, and the first frames allocate little. */
#define _GNU_SOURCE
#include "hle_d3d.h"
#include "hle_input.h"
#include "input_map.h"
#include "kbm_glyphs.h"
#include "hle_spawn.h"
#include "hle_xapi.h"
#include "pacing.h"
#include "hle_sim60.h"
#include "rateprobe.h"
#include "geom.h"
#include "hud_glass.h"
#include "bloom.h"
#include "shadow_range.h"
#include "mem.h"
#include "psh.h"
#include "renderer.h"
#include "texpack.h"
#include "video.h"
#include "vsh.h"
#include "xtex.h"
#include "yuv.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include <time.h>
#include <unistd.h>

#define S_OK 0u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define E_OUTOFMEMORY 0x8007000Eu

/* D3DResource.Common */
#define COMMON_REFMASK 0xFFFFu
#define COMMON_TYPE_MASK 0x70000u
#define COMMON_VERTEXBUFFER 0x00000u
#define COMMON_INDEXBUFFER 0x10000u
#define COMMON_PUSHBUFFER 0x20000u
#define COMMON_PALETTE 0x30000u
#define COMMON_TEXTURE 0x40000u
#define COMMON_SURFACE 0x50000u
#define COMMON_D3DCREATED 0x01000000u
/* D3DPixelContainer.Format fields */
#define FMT_DMA_A 0x1u
#define FMT_CUBEMAP 0x4u
#define FMT_CODE(f) (((f) >> 8) & 0xFFu)
#define FMT_LEVELS(f) (((f) >> 16) & 0xFu)
#define FMT_ULOG(f) (((f) >> 20) & 0xFu)
#define FMT_VLOG(f) (((f) >> 24) & 0xFu)
#define FMT_PLOG(f) (((f) >> 28) & 0xFu)
/* D3DDevice_Clear flags (Xbox) */
#define CLEAR_TARGET 0xF0u

/* Fields of the static CDevice (D3D_DEVICE) that the original library reads; filled so any
 * library code still running sees our objects (offsets from the disassembly of
 * GetRenderTarget2, GetDepthStencilSurface2, GetBackBuffer2 and Swap). */
#define DEV_PUT 0x0
#define DEV_END 0x4
#define DEV_RENDER_TARGET 0x1a04
#define DEV_DEPTH_STENCIL 0x1a08
#define DEV_SURFACES 0x1a14         /* [0] back, [1] front, [2] third buffer */
#define DEV_SWAP_COUNT 0x2478
#define D3D_DEVICE_SET 0x0037e2b8u  /* written as 1 by Direct3D_CreateDevice */
#define D3D_PB_SIZE 0x00380140u     /* Direct3D_SetPushBufferSize globals */
#define D3D_PB_KICKOFF 0x0038013cu
#define D3D_DIRTY_FLAGS 0x0037d4d8u /* or'ed with 0xff1000 by Swap(D3DSWAP_FINISH) */
#define SCRATCH_PB_SIZE 0x100000u

#define G(p) ((void *)(uintptr_t)(p))
#define GA(p) ((uint32_t)(uintptr_t)(p))
#define U32(a, off) (*(uint32_t *)G((a) + (off)))
#define PA(va) ((va) - CONTIG_BASE)
#define VA(pa) ((pa) | CONTIG_BASE)

/* Stack frame built by hle_d3d_entry_common (hle_d3d_thunk.S). */
typedef struct {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t stub_ret, index, ret_addr;
    uint32_t args[];
} D3dFrame;

int hle_d3d_log_limit = 1;
int hle_d3d_frame_limit;
const char *hle_d3d_dump_path;
int hle_d3d_dump_every;   /* with --dump-frame: also writes <path>.<frame>.bmp every N frames */
const char *hle_d3d_video = "sdl";
const char *hle_d3d_renderer = "vulkan";
int hle_d3d_stats;
HudGlassConfig hle_d3d_glass = HUD_GLASS_DEFAULTS;
BloomConfig hle_d3d_bloom = BLOOM_DEFAULTS;
RainConfig hle_d3d_rain = RAIN_DEFAULTS;
SsaoConfig hle_d3d_ssao = SSAO_DEFAULTS;
ShadowRangeConfig hle_d3d_shadow_range = SHADOW_RANGE_DEFAULTS;
_Static_assert(BLOOM_DRAW_DEPTH_TEST == RENDERER_DEPTH_TEST, "bloom_scene_draw reads FrameOp.draw_flags");
_Static_assert(SSAO_DRAW_DEPTH_TEST == RENDERER_DEPTH_TEST, "ssao_opaque_draw reads FrameOp.draw_flags");
int hle_d3d_render_scale, hle_d3d_res_w, hle_d3d_res_h, hle_d3d_msaa, hle_d3d_aniso, hle_d3d_widescreen;
int hle_d3d_supersample, hle_d3d_shadow_scale, hle_d3d_shadow_soft;
int hle_d3d_sharpen;
const char *hle_d3d_present = "auto";
int hle_d3d_frames_in_flight = 2;
const char *hle_d3d_dump_textures, *hle_d3d_texture_packs[HLE_D3D_MAX_PACKS];
int hle_d3d_texture_pack_count;
int hle_d3d_fullscreen;
int hle_d3d_render_targets = 1;
int hle_d3d_button_icons;
static KbmRect glyph_rects[KBM_RECTS_MAX];
static int glyph_nrects;

int hle_d3d_glyphs_load(const char *disc_dir) {
    int n = kbm_rects_load(disc_dir, glyph_rects, KBM_RECTS_MAX);
    glyph_nrects = n > KBM_RECTS_MAX ? KBM_RECTS_MAX : n > 0 ? n : 0;
    return n;
}

void hle_d3d_output_size(int bw, int bh, int *ow, int *oh) {
    int aw = hle_d3d_widescreen ? 16 : bw, ah = hle_d3d_widescreen ? 9 : bh, r[4];
    if (hle_d3d_res_w > 0 && hle_d3d_res_h > 0) {
        video_fit(aw, ah, hle_d3d_res_w, hle_d3d_res_h, r);
    } else {
        int n = hle_d3d_render_scale > 0 ? hle_d3d_render_scale : 1;
        video_fit(aw, ah, bw * n * 4, bh * n, r);   /* the scaled height, the aspect's width */
    }
    *ow = r[2];
    *oh = r[3];
}

/* --stats: timing per 60 presented frames, to tell a slow guest from a slow host renderer.
 * Guest side (the thread that calls Swap): its frame time without the 60 Hz pacing sleep, the
 * part spent recording draws (vertex fetch, vertex programs, lighting) and the time Swap waited
 * for the host to take a queued frame. Host side (hle_d3d_service): replaying a frame through
 * the renderer, the part inside renderer_end (submit, GPU wait, readback) and video_present.
 * Guest values are handed over under d3d_lock at each Swap. */
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static struct {
    double frame_start, record;   /* guest thread only */
    uint32_t verts;               /* guest thread: vertices recorded this frame */
    double read, read_age;        /* guest thread: first XInputGetState of this frame, its sample age */
    double g_frame, g_record, g_wait;   /* handed over under d3d_lock */
    uint32_t g_swaps, g_verts;
    /* Recording breakdown (guest thread; handed over with the totals): ms per phase of
     * record_draw_body (REC_*), the draws, and the guest-memory checks (readable) made while
     * recording, their count and ms. */
    double ph[7], ph_last, rd_ms, g_ph[7], g_rd_ms;
    uint32_t draws, rd_calls, g_draws, g_rd_calls;
    int in_rec;
} gstat;
enum { REC_SETUP, REC_STATE, REC_PS, REC_TEX, REC_VERT, REC_VSH, REC_TAIL, REC_PHASES };
static const char *const rec_phase_name[REC_PHASES] = {"setup", "state", "pixel shader", "textures", "vertices", "vertex programs", "tail"};
/* Closes the current recording phase (--stats only). */
static void rec_mark(int phase) {
    if (!hle_d3d_stats) return;
    double t = now_ms();
    gstat.ph[phase] += t - gstat.ph_last;
    gstat.ph_last = t;
}
/* Input latency (--stats): the host pumps window events into the pad state (last_pump_us), the
 * guest reads it with XInputGetState, finishes the frame and swaps, the frame waits in the queue,
 * then it is rendered and presented. Per presented frame with an input read: sample age at the
 * read, read -> Swap, Swap -> taken by the host, taken -> presented. */
static int64_t last_pump_us;   /* host writes, guest reads (__atomic) */
static void stats_input_read(void) {
    if (gstat.read) return;
    gstat.read = now_ms();
    int64_t p = __atomic_load_n(&last_pump_us, __ATOMIC_RELAXED);
    gstat.read_age = p ? gstat.read - p / 1e3 : 0;
}
static RendererTiming hstat_end;   /* host thread: renderer_end split of the last render_frame */

/* Operations of one frame for the host renderer, recorded by guest threads (cheap copies only)
 * and replayed by the host main thread, which owns the renderer: calls into the graphics driver
 * stay off guest threads (guest stacks, %fs = KPCR) and on the thread that presents. */
enum { OP_CLEAR, OP_DRAW, OP_GLASS };
typedef struct {
    uint32_t kind, flags, color;   /* clear */
    float z;
    RViewport vp;                  /* draw: renderer flags, vertices and indices (into the draw's vertices) */
    uint32_t draw_flags;
    uint32_t first_vertex, nv, first_index, ni;
    uint32_t tex_data[RENDERER_STAGES], tex_format[RENDERER_STAGES], tex_size[RENDERER_STAGES];   /* texture headers (tex_key != 0) */
    uint32_t alpha_func, alpha_ref;            /* renderer_set_alpha_test arguments */
    RState rs;                                 /* renderer_set_state argument */
    uint64_t tex_key[RENDERER_STAGES];
    uint8_t tex_surf[RENDERER_STAGES];         /* stage texture = render-target surface (rt_surf index + 1) */
    uint8_t target_c, target_z;                /* offscreen target surfaces (rt_surf index + 1; 0 = back buffer) */
    uint8_t halo;                              /* bloom_halo_draw: drawn after the bloom split (--bloom only) */
    uint8_t rain;                              /* a rain layer of the title (rain_layer; --rain-hd only): replaced */
    RainLayer rain_layer;                      /* its parameters, and its scroll so far (rain_scroll_layer) */
    float rain_scroll[2];
    float ssao_scale[2];                       /* --ssao: projection x/y scale of an opaque draw (0: unknown) */
    int has_ps;                                /* ps: renderer_set_pixel_shader argument (else NULL) */
    RPixelShader ps;
    RGlass glass;                              /* OP_GLASS: renderer_glass argument (before the bar's draw) */
} FrameOp;
typedef struct {
    FrameOp *ops;
    uint32_t nops, cap;
    RVertex *v;
    uint32_t nv, vcap;
    uint32_t *idx;
    uint32_t ni, icap;
    uint32_t *base;       /* back buffer copied at the first draw (CPU clears, movie quads so far) */
    uint32_t *spare_base; /* a recycled list's base buffer (base_bytes), reused by the first draw */
    size_t base_bytes;
    uint32_t first_draw;  /* index of the first OP_DRAW */
    uint32_t draws;
    uint32_t cpu_draws;   /* movie quads drawn by the CPU into the guest back buffer */
    uint32_t cpu_after_draw;   /* of those, drawn after the first draw (lost in the rendered frame) */
    /* --rain-hd splashes: the camera (view and projection of the last depth-tested fixed-function
     * back-buffer draw with a perspective projection: the particles, the sky) and the title's
     * clock at the first rain layer. */
    float cam_view[16], cam_proj[16];
    uint8_t has_cam, has_clock;
    uint32_t clock_ms;
    /* --rain-hd fade (rain_fade_step): the title's clock at the frame's first draw. */
    uint8_t has_frame_ms;
    uint32_t frame_ms;
} FrameList;
static void frame_list_free(FrameList *l) {
    free(l->ops);
    free(l->v);
    free(l->idx);
    free(l->base);
    free(l->spare_base);
    memset(l, 0, sizeof *l);
}
/* Recycled frame lists (d3d_lock held): a presented or dropped list keeps its buffers for a later
 * frame, so recording does not grow fresh allocations from 4096 entries every frame (realloc
 * copies, and first-touch page faults on megabytes of new vertex memory per frame). Every reused
 * entry is rewritten before it is read (frame_op clears each op; draws fill their vertex and index
 * ranges; the base copy writes every row), so the frame content does not change. */
#define FRAME_SPARES 2
static FrameList frame_spare[FRAME_SPARES];
static int frame_spares;
static void frame_list_recycle(FrameList *l) {
    if (frame_spares == FRAME_SPARES) {
        frame_list_free(l);
        return;
    }
    FrameList *s = &frame_spare[frame_spares++];
    memset(s, 0, sizeof *s);
    s->ops = l->ops;
    s->cap = l->cap;
    s->v = l->v;
    s->vcap = l->vcap;
    s->idx = l->idx;
    s->icap = l->icap;
    s->spare_base = l->base ? l->base : l->spare_base;
    s->base_bytes = l->base_bytes;
    if (l->base) free(l->spare_base);
    memset(l, 0, sizeof *l);
}
/* Posted frame images, recycled the same way (one spare of spare_px_bytes; d3d_lock held). */
static uint32_t *spare_px;
static size_t spare_px_bytes;
static void frame_px_recycle(uint32_t *px, size_t bytes) {
    if (!px) return;
    free(spare_px);
    spare_px = px;
    spare_px_bytes = bytes;
}
static uint32_t *frame_px_take(size_t bytes) {
    uint32_t *px = spare_px;
    if (!px || spare_px_bytes != bytes) {
        free(px);
        px = malloc(bytes);
    }
    spare_px = NULL;
    return px;
}
/* An empty list for the next frame, with recycled buffers when there are any (d3d_lock held). */
static void frame_list_take(FrameList *l) {
    if (frame_spares) *l = frame_spare[--frame_spares];
    else memset(l, 0, sizeof *l);
}
static int grow(void **p, uint32_t *cap, uint32_t need, size_t elem) {
    if (need <= *cap) return 1;
    uint32_t c = *cap ? *cap : 4096;
    while (c < need) c *= 2;
    void *n = realloc(*p, (size_t)c * elem);
    if (!n) return 0;
    *p = n;
    *cap = c;
    return 1;
}
static FrameOp *frame_op(FrameList *l) {
    if (l->nops == l->cap) {
        uint32_t cap = l->cap ? l->cap * 2 : 64;
        FrameOp *ops = realloc(l->ops, cap * sizeof *ops);
        if (!ops) return NULL;
        l->ops = ops;
        l->cap = cap;
    }
    return memset(&l->ops[l->nops++], 0, sizeof(FrameOp));
}

static pthread_mutex_t d3d_lock = PTHREAD_MUTEX_INITIALIZER;
/* Present queue: Swap appends, the host main thread (hle_d3d_service) takes the oldest and signals
 * queue_space. When the host falls behind, Swap waits up to SWAP_WAIT_NS for a free slot, then
 * drops the oldest unpresented frame, so a stalled host never blocks the title for long.
 * One slot, and Swap returns once the host has taken its frame: while the host renders frame N
 * the title works on N+1, so a host slower than the title throttles it with no frame left
 * waiting behind the one in work. Three slots (the earlier depth) kept three frames waiting
 * whenever the host was behind: on lavapipe with 4 cores that was 86 ms of the 135 ms from
 * XInputGetState to present. */
#define FRAME_QUEUE 1
#define SWAP_WAIT_NS 100000000L
static pthread_cond_t queue_space = PTHREAD_COND_INITIALIZER;
#define MAX_LIGHTS 32   /* light indices kept (the title uses a few; D3D allows any index) */
static struct {
    uint32_t surf[3], depth, rt, zs;
    uint32_t w, h;
    uint32_t clear_color, frames, fence, scratch;
    uint32_t tex[4], ps_def, vshader;   /* bound textures, pixel shader definition, vertex shader/FVF */
    uint32_t psc[PSH_CONSTANTS];        /* SetPixelShaderConstant values (ARGB8888) */
    uint32_t fog_color;                 /* SetRenderState_FogColor (ARGB) */
    uint32_t movie_quads;               /* Bink frames drawn by the CPU YUV quad */
    int pb_warned;
    struct timespec next_vblank;
    uint32_t *frame;      /* last posted frame (XRGB8888, w*h) */
    uint32_t fw, fh, frame_seq;
    uint32_t *shown;      /* last presented frame at the output size (--dump-frame), host side */
    uint32_t sw, sh, shown_seq;
    volatile int done;    /* frame limit reached; the service thread exits */
    FrameList rec;        /* operations of the frame being drawn (guest side) */
    /* Vertex state for the host renderer. */
    struct { uint32_t vb, stride; } stream[4];
    float view[16], proj[16], world[16];
    float vsc[192][4];    /* vertex shader constants c-96..c95 */
    RViewport vp;
    uint32_t zenable;     /* D3DRS_ZENABLE as set through SetRenderState_ZEnable (0, 1; 2 = w-buffer) */
    uint32_t shadow_func; /* NV097_SET_SHADOW_DEPTH_FUNC (SetRenderState_ShadowFunc - 0x200) */
    /* --shadow-range: the light projection of the depth stages (RPixelShader.shadow_depth), fitted
     * from the first receiver draw of a frame that allows it, kept until the next fit; the swap
     * count + 1 of that frame (0 = never fitted). */
    float shadow_ab[2];
    uint32_t shadow_fit_frame;
    /* --rain-hd: the title's rain layers seen in the frame being recorded, and their scroll. */
    int rain_layers;
    RainScroll rain_scroll;
    uint8_t target_c, target_z;   /* current render target when not the back buffer (rt_surf index + 1) */
    uint32_t alpha_test, alpha_func, alpha_ref;   /* NV2A alpha test state from SetRenderState_Simple */
    RState rs;            /* blend, color mask, depth write/func (SetRenderState_Simple), cull (CullMode) */
    GeomLight light[MAX_LIGHTS];          /* SetLight by index */
    uint8_t light_on[MAX_LIGHTS];         /* LightEnable */
    GeomMaterial material;                /* SetMaterial */
    uint32_t drawn, skipped_target, skipped_fmt, skipped_up;
    uint32_t tex_draws[256], tex_unsupported[256];   /* draws per stage-0 texture format (decoded / not) */
    /* Swapped frames waiting for the host main thread (FIFO): pixels, operations, frame_seq. */
    struct { uint32_t *px; uint32_t w, h, seq; FrameList ops; double t_read, read_age, t_swap; } queue[FRAME_QUEUE];
    uint32_t q_head, q_count;
    uint32_t presented, dropped;   /* frames handed to the video backend / swaps never presented */
} st;

/* ---- guest allocators (bump, never freed) ---- */
static uint8_t *hdr_cur, *hdr_end;
static uint32_t hdr_alloc(uint32_t n) {
    n = (n + 15) & ~15u;
    if (!hdr_cur || hdr_cur + n > hdr_end) {
        hdr_cur = guest_alloc(0x100000);
        if (!hdr_cur) return 0;
        hdr_end = hdr_cur + 0x100000;
    }
    uint32_t r = GA(hdr_cur);
    hdr_cur += n;
    return r;
}
static uint32_t data_cur, data_end;
static uint32_t data_alloc(uint32_t n) {   /* contiguous memory, returns the VA */
    n = (n + 127) & ~127u;
    if (n > 0x100000) return contig_alloc(n, 0x1000, 0, 0);
    if (!data_cur || data_cur + n > data_end) {
        data_cur = contig_alloc(0x400000, 0x1000, 0, 0);
        if (!data_cur) return 0;
        data_end = data_cur + 0x400000;
    }
    uint32_t r = data_cur;
    data_cur += n;
    return r;
}

/* ---- formats ---- */
static int fmt_depth(uint32_t c) { return c >= 0x2A && c <= 0x31; }   /* D24S8 .. LIN_F16 */
/* Swizzled/DXT textures must have power-of-two sizes; linear ones keep theirs. */
static uint32_t log2u(uint32_t v) { uint32_t l = 0; while ((1u << l) < v) l++; return l; }
/* Width, height and pitch of a surface or texture header (level 0). */
static void res_dims(uint32_t r, uint32_t *w, uint32_t *h, uint32_t *pitch) {
    uint32_t f = U32(r, 0xc), size = U32(r, 0x10);
    if (size) {
        *w = (size & 0xFFF) + 1;
        *h = ((size >> 12) & 0xFFF) + 1;
        *pitch = ((size >> 24) + 1) * 64;
    } else {
        *w = 1u << FMT_ULOG(f);
        *h = 1u << FMT_VLOG(f);
        *pitch = xtex_level_pitch(FMT_CODE(f), *w);
    }
}

static uint32_t make_surface(uint32_t w, uint32_t h, uint32_t code) {
    uint32_t s = hdr_alloc(0x18), pitch = xtex_level_pitch(code, w);
    uint32_t data = s ? data_alloc(pitch * h) : 0;
    if (!data) return 0;
    U32(s, 0) = 1 | COMMON_SURFACE | COMMON_D3DCREATED;
    U32(s, 4) = PA(data);
    U32(s, 0xc) = FMT_DMA_A | (2u << 4) | (code << 8) | (1u << 16);
    U32(s, 0x10) = (w - 1) | ((h - 1) << 12) | ((pitch / 64 - 1) << 24);
    return s;
}
/* Title render targets: every color or depth surface the title renders
 * into other than the back buffer, by guest physical address and size, so the host renderer keeps
 * one offscreen surface per entry (renderer_surface) and a stage texture over the same memory
 * samples it. Appended by the guest thread at SetRenderTarget and only read through the indices
 * recorded in queued frames (published under d3d_lock), never changed or removed. */
#define MAX_RT_SURFACES 32
typedef struct { uint32_t pa, kind, w, h; } RtSurface;
static RtSurface rt_surf[MAX_RT_SURFACES];
static int n_rt_surf;
static uint32_t rt_kind(uint32_t code) {
    switch (code) {
    case 0x2C: case 0x30: case 0x31: return RENDERER_SURFACE_DEPTH16;   /* D16, LIN_D16, LIN_F16 */
    case 0x2A: case 0x2B: case 0x2D: case 0x2E: case 0x2F: return RENDERER_SURFACE_DEPTH24;   /* D24S8, F24S8, F16, LIN_D24S8, LIN_F24S8 */
    default: return RENDERER_SURFACE_COLOR;
    }
}
static void res_dims(uint32_t r, uint32_t *w, uint32_t *h, uint32_t *pitch);
/* Registry index + 1 of a surface (added when new); 0 for none or a color surface without memory. */
static uint8_t rt_register(uint32_t surf) {
    if (!surf || !U32(surf, 4)) return 0;
    uint32_t w, h, pitch, kind = rt_kind(FMT_CODE(U32(surf, 0xc))), pa = U32(surf, 4) & 0x0FFFFFFFu;
    res_dims(surf, &w, &h, &pitch);
    for (int i = 0; i < n_rt_surf; i++)
        if (rt_surf[i].pa == pa && rt_surf[i].kind == kind && rt_surf[i].w == w && rt_surf[i].h == h) return (uint8_t)(i + 1);
    if (n_rt_surf == MAX_RT_SURFACES) return 0;
    rt_surf[n_rt_surf] = (RtSurface){pa, kind, w, h};
    return (uint8_t)++n_rt_surf;
}
/* Registered surface over the memory of a texture header (same address and size), or 0. */
static uint8_t rt_find(uint32_t tex) {
    uint32_t w, h, pitch, pa = U32(tex, 4) & 0x0FFFFFFFu;
    res_dims(tex, &w, &h, &pitch);
    for (int i = 0; i < n_rt_surf; i++)
        if (rt_surf[i].pa == pa && rt_surf[i].w == w && rt_surf[i].h == h) return (uint8_t)(i + 1);
    return 0;
}
static void addref(uint32_t r) { if (r && (U32(r, 0) & COMMON_REFMASK) < COMMON_REFMASK) U32(r, 0)++; }

/* ---- device ---- */
static uint32_t ok(D3dFrame *f, const uint32_t *a) { (void)f; (void)a; return S_OK; }
static uint32_t ret_one(D3dFrame *f, const uint32_t *a) { (void)f; (void)a; return 1; }

/* (dwPushBufferSize, dwKickOffSize): the original only stores them. */
static uint32_t set_pb_size(D3dFrame *f, const uint32_t *a) {
    (void)f;
    U32(D3D_PB_SIZE, 0) = a[0];
    U32(D3D_PB_KICKOFF, 0) = a[1];
    return S_OK;
}

static uint32_t to_linear(uint32_t code) {   /* back buffers are linear on the Xbox */
    switch (code) {
    case 0x06: return 0x12;   /* A8R8G8B8 */
    case 0x07: return 0x1E;   /* X8R8G8B8 */
    case 0x05: return 0x11;   /* R5G6B5 */
    case 0x03: return 0x1C;   /* X1R5G5B5 */
    case 0x2A: return 0x2E;   /* D24S8 */
    case 0x2C: return 0x30;   /* D16 */
    default: return code;
    }
}

/* (Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppDevice).
 * D3DPRESENT_PARAMETERS: BackBufferWidth, Height, Format, Count, MultiSampleType, SwapEffect,
 * hDeviceWindow, Windowed, EnableAutoDepthStencil, AutoDepthStencilFormat, ... */
static uint32_t create_device(D3dFrame *f, const uint32_t *a) {
    (void)f;
    const uint32_t *pp = G(a[4]);
    uint32_t w = pp && pp[0] ? pp[0] : 640, h = pp && pp[1] ? pp[1] : 480;
    uint32_t fmt = to_linear(pp && pp[2] ? pp[2] : 0x1E);
    uint32_t zfmt = to_linear(pp && pp[9] ? pp[9] : 0x2A);
    if (!st.surf[0]) {
        for (int i = 0; i < 3; i++) st.surf[i] = make_surface(w, h, fmt);
        st.depth = make_surface(w, h, zfmt);
        st.scratch = hdr_alloc(SCRATCH_PB_SIZE);
        if (!st.surf[0] || !st.surf[1] || !st.surf[2] || !st.depth || !st.scratch) return E_OUTOFMEMORY;
    }
    st.w = w;
    st.h = h;
    for (int i = 0; i < 16; i++) st.view[i] = st.proj[i] = st.world[i] = i % 5 == 0;   /* identity */
    st.vp = (RViewport){0, 0, (float)w, (float)h, 0, 1};
    st.zenable = pp && !pp[8] ? 0 : 1;   /* D3D enables Z with an auto depth buffer */
    st.rt = st.surf[0];
    st.zs = pp && !pp[8] ? 0 : st.depth;
    U32(D3D_G_PDEVICE, 0) = D3D_DEVICE;
    U32(D3D_DEVICE_SET, 0) = 1;
    /* Push buffer: a scratch area, so library code we did not replace writes harmlessly; Swap
     * reports it when the put pointer moved. */
    U32(D3D_DEVICE, DEV_PUT) = st.scratch;
    U32(D3D_DEVICE, DEV_END) = st.scratch + SCRATCH_PB_SIZE - 0x1000;
    U32(D3D_DEVICE, DEV_RENDER_TARGET) = st.rt;
    U32(D3D_DEVICE, DEV_DEPTH_STENCIL) = st.zs;
    for (int i = 0; i < 3; i++) U32(D3D_DEVICE, DEV_SURFACES + 4 * i) = st.surf[i];
    if (a[5]) *(uint32_t *)G(a[5]) = D3D_DEVICE;
    printf("[d3d] device %ux%u, back buffer format %02x (%08x), depth format %02x (%08x)\n", w, h, fmt, st.surf[0], zfmt, st.depth);
    return S_OK;
}
/* GetBackBuffer2(index): -1 = front buffer, 0 = back buffer, other = third buffer (original). */
static uint32_t get_back_buffer(D3dFrame *f, const uint32_t *a) {
    (void)f;
    uint32_t s = st.surf[a[0] == 0xFFFFFFFFu ? 1 : a[0] == 0 ? 0 : 2];
    addref(s);
    return s;
}
static uint32_t get_render_target(D3dFrame *f, const uint32_t *a) { (void)f; (void)a; addref(st.rt); return st.rt; }
static uint32_t get_depth_stencil(D3dFrame *f, const uint32_t *a) { (void)f; (void)a; addref(st.zs); return st.zs; }
/* (pRenderTarget, pNewZStencil): a NULL render target keeps the current one. */
static uint32_t set_render_target(D3dFrame *f, const uint32_t *a) {
    (void)f;
    if (a[0]) st.rt = a[0];
    st.zs = a[1];
    int off = st.rt != st.surf[0];
    st.target_c = off ? rt_register(st.rt) : 0;
    st.target_z = off ? rt_register(st.zs) : 0;
    if (st.target_c && rt_surf[st.target_c - 1].kind != RENDERER_SURFACE_COLOR) st.target_c = 0;
    if (st.target_z && rt_surf[st.target_z - 1].kind == RENDERER_SURFACE_COLOR) st.target_z = 0;
    uint32_t w, h, pitch;   /* like D3D8, a new render target resets the viewport to its size */
    if (st.rt) {
        res_dims(st.rt, &w, &h, &pitch);
        st.vp = (RViewport){0, 0, (float)w, (float)h, 0, 1};
    }
    U32(D3D_DEVICE, DEV_RENDER_TARGET) = st.rt;
    U32(D3D_DEVICE, DEV_DEPTH_STENCIL) = st.zs;
    return S_OK;
}

/* ---- resources ---- */
/* D3DRTYPE from Common (and Format for textures/surfaces), as the original GetType. */
static uint32_t get_type(D3dFrame *f, const uint32_t *a) {
    (void)f;
    if (!a[0]) return 0;
    uint32_t t = U32(a[0], 0) & COMMON_TYPE_MASK, fm = t >= COMMON_TEXTURE ? U32(a[0], 0xc) : 0;
    switch (t) {
    case COMMON_VERTEXBUFFER: return 6;
    case COMMON_INDEXBUFFER: return 7;
    case COMMON_PUSHBUFFER: return 8;
    case COMMON_PALETTE: return 9;
    case COMMON_TEXTURE: return fm & FMT_CUBEMAP ? 5 : (fm & 0xF0) > 0x20 ? 4 : 3;
    case COMMON_SURFACE: return (fm & 0xF0) > 0x20 ? 2 : 1;
    default: return 10;   /* fixup */
    }
}
/* Release: decrements the refcount; at the last reference a surface releases its parent. The
 * memory is kept (see the header comment). */
static uint32_t release_res(uint32_t r) {
    if (!r) return 0;
    uint32_t c = U32(r, 0) & COMMON_REFMASK;
    if (c <= 1) {
        if ((U32(r, 0) & COMMON_TYPE_MASK) == COMMON_SURFACE && U32(r, 0x14)) release_res(U32(r, 0x14));
        U32(r, 0) &= ~COMMON_REFMASK;
        return 0;
    }
    U32(r, 0)--;
    return c - 1;
}
static uint32_t release(D3dFrame *f, const uint32_t *a) { (void)f; return release_res(a[0]); }
/* (pResource, pBase): Data += base; masked to a physical address except for push buffers. */
static uint32_t register_res(D3dFrame *f, const uint32_t *a) {
    (void)f;
    if (!a[0]) return 0;
    uint32_t d = U32(a[0], 4) + a[1];
    if ((U32(a[0], 0) & COMMON_TYPE_MASK) != COMMON_PUSHBUFFER) d &= 0x0FFFFFFFu;
    U32(a[0], 4) = d;
    return S_OK;
}
/* (pSurface, pDesc) -> D3DSURFACE_DESC {Format, Type, Usage, Size, MultiSampleType, Width, Height} */
static uint32_t surface_desc(D3dFrame *f, const uint32_t *a) {
    (void)f;
    if (!a[0] || !a[1]) return D3DERR_INVALIDCALL;
    uint32_t w, h, pitch, code = FMT_CODE(U32(a[0], 0xc)), *d = G(a[1]);
    res_dims(a[0], &w, &h, &pitch);
    d[0] = code;
    d[1] = 1;   /* D3DRTYPE_SURFACE */
    d[2] = fmt_depth(code) ? 2 : U32(a[0], 0x14) ? 0 : 1;   /* DEPTHSTENCIL / RENDERTARGET for device buffers */
    d[3] = xtex_level_size(code, w, h);
    d[4] = 0x11;   /* D3DMULTISAMPLE_NONE */
    d[5] = w;
    d[6] = h;
    return S_OK;
}

/* (Width, Height, Depth, Levels, Usage, Format, D3DRTYPE) -> texture (returned in eax). */
static uint32_t create_texture(D3dFrame *f, const uint32_t *a) {
    (void)f;
    uint32_t w = a[0] ? a[0] : 1, h = a[1] ? a[1] : 1, d = a[2] ? a[2] : 1, code = a[5] & 0xFF, type = a[6];
    int linear = xtex_fmt_linear(code);
    uint32_t levels = a[3], full = log2u(w > h ? w : h) + 1;
    if (linear) levels = 1;
    else if (!levels || levels > full) levels = full;
    uint32_t faces = type == 5 ? 6 : 1, bytes = 0;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t lw = w >> l, lh = h >> l, ld = type == 4 ? d >> l : 1;
        bytes += xtex_level_size(code, lw ? lw : 1, lh ? lh : 1) * (ld ? ld : 1);
    }
    bytes = ((bytes + 127) & ~127u) * faces;
    uint32_t t = hdr_alloc(0x14), data = t ? data_alloc(bytes) : 0;
    if (!data) return 0;
    U32(t, 0) = 1 | COMMON_TEXTURE | COMMON_D3DCREATED;
    U32(t, 4) = PA(data);
    uint32_t dim = type == 4 ? 3 : 2, fm = FMT_DMA_A | (dim << 4) | (code << 8) | (levels << 16);
    if (type == 5) fm |= FMT_CUBEMAP;
    if (linear) {
        U32(t, 0x10) = (w - 1) | ((h - 1) << 12) | ((xtex_level_pitch(code, w) / 64 - 1) << 24);
    } else {
        fm |= (log2u(w) << 20) | (log2u(h) << 24) | ((type == 4 ? log2u(d) : 0) << 28);
        U32(t, 0x10) = 0;
    }
    U32(t, 0xc) = fm;
    return t;
}
/* Byte offset, size and pitch of mip level `level` of a 2D texture. */
static uint32_t level_offset(uint32_t tex, uint32_t level, uint32_t *lw, uint32_t *lh, uint32_t *pitch) {
    uint32_t w, h, p, code = FMT_CODE(U32(tex, 0xc)), off = 0;
    res_dims(tex, &w, &h, &p);
    for (uint32_t l = 0; l < level; l++) {
        off += xtex_level_size(code, w, h);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    *lw = w;
    *lh = h;
    *pitch = U32(tex, 0x10) ? p : xtex_level_pitch(code, w);
    return off;
}
/* (pTexture, Level) -> surface sharing the texture's memory, Parent = texture. */
static uint32_t get_surface_level(D3dFrame *f, const uint32_t *a) {
    (void)f;
    if (!a[0]) return 0;
    uint32_t w, h, pitch, off = level_offset(a[0], a[1], &w, &h, &pitch), fm = U32(a[0], 0xc);
    uint32_t s = hdr_alloc(0x18);
    if (!s) return 0;
    U32(s, 0) = 1 | COMMON_SURFACE | COMMON_D3DCREATED;
    U32(s, 4) = U32(a[0], 4) + off;
    U32(s, 0xc) = (fm & 0x0000FFFFu & ~FMT_CUBEMAP) | (1u << 16) | (U32(a[0], 0x10) ? 0 : (log2u(w) << 20) | (log2u(h) << 24));
    U32(s, 0x10) = U32(a[0], 0x10) ? (w - 1) | ((h - 1) << 12) | ((pitch / 64 - 1) << 24) : 0;
    U32(s, 0x14) = a[0];
    addref(a[0]);
    return s;
}
/* (pTexture, Level, pLockedRect{Pitch, pBits}, pRect, Flags) */
static uint32_t lock_rect(D3dFrame *f, const uint32_t *a) {
    (void)f;
    if (!a[0] || !a[2]) return D3DERR_INVALIDCALL;
    uint32_t w, h, pitch, off = level_offset(a[0], a[1], &w, &h, &pitch), *lr = G(a[2]);
    uint32_t bits = VA(U32(a[0], 4)) + off, code = FMT_CODE(U32(a[0], 0xc));
    if (a[3] && xtex_fmt_linear(code)) {   /* RECT {left, top, right, bottom} */
        const uint32_t *r = G(a[3]);
        bits += r[1] * pitch + r[0] * xtex_fmt_bpp(code) / 8;
    }
    lr[0] = pitch;
    lr[1] = bits;
    return S_OK;
}
/* (Length) -> index buffer; like the original, header and indices share one allocation and Data
 * is the virtual address right after the 12-byte header (index data is read by the CPU). */
static uint32_t create_index_buffer(D3dFrame *f, const uint32_t *a) {
    (void)f;
    uint32_t ib = hdr_alloc(a[0] + 12);
    if (!ib) return 0;
    U32(ib, 0) = 1 | COMMON_INDEXBUFFER | COMMON_D3DCREATED;
    U32(ib, 4) = ib + 12;
    return ib;
}
/* (pVertexBuffer, Flags) -> pointer to the vertex data (Data | 0x80000000). */
static uint32_t vb_lock(D3dFrame *f, const uint32_t *a) { (void)f; return a[0] ? U32(a[0], 4) | CONTIG_BASE : 0; }

static int readable(uint32_t addr, uint32_t len);
/* ---- shaders, fences, modes ---- */
/* Vertex shaders: the guest object (16 bytes) holds a magic and an index into vshaders, which
 * keeps the parsed declaration and the decoded program (XGAssembleShader microcode, run on the CPU
 * per vertex by src/vsh.c). Without a decodable program the draw
 * falls back to oPos = dp4 with c69..c72 (after three-bone skinning for the skinned declaration,
 * inputs v4/v5 in stream 1) and the vertex diffuse color. */
#define VSH_MAGIC 0x31485356u   /* "VSH1" */
#define MAX_VSHADERS 64
typedef struct {
    GeomLayout layout;
    int valid, skinned, has_prog;
    VshProgram prog;
} VShader;
static VShader vshaders[MAX_VSHADERS];
static int nvshaders;
/* (pDeclaration, pFunction, pHandle, Usage): handles of programmable shaders are odd. */
static uint32_t create_vertex_shader(D3dFrame *f, const uint32_t *a) {
    (void)f;
    uint32_t obj = hdr_alloc(16);
    if (!obj) return E_OUTOFMEMORY;
    if (nvshaders < MAX_VSHADERS && a[0]) {
        VShader *v = &vshaders[nvshaders];
        v->valid = !geom_layout_from_decl(G(a[0]), 64, &v->layout);
        const GeomElement *bi = geom_find(&v->layout, 4), *bw = geom_find(&v->layout, 5);
        v->skinned = v->valid && a[1] && bi && bw && bi->stream == 1;
        /* Header (instruction count in bits 16-31), then four dwords per instruction. */
        uint32_t n = a[1] && readable(a[1], 4) ? *(const uint32_t *)G(a[1]) >> 16 : 0;
        v->has_prog = n && readable(a[1], (1 + 4 * n) * 4) && !vsh_load(G(a[1]), 1 + 4 * n, &v->prog);
        U32(obj, 0) = VSH_MAGIC;
        U32(obj, 4) = nvshaders++;
    }
    if (a[2]) *(uint32_t *)G(a[2]) = obj | 1;
    return S_OK;
}
static const VShader *vshader_of(uint32_t handle) {
    uint32_t obj = handle & ~1u;
    if (!obj || U32(obj, 0) != VSH_MAGIC || U32(obj, 4) >= (uint32_t)nvshaders) return NULL;
    return &vshaders[U32(obj, 4)];
}
/* (pPSDef, ppShader): D3DPixelShader {RefCount, D3DOwned, pPSDef} plus a copy of the definition. */
static uint32_t create_pixel_shader(D3dFrame *f, const uint32_t *a) {
    (void)f;
    uint32_t obj = hdr_alloc(16 + 0x100);
    if (!obj) return E_OUTOFMEMORY;
    U32(obj, 0) = 1;
    U32(obj, 4) = 1;
    U32(obj, 8) = obj + 16;
    if (a[0]) memcpy(G(obj + 16), G(a[0]), 0xF0);   /* sizeof(D3DPIXELSHADERDEF) = 0xF0 [I] */
    if (a[1]) *(uint32_t *)G(a[1]) = obj;
    return S_OK;
}
static uint32_t insert_fence(D3dFrame *f, const uint32_t *a) { (void)f; (void)a; return __atomic_add_fetch(&st.fence, 1, __ATOMIC_SEQ_CST); }
static uint32_t mode_count(D3dFrame *f, const uint32_t *a) { (void)f; (void)a; return 1; }
/* (Adapter, Mode, pMode) -> D3DDISPLAYMODE {Width, Height, RefreshRate, Flags, Format}: one
 * 640x480 interlaced 60 Hz mode. */
static uint32_t enum_modes(D3dFrame *f, const uint32_t *a) {
    (void)f;
    if (a[1] >= 1 || !a[2]) return D3DERR_INVALIDCALL;
    uint32_t *m = G(a[2]);
    m[0] = 640; m[1] = 480; m[2] = 60; m[3] = 0x20; m[4] = 0x1E;
    return S_OK;
}

/* ---- Clear / Swap ---- */
static void fill_rect(uint32_t surf, int x0, int y0, int x1, int y1, uint32_t color, uint32_t mask) {
    uint32_t w, h, pitch, code = FMT_CODE(U32(surf, 0xc));
    if (xtex_fmt_bpp(code) != 32 || !U32(surf, 4)) return;
    res_dims(surf, &w, &h, &pitch);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)w) x1 = w;
    if (y1 > (int)h) y1 = h;
    uint8_t *base = G(VA(U32(surf, 4)));
    for (int y = y0; y < y1; y++) {
        uint32_t *row = (uint32_t *)(base + (size_t)y * pitch);
        for (int x = x0; x < x1; x++) row[x] = (row[x] & ~mask) | (color & mask);
    }
}
/* (Count, pRects, Flags, Color, Z, Stencil): fills the render target (whole surface, or the
 * D3DRECTs {x1, y1, x2, y2}); depth/stencil clears are recorded only. */
static uint32_t clear(D3dFrame *f, const uint32_t *a) {
    (void)f;
    uint32_t fl = a[2];
    if (!st.rt) return S_OK;
    int off = st.rt != st.surf[0];
    if (off && hle_d3d_render_targets && (st.target_c || st.target_z) && (fl & (CLEAR_TARGET | 1))) {
        FrameOp *op = frame_op(&st.rec);   /* a title render target: replayed in its offscreen pass */
        if (op) {
            op->kind = OP_CLEAR;
            op->flags = (fl & CLEAR_TARGET ? RENDERER_CLEAR_COLOR : 0) | (fl & 1 ? RENDERER_CLEAR_DEPTH : 0);
            op->color = a[3];
            memcpy(&op->z, &a[4], 4);
            op->target_c = st.target_c;
            op->target_z = st.target_z;
        }
    }
    if (!(fl & CLEAR_TARGET)) {   /* depth/stencil only: nothing to fill on the CPU */
        FrameOp *op = !off && (fl & 1) ? frame_op(&st.rec) : NULL;
        if (op) {
            op->kind = OP_CLEAR;
            op->flags = RENDERER_CLEAR_DEPTH;
            memcpy(&op->z, &a[4], 4);
        }
        return S_OK;
    }
    uint32_t mask = (fl & 0x10 ? 0x00FF0000u : 0) | (fl & 0x20 ? 0x0000FF00u : 0) | (fl & 0x40 ? 0x000000FFu : 0) |
                    (fl & 0x80 ? 0xFF000000u : 0);
    pthread_mutex_lock(&d3d_lock);
    st.clear_color = a[3];
    pthread_mutex_unlock(&d3d_lock);
    if (st.rt == st.surf[0]) {   /* only the back buffer is rendered by the host renderer so far */
        FrameOp *op = frame_op(&st.rec);
        if (op) {
            op->kind = OP_CLEAR;
            op->flags = (fl & CLEAR_TARGET ? RENDERER_CLEAR_COLOR : 0) | (fl & 1 ? RENDERER_CLEAR_DEPTH : 0);
            op->color = a[3];
            memcpy(&op->z, &a[4], 4);
        }
    }
    if (!a[0] || !a[1]) {
        fill_rect(st.rt, 0, 0, 0x7FFFFFFF, 0x7FFFFFFF, a[3], mask);
    } else {
        const int32_t *r = G(a[1]);
        for (uint32_t i = 0; i < a[0]; i++, r += 4) fill_rect(st.rt, r[0], r[1], r[2], r[3], a[3], mask);
    }
    return S_OK;
}

/* ---- world-update cadence (--stats; game-specific addresses) ----
 * The Warriors: the step loop FUN_002b2430 runs fixed 1/60 s steps in pairs; each step calls
 * FUN_0016d8d0, which increments the dword at 0x5825fc and runs the world update (AI, characters,
 * cameras) only when the new value is even. Read at each Swap, its change gives the world updates
 * shown by that frame (src/pacing.h). The game clock object is [0x4aca1c] (vtable 0x41dea4, time
 * from QueryPerformanceCounter); [obj+0x40] is its millisecond clock, compared with host time per
 * window as a game-speed check. Both are read only when mapped; an unreadable counter breaks the
 * sequence instead of counting. */
#define WARRIORS_STEP_COUNTER 0x5825fcu
#define WARRIORS_CLOCK_OBJ 0x4aca1cu
#define WARRIORS_CLOCK_MS 0x40u
#define PACING_WINDOW 300u   /* presented frames per [pacing] line */
static int readable(uint32_t addr, uint32_t len);
static struct {
    PacingStats all, mark;
    double host_ms;
    uint32_t guest_ms;
    int have_ms;
} pst;
static int guest_clock_ms(uint32_t *ms) {
    if (!readable(WARRIORS_CLOCK_OBJ, 4)) return 0;
    uint32_t obj = U32(WARRIORS_CLOCK_OBJ, 0);
    if (!readable(obj + WARRIORS_CLOCK_MS, 4)) return 0;
    *ms = U32(obj, WARRIORS_CLOCK_MS);
    return 1;
}
static void pacing_frame(uint32_t n, int last) {
    /* With --sim60 every step is a world update: counted as if each one were an even value. */
    uint32_t per_step = hle_sim60_active ? 2u : 1u;
    if (readable(WARRIORS_STEP_COUNTER, 4)) pacing_stats_frame(&pst.all, U32(WARRIORS_STEP_COUNTER, 0) * per_step);
    else pacing_stats_break(&pst.all);
    if (n % PACING_WINDOW && !last) return;
    PacingStats w;
    char line[160], speed[96] = "";
    pacing_stats_diff(&pst.all, &pst.mark, &w);
    pacing_stats_format(&w, line, sizeof line);
    double t = now_ms();
    uint32_t ms;
    int have = guest_clock_ms(&ms);
    if (have && pst.have_ms && t > pst.host_ms)
        snprintf(speed, sizeof speed, " | guest clock +%u ms in %.0f ms host (x%.4f)", ms - pst.guest_ms, t - pst.host_ms,
                 (ms - pst.guest_ms) / (t - pst.host_ms));
    uint64_t presents, resyncs;
    char lock[64] = " | frame lock off";
    if (hle_xapi_frame_lock_stats(&presents, &resyncs)) snprintf(lock, sizeof lock, " | frame lock: %llu resyncs", (unsigned long long)resyncs);
    printf("[pacing] swap %u: %s | total 2 frames %.1f%%%s%s\n", n, line, 100.0 * pacing_stats_hold2(&pst.all), speed, lock);
    if (last) {
        pacing_stats_format(&pst.all, line, sizeof line);
        printf("[pacing] total at swap %u: %s\n", n, line);
    }
    pst.mark = pst.all;
    pst.host_ms = t;
    pst.guest_ms = ms;
    pst.have_ms = have;
}

/* --rate-probe (rateprobe.h): snapshots keyed to the same title ms clock, taken at Swap. */
static const void *probe_read(uint32_t addr, uint32_t len, void *ctx) {
    (void)ctx;
    return readable(addr, len) ? G(addr) : NULL;
}
static void rate_probe_frame(uint32_t n, int last) {
    static int stopped;
    uint32_t ms, snaps;
    uint64_t bytes;
    int failed = !stopped && guest_clock_ms(&ms) && rateprobe_frame(rateprobe_active, n, ms, probe_read, NULL) < 0;
    if (failed) stopped = 1;
    if (!failed && !last) return;
    rateprobe_counts(rateprobe_active, &snaps, &bytes);
    printf("[rateprobe] %s%u snapshots, %llu bytes at swap %u\n", failed ? "write failed, probe stopped: " : "", snaps,
           (unsigned long long)bytes, n);
}

static void list_frame_calls(void);
static void reset_frame_calls(void);
/* (Flags) -> swap count. Posts the back buffer to the host video thread, then paces the caller
 * to a 60 Hz vertical blank. Once the frame limit is reached the caller blocks until exit. */
static void vp_wait_all(void);
static uint32_t swap(D3dFrame *f, const uint32_t *a) {
    (void)f;
    uint32_t flags = a[0] ? a[0] : 5;   /* 0 = D3DSWAP_DEFAULT */
    if (flags & 4) U32(D3D_DIRTY_FLAGS, 0) |= 0xff1000u;
    uint32_t bb = st.surf[0], w = 0, h = 0, pitch = 0;
    if (bb) res_dims(bb, &w, &h, &pitch);
    vp_wait_all();   /* the frame's vertex program outputs are complete before it is handed over */
    double t_swap = hle_d3d_stats ? now_ms() : 0, waited = 0;
    pthread_mutex_lock(&d3d_lock);
    uint32_t n = ++st.frames;
    if (bb) U32(D3D_DEVICE, DEV_SWAP_COUNT) = n;
    if (hle_d3d_rain.on) {   /* layers not drawn in this frame start their scroll over */
        rain_scroll_frame(&st.rain_scroll);
        st.rain_layers = 0;
    }
    if (w && h && (st.fw != w || st.fh != h)) {
        free(st.frame);
        st.frame = malloc((size_t)w * h * 4);
        st.fw = w;
        st.fh = h;
    }
    if (st.frame) {
        if (xtex_fmt_bpp(FMT_CODE(U32(bb, 0xc))) == 32)
            for (uint32_t y = 0; y < h; y++) memcpy(st.frame + y * w, (uint8_t *)G(VA(U32(bb, 4))) + (size_t)y * pitch, w * 4);
        else
            for (uint32_t i = 0; i < w * h; i++) st.frame[i] = st.clear_color;
        st.frame_seq++;
    }
    if (st.frame) {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);   /* queue_space uses the default (realtime) clock */
        dl.tv_nsec += SWAP_WAIT_NS;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        while (st.q_count == FRAME_QUEUE && pthread_cond_timedwait(&queue_space, &d3d_lock, &dl) == 0) {}
        if (st.q_count == FRAME_QUEUE) {   /* the host is stalled: drop the oldest frame */
            frame_px_recycle(st.queue[st.q_head].px, (size_t)st.queue[st.q_head].w * st.queue[st.q_head].h * 4);
            frame_list_recycle(&st.queue[st.q_head].ops);
            st.q_head = (st.q_head + 1) % FRAME_QUEUE;
            st.q_count--;
            st.dropped++;
        }
        uint32_t i = (st.q_head + st.q_count) % FRAME_QUEUE;
        st.queue[i].px = frame_px_take((size_t)w * h * 4);
        if (st.queue[i].px) {
            memcpy(st.queue[i].px, st.frame, (size_t)w * h * 4);
            st.queue[i].w = w;
            st.queue[i].h = h;
            st.queue[i].seq = st.frame_seq;
            st.queue[i].ops = st.rec;
            st.queue[i].t_read = gstat.read;
            st.queue[i].read_age = gstat.read_age;
            st.queue[i].t_swap = t_swap;
            st.q_count++;
            /* Hand-off: return once the host has taken the frame (same deadline), so the title's
             * next input read is no older than the frame the host starts next. */
            while (st.q_count == FRAME_QUEUE && pthread_cond_timedwait(&queue_space, &d3d_lock, &dl) == 0) {}
        } else {
            frame_list_recycle(&st.rec);
            st.dropped++;
        }
        if (hle_d3d_stats) waited = now_ms() - t_swap;
    } else {
        frame_list_recycle(&st.rec);
    }
    frame_list_take(&st.rec);
    uint32_t put = st.scratch ? U32(D3D_DEVICE, DEV_PUT) : 0;
    if (put && put != st.scratch) {   /* library code we did not replace wrote push-buffer data */
        if (!st.pb_warned++) printf("[d3d] unreplaced D3D code wrote %u push-buffer bytes before swap %u\n", put - st.scratch, n);
        U32(D3D_DEVICE, DEV_PUT) = st.scratch;
    }
    uint32_t color = st.clear_color;
    if (hle_d3d_stats && gstat.frame_start) {
        gstat.g_frame += t_swap - gstat.frame_start;
        gstat.g_record += gstat.record;
        gstat.g_wait += waited;
        gstat.g_verts += gstat.verts;
        gstat.g_swaps++;
        for (int i = 0; i < REC_PHASES; i++) gstat.g_ph[i] += gstat.ph[i];
        gstat.g_draws += gstat.draws;
        gstat.g_rd_calls += gstat.rd_calls;
        gstat.g_rd_ms += gstat.rd_ms;
    }
    gstat.record = 0;
    gstat.verts = 0;
    memset(gstat.ph, 0, sizeof gstat.ph);
    gstat.draws = gstat.rd_calls = 0;
    gstat.rd_ms = 0;
    gstat.read = 0;
    pthread_mutex_unlock(&d3d_lock);
    input_frame(n);   /* --input-script steps are keyed to the Swap number */
    if (n <= 3 || n % 60 == 0) printf("[d3d] frame %u presented (%ux%u, clear color %08x)\n", n, w, h, color);
    if (n == 2) list_frame_calls();
    /* 60 Hz pacing (D3DPRESENT_INTERVAL_ONE). */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (st.next_vblank.tv_sec == 0 || now.tv_sec > st.next_vblank.tv_sec + 1) st.next_vblank = now;
    st.next_vblank.tv_nsec += 16666667;
    if (st.next_vblank.tv_nsec >= 1000000000) { st.next_vblank.tv_sec++; st.next_vblank.tv_nsec -= 1000000000; }
    /* With the frame lock, a frame more than half a period late restarts the schedule from now
     * instead of presenting the following frames back to back to catch up (each would advance
     * the frame-locked clock by a whole period). A frame only slightly late keeps the grid: the
     * next one waits less, so a little jitter does not lose time and the rate stays at 60. */
    if (hle_xapi_frame_lock &&
        (int64_t)(now.tv_sec - st.next_vblank.tv_sec) * 1000000000 + (now.tv_nsec - st.next_vblank.tv_nsec) > 8333333)
        st.next_vblank = now;
    plat_sleep_until(&st.next_vblank);
    hle_xapi_frame_presented();   /* the frame-locked QueryPerformanceCounter advances one period */
    if (rateprobe_active) rate_probe_frame(n, hle_d3d_frame_limit && n >= (uint32_t)hle_d3d_frame_limit);
    if (hle_d3d_stats) {
        gstat.frame_start = now_ms();
        pacing_frame(n, hle_d3d_frame_limit && n >= (uint32_t)hle_d3d_frame_limit);
    }
    if (hle_d3d_frame_limit && n >= (uint32_t)hle_d3d_frame_limit) {
        st.done = 1;
        plat_park();   /* the host main thread presents this frame and exits */
    }
    return n;
}

/* ---- bound state the CPU movie quad needs ---- */
/* (Stage, pTexture). The title's own wrapper (0xb8f50) skips the call when the stage already
 * holds that texture, so the last value per stage is the bound one. */
static uint32_t set_texture(D3dFrame *f, const uint32_t *a) { (void)f; if (a[0] < 4) st.tex[a[0]] = a[1]; return S_OK; }
/* (pPSDef): the D3DPIXELSHADERDEF in guest memory, 0 = fixed function. */
static uint32_t set_ps_program(D3dFrame *f, const uint32_t *a) { (void)f; st.ps_def = a[0]; return S_OK; }
/* (Handle): an object from CreatePixelShader ({RefCount, D3DOwned, pPSDef}) or 0. */
static uint32_t set_pixel_shader(D3dFrame *f, const uint32_t *a) { (void)f; st.ps_def = a[0] ? U32(a[0], 8) : 0; return S_OK; }
static uint32_t set_vertex_shader(D3dFrame *f, const uint32_t *a) { (void)f; st.vshader = a[0]; return S_OK; }
static int readable(uint32_t addr, uint32_t len);
/* (Register, pConstantData, ConstantCount): float4 constants, kept as the ARGB8888 values the
 * combiners load (psh_argb). The title sets c0/c1 before its shadowed draws (T1). */
static uint32_t set_ps_constant(D3dFrame *f, const uint32_t *a) {
    (void)f;
    for (uint32_t i = 0; i < a[2] && a[0] + i < PSH_CONSTANTS; i++)
        if (readable(a[1] + 16 * i, 16)) st.psc[a[0] + i] = psh_argb(G(a[1] + 16 * i));
    return S_OK;
}
/* (Value): D3DRS_FOGCOLOR, ARGB. */
static uint32_t set_fog_color(D3dFrame *f, const uint32_t *a) { (void)f; st.fog_color = a[0]; return S_OK; }

/* ---- vertex state for the host renderer ---- */
/* (StreamNumber, pStreamData, Stride) */
static uint32_t set_stream_source(D3dFrame *f, const uint32_t *a) {
    (void)f;
    if (a[0] < 4) { st.stream[a[0]].vb = a[1]; st.stream[a[0]].stride = a[2]; }
    return S_OK;
}
/* (State, pMatrix), Xbox numbering: VIEW 0, PROJECTION 1, TEXTURE0-3 2-5, WORLD 6. */
static uint32_t set_transform(D3dFrame *f, const uint32_t *a) {
    (void)f;
    float *m = a[0] == 0 ? st.view : a[0] == 1 ? st.proj : a[0] == 6 ? st.world : NULL;
    if (m && a[1]) memcpy(m, G(a[1]), 64);
    return S_OK;
}
/* (pViewport) -> D3DVIEWPORT8 {X, Y, Width, Height, MinZ, MaxZ} */
static uint32_t set_viewport(D3dFrame *f, const uint32_t *a) {
    (void)f;
    if (!a[0]) return S_OK;
    const uint32_t *v = G(a[0]);
    st.vp = (RViewport){(float)v[0], (float)v[1], (float)v[2], (float)v[3], 0, 1};
    memcpy(&st.vp.minz, &v[4], 4);
    memcpy(&st.vp.maxz, &v[5], 4);
    return S_OK;
}
/* Register arguments: ecx = NV2A method header (count << 18 | method), edx = value. The title sets
 * its simple states this way; kept:
 * the alpha test and the output state of RState (methods from xemu hw/xbox/nv2a/nv2a_regs.h). */
#define NV097_SET_ALPHA_TEST_ENABLE 0x300u
#define NV097_SET_BLEND_ENABLE 0x304u
#define NV097_SET_ALPHA_FUNC 0x33cu   /* GL compare enums 0x200 NEVER .. 0x207 ALWAYS */
#define NV097_SET_ALPHA_REF 0x340u
#define NV097_SET_BLEND_FUNC_SFACTOR 0x344u
#define NV097_SET_BLEND_FUNC_DFACTOR 0x348u
#define NV097_SET_BLEND_COLOR 0x34cu
#define NV097_SET_BLEND_EQUATION 0x350u
#define NV097_SET_DEPTH_FUNC 0x354u
#define NV097_SET_COLOR_MASK 0x358u
#define NV097_SET_DEPTH_MASK 0x35cu
static uint32_t set_render_state_simple(D3dFrame *f, const uint32_t *a) {
    (void)a;
    uint32_t v = f->edx;
    switch (f->ecx & 0x1FFCu) {
    case NV097_SET_ALPHA_TEST_ENABLE: st.alpha_test = v; break;
    case NV097_SET_ALPHA_FUNC: st.alpha_func = v; break;
    case NV097_SET_ALPHA_REF: st.alpha_ref = v; break;
    case NV097_SET_BLEND_ENABLE: st.rs.blend_enable = v; break;
    case NV097_SET_BLEND_FUNC_SFACTOR: st.rs.blend_src = v; break;
    case NV097_SET_BLEND_FUNC_DFACTOR: st.rs.blend_dst = v; break;
    case NV097_SET_BLEND_COLOR: st.rs.blend_color = v; break;
    case NV097_SET_BLEND_EQUATION: st.rs.blend_eq = v; break;
    case NV097_SET_DEPTH_FUNC: st.rs.depth_func = v; break;
    case NV097_SET_COLOR_MASK: st.rs.color_mask = v; break;
    case NV097_SET_DEPTH_MASK: st.rs.depth_write = v; break;
    }
    return S_OK;
}
/* (Value): D3DCULL_NONE 0, D3DCULL_CW 0x900, D3DCULL_CCW 0x901 (Xbox values). */
static uint32_t set_cull_mode(D3dFrame *f, const uint32_t *a) { (void)f; st.rs.cull = a[0]; return S_OK; }
void hle_d3d_render_state(RState *out) { *out = st.rs; }
uint32_t hle_d3d_recorded(const RVertex **v, uint32_t *nv, uint32_t *ni) {
    vp_wait_all();
    const FrameList *l = &st.rec;
    const FrameOp *op = NULL;
    for (uint32_t i = l->nops; i-- > 0;)
        if (l->ops[i].kind == OP_DRAW) { op = &l->ops[i]; break; }
    *v = op ? l->v + op->first_vertex : NULL;
    *nv = op ? op->nv : 0;
    *ni = op ? op->ni : 0;
    return l->draws;
}
int hle_d3d_last_draw(HleD3dDrawInfo *d) {
    const FrameList *l = &st.rec;
    memset(d, 0, sizeof *d);
    for (uint32_t i = l->nops; i-- > 0;) {
        const FrameOp *op = &l->ops[i];
        if (op->kind != OP_DRAW) continue;
        d->target_color = op->target_c ? rt_surf[op->target_c - 1].pa : 0;
        d->target_depth = op->target_z ? rt_surf[op->target_z - 1].pa : 0;
        for (int s = 0; s < RENDERER_STAGES; s++) d->stage_surface[s] = op->tex_surf[s] ? rt_surf[op->tex_surf[s] - 1].pa : 0;
        d->tex_modes = op->has_ps ? op->ps.tex_modes : 0;
        d->shadow_func = op->has_ps ? op->ps.shadow_func : 0;
        d->vp_w = (uint32_t)op->vp.w;
        d->vp_h = (uint32_t)op->vp.h;
        return 1;
    }
    return 0;
}
void hle_d3d_present_stats(uint32_t *presented, uint32_t *dropped) { *presented = st.presented; *dropped = st.dropped; }
/* Fixed-function lighting state: (Index, pLight) D3DLIGHT8, (Index, Enable), (pMaterial)
 * D3DMATERIAL8. The library versions would also program the NV2A; the draws use these copies. */
static uint32_t set_light(D3dFrame *f, const uint32_t *a) {
    (void)f;
    if (a[0] < MAX_LIGHTS && readable(a[1], sizeof(GeomLight))) memcpy(&st.light[a[0]], G(a[1]), sizeof(GeomLight));
    return S_OK;
}
static uint32_t light_enable(D3dFrame *f, const uint32_t *a) {
    (void)f;
    if (a[0] < MAX_LIGHTS) st.light_on[a[0]] = a[1] != 0;
    return S_OK;
}
static uint32_t set_material(D3dFrame *f, const uint32_t *a) {
    (void)f;
    if (readable(a[0], sizeof(GeomMaterial))) memcpy(&st.material, G(a[0]), sizeof(GeomMaterial));
    return S_OK;
}
/* Fixed-function lighting of the next FVF draw from the D3D render states the title writes into
 * D3D_g_RenderState (Xbox D3DRS numbering, Cxbx-Reloaded XbD3D8Types.h; the deferred states are
 * only consumed by library code the HLE replaces) and the camera of the view matrix. Returns 0
 * when D3DRS_LIGHTING is off. */
#define D3D_RENDER_STATE 0x0037d6e0u   /* D3D8__D3D_g_RenderState */
#define RS(i) U32(D3D_RENDER_STATE, (i) * 4)
enum { RS_LIGHTING = 102, RS_SPECULARENABLE = 103, RS_LOCALVIEWER = 104, RS_COLORVERTEX = 105, RS_SPECULARMATERIALSOURCE = 110,
       RS_DIFFUSEMATERIALSOURCE = 111, RS_AMBIENTMATERIALSOURCE = 112, RS_EMISSIVEMATERIALSOURCE = 113, RS_AMBIENT = 115,
       RS_FOGENABLE = 92, RS_FOGTABLEMODE = 93, RS_FOGSTART = 94, RS_FOGEND = 95, RS_FOGDENSITY = 96, RS_RANGEFOGENABLE = 97 };
static float rs_float(int i) { float f; uint32_t u = RS(i); memcpy(&f, &u, 4); return f; }
/* Fog amount (1 - fog factor) of a vertex at fog distance d: D3DRS_FOGENABLE with the table mode,
 * start, end and density of D3D_g_RenderState (geom_fog_factor). */
typedef struct { int on, range; uint32_t mode; float start, end, density; } DrawFog;
static void draw_fog(DrawFog *f) {
    f->on = RS(RS_FOGENABLE) != 0;
    f->mode = RS(RS_FOGTABLEMODE);
    f->range = RS(RS_RANGEFOGENABLE) != 0;
    f->start = rs_float(RS_FOGSTART);
    f->end = rs_float(RS_FOGEND);
    f->density = rs_float(RS_FOGDENSITY);
}
static float fog_amount(const DrawFog *f, float d) { return f->on ? 1 - geom_fog_factor(f->mode, f->start, f->end, f->density, d) : 0; }

/* --vp-threads: a draw's vertex program batches are independent (own inputs, outputs and
 * interpreter registers), so large draws split them across worker threads; the recording thread
 * takes part and returns only when every batch is done, so the draw's output is unchanged. */
typedef struct {
    const VshProgram *prog;
    const GeomLayout *l;
    uint32_t fed, lo, nv;
    const uint8_t *base[4];
    uint32_t stride[4];
    const float (*vsc)[4];
    RVertex *out;
    int real_color;
    const GeomElement *nrm;
    const DrawFog *fog;
    const float (*tscale)[2];
} VpJob;

static void vp_batch(const VpJob *jb, uint32_t i0) {
    static __thread float in[16][4][VSH_BATCH], vo[VSH_NOUT][4][VSH_BATCH];
    int cnt = jb->nv - i0 < VSH_BATCH ? (int)(jb->nv - i0) : VSH_BATCH;
    for (int r = 0; r < 16; r++)
        for (int j = 0; j < cnt && !(jb->fed >> r & 1); j++) { in[r][0][j] = in[r][1][j] = in[r][2][j] = 0; in[r][3][j] = 1; }
    for (int k = 0; k < jb->l->n; k++) {
        const GeomElement *e = &jb->l->e[k];
        if (e->reg >= 16) continue;
        const uint8_t *src = jb->base[e->stream] + (jb->lo + i0) * jb->stride[e->stream] + e->offset;
        geom_fetch_rows(e->type, src, jb->stride[e->stream], cnt, &in[e->reg][0][0], VSH_BATCH);
    }
    vsh_run_batch(jb->prog, cnt, (const float (*)[4][VSH_BATCH])in, jb->vsc, vo);
    for (int j = 0; j < cnt; j++) {
        RVertex *o = &jb->out[i0 + j];
        float pos4[4], c[4], col[4];
        memset(o, 0, sizeof *o);
        for (int q = 0; q < 4; q++) pos4[q] = vo[VSH_OPOS][q][j];
        vsh_clip(pos4, c);
        memcpy(&o->x, c, 16);
        if (jb->real_color) {
            for (int q = 0; q < 4; q++) col[q] = vo[VSH_OD0][q][j];
            o->color = psh_argb(col);
        } else {
            float nn[4], p0[4];
            for (int q = 0; q < 4; q++) { p0[q] = in[0][q][j]; if (jb->nrm) nn[q] = in[jb->nrm->reg][q][j]; }
            o->color = geom_debug_color(jb->nrm ? nn : NULL, p0) | 0xFF000000u;
        }
        for (int q = 0; q < 4; q++) col[q] = vo[VSH_OD1][q][j];
        o->specular = psh_argb(col);
        o->fog = fog_amount(jb->fog, vo[VSH_OFOG][0][j]);   /* oFog.x: the fog distance (xemu vsh.c) */
        for (int t = 0; t < RENDERER_STAGES; t++) {
            for (int q = 0; q < 4; q++) o->tex[t][q] = vo[VSH_OT0 + t][q][j];
            o->tex[t][0] *= jb->tscale[t][0];
            o->tex[t][1] *= jb->tscale[t][1];
        }
    }
}

int hle_d3d_vp_threads = -1;   /* worker threads; -1 = auto (cores - 1, at most 7), 0 = off */
#define VP_SPIN 4000                 /* idle polls before a worker sleeps (draws come back to back) */
#define VP_MAX_JOBS 8192
#define VP_MAX_ITEMS 65536
#define VP_ARENA (24u << 20)         /* per-frame copies of each draw's inputs and constants */
/* With workers, a draw does not wait for its vertex programs: its inputs (the stream bytes it
 * reads, the constants, layout, fog and texture scales) are copied into vp_arena and its batches
 * are queued; the outputs land in the recording list's vertex array, which is only read at Swap
 * (vp_wait_all) and never reallocated while batches are pending. vp_claim and vp_pub pack a
 * generation (high half, bumped at every vp_wait_all) with an item index, so a worker can never
 * claim an item of a newer generation it has not seen published. */
typedef struct { uint32_t job, batch; } VpItem;
static VpJob vp_jobs[VP_MAX_JOBS];
static VpItem vp_items[VP_MAX_ITEMS];
static uint8_t *vp_arena;
static uint32_t vp_njobs, vp_arena_used, vp_gen = 1, vp_finished;
static uint64_t vp_claim = 1ull << 32, vp_pub = 1ull << 32;
static pthread_mutex_t vp_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vp_go = PTHREAD_COND_INITIALIZER;
static int vp_sleepers, vp_workers = -1;

static int vp_take(void) {   /* runs one published item; 0 when none is available */
    uint64_t c = __atomic_load_n(&vp_claim, __ATOMIC_ACQUIRE);
    for (;;) {
        uint64_t p = __atomic_load_n(&vp_pub, __ATOMIC_ACQUIRE);
        if ((uint32_t)(c >> 32) != (uint32_t)(p >> 32) || (uint32_t)c >= (uint32_t)p) return 0;
        if (__atomic_compare_exchange_n(&vp_claim, &c, c + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
    }
    const VpItem it = vp_items[(uint32_t)c];
    vp_batch(&vp_jobs[it.job], it.batch * VSH_BATCH);
    __atomic_fetch_add(&vp_finished, 1, __ATOMIC_RELEASE);
    return 1;
}

static void *vp_worker(void *arg) {
    (void)arg;
    for (;;) {
        int idle = 0;
        while (!vp_take())
            if (++idle < VP_SPIN) __builtin_ia32_pause();
            else {
                pthread_mutex_lock(&vp_mu);
                __atomic_add_fetch(&vp_sleepers, 1, __ATOMIC_SEQ_CST);   /* pairs with the seq_cst publish in vp_run */
                uint64_t c = __atomic_load_n(&vp_claim, __ATOMIC_SEQ_CST), p = __atomic_load_n(&vp_pub, __ATOMIC_SEQ_CST);
                if ((uint32_t)(c >> 32) != (uint32_t)(p >> 32) || (uint32_t)c >= (uint32_t)p) pthread_cond_wait(&vp_go, &vp_mu);
                __atomic_sub_fetch(&vp_sleepers, 1, __ATOMIC_SEQ_CST);
                pthread_mutex_unlock(&vp_mu);
                idle = 0;
            }
    }
    return NULL;
}

/* Runs every queued batch to completion (the recording thread helps) and starts a new generation. */
static void vp_wait_all(void) {
    if (vp_workers <= 0) return;
    uint32_t n = (uint32_t)__atomic_load_n(&vp_pub, __ATOMIC_ACQUIRE);
    if (!n) return;
    while (vp_take()) {}
    while (__atomic_load_n(&vp_finished, __ATOMIC_ACQUIRE) < n) __builtin_ia32_pause();
    vp_gen++;
    __atomic_store_n(&vp_finished, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&vp_claim, (uint64_t)vp_gen << 32, __ATOMIC_RELEASE);
    __atomic_store_n(&vp_pub, (uint64_t)vp_gen << 32, __ATOMIC_RELEASE);
    vp_njobs = vp_arena_used = 0;
}

static void *vp_alloc(uint32_t size) {   /* NULL when the arena is full (the caller drains first) */
    size = (size + 15) & ~15u;
    if (vp_arena_used + size > VP_ARENA) return NULL;
    void *r = vp_arena + vp_arena_used;
    vp_arena_used += size;
    return r;
}

static void vp_start(void) {
    int n = hle_d3d_vp_threads;
    if (n < 0) {
#ifdef _WIN32
        n = pthread_num_processors_np() - 1;
#else
        n = (int)sysconf(_SC_NPROCESSORS_ONLN) - 1;
#endif
        if (n > 7) n = 7;
    }
    vp_workers = 0;
    if (n > 0 && !(vp_arena = malloc(VP_ARENA))) n = 0;
    for (int i = 0; i < n; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, vp_worker, NULL) == 0) vp_workers++;
    }
    printf("[d3d] vertex programs: %d worker thread%s\n", vp_workers, vp_workers == 1 ? "" : "s");
}

static void vp_run(const VpJob *jb) {
    uint32_t nb = (jb->nv + VSH_BATCH - 1) / VSH_BATCH;
    if (vp_workers < 0) vp_start();
    if (vp_workers == 0) {
        for (uint32_t b = 0; b < nb; b++) vp_batch(jb, b * VSH_BATCH);
        return;
    }
    uint32_t bytes[4] = {0}, need = sizeof(GeomLayout) + sizeof(DrawFog) + RENDERER_STAGES * 2 * sizeof(float) + VSH_NCONST * 16 + 16 * 8;
    for (int k = 0; k < 4; k++)
        if (jb->base[k]) need += bytes[k] = jb->nv * jb->stride[k];
    uint32_t items = (uint32_t)__atomic_load_n(&vp_pub, __ATOMIC_RELAXED);
    if (need > VP_ARENA) {   /* too large to copy: run it here, in place */
        for (uint32_t b = 0; b < nb; b++) vp_batch(jb, b * VSH_BATCH);
        return;
    }
    if (vp_njobs == VP_MAX_JOBS || items + nb > VP_MAX_ITEMS || vp_arena_used + need > VP_ARENA) vp_wait_all();
    VpJob *q = &vp_jobs[vp_njobs];
    *q = *jb;
    GeomLayout *l = vp_alloc(sizeof *l);
    *l = *jb->l;
    q->l = l;
    if (jb->nrm) q->nrm = &l->e[jb->nrm - jb->l->e];
    DrawFog *fog = vp_alloc(sizeof *fog);
    *fog = *jb->fog;
    q->fog = fog;
    float (*ts)[2] = vp_alloc(RENDERER_STAGES * 2 * sizeof(float));
    memcpy(ts, jb->tscale, RENDERER_STAGES * 2 * sizeof(float));
    q->tscale = (const float (*)[2])ts;
    float (*c)[4] = vp_alloc(VSH_NCONST * 16);
    memcpy(c, jb->vsc, VSH_NCONST * 16);
    q->vsc = (const float (*)[4])c;
    for (int k = 0; k < 4; k++)
        if (jb->base[k]) {
            uint8_t *d = vp_alloc(bytes[k]);
            memcpy(d, jb->base[k] + (size_t)jb->lo * jb->stride[k], bytes[k]);
            q->base[k] = d;
        }
    q->lo = 0;
    items = (uint32_t)__atomic_load_n(&vp_pub, __ATOMIC_RELAXED);
    for (uint32_t b = 0; b < nb; b++) vp_items[items + b] = (VpItem){vp_njobs, b};
    vp_njobs++;
    __atomic_store_n(&vp_pub, ((uint64_t)vp_gen << 32) | (items + nb), __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&vp_sleepers, __ATOMIC_SEQ_CST)) {
        pthread_mutex_lock(&vp_mu);
        pthread_cond_broadcast(&vp_go);
        pthread_mutex_unlock(&vp_mu);
    }
}

static int draw_lighting(GeomLighting *L) {
    if (!RS(RS_LIGHTING)) return 0;
    memset(L, 0, sizeof *L);
    for (int i = 0; i < MAX_LIGHTS && L->n < GEOM_MAX_LIGHTS; i++)
        if (st.light_on[i]) L->light[L->n++] = st.light[i];
    L->mat = st.material;
    uint32_t amb = RS(RS_AMBIENT);
    for (int k = 0; k < 4; k++) L->ambient[k] = ((amb >> (k == 3 ? 24 : 16 - 8 * k)) & 0xFF) / 255.0f;
    L->dsrc = RS(RS_DIFFUSEMATERIALSOURCE);
    L->asrc = RS(RS_AMBIENTMATERIALSOURCE);
    L->ssrc = RS(RS_SPECULARMATERIALSOURCE);
    L->esrc = RS(RS_EMISSIVEMATERIALSOURCE);
    L->color_vertex = RS(RS_COLORVERTEX) != 0;
    L->specular_enable = RS(RS_SPECULARENABLE) != 0;
    L->local_viewer = RS(RS_LOCALVIEWER) != 0;
    /* View = [R 0; t 1] (row vectors): eye = -t R^T, toward the viewer = -(0, 0, 1) R^T. */
    const float *v = st.view;
    for (int j = 0; j < 3; j++) {
        L->eye[j] = -(v[12] * v[4 * j] + v[13] * v[4 * j + 1] + v[14] * v[4 * j + 2]);
        L->view_dir[j] = -v[4 * j + 2];
    }
    return 1;
}

/* (Value): only these calls are seen; the title also writes D3D_g_RenderState inline [I], which
 * is not read yet. */
static uint32_t set_zenable(D3dFrame *f, const uint32_t *a) { (void)f; st.zenable = a[0]; return S_OK; }
/* (D3DCMPFUNC 0x200-0x207): the library pushes NV097_SET_SHADOW_COMPARE_FUNC = value - 0x200
 * (0x35d7f0). */
static uint32_t set_shadow_func(D3dFrame *f, const uint32_t *a) { (void)f; st.shadow_func = (a[0] - 0x200u) & 7u; return S_OK; }

/* Guest memory [addr, addr + len) is mapped (first and last page). */
/* Both ends of the range are mapped (one check when they share a 4 KB page, the granule of
 * mem_is_mapped; on Linux each check is a mincore system call). */
static int range_mapped(uint32_t addr, uint32_t len) {
    if (!addr || !len || addr + len <= addr) return 0;
    uint32_t last = addr + len - 1;
    return mem_is_mapped(addr) && ((addr >> 12) == (last >> 12) || mem_is_mapped(last));
}
static int readable(uint32_t addr, uint32_t len) {
    if (!gstat.in_rec) return range_mapped(addr, len);
    double t = now_ms();   /* --stats, inside record_draw: counted and timed */
    int r = range_mapped(addr, len);
    gstat.rd_ms += now_ms() - t;
    gstat.rd_calls++;
    return r;
}

/* Register-argument helpers: ecx = register + 96, edx = pointer to the data; Fast1 loads one
 * vector, NotInlineFast takes the dword count on the stack. */
static void set_constants(uint32_t reg, uint32_t data, uint32_t dwords) {
    if (reg >= 192) return;
    if (dwords > (192 - reg) * 4) dwords = (192 - reg) * 4;
    if (!readable(data, dwords * 4)) return;
    memcpy(st.vsc[reg], G(data), dwords * 4);
}
static uint32_t set_vs_constant1(D3dFrame *f, const uint32_t *a) { (void)a; set_constants(f->ecx, f->edx, 4); return S_OK; }
static uint32_t set_vs_constants(D3dFrame *f, const uint32_t *a) { set_constants(f->ecx, f->edx, a[0]); return S_OK; }

/* Texture of a stage for a draw: the header is read now and the texels are keyed by a sparse
 * checksum (xtex_key); the host main thread decodes and uploads them on a cache miss. Cube maps,
 * volumes, palettized, depth (the title's shadow map) and other formats xtex cannot sample give 0
 * (the renderer binds white). The per-format statistics count stage 0. */
static uint64_t stage_texture(FrameOp *op, int stage, XTexInfo *ti, int *hdr_ok) {
    uint32_t t = st.tex[stage];
    *hdr_ok = t && readable(t, 0x14);   /* the header check, reused for the depth-texture rewrite */
    if (!*hdr_ok || (U32(t, 0) & COMMON_TYPE_MASK) != COMMON_TEXTURE || !U32(t, 4)) return 0;
    uint32_t fmt = U32(t, 0xc), size = U32(t, 0x10), data = VA(U32(t, 4));
    uint8_t surf = hle_d3d_render_targets && !(fmt & FMT_CUBEMAP) ? rt_find(t) : 0;
    if (surf) {   /* the memory of a title render target: its offscreen surface */
        uint32_t pitch;
        memset(ti, 0, sizeof *ti);
        ti->code = FMT_CODE(fmt);
        ti->linear = xtex_fmt_linear(ti->code);
        res_dims(t, &ti->w, &ti->h, &pitch);
        op->tex_surf[stage] = surf;
        return op->tex_key[stage] = 1;
    }
    if (xtex_info(fmt, size, ti) || !readable(data, xtex_total_bytes(ti))) {
        if (!stage) st.tex_unsupported[XTEX_CODE(fmt)]++;
        return 0;
    }
    if (!stage) st.tex_draws[ti->code]++;
    op->tex_data[stage] = data;
    op->tex_format[stage] = fmt;
    op->tex_size[stage] = size;
    return op->tex_key[stage] = xtex_key(data, fmt, size, G(data), xtex_total_bytes(ti)) | 1;   /* never 0 */
}

/* Pixel shader of a draw: the bound D3DPIXELSHADERDEF with the current SetPixelShaderConstant
 * values and the fog color (fog register alpha 1; the per-vertex fog amount scales it, see
 * draw_fog). Returns 0 without a definition (the renderer's default texture x color program). */
static int draw_pixel_shader(RPixelShader *ps) {
    if (!st.ps_def || !readable(st.ps_def, PSH_DEF_DWORDS * 4)) return 0;
    psh_from_def(G(st.ps_def), st.psc, 0xFF000000u | (st.fog_color & 0xFFFFFFu), ps);
    return 1;
}

/* Records one draw for the host renderer: vertex fetch per the bound declaration or FVF, the
 * transform on the CPU (fixed function: v * World * View * Projection; programs: the title's own
 * vertex program interpreted per vertex, src/vsh.c), primitive expansion to a triangle list, the
 * vertex colors and texture coordinates of stages 0-3, the stage textures and the pixel shader.
 * Vertex data is copied per draw (indices min..max), so later title writes cannot change this
 * frame. up_data/up_stride replace stream 0 for the *UP draws. Returns 1 when recorded.
 * Programs (xbox_shaders.lua in Main.wad0) compute oD0 from their lights, material, ambient or
 * emissive constants and v2, pass v3 (world/object) or v2 (skinned) to oT0, and end with the
 * screen-space transform the D3D library appends (c-38/c-37, see vsh_set_screen_constants).
 * FVF draws: diffuse (white without one), specular (0 without one), TEXi for stage i. Draws
 * without a pixel shader, a stage-0 texture and an FVF diffuse color keep the debug color. */
#define C_WVP (69 + 96)
#define C_BONES (-36 + 96)
static int record_draw_body(uint32_t prim, uint32_t count, uint32_t idx_addr, uint32_t up_data, uint32_t up_stride);
/* --hud-glass: the target health bars (hud_glass_find_targets; the rage meter is left alone) of a just-recorded draw (v, its nv vertices; the n indices at
 * rl->idx + rl->ni) get a glass panel operation each, recorded before the draw, and their quads
 * are faded to the configured opacity. Only small textured draws without a vertex program are
 * looked at (the HUD batches: FVF 0x142, a few dozen quads). */
#define GLASS_MAX_VERTS 1024
static void glass_bars(FrameList *rl, RVertex *v, uint32_t nv, uint32_t n) {
    static uint8_t member[GLASS_MAX_VERTS];
    HudGlassRect bars[HUD_GLASS_MAX_BARS];
    int nb = hud_glass_find_targets(v, nv, rl->idx + rl->ni, n, &st.vp, bars, HUD_GLASS_MAX_BARS, member);
    if (!nb) return;
    hud_glass_fade(v, nv, member, hle_d3d_glass.opacity);
    for (int b = 0; b < nb; b++) {
        FrameOp *g = frame_op(rl);
        if (!g) return;
        g->kind = OP_GLASS;
        g->glass = hud_glass_panel(&hle_d3d_glass, bars[b]);
    }
}
static int record_draw(uint32_t prim, uint32_t count, uint32_t idx_addr, uint32_t up_data, uint32_t up_stride) {
    if (!hle_d3d_stats) return record_draw_body(prim, count, idx_addr, up_data, up_stride);
    double t0 = now_ms();
    gstat.ph_last = t0;
    gstat.in_rec = 1;
    int r = record_draw_body(prim, count, idx_addr, up_data, up_stride);
    gstat.in_rec = 0;
    double t1 = now_ms();
    gstat.record += t1 - t0;
    if (r) gstat.ph[REC_TAIL] += t1 - gstat.ph_last;
    else gstat.ph[REC_SETUP] += t1 - gstat.ph_last;   /* skipped draws count as setup */
    gstat.draws += r;
    return r;
}
static int record_draw_body(uint32_t prim, uint32_t count, uint32_t idx_addr, uint32_t up_data, uint32_t up_stride) {
    static uint32_t *tri;
    static uint32_t tri_cap;
    if (!strcmp(hle_d3d_renderer, "none")) return 0;
    int off = st.rt != st.surf[0];
    if (off && (!hle_d3d_render_targets || (!st.target_c && !st.target_z))) { st.skipped_target++; return 0; }
    GeomLayout fvf_layout;
    const GeomLayout *l = NULL;
    const VShader *vs = NULL;
    int rhw = 0, normal_reg;
    if (st.vshader & 1) {
        vs = vshader_of(st.vshader);
        if (vs && vs->valid) l = &vs->layout;
        normal_reg = 1;   /* in_vNormal in every program of the title */
    } else {
        if (geom_layout_from_fvf(st.vshader, &fvf_layout, &rhw) > 0) l = &fvf_layout;
        normal_reg = GEOM_REG_NORMAL;
    }
    const GeomElement *pos = l ? geom_find(l, 0) : NULL, *nrm = l ? geom_find(l, normal_reg) : NULL;
    const GeomElement *dif = l && !(vs && vs->skinned) ? geom_find(l, vs ? 2 : GEOM_REG_DIFFUSE) : NULL;
    const GeomElement *spec = l && !vs ? geom_find(l, GEOM_REG_SPECULAR) : NULL;
    if (dif && dif->type != GEOM_D3DCOLOR) dif = NULL;
    if (spec && spec->type != GEOM_D3DCOLOR) spec = NULL;
    /* Texture coordinates of the fallback paths: FVF TEXi; programs without microcode v3 / v2. */
    const GeomElement *uv[RENDERER_STAGES] = {0};
    for (int i = 0; l && i < RENDERER_STAGES; i++) uv[i] = geom_find(l, vs ? (i ? 0xFF : vs->skinned ? 2 : 3) : GEOM_REG_TEX0 + i);
    if (!pos || (idx_addr && !readable(idx_addr, count * 2))) { st.skipped_fmt++; return 0; }
    uint32_t max = geom_triangulated_max(count);
    if (!grow((void **)&tri, &tri_cap, max, 4)) return 0;
    uint32_t n = geom_triangulate(prim, count, tri, max);
    if (!n) return 0;
    const uint16_t *ix = idx_addr ? G(idx_addr) : NULL;
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = ix ? ix[tri[i]] : tri[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    uint32_t nv = hi - lo + 1;
    const uint8_t *base[4] = {0};
    uint32_t stride[4] = {0};
    for (int k = 0; k < l->n; k++) {
        int sn = l->e[k].stream;
        if (base[sn]) continue;
        uint32_t addr = sn == 0 && up_data ? up_data : st.stream[sn].vb ? VA(U32(st.stream[sn].vb, 4)) : 0;
        stride[sn] = sn == 0 && up_data ? up_stride : st.stream[sn].stride;
        uint32_t end = (hi + 1) * stride[sn];
        if (!stride[sn] || !readable(addr + lo * stride[sn], end - lo * stride[sn])) { st.skipped_fmt++; return 0; }
        base[sn] = G(addr);
    }
    rec_mark(REC_SETUP);
    FrameList *rl = &st.rec;
    /* Room for the outline quads of --hud-hd (hud_hd_restyle_style: up to 4 per bar) after this draw's vertices and indices. */
    if (rl->nv + nv + 16 * HUD_GLASS_MAX_BARS > rl->vcap) vp_wait_all();   /* pending batches write into rl->v */
    if (!grow((void **)&rl->v, &rl->vcap, rl->nv + nv + 16 * HUD_GLASS_MAX_BARS, sizeof(RVertex)) ||
        !grow((void **)&rl->idx, &rl->icap, rl->ni + n + 24 * HUD_GLASS_MAX_BARS, 4))
        return 0;
    if (!rl->draws && !rl->base) {   /* everything drawn so far (CPU clears, movie quads) */
        uint32_t w, h, pitch;
        res_dims(st.surf[0], &w, &h, &pitch);
        size_t bytes = (size_t)w * h * 4;
        if (rl->spare_base && rl->base_bytes >= bytes) {
            rl->base = rl->spare_base;
        } else {
            free(rl->spare_base);
            rl->base = malloc(bytes);
            rl->base_bytes = rl->base ? bytes : 0;
        }
        rl->spare_base = NULL;
        if (rl->base)
            for (uint32_t y = 0; y < h; y++) memcpy(rl->base + y * w, (uint8_t *)G(VA(U32(st.surf[0], 4))) + (size_t)y * pitch, w * 4);
    }
    float wvp[16], wv[16];
    DrawFog fog;
    draw_fog(&fog);
    GeomLighting lit;
    int lighting = !vs && !rhw && draw_lighting(&lit);
    if (!vs) {
        geom_mat_mul(st.world, st.view, wv);
        geom_mat_mul(wv, st.proj, wvp);
    }
    const GeomElement *bi = vs && vs->skinned ? geom_find(l, 4) : NULL, *bw = bi ? geom_find(l, 5) : NULL;
    FrameOp rec = {0};
    rec_mark(REC_STATE);
    rec.has_ps = draw_pixel_shader(&rec.ps);
    rec_mark(REC_PS);
    /* Stage textures: the stages the pixel shader samples (stage 0 without one). Linear (LIN_*)
     * textures take unnormalized coordinates on the Xbox, so s and t are scaled by 1/size. */
    float tscale[RENDERER_STAGES][2];
    int textured = 0, hdr_ok[RENDERER_STAGES] = {0};
    uint32_t depth_stages = 0;   /* stages compared against a depth texture (PROJECT3D) */
    for (int i = 0; i < RENDERER_STAGES; i++) {
        XTexInfo ti;
        int used = rec.has_ps ? psh_tex_mode(&rec.ps, i) != 0 : i == 0;
        int ok = used && stage_texture(&rec, i, &ti, &hdr_ok[i]);
        tscale[i][0] = ok && ti.linear ? 1.0f / ti.w : 1.0f;
        tscale[i][1] = ok && ti.linear ? 1.0f / ti.h : 1.0f;
        if (!i) textured = ok;
    }
    if (rec.has_ps) {
        /* Shadow compare (renderer.h shadow_func), and the D3D library's texture-mode rewrite of
         * stages holding a depth texture: PROJECT2D/3D/CUBE -> PROJECT3D (0x3695xx). */
        rec.ps.shadow_func = st.shadow_func;
        for (int i = 0; i < RENDERER_STAGES; i++) {
            uint32_t m = psh_tex_mode(&rec.ps, i), t = st.tex[i];
            if (m >= 1 && m <= 3 && hdr_ok[i] && fmt_depth(FMT_CODE(U32(t, 0xc)))) {   /* m != 0: stage i was checked */
                rec.ps.tex_modes = (rec.ps.tex_modes & ~(31u << (5 * i))) | 2u << (5 * i);
                depth_stages |= 1u << i;
            }
        }
    }
    rec_mark(REC_TEX);
    int real_color = rec.has_ps || textured || (!vs && dif);   /* otherwise the debug color */
    RVertex *out = rl->v + rl->nv;
    int prog = vs && vs->has_prog;
    if (prog) {   /* the title's vertex program, VSH_BATCH vertices per interpreter pass */
        vsh_set_screen_constants(st.vsc);   /* D3D-reserved; the library's SetViewport is replaced */
        uint32_t fed = 0;   /* input registers the declaration fills (geom_fetch writes all 4 components) */
        for (int k = 0; k < l->n; k++)
            if (l->e[k].reg < 16) fed |= 1u << l->e[k].reg;
        VpJob jb = {&vs->prog, l, fed, lo, nv, {base[0], base[1], base[2], base[3]}, {stride[0], stride[1], stride[2], stride[3]},
                    (const float (*)[4])st.vsc, out, real_color, nrm, &fog, (const float (*)[2])tscale};
        vp_run(&jb);
        rec_mark(REC_VSH);
    }
    for (uint32_t i = 0; i < nv && !prog; i++) {
        float p[4], nn[4], c[4];
        uint32_t v = lo + i;
        RVertex *o = &out[i];
        memset(o, 0, sizeof *o);
        geom_fetch(pos->type, base[pos->stream] + v * stride[pos->stream] + pos->offset, p);
        if (nrm) geom_fetch(nrm->type, base[nrm->stream] + v * stride[nrm->stream] + nrm->offset, nn);
        if (bw) {
            float bix[4], bwt[4];
            geom_fetch(bi->type, base[bi->stream] + v * stride[bi->stream] + bi->offset, bix);
            geom_fetch(bw->type, base[bw->stream] + v * stride[bw->stream] + bw->offset, bwt);
            geom_skin3(p, bix, bwt, (const float (*)[4])st.vsc, C_BONES, p);
        }
        if (vs) geom_dp4_rows(p, st.vsc[C_WVP], c);
        else if (rhw) geom_screen_to_clip(p, st.vp.x, st.vp.y, st.vp.w, st.vp.h, c);
        else geom_row_xform(p, wvp, c);
        memcpy(&o->x, c, 16);
        o->color = real_color ? 0xFFFFFFFFu : geom_debug_color(nrm ? nn : NULL, p) | 0xFF000000u;
        if (real_color && dif) memcpy(&o->color, base[dif->stream] + v * stride[dif->stream] + dif->offset, 4);
        if (spec) memcpy(&o->specular, base[spec->stream] + v * stride[spec->stream] + spec->offset, 4);
        if (fog.on) {   /* fixed function: vertex fog (table mode none) from the specular alpha, else
                           the view-space depth (or distance with D3DRS_RANGEFOGENABLE); RHW: w */
            float vp[4], d;
            if (fog.mode == GEOM_FOG_NONE) d = spec ? (o->specular >> 24) / 255.0f : 1;
            else if (rhw) d = c[3];
            else {
                geom_row_xform(p, wv, vp);
                d = fog.range ? sqrtf(vp[0] * vp[0] + vp[1] * vp[1] + vp[2] * vp[2]) : fabsf(vp[2]);
            }
            o->fog = fog_amount(&fog, d);
        }
        if (lighting) {   /* world-space position and normal; vertex colors as material sources */
            float wp[4], wn[4] = {0, 0, 0, 0}, vd[4], vsp[4], ld[4], ls[4];
            geom_row_xform(p, st.world, wp);
            if (nrm) { nn[3] = 0; geom_row_xform(nn, st.world, wn); }
            if (dif) geom_fetch(GEOM_D3DCOLOR, base[dif->stream] + v * stride[dif->stream] + dif->offset, vd);
            if (spec) geom_fetch(GEOM_D3DCOLOR, base[spec->stream] + v * stride[spec->stream] + spec->offset, vsp);
            geom_light(&lit, wp, nrm ? wn : NULL, dif ? vd : NULL, spec ? vsp : NULL, ld, ls);
            o->color = psh_argb(ld);
            o->specular = psh_argb(ls);
        }
        for (int s = 0; s < RENDERER_STAGES; s++) {
            float t[4] = {0, 0, 0, 1};
            if (uv[s]) geom_fetch(uv[s]->type, base[uv[s]->stream] + v * stride[uv[s]->stream] + uv[s]->offset, t);
            o->tex[s][0] = t[0] * tscale[s][0];
            o->tex[s][1] = t[1] * tscale[s][1];
            o->tex[s][2] = t[2];
            o->tex[s][3] = t[3];
        }
    }
    rec_mark(REC_VERT);
    if (depth_stages && hle_d3d_shadow_range.on) {   /* --shadow-range: the light projection, once per frame */
        const int ds = __builtin_ctz(depth_stages);
        float ab[2];
        if (st.shadow_fit_frame != st.frames + 1 && !shadow_range_fit(&out[0].tex[ds][2], sizeof(RVertex) / sizeof(float), nv, ab)) {
            memcpy(st.shadow_ab, ab, sizeof ab);
            st.shadow_fit_frame = st.frames + 1;
        }
        memcpy(rec.ps.shadow_depth, st.shadow_ab, sizeof st.shadow_ab);
    }
    for (uint32_t i = 0; i < n; i++) rl->idx[rl->ni + i] = (ix ? ix[tri[i]] : tri[i]) - lo;
    if (hle_d3d_glass.hd && !off && !prog && textured && nv <= GLASS_MAX_VERTS) {
        HudGlassRect outlines[HUD_GLASS_MAX_BARS];
        const HudHdStyle style = hud_hd_style(&hle_d3d_glass);
        int nb = hud_hd_restyle_style(out, &nv, rl->idx + rl->ni, &n, &st.vp, &style, outlines);
        static int logged;
        if (nb && !logged++)   /* once: proves on the user's log that the recognizer matched */
            printf("[hud] target bar restyled at %.0f,%.0f %.0fx%.0f (viewport %.0fx%.0f)\n", outlines[0].x0, outlines[0].y0,
                   outlines[0].x1 - outlines[0].x0, outlines[0].y1 - outlines[0].y0, st.vp.w, st.vp.h);
        for (int b = 0; b < nb && hle_d3d_glass.hd_frost > 0; b++) {   /* --hud-hd-frost: blur behind the bar first */
            FrameOp *g = frame_op(rl);
            if (!g) break;
            g->kind = OP_GLASS;
            g->glass = hud_hd_frost(outlines[b], hle_d3d_glass.hd_frost);
        }
    }
    if (hle_d3d_glass.on && !off && !prog && textured && nv <= GLASS_MAX_VERTS) glass_bars(rl, out, nv, n);
    FrameOp *op = frame_op(rl);
    if (!op) return 0;
    *op = rec;
    op->kind = OP_DRAW;
    op->vp = st.vp;
    op->draw_flags = st.zenable ? RENDERER_DEPTH_TEST : 0;
    if (st.alpha_test && st.alpha_func >= 0x200 && st.alpha_func <= 0x207) {   /* to D3DCMPFUNC (1..8) */
        op->alpha_func = st.alpha_func - 0x200 + 1;
        op->alpha_ref = st.alpha_ref;
    }
    op->rs = st.rs;
    const BloomDraw bd = {st.rs.blend_enable, st.rs.blend_src, st.rs.blend_dst, st.rs.blend_eq, st.zenable != 0, st.rs.depth_write,
                          st.vshader & 1 ? 0 : st.vshader, prim};
    op->halo = (uint8_t)bloom_halo_draw(&bd);
    op->target_c = off ? st.target_c : 0;
    op->target_z = off ? st.target_z : 0;
    op->first_vertex = rl->nv;
    op->nv = nv;
    op->first_index = rl->ni;
    op->ni = n;
    if (hle_d3d_ssao.on && !off && st.zenable && st.rs.depth_write &&   /* --ssao: the camera's x/y scale */
        (prog ? ssao_proj_scale(st.vsc[C_WVP], st.vsc[C_WVP + 1], st.vsc[C_WVP + 3], op->ssao_scale)
              : ssao_proj_scale((const float[4]){st.proj[0], st.proj[4], st.proj[8], 0}, (const float[4]){st.proj[1], st.proj[5], st.proj[9], 0},
                                (const float[4]){st.proj[3], st.proj[7], st.proj[11], 0}, op->ssao_scale)))
        op->ssao_scale[0] = op->ssao_scale[1] = 0;
    if (hle_d3d_rain.on && !rl->has_frame_ms) rl->has_frame_ms = (uint8_t)guest_clock_ms(&rl->frame_ms);
    if (hle_d3d_rain.on && !off && !(st.vshader & 1)) {   /* fixed-function: out is complete here */
        const RainDraw rd = {st.vshader, prim, nv, n, st.zenable != 0, st.rs.blend_enable, st.rs.blend_src, st.rs.blend_dst, out,
                             st.vp.h > 0 ? st.vp.w / st.vp.h : 0};
        if (rain_layer(&rd, &op->rain_layer)) {
            op->rain = 1;
            rain_scroll_layer(&st.rain_scroll, st.rain_layers++, &op->rain_layer, op->rain_scroll);
            if (!rl->has_clock) rl->has_clock = (uint8_t)guest_clock_ms(&rl->clock_ms);
        } else if (hle_d3d_rain.splash && st.zenable && st.proj[15] == 0 && fabsf(st.proj[11]) == 1) {
            memcpy(rl->cam_view, st.view, sizeof rl->cam_view);
            memcpy(rl->cam_proj, st.proj, sizeof rl->cam_proj);
            rl->has_cam = 1;
        }
    }
    if (!rl->draws++) rl->first_draw = rl->nops - 1;
    rl->nv += nv;
    rl->ni += n;
    st.drawn++;
    gstat.verts += nv;
    return 1;
}
/* (PrimitiveType, VertexCount, pIndexData): 16-bit indices from a CPU pointer, streams 0-3. */
static uint32_t draw_indexed_vertices(D3dFrame *f, const uint32_t *a) {
    (void)f;
    if (a[2]) record_draw(a[0], a[1], a[2], 0, 0);
    return S_OK;
}
/* (PrimitiveType, VertexCount, pIndexData, pVertexStreamZeroData, VertexStreamZeroStride) */
static uint32_t draw_indexed_vertices_up(D3dFrame *f, const uint32_t *a) {
    (void)f;
    if (a[2] && a[3]) record_draw(a[0], a[1], a[2], a[3], a[4]);
    return S_OK;
}

/* ---- Bink movie quad: CPU stand-in for the title's YUV pixel shader ----
 * Movie\BinkMovie.cpp draws each movie frame as one DrawVerticesUP(QUADLIST, 4 vertices, stride
 * 0x24) with FVF 0x244 (XYZRHW | DIFFUSE | TEX2), the Bink YUV register-combiner program set by
 * SetPixelShaderProgram, and three 8-bit planes on stages 0-2 (Y, then the planes the program
 * treats as Cr and Cb). Texture coordinate set 0 feeds stage 0 and set 1 feeds stages 1 and 2
 * (TexCoordIndex written by the title at 0x249589). When that exact state is bound, the planes
 * are converted on the CPU into the render target over the quad's screen rectangle with the
 * coefficients read from the title's own shader definition. A real renderer replaces this. */
#define FVF_XYZRHW_DIFFUSE_TEX2 0x244u
#define PRIM_QUADLIST 8u
#define MOVIE_VERTEX_DWORDS 9   /* x, y, z, rhw, diffuse, u0, v0, u1, v1 */
static int movie_plane(int stage, int coord_set, const float *tl, const float *br, YuvPlane *pl) {
    static uint8_t *unswizzled[3];
    uint32_t t = st.tex[stage];
    if (!t || (U32(t, 0) & COMMON_TYPE_MASK) != COMMON_TEXTURE || !U32(t, 4)) return 0;
    uint32_t code = FMT_CODE(U32(t, 0xc)), w, h, pitch;
    if (code != 0x13 && code != 0x00) return 0;   /* LIN_L8 or swizzled L8 */
    res_dims(t, &w, &h, &pitch);
    pl->p = G(VA(U32(t, 4)));
    pl->w = w;
    pl->h = h;
    pl->pitch = pitch;
    pl->u0 = tl[5 + 2 * coord_set];
    pl->v0 = tl[6 + 2 * coord_set];
    pl->u1 = br[5 + 2 * coord_set];
    pl->v1 = br[6 + 2 * coord_set];
    if (code == 0x00) {   /* swizzled: normalized coordinates, Morton-ordered texels */
        uint8_t *buf = realloc(unswizzled[stage], (size_t)w * h);
        if (!buf) return 0;
        unswizzled[stage] = buf;
        xbox_unswizzle(pl->p, buf, w, h, 1, w);
        pl->p = buf;
        pl->pitch = w;
        pl->u0 = pl->u0 * w - 0.5f;
        pl->u1 = pl->u1 * w - 0.5f;
        pl->v0 = pl->v0 * h - 0.5f;
        pl->v1 = pl->v1 * h - 0.5f;
    }
    return 1;
}
/* (PrimitiveType, VertexCount, pVertexStreamZeroData, VertexStreamZeroStride) */
static int draw_movie_quad(const uint32_t *a) {
    static int logged;
    if (a[0] != PRIM_QUADLIST || a[1] != 4 || a[3] != MOVIE_VERTEX_DWORDS * 4 || !a[2] || st.vshader != FVF_XYZRHW_DIFFUSE_TEX2 ||
        !st.ps_def || !st.rt)
        return 0;
    YuvCoeffs c;
    if (!yuv_coeffs_from_psdef(G(st.ps_def), &c)) return 0;
    const float *v = G(a[2]), *tl = v, *br = v;   /* corners with the smallest and largest x + y */
    for (int i = 1; i < 4; i++) {
        const float *q = v + i * MOVIE_VERTEX_DWORDS;
        if (q[0] + q[1] < tl[0] + tl[1]) tl = q;
        if (q[0] + q[1] > br[0] + br[1]) br = q;
    }
    YuvPlane pl[3];
    if (!movie_plane(0, 0, tl, br, &pl[0]) || !movie_plane(1, 1, tl, br, &pl[1]) || !movie_plane(2, 1, tl, br, &pl[2])) return 0;
    uint32_t w, h, pitch;
    if (xtex_fmt_bpp(FMT_CODE(U32(st.rt, 0xc))) != 32 || !U32(st.rt, 4)) return 0;
    res_dims(st.rt, &w, &h, &pitch);
    int x0 = (int)lroundf(tl[0]), y0 = (int)lroundf(tl[1]), x1 = (int)lroundf(br[0]), y1 = (int)lroundf(br[1]);
    yuv_draw_quad(G(VA(U32(st.rt, 4))), pitch, w, h, x0, y0, x1, y1, pl, &c);
    if (!logged++) {
        printf("[d3d] movie quad (%d,%d)-(%d,%d): planes", x0, y0, x1, y1);
        for (int i = 0; i < 3; i++)
            printf(" %ux%u fmt %02x pitch %u uv %.1f,%.1f-%.1f,%.1f;", pl[i].w, pl[i].h, FMT_CODE(U32(st.tex[i], 0xc)),
                   pl[i].pitch, pl[i].u0, pl[i].v0, pl[i].u1, pl[i].v1);
        printf(" shader %08x: off %.4f %.4f %.4f, R %.4f %.4f %.4f, G %.4f %.4f %.4f, B %.4f %.4f %.4f\n", st.ps_def,
               c.off[0], c.off[1], c.off[2], c.m[0][0], c.m[0][1], c.m[0][2], c.m[1][0], c.m[1][1], c.m[1][2], c.m[2][0],
               c.m[2][1], c.m[2][2]);
    }
    return 1;
}
static uint32_t draw_vertices_up(D3dFrame *f, const uint32_t *a) {
    (void)f;
    if (draw_movie_quad(a)) {
        __atomic_add_fetch(&st.movie_quads, 1, __ATOMIC_SEQ_CST);
        st.rec.cpu_draws++;
        if (st.rec.draws) st.rec.cpu_after_draw++;
    } else {
        /* Everything else (the title's full-screen tint and frame-feedback quads, FVF 0x44/0x144,
         * and world-space fans, FVF 0x142) goes through the regular draw path: (PrimitiveType,
         * VertexCount, pVertexStreamZeroData, VertexStreamZeroStride). */
        if (!a[2] || !record_draw(a[0], a[1], 0, a[2], a[3])) st.skipped_up++;
    }
    return S_OK;
}

typedef uint32_t (*D3dFn)(D3dFrame *f, const uint32_t *a);
typedef struct {
    const char *name;   /* symbol suffix after D3D8__ when XbSymbolDatabase knows it */
    uint32_t addr;
    int argbytes;       /* from the function's own "ret N" (0 = plain ret: register or no args) */
    D3dFn fn;
    uint8_t sig[HLE_SIG_LEN];
    uint32_t calls, frame_calls;
} D3dEntry;

#define E(name, addr, n, fn, ...) {name, addr, n, fn, {__VA_ARGS__}, 0, 0}
static D3dEntry entries[] = {
    E("D3DDevice_SetRenderState_PSTextureModes", 0x35cf00, 4, ok, 0x8b, 0x44, 0x24, 0x04, 0x8b),
    E("D3DDevice_SetRenderState_Simple", 0x35cf30, 0, set_render_state_simple, 0xa1, 0x80, 0xd9, 0x37, 0x00),   /* ecx = method, edx = value */
    E("D3DDevice_SetRenderState_EdgeAntiAlias", 0x35d7b0, 4, ok, 0x56, 0x8b, 0x35, 0x78, 0xd9),
    E("D3DDevice_SetRenderState_ShadowFunc", 0x35d7f0, 4, set_shadow_func, 0x56, 0x8b, 0x35, 0x78, 0xd9),
    E("D3DDevice_SetRenderState_FogColor", 0x35d830, 4, set_fog_color, 0x56, 0x8b, 0x35, 0x78, 0xd9),
    E("D3DDevice_SetRenderState_CullMode", 0x35d880, 4, set_cull_mode, 0x56, 0x8b, 0x35, 0x78, 0xd9),
    E("D3DDevice_SetRenderState_FrontFace", 0x35d8f0, 4, ok, 0x56, 0x8b, 0x35, 0x78, 0xd9),
    E("D3DDevice_SetRenderState_NormalizeNormals", 0x35d930, 4, ok, 0x56, 0x8b, 0x35, 0x78, 0xd9),
    E("D3DDevice_SetRenderState_TextureFactor", 0x35d970, 4, ok, 0x56, 0x8b, 0x35, 0x78, 0xd9),
    E("D3DDevice_SetRenderState_LineWidth", 0x35d9d0, 4, ok, 0x53, 0x8b, 0x5c, 0x24, 0x08),
    E("D3DDevice_SetRenderState_Dxt1NoiseEnable", 0x35da40, 4, ok, 0x53, 0x8b, 0x5c, 0x24, 0x08),
    E("D3DDevice_SetRenderState_ZBias", 0x35dac0, 4, ok, 0x51, 0x56, 0x33, 0xc0, 0x57),
    E("D3DDevice_SetRenderState_LogicOp", 0x35db40, 4, ok, 0x56, 0x8b, 0x35, 0x78, 0xd9),
    E("D3DDevice_SetRenderState_FillMode", 0x35dba0, 4, ok, 0x56, 0x8b, 0x35, 0x78, 0xd9),
    E("D3DDevice_SetRenderState_BackFillMode", 0x35dbf0, 4, ok, 0x8b, 0x44, 0x24, 0x04, 0x56),
    E("D3DDevice_SetRenderState_TwoSidedLighting", 0x35dc50, 4, ok, 0x56, 0x8b, 0x35, 0x78, 0xd9),
    E("D3DDevice_SetRenderState_VertexBlend", 0x35dcd0, 4, ok, 0x8b, 0x15, 0xd8, 0xd4, 0x37),
    E("D3DDevice_SetTextureState_TexCoordIndex", 0x35dd20, 8, ok, 0x53, 0x8b, 0x5c, 0x24, 0x0c),
    E("D3DDevice_SetTextureState_BumpEnv", 0x35de30, 12, ok, 0x53, 0x55, 0x56, 0x8b, 0x74),
    E("D3DDevice_SetTextureState_BorderColor", 0x35dea0, 8, ok, 0x56, 0x8b, 0x35, 0x78, 0xd9),
    E("D3DDevice_SetTextureState_ColorKeyColor", 0x35dee0, 8, ok, 0x56, 0x8b, 0x35, 0x78, 0xd9),
    E("D3DDevice_SetRenderState_ZEnable", 0x35e850, 4, set_zenable, 0x56, 0x8b, 0x35, 0x78, 0xd9),
    E("D3DDevice_SetRenderState_StencilEnable", 0x35e8e0, 4, ok, 0x56, 0x8b, 0x35, 0x78, 0xd9),
    E("D3DDevice_SetRenderState_StencilFail", 0x35e980, 4, ok, 0x56, 0x8b, 0x35, 0x78, 0xd9),
    E("D3DDevice_SetRenderState_YuvEnable", 0x35e9f0, 4, ok, 0x8b, 0x44, 0x24, 0x04, 0x56),
    E("D3DDevice_SetRenderState_OcclusionCullEnable", 0x35ea20, 4, ok, 0x8b, 0x44, 0x24, 0x04, 0x56),
    E("D3DDevice_SetRenderState_StencilCullEnable", 0x35ea90, 4, ok, 0x8b, 0x44, 0x24, 0x04, 0x56),
    E("D3DDevice_SetRenderState_RopZCmpAlwaysRead", 0x35eb00, 4, ok, 0x8b, 0x44, 0x24, 0x04, 0xa3),
    E("D3DDevice_SetRenderState_RopZRead", 0x35eb20, 4, ok, 0x8b, 0x44, 0x24, 0x04, 0xa3),
    E("D3DDevice_SetRenderState_DoNotCullUncompressed", 0x35eb40, 4, ok, 0x8b, 0x44, 0x24, 0x04, 0xa3),
    E("D3DDevice_SetRenderState_MultiSampleMode", 0x35eb60, 4, ok, 0x8b, 0x44, 0x24, 0x04, 0x8b),
    E("D3DDevice_SetRenderState_MultiSampleRenderTargetMode", 0x35eb90, 4, ok, 0x8b, 0x44, 0x24, 0x04, 0x8b),
    E("D3DDevice_SetRenderState_MultiSampleAntiAlias", 0x35ebc0, 4, ok, 0x8b, 0x44, 0x24, 0x04, 0x56),
    E("D3DDevice_SetRenderState_MultiSampleMask", 0x35ec40, 4, ok, 0x8b, 0x44, 0x24, 0x04, 0x8b),
    E("D3DDevice_SetRenderState_SampleAlpha", 0x35ec90, 4, ok, 0x8b, 0x44, 0x24, 0x04, 0x56),
    E("D3DDevice_DrawVerticesUP", 0x35efe0, 16, draw_vertices_up, 0x55, 0x8b, 0xec, 0x83, 0xec),
    E("D3DDevice_DrawIndexedVerticesUP", 0x35f150, 20, draw_indexed_vertices_up, 0x55, 0x8b, 0xec, 0x83, 0xec),
    E("D3DDevice_DrawIndexedVertices", 0x35f330, 12, draw_indexed_vertices, 0x55, 0x8b, 0xec, 0x83, 0xec),
    E("D3DDevice_CreateVertexShader", 0x35fe00, 16, create_vertex_shader, 0x51, 0x53, 0x55, 0x8b, 0x6c),
    E("D3DDevice_SetVertexShaderConstant1Fast", 0x35ffc0, 0, set_vs_constant1, 0xa1, 0x80, 0xd9, 0x37, 0x00),   /* ecx, edx */
    E("D3DDevice_SetVertexShaderConstantNotInlineFast", 0x3600c0, 4, set_vs_constants, 0x56, 0x57, 0x8b, 0x3d, 0x80),
    E("D3DDevice_SetStreamSource", 0x360450, 12, set_stream_source, 0x8b, 0x0d, 0x78, 0xd9, 0x37),
    E("D3DDevice_SetShaderConstantMode", 0x360740, 4, ok, 0x8b, 0x44, 0x24, 0x04, 0xa8),
    E("D3DDevice_SetVertexShader", 0x3609b0, 4, set_vertex_shader, 0x51, 0x53, 0x8b, 0x5c, 0x24),
    E("D3DDevice_SetGammaRamp", 0x361110, 8, ok, 0x53, 0x55, 0x8b, 0x6c, 0x24),
    E("D3DDevice_SetTransform", 0x361230, 8, set_transform, 0x8b, 0x44, 0x24, 0x04, 0x8b),
    E("D3DDevice_SetMaterial", 0x361420, 4, set_material, 0x56, 0x8b, 0x74, 0x24, 0x08),
    E("D3DDevice_BlockUntilIdle?", 0x361600, 0, ok, 0xe9, 0xfb, 0x6c, 0x00, 0x00),   /* jmp D3D_KickOffAndWaitForIdle */
    E("D3DDevice_InsertFence", 0x3619e0, 0, insert_fence, 0x6a, 0x00, 0xe8, 0x49, 0x65),
    E("D3DDevice_SetRenderTarget", 0x362120, 8, set_render_target, 0x83, 0xec, 0x0c, 0x53, 0x55),
    E("D3DDevice_GetBackBuffer2", 0x3624c0, 4, get_back_buffer, 0x8b, 0x44, 0x24, 0x04, 0x83),
    E("D3DDevice_GetRenderTarget2", 0x362be0, 0, get_render_target, 0xa1, 0x78, 0xd9, 0x37, 0x00),
    E("D3DDevice_GetDepthStencilSurface2", 0x362c00, 0, get_depth_stencil, 0xa1, 0x78, 0xd9, 0x37, 0x00),
    E("D3DDevice_SetViewport", 0x362c20, 4, set_viewport, 0x83, 0xec, 0x0c, 0x55, 0x8b),
    E("D3DDevice_SetLight", 0x362e50, 8, set_light, 0x8b, 0x44, 0x24, 0x04, 0x83),
    E("D3DDevice_LightEnable", 0x363110, 8, light_enable, 0x83, 0xec, 0x68, 0x53, 0x8b),
    E("D3DDevice_SetTexture", 0x3632a0, 8, set_texture, 0x83, 0xec, 0x08, 0x53, 0x55),
    E("D3DDevice_SetTile", 0x363b00, 8, ok, 0xe8, 0xfb, 0x47, 0x00, 0x00),   /* call KickOffAndWaitForIdle; jmp SetTileNoWait (ret 8) */
    E("D3DResource_Release", 0x3648c0, 4, release, 0x56, 0x8b, 0x74, 0x24, 0x08),
    E("D3DResource_GetType", 0x364910, 4, get_type, 0x8b, 0x4c, 0x24, 0x04, 0x8b),
    E("D3DResource_IsBusy", 0x3649a0, 4, ok, 0x8b, 0x4c, 0x24, 0x04, 0x8b),
    E("D3DResource_Register", 0x364a50, 8, register_res, 0x8b, 0x4c, 0x24, 0x04, 0x8b),
    E("D3DResource_BlockUntilNotBusy", 0x364a80, 4, ok, 0xe9, 0x9b, 0x38, 0x00, 0x00),   /* jmp D3D_BlockOnResource (ret 4) */
    E("D3DDevice_CreateIndexBuffer2", 0x364cb0, 4, create_index_buffer, 0x8b, 0x44, 0x24, 0x04, 0x68),
    E("D3DVertexBuffer_Lock2", 0x364dc0, 8, vb_lock, 0x53, 0x8a, 0x5c, 0x24, 0x0c),
    E("D3DDevice_Clear", 0x364f50, 24, clear, 0x83, 0xec, 0x38, 0x53, 0x55),
    E("D3DDevice_Swap", 0x366050, 4, swap, 0x56, 0x8b, 0x35, 0x78, 0xd9),
    E("D3D_GetDeviceCount?", 0x366310, 4, ret_one, 0xb8, 0x01, 0x00, 0x00, 0x00),   /* original: mov eax,1; ret 4 */
    E("D3D_GetAdapterModeCount", 0x366350, 4, mode_count, 0x83, 0xec, 0x08, 0x53, 0x55),
    E("D3D_EnumAdapterModes", 0x3663f0, 12, enum_modes, 0x83, 0xec, 0x08, 0x8b, 0x4c),
    E("Direct3D_SetPushBufferSize?", 0x366820, 8, set_pb_size, 0x8b, 0x44, 0x24, 0x04, 0x8b),
    E("Direct3D_CreateDevice", 0x366840, 24, create_device, 0xa1, 0x40, 0x01, 0x38, 0x00),
    E("D3DDevice_CreateTexture2", 0x366940, 28, create_texture, 0x56, 0x57, 0x33, 0xd2, 0x8d),
    E("D3DTexture_GetSurfaceLevel2", 0x366a10, 8, get_surface_level, 0x83, 0xec, 0x0c, 0x56, 0x8b),
    E("D3DTexture_LockRect", 0x366a60, 20, lock_rect, 0x8b, 0x44, 0x24, 0x14, 0x8b),
    E("D3DDevice_CreatePixelShader", 0x366b70, 8, create_pixel_shader, 0x68, 0x00, 0x00, 0x80, 0x24),
    E("D3DDevice_SetPixelShaderProgram", 0x366be0, 4, set_ps_program, 0x8b, 0x54, 0x24, 0x04, 0x85),
    E("D3DDevice_SetPixelShader", 0x367020, 4, set_pixel_shader, 0x51, 0x53, 0x8b, 0x1d, 0x78),
    E("D3DDevice_SetPixelShaderConstant", 0x367220, 12, set_ps_constant, 0x55, 0x8b, 0xec, 0x83, 0xec),
    E("D3DSurface_GetDesc", 0x3676e0, 8, surface_desc, 0x8b, 0x44, 0x24, 0x08, 0x8b),
};
#define NENTRIES ((int)(sizeof entries / sizeof *entries))

int hle_d3d_entry_count(void) { return NENTRIES; }
uint32_t hle_d3d_entry_addr(int i) { return entries[i].addr; }
int hle_d3d_entry_argbytes(int i) { return entries[i].argbytes; }
const char *hle_d3d_entry_name(int i) { return entries[i].name; }
const uint8_t *hle_d3d_entry_sig(int i) { return entries[i].sig; }
uint32_t hle_d3d_frame_count(void) { return __atomic_load_n(&st.frames, __ATOMIC_SEQ_CST); }
uint32_t hle_d3d_clear_color(void) { return st.clear_color; }

uint32_t *hle_d3d_last_frame(int *w, int *h) {
    pthread_mutex_lock(&d3d_lock);
    int shown = st.shown && st.shown_seq == st.frame_seq;   /* the renderer's result of the last posted frame */
    uint32_t fw = shown ? st.sw : st.fw, fh = shown ? st.sh : st.fh;
    uint32_t *copy = st.frame ? malloc((size_t)fw * fh * 4) : NULL;
    if (copy) {
        memcpy(copy, shown ? st.shown : st.frame, (size_t)fw * fh * 4);
        *w = fw;
        *h = fh;
    }
    pthread_mutex_unlock(&d3d_lock);
    return copy;
}

/* frame_calls counts the calls of the current frame; reset after each Swap. */
static void reset_frame_calls(void) {
    for (int i = 0; i < NENTRIES; i++) __atomic_store_n(&entries[i].frame_calls, 0, __ATOMIC_SEQ_CST);
}

/* Every call is logged until the first Swap (the call sequence to the first frame, capped);
 * afterwards hle_d3d_log_limit lines per function (0 = silent, also before the first Swap). */
#define TRACE_CAP 1500
static int trace_lines;
void hle_d3d_dispatch(D3dFrame *f) {
    D3dEntry *e = &entries[f->index];
    uint32_t r = e->fn(f, f->args);
    f->eax = r;
    uint32_t n = __atomic_add_fetch(&e->calls, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&e->frame_calls, 1, __ATOMIC_SEQ_CST);
    if (e->fn == swap) reset_frame_calls();
    int before_first_frame = hle_d3d_log_limit && (hle_d3d_frame_count() == 0 || (e->fn == swap && r == 1));
    if (!(before_first_frame && trace_lines < TRACE_CAP) && (int)n > hle_d3d_log_limit) return;
    char buf[160];
    int len = 0;
    if (!e->argbytes) len = snprintf(buf, sizeof buf, "ecx=%x, edx=%x", f->ecx, f->edx);
    for (int i = 0; i < e->argbytes / 4 && len < (int)sizeof buf - 12; i++)
        len += snprintf(buf + len, sizeof buf - len, "%s%x", i ? ", " : "", f->args[i]);
    buf[len] = 0;
    pthread_mutex_lock(&d3d_lock);
    if (before_first_frame) trace_lines++;
    printf("[d3d] %s(%s) -> %x [from %08x]%s\n", e->name, buf, r, f->ret_addr,
           !before_first_frame && (int)n == hle_d3d_log_limit ? " (further calls not logged)" : "");
    fflush(stdout);
    pthread_mutex_unlock(&d3d_lock);
}

/* Called at the end of frame 2: which entry points one steady-state frame uses. */
static void list_frame_calls(void) {
    char buf[4096];
    int len = 0;
    for (int i = 0; i < NENTRIES; i++) {
        uint32_t c = __atomic_load_n(&entries[i].frame_calls, __ATOMIC_SEQ_CST);
        if (c && len < (int)sizeof buf - 80) len += snprintf(buf + len, sizeof buf - len, " %s=%u", entries[i].name, c);
    }
    buf[len] = 0;
    printf("[d3d] calls during frame 2:%s\n", buf);
}

extern void hle_d3d_entry_common(void);

int hle_d3d_install(HleLookup lookup) {
    uint8_t *stubs = plat_alloc_exec(NENTRIES * 16);
    if (!stubs) return 0;
    int patched = 0;
    renderer_state_default(&st.rs);
    if (hle_d3d_stats) input_read_hook = stats_input_read;
    for (int i = 0; i < NENTRIES; i++) {
        D3dEntry *e = &entries[i];
        uint8_t *p = G(e->addr);
        char sym[96];
        snprintf(sym, sizeof sym, "D3D8__%s", e->name);
        uint32_t s = lookup && !strchr(e->name, '?') ? lookup(sym) : 0;
        if (s && s != e->addr) {
            printf("[d3d] skip %s: symbol at %08x, table expects %08x\n", e->name, s, e->addr);
            continue;
        }
        if (memcmp(p, e->sig, HLE_SIG_LEN)) {
            printf("[d3d] skip %s at %08x: code does not match the expected entry bytes\n", e->name, e->addr);
            continue;
        }
        /* Stub: push i; call hle_d3d_entry_common; add esp,4; ret argbytes */
        uint8_t *stb = stubs + i * 16;
        stb[0] = 0x68; memcpy(stb + 1, &i, 4);
        int32_t rel = (int32_t)((uintptr_t)hle_d3d_entry_common - (uintptr_t)(stb + 10));
        stb[5] = 0xE8; memcpy(stb + 6, &rel, 4);
        stb[10] = 0x83; stb[11] = 0xC4; stb[12] = 0x04;
        stb[13] = 0xC2; stb[14] = e->argbytes & 0xFF; stb[15] = e->argbytes >> 8;
        rel = (int32_t)((uintptr_t)stb - (uintptr_t)(p + 5));
        p[0] = 0xE9; memcpy(p + 1, &rel, 4);
        patched++;
    }
    printf("[d3d] %d of %d D3D8 entry points replaced\n", patched, NENTRIES);
    return patched;
}

void hle_d3d_summary(void) {
    uint32_t total = 0;
    char buf[4096];
    int len = 0;
    for (int i = 0; i < NENTRIES; i++) {
        uint32_t c = __atomic_load_n(&entries[i].calls, __ATOMIC_SEQ_CST);
        total += c;
        if (c && len < (int)sizeof buf - 80) len += snprintf(buf + len, sizeof buf - len, " %s=%u", entries[i].name, c);
    }
    buf[len] = 0;
    printf("[d3d] %u swaps (%u presented, %u dropped), last clear color %08x, %u movie quads drawn; %u draws recorded for the renderer "
           "(skipped: %u other render target, %u unsupported format/memory, %u DrawVerticesUP not recorded); D3D calls: %u total;%s\n",
           hle_d3d_frame_count(), st.presented, st.dropped, st.clear_color, st.movie_quads, st.drawn, st.skipped_target, st.skipped_fmt, st.skipped_up,
           total, buf);
    len = 0;
    for (int c = 0; c < 256; c++)
        if ((st.tex_draws[c] || st.tex_unsupported[c]) && len < (int)sizeof buf - 40)
            len += snprintf(buf + len, sizeof buf - len, " %02x=%u%s", c, st.tex_draws[c] + st.tex_unsupported[c],
                            st.tex_unsupported[c] ? " (unsupported)" : "");
    buf[len] = 0;
    printf("[d3d] stage-0 texture formats of recorded draws:%s\n", len ? buf : " none");
}

/* --dump-textures / --texture-pack (src/texpack.h): opened on the main thread with the first
 * texture; the manifest is rewritten every 600 frames and on exit. */
static struct {
    int init, dump, pack;
    uint32_t flushed_at, replaced, missing;
    double load_ms;
} tp;
static int tp_loaders;   /* texture pack loader threads (see tp_loader) */
static void tp_start_loaders(void);

void (*hle_d3d_on_exit)(void);

static int presenting;   /* the renderer presents to the window (renderer_present) */
static void finish(const char *why) {
    printf("[d3d] exit: %s\n", why);
    if (hle_d3d_on_exit) hle_d3d_on_exit();
    if (presenting) renderer_shutdown();   /* release the swapchain before its window */
    if (hle_d3d_dump_path) {
        int w = 0, h = 0;
        uint32_t *px = hle_d3d_last_frame(&w, &h);
        if (px && !video_write_bmp(hle_d3d_dump_path, px, w, h, w * 4))
            printf("[d3d] frame %u written to %s (%dx%d)\n", hle_d3d_frame_count(), hle_d3d_dump_path, w, h);
        else
            printf("[d3d] no frame written to %s\n", hle_d3d_dump_path);
        free(px);
    }
    hle_d3d_summary();
    if (tp.dump) printf("[texpack] %d textures listed in %s/manifest.csv\n", texpack_dump_close(), hle_d3d_dump_textures);
    if (tp.pack) printf("[texpack] %u textures replaced (%.0f ms uploading, %d loader threads)\n", tp.replaced, tp.load_ms, tp_loaders);
    video_close();
    plat_flush_output();
    _exit(0);   /* guest threads are still running; skip atexit handlers of the host libraries */
}

/* Host main thread: creates the renderer next to the video output. --renderer none (or a failed
 * initialization) keeps the CPU-only path. */
static int renderer_on, out_w, out_h;   /* renderer target size (renderer_output_size) */
static int present_mode(void) {
    return !strcmp(hle_d3d_present, "fifo") ? RENDERER_PRESENT_FIFO : !strcmp(hle_d3d_present, "immediate") ? RENDERER_PRESENT_IMMEDIATE
           : !strcmp(hle_d3d_present, "mailbox")                    ? RENDERER_PRESENT_MAILBOX
                                                                    : RENDERER_PRESENT_AUTO;
}
/* After video_open: the renderer, presenting to the window when the video backend made a Vulkan
 * window, else offscreen; logs the present path. */
static void renderer_open(uint32_t w, uint32_t h) {
    if (hle_d3d_glass.hd)
    {
        printf("[hud] options: hd on (height %.2f, opacity %.2f, frost %.1f", hle_d3d_glass.hd_height, hle_d3d_glass.hd_opacity,
               hle_d3d_glass.hd_frost);
        if (hle_d3d_glass.hd_frame) {
            const HudHdStyle s = hud_hd_style(&hle_d3d_glass);
            printf(", glass style: track %.2f, fill %.2f, vivid %.2f", s.track_opacity, s.fill_opacity, s.vivid);
        }
        printf("), ");
    }
    else
        printf("[hud] options: hd off, ");
    if (hle_d3d_glass.on)
        printf("glass on (blur %.1f, opacity %.2f, tint %08x)\n", hle_d3d_glass.blur, hle_d3d_glass.opacity, hle_d3d_glass.tint);
    else
        printf("glass off\n");
    if (strcmp(hle_d3d_renderer, "vulkan")) {
        printf("[renderer] none (CPU path only)\n");
        video_vulkan_fallback();
        return;
    }
    RendererConfig c = {.samples = hle_d3d_msaa > 1 ? hle_d3d_msaa : 1, .aniso = hle_d3d_aniso, .present_mode = present_mode(),
                        .frames_in_flight = hle_d3d_frames_in_flight, .supersample = hle_d3d_supersample,
                        .shadow_scale = hle_d3d_shadow_scale, .shadow_soft = hle_d3d_shadow_soft, .sharpen = hle_d3d_sharpen,
                        .glass = hle_d3d_glass.on || (hle_d3d_glass.hd && hle_d3d_glass.hd_frost > 0), .bloom = hle_d3d_bloom.on,
                        .bloom_strength = hle_d3d_bloom.strength, .bloom_threshold = hle_d3d_bloom.threshold,
                        .shadow_range = hle_d3d_shadow_range.on ? hle_d3d_shadow_range.dist : 0, .rain = hle_d3d_rain.on,
                        .rain_wet = hle_d3d_rain.on && hle_d3d_rain.wet, .rain_cover = hle_d3d_rain.on && hle_d3d_rain.cover,
                        .ssao = hle_d3d_ssao.on, .ssao_strength = hle_d3d_ssao.strength, .ssao_radius = hle_d3d_ssao.radius};
    hle_d3d_output_size((int)w, (int)h, &c.out_w, &c.out_h);
    c.instance_ext = video_vulkan_extensions(&c.instance_ext_count);
    if (c.instance_ext) {
        c.create_surface = video_vulkan_surface;
        c.drawable_size = video_vulkan_drawable;
    }
    renderer_configure(&c);
    int failed = renderer_init((int)w, (int)h);
    if (failed && c.create_surface) {   /* retry offscreen */
        printf("[renderer] Vulkan with a window surface failed (%s); retrying offscreen\n", renderer_error());
        c.instance_ext = NULL;
        c.instance_ext_count = 0;
        c.create_surface = NULL;
        renderer_configure(&c);
        failed = renderer_init((int)w, (int)h);
    }
    presenting = !failed && renderer_can_present();
    if (!presenting && video_vulkan_fallback()) finish("no window");
    if (failed) {
        printf("[renderer] Vulkan unavailable (%s); using the CPU path only\n", renderer_error());
        return;
    }
    renderer_on = 1;
    renderer_output_size(&out_w, &out_h);
    printf("[renderer] Vulkan on %s, %dx%d (back buffer %ux%u), %d samples, anisotropy %.0f\n", renderer_device_name(), out_w, out_h, w,
           h, renderer_samples(), renderer_anisotropy());
    if (renderer_supersample() > 1 || hle_d3d_shadow_scale > 1 || hle_d3d_shadow_soft)
        printf("[renderer] supersample %dx (internal %dx%d), shadow maps %dx, soft shadows %s\n", renderer_supersample(),
               out_w * renderer_supersample(), out_h * renderer_supersample(), hle_d3d_shadow_scale > 1 ? hle_d3d_shadow_scale : 1,
               hle_d3d_shadow_soft ? "on" : "off");
    if (hle_d3d_shadow_range.on) printf("[renderer] shadow range %.2f (character shadows fade out from %.2f to %.2f below the caster)\n",
                                        hle_d3d_shadow_range.dist, hle_d3d_shadow_range.dist, 1.5f * hle_d3d_shadow_range.dist);
    if (hle_d3d_sharpen > 0) {
        if (renderer_sharpen()) printf("[renderer] sharpening (contrast-adaptive) %d\n", renderer_sharpen());
        else printf("[renderer] sharpening unavailable on this device; frames are not sharpened\n");
    }
    if (hle_d3d_bloom.on) {
        if (renderer_bloom_on()) printf("[renderer] bloom on (strength %.2f, threshold %.2f)\n", hle_d3d_bloom.strength, hle_d3d_bloom.threshold);
        else printf("[renderer] bloom unavailable on this device; frames are drawn without it\n");
    }
    if (hle_d3d_ssao.on) {
        if (renderer_ssao_on()) printf("[renderer] ambient occlusion on (strength %.2f, radius %.2f)\n", hle_d3d_ssao.strength, hle_d3d_ssao.radius);
        else printf("[renderer] ambient occlusion unavailable on this device; frames are drawn without it\n");
    }
    if (hle_d3d_rain.on) {
        if (renderer_rain_on()) printf("[renderer] better rain on (density %.2f)\n", hle_d3d_rain.density);
        else printf("[renderer] better rain unavailable on this device; the title's rain is drawn as is\n");
    }
    if (presenting)
        printf("[video] present: Vulkan %s (no readback), %d frame%s in flight\n", renderer_present_info(), renderer_frames_in_flight(),
               renderer_frames_in_flight() > 1 ? "s" : "");
    else
        printf("[video] present: readback through the %s backend%s%s%s\n", hle_d3d_video, c.create_surface ? " (" : "",
               c.create_surface ? renderer_present_info() : "", c.create_surface ? ")" : "");
}

/* Once, on the first frame drawn only by clears: replays its clears through the renderer and
 * compares the readback with the CPU-filled back buffer. */
static void clear_parity_check(const FrameList *l, const uint32_t *px, uint32_t w, uint32_t h) {
    static int done;
    if (done || !renderer_on || l->cpu_draws || !l->nops || out_w != (int)w || out_h != (int)h) return;
    done = 1;
    uint32_t *out = malloc((size_t)w * h * 4), same = 0;
    if (!out) return;
    renderer_begin(NULL, 0);
    for (uint32_t i = 0; i < l->nops; i++)
        if (l->ops[i].kind == OP_CLEAR) renderer_clear(l->ops[i].flags, l->ops[i].color, l->ops[i].z);
    if (!renderer_end(out, (int)w * 4))
        for (uint32_t i = 0; i < w * h; i++) same += (out[i] & 0xFFFFFFu) == (px[i] & 0xFFFFFFu);
    printf("[renderer] clear parity: %u of %u pixels match the CPU clear (%u ops)\n", same, w * h, l->nops);
    free(out);
}

#define TEXPACK_BUDGET (1024ull << 20)   /* renderer texture cache budget with a pack */

static void texpack_init(void) {
    tp.init = 1;
    if (hle_d3d_dump_textures) {
        tp.dump = !texpack_dump_open(hle_d3d_dump_textures);
        printf("[texpack] dumping textures to %s%s\n", hle_d3d_dump_textures, tp.dump ? "" : ": cannot create the directory");
    }
    int total = 0;
    for (int i = 0; i < hle_d3d_texture_pack_count; i++) {
        const char *dir = hle_d3d_texture_packs[i];
        int n = i ? texpack_add(dir) : texpack_open(dir);
        if (n < 0) printf("[texpack] cannot read the texture pack %s\n", dir);
        else {
            printf("[texpack] %d replacement textures in %s%s\n", n, dir, i ? " (not in an earlier pack)" : "");
            total += n;
        }
    }
    if (hle_d3d_texture_pack_count) {
        tp.pack = total > 0;
        if (texpack_pack_count() > 1) {
            printf("[texpack] %d packs, first match wins:", texpack_pack_count());
            for (int i = 0; i < hle_d3d_texture_pack_count; i++) printf(" %s", hle_d3d_texture_packs[i]);
            printf(" (%d textures)\n", total);
        }
        /* Replacements are uncompressed and typically 4x larger: a larger budget keeps the
         * renderer from dropping (and the pack from reloading) them every few frames. */
        if (tp.pack) {
            renderer_set_texture_budget(TEXPACK_BUDGET);
            tp_start_loaders();
        }
    }
}

/* Content hash of a draw's stage texture (memoized per cache key); a texture seen for the first
 * time is dumped (level 0 decoded on the CPU). */
static uint64_t texture_hash(const FrameOp *op, int stage, const XTexInfo *t) {
    uint64_t key = op->tex_key[stage], hash;
    if (texpack_key_hash(key, &hash)) return hash;
    const uint8_t *src = G(op->tex_data[stage]);
    hash = texpack_hash(t, src);
    texpack_key_set(key, hash);
    if (tp.dump) {
        uint32_t *px = malloc((size_t)t->w * t->h * 4);
        uint32_t frame = hle_d3d_frame_count();
        if (px && !xtex_decode_level(t, 0, src, px) && texpack_dump_texture(hash, t, px, frame) < 0 && !tp.missing++)
            printf("[texpack] cannot write textures to %s\n", hle_d3d_dump_textures);
        free(px);
    }
    return hash;
}

/* Replacements are decoded (PNG + mip chain, tens of ms for an upscaled pack) on loader threads,
 * so a texture seen for the first time never stalls the frame: until its replacement is ready the
 * draw samples the guest's own texture, cached under TP_FALLBACK(key) so it never takes the
 * replacement's key. Slots: 0 free, 1 queued, 2 loading, 3 ready, 4 failed. */
#define TP_SLOTS 64
#define TP_LOADERS 2
#define TP_UPLOADS 2
#define TP_FALLBACK(key) ((key) ^ 0x5bd1e9955bd1e995ull)
static struct {
    uint64_t hash;
    char *path;
    uint32_t *px, w, h, levels;
    int state;
} tp_req[TP_SLOTS];
static pthread_mutex_t tp_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tp_cv = PTHREAD_COND_INITIALIZER;

static void *tp_loader(void *arg) {
    (void)arg;
    pthread_mutex_lock(&tp_mx);
    for (;;) {
        int i = 0;
        while (i < TP_SLOTS && tp_req[i].state != 1) i++;
        if (i == TP_SLOTS) {
            pthread_cond_wait(&tp_cv, &tp_mx);
            continue;
        }
        tp_req[i].state = 2;
        char *path = tp_req[i].path;
        tp_req[i].path = NULL;
        pthread_mutex_unlock(&tp_mx);
        uint32_t w = 0, h = 0, levels = 0, *px = texpack_load_file(path, &w, &h, &levels);
        free(path);
        pthread_mutex_lock(&tp_mx);
        tp_req[i].px = px;
        tp_req[i].w = w;
        tp_req[i].h = h;
        tp_req[i].levels = levels;
        tp_req[i].state = px ? 3 : 4;
    }
    return NULL;
}

static void tp_start_loaders(void) {
    for (int i = 0; i < TP_LOADERS; i++) {
        pthread_t th;
        if (pthread_create(&th, NULL, tp_loader, NULL)) break;
        pthread_detach(th);
        tp_loaders++;
    }
}

static void tp_failed(uint64_t hash) {
    if (!tp.missing++) printf("[texpack] cannot load the replacement %016llx\n", (unsigned long long)hash);
    texpack_disable(hash);   /* the original is used from now on */
}

/* Replacement from the pack, uploaded with its mip chain under the guest texture's cache key.
 * Texture coordinates are normalized by then (LIN_* coordinates are divided by the guest size
 * when recorded), so a replacement of any size samples the same area. Returns 0 with *pending
 * set while the loaders still work on it. */
static uint32_t replace_texture(uint64_t key, uint64_t hash, int *pending) {
    uint32_t w, h, levels, *px = NULL;
    *pending = 0;
    if (!tp_loaders) {
        px = texpack_load(hash, &w, &h, &levels);
        if (!px) {
            tp_failed(hash);
            return 0;
        }
    } else {
        int i = 0, free_slot = -1, state;
        pthread_mutex_lock(&tp_mx);
        for (; i < TP_SLOTS && !(tp_req[i].state && tp_req[i].hash == hash); i++)
            if (!tp_req[i].state && free_slot < 0) free_slot = i;
        if (i == TP_SLOTS) {   /* not requested yet */
            if (free_slot >= 0 && (tp_req[free_slot].path = texpack_path(hash))) {
                tp_req[free_slot].hash = hash;
                tp_req[free_slot].state = 1;
                pthread_cond_signal(&tp_cv);
            }
            pthread_mutex_unlock(&tp_mx);
            *pending = 1;   /* a full queue retries on a later draw */
            return 0;
        }
        /* Uploads cost a few ms each (mips included): at most TP_UPLOADS per frame, the rest
         * wait in their slots for the next frames. */
        static uint32_t up_frame, ups;
        uint32_t frame = hle_d3d_frame_count();
        if (frame != up_frame) {
            up_frame = frame;
            ups = 0;
        }
        state = tp_req[i].state;
        if (state == 3 && ups >= TP_UPLOADS) state = 2;
        if (state >= 3) {
            ups++;
            px = tp_req[i].px;
            w = tp_req[i].w;
            h = tp_req[i].h;
            levels = tp_req[i].levels;
            tp_req[i].px = NULL;
            tp_req[i].state = 0;
        }
        pthread_mutex_unlock(&tp_mx);
        if (state < 3) {
            *pending = 1;
            return 0;
        }
        if (!px) {
            tp_failed(hash);
            return 0;
        }
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t handle = renderer_texture_create(key, RENDERER_TEX_BGRA8, w, h, levels, px);
    free(px);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    tp.load_ms += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    tp.replaced += handle != 0;
    return handle;
}

/* Keyboard/mouse button icons (--button-icons kbm|auto): the font/icon atlas painted with the
 * current key map's icons, uploaded once under KBM_KEY(key) beside the original; each draw picks
 * the key for the last used device. The painted mip chain is kept, so the renderer dropping the
 * texture costs one upload, not another paint. */
#define KBM_KEY(key) ((key) ^ 0x6b626d5f69636f6eull)
static int kbm_icons_now(void) {
    if (!glyph_nrects || hle_d3d_button_icons == HLE_ICONS_XBOX) return 0;
    return hle_d3d_button_icons == HLE_ICONS_KBM || input_last_device() != INPUT_DEVICE_PAD;
}

static uint32_t kbm_atlas(uint64_t key, uint64_t hash, const XTexInfo *t, const uint8_t *src) {
    static uint32_t *chain, cw, ch, clevels;
    static int failed;
    if (failed) return 0;
    if (!chain) {
        uint32_t w = t->w, h = t->h, levels;
        uint32_t *px = tp.pack && texpack_has(hash) ? texpack_load(hash, &w, &h, &levels) : NULL;
        const char *from = px ? "the texture pack's atlas" : "the game's atlas";
        if (!px && (px = malloc((size_t)t->w * t->h * 4)) && xtex_decode_level(t, 0, src, px)) {
            free(px);
            px = NULL;
        }
        if (!px) {
            failed = 1;
            printf("[glyphs] cannot read the font atlas: Xbox button icons kept\n");
            return 0;
        }
        int keys[IN_COUNT];
        for (int c = 0; c < IN_COUNT; c++) keys[c] = input_key_for_control(c);
        int n = kbm_paint_atlas(px, w, h, glyph_rects, glyph_nrects, keys);
        printf("[glyphs] keyboard/mouse icons: %d cells painted on %s (%ux%u)", n, from, w, h);
        for (int i = 0, first = 1; i < kbm_cell_count(); i++)
            if (keys[kbm_cell(i)->control] < 0) {
                printf("%s %s", first ? "; unbound, Xbox glyph kept:" : ",", kbm_cell(i)->what);
                first = 0;
            }
        printf("\n");
        chain = texpack_mips(px, w, h, &clevels);
        free(px);
        if (!chain) {
            failed = 1;
            return 0;
        }
        cw = w;
        ch = h;
    }
    return renderer_texture_create(KBM_KEY(key), RENDERER_TEX_BGRA8, cw, ch, clevels, chain);
}

/* Host texture of a recorded draw's stage: cached by key, otherwise the pack's replacement (by
 * content hash) or decoded from guest memory (DXT as BC blocks when the device samples them,
 * everything else to B8G8R8A8 on the CPU) and uploaded. */
static uint32_t resolve_texture(const FrameOp *op, int stage) {
    static uint32_t *buf;
    static size_t cap;
    static int logged, failed;
    uint64_t key = op->tex_key[stage], hash = 0;
    if (!tp.init) texpack_init();
    XTexInfo t;
    int known = !xtex_info(op->tex_format[stage], op->tex_size[stage], &t);
    if (known && (tp.dump || tp.pack || glyph_nrects)) {
        hash = texture_hash(op, stage, &t);
        if (tp.dump) {
            texpack_dump_draw(hash);
            uint32_t frame = hle_d3d_frame_count();
            if (frame >= tp.flushed_at + 600) {
                tp.flushed_at = frame;
                texpack_dump_flush();
            }
        }
    }
    if (known && hash == KBM_ATLAS_HASH && kbm_icons_now()) {
        uint32_t k = renderer_texture_find(KBM_KEY(key));
        if (k || (k = kbm_atlas(key, hash, &t, G(op->tex_data[stage])))) return k;
    }
    uint32_t h = renderer_texture_find(key);
    if (h || !known) return h;
    if (tp.pack && texpack_has(hash)) {
        int pending;
        if ((h = replace_texture(key, hash, &pending))) return h;
        if (pending) {
            key = TP_FALLBACK(key);
            if ((h = renderer_texture_find(key))) return h;
        }
    }
    uint32_t max_levels = 1;
    while ((1u << (max_levels - 1)) < (t.w > t.h ? t.w : t.h)) max_levels++;
    uint32_t levels = t.levels < max_levels ? t.levels : max_levels;
    const uint8_t *src = G(op->tex_data[stage]);
    int bc = t.dxt && renderer_has_bc();
    if (bc) {
        h = renderer_texture_create(key, t.code == 0x0C ? RENDERER_TEX_BC1 : t.code == 0x0E ? RENDERER_TEX_BC2 : RENDERER_TEX_BC3,
                                    t.w, t.h, levels, src);
    } else {
        size_t need = 0;
        for (uint32_t l = 0; l < levels; l++) need += (size_t)xtex_level_w(&t, l) * xtex_level_h(&t, l);
        if (need > cap) {
            uint32_t *n = realloc(buf, need * 4);
            if (!n) return 0;
            buf = n;
            cap = need;
        }
        size_t off = 0;
        for (uint32_t l = 0; l < levels; l++) {
            if (xtex_decode_level(&t, l, src, buf + off)) return 0;
            off += (size_t)xtex_level_w(&t, l) * xtex_level_h(&t, l);
        }
        h = renderer_texture_create(key, RENDERER_TEX_BGRA8, t.w, t.h, levels, buf);
    }
    if (!h && !failed++) printf("[renderer] texture upload failed: %s\n", renderer_error());
    if (h && !logged++) printf("[renderer] textures: DXT uploaded as %s\n", renderer_has_bc() ? "BC1-3 blocks" : "B8G8R8A8 (CPU decode)");
    return h;
}

/* Replays a frame into out (the renderer's output size), or presents it when out is NULL: the
 * back buffer as it was at the first draw is the initial color (px, the posted frame, when it has
 * no draws), earlier clears only contribute their depth part, later operations run in order.
 * Returns 0 when out holds the frame (or it was presented). */
/* Host surface of a registry entry (created on first use; renderer_surface keeps it by key). */
static uint32_t rt_handle(uint8_t idx) {
    static uint32_t handle[MAX_RT_SURFACES];
    if (!idx) return 0;
    const RtSurface *r = &rt_surf[idx - 1];
    if (!handle[idx - 1]) handle[idx - 1] = renderer_surface((uint64_t)r->pa << 8 | r->kind, r->kind, (int)r->w, (int)r->h);
    return handle[idx - 1];
}
/* The depth projection of a frame (clip z = A w - B; ab = {A, B}) fitted from its depth-tested
 * back-buffer draws (rain_depth_fit per draw, rain_depth_pick of them); returns the draws fitted. */
static uint32_t frame_depth_fit(const FrameList *l, float ab[2]) {
    enum { FITS = 256, FIT_VERTS = 256 };
    static float fa[FITS], fb[FITS];
    uint32_t nf = 0;
    for (uint32_t i = 0; i < l->nops && nf < FITS; i++) {
        const FrameOp *op = &l->ops[i];
        if (op->kind != OP_DRAW || op->target_c || op->target_z || !(op->draw_flags & RENDERER_DEPTH_TEST) || op->nv < 3) continue;
        float f[2];
        if (!rain_depth_fit(&l->v[op->first_vertex].z, sizeof(RVertex) / sizeof(float), op->nv < FIT_VERTS ? op->nv : FIT_VERTS, f)) {
            fa[nf] = f[0];
            fb[nf++] = f[1];
        }
    }
    rain_depth_pick(fa, fb, nf, ab);
    return nf;
}
/* --rain-hd: the frame's rain layer ops (their parameters and scroll were read while recording);
 * returns how many. */
static int frame_layers(const FrameList *l, RainLayer *layers, float (*scroll)[2]) {
    int n = 0;
    for (uint32_t i = 0; i < l->nops && n < RAIN_MAX_LAYERS; i++)
        if (l->ops[i].kind == OP_DRAW && l->ops[i].rain) {
            layers[n] = l->ops[i].rain_layer;
            memcpy(scroll[n++], l->ops[i].rain_scroll, sizeof scroll[0]);
        }
    return n;
}
/* --rain-hd: the procedural rain for the layers to draw (rain_fade_step: the frame's, or the last
 * ones while fading out) with the frame's depth projection (frame_depth_fit). Returns 0 without
 * layers. */
static int frame_rain(const FrameList *l, const RainLayer *layers, const float (*scroll)[2], int n, RRain *r) {
    if (!n) return 0;
    float ab[2];
    const uint32_t nf = frame_depth_fit(l, ab);
    static int logged;
    if (!logged++)
        printf("[rain] the title's rain (%d layer%s, colour %08x) is drawn as procedural rain; depth projection A %.5f B %.5f from %u draws\n", n,
               n == 1 ? "" : "s", layers[0].color, ab[0], ab[1], nf);
    return rain_build(&hle_d3d_rain, layers, scroll, n, ab, r);
}
/* The end of a frame's opaque world (ssao_split: its last depth-tested, depth-writing back-buffer
 * draw after the last depth clear), or l->nops; kind: the classification (valid until the next call). */
static uint32_t frame_world_end(const FrameList *l, const uint8_t **kind_out) {
    static uint8_t *kind;
    static uint32_t kind_cap;
    if (!grow((void **)&kind, &kind_cap, l->nops, sizeof *kind)) return l->nops;
    for (uint32_t i = 0; i < l->nops; i++) {
        const FrameOp *op = &l->ops[i];
        const int off = op->target_c || op->target_z;
        kind[i] = op->kind == OP_CLEAR ? (!off && (op->flags & RENDERER_CLEAR_DEPTH) ? SSAO_OP_CLEAR : SSAO_OP_OTHER)
                  : op->kind == OP_DRAW && ssao_opaque_draw(off, op->draw_flags, op->rs.depth_write) ? SSAO_OP_OPAQUE
                                                                                                    : SSAO_OP_OTHER;
    }
    if (kind_out) *kind_out = kind;
    return ssao_split(kind, l->nops);
}
/* --ssao: the operation after which the occlusion runs (frame_world_end), or l->nops; p = the
 * frame's depth projection (frame_depth_fit) and the camera's x/y scale (that draw's, else the
 * nearest opaque draw before it that has one, else the usual camera's). */
static uint32_t frame_ssao(const FrameList *l, RSsao *p) {
    const uint8_t *kind;
    const uint32_t at = frame_world_end(l, &kind);
    if (at == l->nops) return at;
    float ab[2];
    const uint32_t nf = frame_depth_fit(l, ab);
    p->depth_a = ab[0];
    p->depth_b = ab[1];
    p->px = SSAO_PX_DEFAULT;
    p->py = SSAO_PY_DEFAULT;
    for (uint32_t i = at + 1; i-- > 0;)
        if (kind[i] == SSAO_OP_OPAQUE && l->ops[i].ssao_scale[0] > 0) {
            p->px = l->ops[i].ssao_scale[0];
            p->py = l->ops[i].ssao_scale[1];
            break;
        }
    static int logged;
    if (!logged && nf && (logged = 1))   /* the first frame with a fitted projection */
        printf("[ssao] occlusion after the opaque world (op %u of %u); depth projection A %.5f B %.5f from %u draws, scale %.4f %.4f\n", at,
               l->nops, ab[0], ab[1], nf, p->px, p->py);
    return at;
}
/* --rain-hd splashes: the operation after which they are drawn (the end of the opaque world,
 * frame_world_end), or l->nops without layers (n of them, as for frame_rain), camera or opaque
 * world; p = their parameters (rain_splash_build: the layers, the camera, the title's clock, the
 * depth projection). */
static uint32_t frame_splash(const FrameList *l, const RainLayer *layers, int n, RRainSplash *p) {
    if (!n || !l->has_cam) return l->nops;
    const uint32_t at = frame_world_end(l, NULL);
    if (at == l->nops) return at;
    static uint32_t fallback_ms;   /* without the title's clock: 60 frames per second */
    fallback_ms += 17;
    float ab[2];
    frame_depth_fit(l, ab);
    const uint32_t ms = l->has_clock ? l->clock_ms : l->has_frame_ms ? l->frame_ms : fallback_ms;
    if (!rain_splash_build(&hle_d3d_rain, layers, n, l->cam_view, l->cam_proj, ms, ab, p)) return l->nops;
    static int logged;
    if (!logged++)
        printf("[rain] splashes and ripples after the opaque world (op %u of %u): grid %.3f, chance %.2f, camera scale %.4f %.4f%s\n", at, l->nops,
               p->cell, p->chance, p->px, p->py, l->has_clock || l->has_frame_ms ? "" : ", no title clock");
    return at;
}
/* --rain-hd wet ground: 1 and its parameters (rain_wet_build: the layers, n of them as for
 * frame_rain, the camera, the depth projection) with layers and a camera, else 0. */
static int frame_wet(const FrameList *l, const RainLayer *layers, int n, RRainWet *p) {
    if (!n || !l->has_cam) return 0;
    float ab[2];
    frame_depth_fit(l, ab);
    if (!rain_wet_build(&hle_d3d_rain, layers, n, l->cam_view, l->cam_proj, ab, p)) return 0;
    static int logged;
    if (!logged++)
        printf("[rain] wet ground where the 3D scene ends: reflection %.2f, darken %.2f, up (view) %.3f %.3f %.3f\n", p->reflect, p->darken, p->up_view[0],
               p->up_view[1], p->up_view[2]);
    return 1;
}
static int render_frame(const FrameList *l, const uint32_t *px, uint32_t w, uint32_t *out) {
    static int warned;
    renderer_begin(l->draws && l->base ? l->base : px, (int)w * 4);
    /* --bloom: the scene ends with its last depth-tested back-buffer draw; the HUD, text and menu
     * overlays come after it without depth test (none in a frame of menus or movies: no bloom). The
     * lamp halo draws move to right after the split (bloom_order): they are not bloomed. Without
     * bloom the ops replay in their recorded order. */
    static uint32_t *order;
    static uint8_t *kind;
    static uint32_t order_cap, kind_cap;
    uint32_t split = l->nops;   /* replay position after which renderer_bloom runs (none: nops) */
    const uint32_t *ord = NULL;
    if (hle_d3d_bloom.on && renderer_bloom_on() && l->draws && grow((void **)&order, &order_cap, l->nops, sizeof *order) &&
        grow((void **)&kind, &kind_cap, l->nops, sizeof *kind)) {
        for (uint32_t i = 0; i < l->nops; i++) {
            const FrameOp *op = &l->ops[i];
            const int off = op->target_c || op->target_z;
            kind[i] = op->kind == OP_CLEAR ? (off ? BLOOM_OP_OTHER : BLOOM_OP_CLEAR)
                      : op->kind != OP_DRAW || !bloom_scene_draw(off, op->draw_flags) ? BLOOM_OP_OTHER
                      : op->halo ? BLOOM_OP_HALO : BLOOM_OP_SCENE;
        }
        const uint32_t at = bloom_order(kind, l->nops, order);
        if (at) split = at - 1;
        ord = order;
    }
    /* --rain-hd: the title's rain layer draws are replaced, at the first of them, by the procedural
     * rain (no depth test: they come after the bloom split, before the HUD drawn after them). When
     * the title starts or stops its layers the rain fades in or out by its clock (rain_fade_step);
     * while fading out (no layer ops) it is drawn after the frame's last depth-tested back-buffer
     * draw, where the layers would be. */
    static RainFade fade;
    RainLayer layers[RAIN_MAX_LAYERS];
    float scroll[RAIN_MAX_LAYERS][2];
    int nlayers = 0;
    float level = 0;
    if (hle_d3d_rain.on && renderer_rain_on() && l->draws) {
        nlayers = frame_layers(l, layers, scroll);
        level = rain_fade_step(&fade, layers, (const float(*)[2])scroll, nlayers, l->has_frame_ms, l->frame_ms);
    }
    RRain rr;
    int rain = level > 0 && frame_rain(l, fade.layer, (const float(*)[2])fade.scroll, fade.n, &rr);
    /* The replay position of the frame's last depth-tested back-buffer draw (not a rain layer):
     * the 3D scene ends there (with bloom: the halos moved after the split, else the split's own
     * draw); the HUD, text and other overlays come after it without depth test. */
    uint32_t scene_k = l->nops;
    for (uint32_t k = 0; k < l->nops && rain; k++) {
        const FrameOp *op = &l->ops[ord ? ord[k] : k];
        if (op->kind == OP_DRAW && !op->rain && !op->target_c && !op->target_z && (op->draw_flags & RENDERER_DEPTH_TEST)) scene_k = k;
    }
    uint32_t rain_k = l->nops;   /* fading out: the replay position the rain follows */
    if (rain && !nlayers) {
        rain_k = scene_k;
        rain = rain_k < l->nops;   /* no 3D scene (menus): none */
        static int logged;
        if (rain && !logged++) printf("[rain] the title's rain stopped: rain HD fades out over %.0f ms of the title's clock\n", RAIN_FADE_MS);
    }
    if (rain) rain_fade_apply(level, &rr, NULL, NULL);
    /* --ssao: after the opaque world's last draw, before the effects, halos, rain and HUD. */
    RSsao sp;
    const uint32_t ssao_at = hle_d3d_ssao.on && renderer_ssao_on() && l->draws ? frame_ssao(l, &sp) : l->nops;
    /* --rain-hd splashes: right after it (the same split when both run). */
    RRainSplash rs;
    const uint32_t splash_at = rain && hle_d3d_rain.splash && renderer_rain_splash_on() ? frame_splash(l, fade.layer, fade.n, &rs) : l->nops;
    if (splash_at < l->nops) rain_fade_apply(level, NULL, &rs, NULL);
    /* --rain-hd wet ground: where the 3D scene ends (scene_k: the effects, halos and bloom are
     * drawn by then, so they are reflected too), before any HUD, text or overlay, which the colour
     * it reflects must not hold (the title may draw them before its rain layers). */
    RRainWet rw;
    const int wet = rain && scene_k < l->nops && hle_d3d_rain.wet && renderer_rain_wet_on() && frame_wet(l, fade.layer, fade.n, &rw);
    if (wet) rain_fade_apply(level, NULL, NULL, &rw);
    const uint32_t wet_k = wet ? scene_k : l->nops;
    /* --rain-hd-cover: the opaque world (the SSAO set: depth-tested, depth-writing back-buffer
     * draws after the frame's last depth clear, so not the sky) is drawn again from above into the
     * rain cover map, and covered places get no streaks, splashes or wet ground. */
    uint32_t cover_from = l->nops, cover_to = 0;
    RRainCover rc;
    if (rain && hle_d3d_rain.cover && renderer_rain_cover_on() && l->has_cam && rain_cover_build(&hle_d3d_rain, l->cam_view, l->cam_proj, &rc)) {
        const uint8_t *kind;
        const uint32_t end = frame_world_end(l, &kind);
        if (end < l->nops) {
            uint32_t from = 0, n = 0;
            for (uint32_t i = 0; i <= end; i++)
                if (kind[i] == SSAO_OP_CLEAR) from = i + 1;
            for (uint32_t i = from; i <= end; i++) n += kind[i] == SSAO_OP_OPAQUE;
            cover_from = from;
            cover_to = end;
            renderer_rain_cover(&rc);
            static int logged;
            if (!logged++)
                printf("[rain] cover from above: %u opaque world draws (ops %u-%u) in a %.0f x %.0f unit map around the camera\n", n, from, end,
                       RAIN_COVER_RANGE, RAIN_COVER_RANGE);
        }
    }
    int rain_done = 0;
    for (uint32_t k = 0; k < l->nops && l->draws; k++) {
        const uint32_t i = ord ? ord[k] : k;
        const FrameOp *op = &l->ops[i];
        int off = op->target_c || op->target_z;
        renderer_set_target(rt_handle(op->target_c), rt_handle(op->target_z));
        if (op->kind == OP_CLEAR) {
            uint32_t fl = i < l->first_draw && !off ? op->flags & RENDERER_CLEAR_DEPTH : op->flags;
            if (fl) renderer_clear(fl, op->color, op->z);
        } else if (op->kind == OP_GLASS) {
            renderer_glass(&op->glass);
        } else if (rain && op->rain) {
            if (!rain_done++) renderer_rain(&rr);
        } else {
            for (int s = 0; s < RENDERER_STAGES; s++)
                renderer_set_stage_texture(s, op->tex_surf[s] ? rt_handle(op->tex_surf[s]) : op->tex_key[s] ? resolve_texture(op, s) : 0);
            renderer_set_pixel_shader(op->has_ps ? &op->ps : NULL);
            /* --rain-hd: the title blends most of its world with depth writes and an alpha test that
             * keeps nearly every texel (GREATER 1), so the see-through parts of fences, wire and
             * crates would hide the rain slices behind them in flat panels. Before the rain such a
             * draw drops its texels under an eighth opaque (rain_soft_depth). */
            RainSoftDepth sd;
            if (rain && !rain_done && !off &&
                rain_soft_depth((op->draw_flags & RENDERER_DEPTH_TEST) != 0, op->rs.depth_write, op->rs.blend_enable, op->rs.blend_src, op->alpha_func, op->alpha_ref, &sd))
                renderer_set_alpha_test(sd.depth_func, sd.depth_ref);
            else
                renderer_set_alpha_test(op->alpha_func, op->alpha_ref);
            renderer_set_state(&op->rs);
            const int cover = i >= cover_from && i <= cover_to && !off && ssao_opaque_draw(off, op->draw_flags, op->rs.depth_write);
            renderer_draw(&op->vp, op->draw_flags | (cover ? RENDERER_RAIN_COVER : 0), l->v + op->first_vertex, op->nv, l->idx + op->first_index, op->ni);
        }
        if (i == ssao_at) renderer_ssao(&sp);
        if (i == splash_at) renderer_rain_splash(&rs);
        if (k == split) renderer_bloom();
        if (k == wet_k) renderer_rain_wet(&rw);
        if (k == rain_k && !rain_done++) renderer_rain(&rr);
    }
    int failed = out ? renderer_end(out, out_w * 4) : renderer_present();
    if (hle_d3d_stats) renderer_last_timing(&hstat_end);
    if (failed) {
        if (!warned++) printf("[renderer] frame failed: %s\n", renderer_error());
        return -1;
    }
    if (l->cpu_after_draw && warned++ < 3) printf("[renderer] %u movie quads after 3D draws were not composited\n", l->cpu_after_draw);
    return 0;
}

/* Keeps the presented pixels of frame seq for --dump-frame (hle_d3d_last_frame). */
static void keep_shown(const uint32_t *px, uint32_t w, uint32_t h, uint32_t seq) {
    pthread_mutex_lock(&d3d_lock);
    if (st.sw != w || st.sh != h) {
        free(st.shown);
        st.shown = malloc((size_t)w * h * 4);
        st.sw = w;
        st.sh = h;
    }
    if (st.shown) {
        memcpy(st.shown, px, (size_t)w * h * 4);
        st.shown_seq = seq;
    }
    pthread_mutex_unlock(&d3d_lock);
}

/* Host thread: accumulates one presented frame and prints a line every 60. */
typedef struct { double t_read, read_age, t_swap, t_take; } FrameTimes;
static void host_stats(double render, double present, uint32_t draws, const FrameTimes *ft) {
    static double t_start, r_sum, e_sum[4], p_sum, lat[4];
    static uint32_t frames, draws_sum, lat_n;
    double t = now_ms();
    if (!t_start) t_start = t;
    if (ft->t_read) {   /* this frame read the pad: its input-to-present path */
        lat[0] += ft->read_age;
        lat[1] += ft->t_swap - ft->t_read;
        lat[2] += ft->t_take - ft->t_swap;
        lat[3] += t - ft->t_take;
        lat_n++;
    }
    r_sum += render;
    e_sum[0] += hstat_end.record_ms;   /* zero when the frame skipped the renderer */
    e_sum[1] += hstat_end.gpu_ms;
    e_sum[2] += hstat_end.readback_ms;
    e_sum[3] += hstat_end.present_ms;
    p_sum += present;
    draws_sum += draws;
    if (++frames < 60) return;
    double secs = (t - t_start) / 1e3;
    pthread_mutex_lock(&d3d_lock);
    double gf = gstat.g_frame, gr = gstat.g_record, gw = gstat.g_wait;
    uint32_t gs = gstat.g_swaps, sw = st.frames, dr = st.dropped, gv = gstat.g_verts;
    double ph[REC_PHASES], rd_ms = gstat.g_rd_ms;
    uint32_t gd = gstat.g_draws, rd_calls = gstat.g_rd_calls;
    memcpy(ph, gstat.g_ph, sizeof ph);
    gstat.g_frame = gstat.g_record = gstat.g_wait = 0;
    gstat.g_swaps = gstat.g_verts = 0;
    memset(gstat.g_ph, 0, sizeof gstat.g_ph);
    gstat.g_draws = gstat.g_rd_calls = 0;
    gstat.g_rd_ms = 0;
    pthread_mutex_unlock(&d3d_lock);
    double g = gs ? gs : 1;
    printf("[stats] swap %u: %.1f presented/s, %.0f draws/frame, %u dropped so far | guest %.1f ms/frame (recording draws %.1f, %.0f vertices), "
           "waited for host %.1f | host render %.1f ms/frame (command recording %.1f, GPU wait %.1f, readback %.1f, swapchain %.1f), present %.1f\n",
           sw, frames / (secs > 0 ? secs : 1), (double)draws_sum / frames, dr, gf / g, gr / g, gv / g, gw / g, r_sum / frames,
           e_sum[0] / frames, e_sum[1] / frames, e_sum[2] / frames, e_sum[3] / frames, p_sum / frames);
    if (gd) {   /* where the recording time goes, per recorded draw */
        char line[256];
        int k = 0;
        double us = 1e3 / gd;
        for (int i = 0; i < REC_PHASES; i++)
            k += snprintf(line + k, sizeof line - k, "%s%s %.2f", i ? ", " : "", rec_phase_name[i], ph[i] * us);
        printf("[stats] swap %u: recording %.2f us/draw over %.0f draws/frame, %.1f vertices/draw: %s; memory checks %.1f/draw, %.2f us/draw\n",
               sw, gr * us, gd / g, (double)gv / gd, line, (double)rd_calls / gd, rd_ms * us);
    }
    if (lat_n) {
        double n = lat_n;
        printf("[stats] swap %u: input latency %.1f ms = sample age at XInputGetState %.1f + read to Swap %.1f + queued %.1f "
               "+ render and present %.1f (%u frames read the pad)\n",
               sw, (lat[0] + lat[1] + lat[2] + lat[3]) / n, lat[0] / n, lat[1] / n, lat[2] / n, lat[3] / n, lat_n);
    }
    t_start = t;
    r_sum = p_sum = e_sum[0] = e_sum[1] = e_sum[2] = e_sum[3] = 0;
    lat[0] = lat[1] = lat[2] = lat[3] = 0;
    frames = draws_sum = lat_n = 0;
}

void hle_d3d_service(void) {
    static struct timespec last_pump;
    static uint32_t *px, *out;
    static size_t px_bytes;
    static int opened;
    static FrameList ops;
    FrameTimes ft = {0};
    int fresh = 0, done = st.done, pending;
    uint32_t w = 0, h = 0, seq = 0;
    pthread_mutex_lock(&d3d_lock);
    if (st.q_count) {   /* one frame per call, oldest first: every swap is presented in order */
        frame_px_recycle(px, px_bytes);
        frame_list_recycle(&ops);
        px = st.queue[st.q_head].px;
        w = st.queue[st.q_head].w;
        h = st.queue[st.q_head].h;
        px_bytes = (size_t)w * h * 4;
        seq = st.queue[st.q_head].seq;
        ops = st.queue[st.q_head].ops;
        ft = (FrameTimes){st.queue[st.q_head].t_read, st.queue[st.q_head].read_age, st.queue[st.q_head].t_swap,
                          hle_d3d_stats ? now_ms() : 0};
        memset(&st.queue[st.q_head], 0, sizeof st.queue[0]);
        st.q_head = (st.q_head + 1) % FRAME_QUEUE;
        st.q_count--;
        fresh = 1;
        pthread_cond_signal(&queue_space);
    }
    pending = st.q_count;
    pthread_mutex_unlock(&d3d_lock);
    if (fresh) {
        if (!opened) {
            /* Window: --resolution, else the back buffer (16:9 wide with --widescreen). A swapchain
             * needs the Vulkan renderer, the SDL window and no --dump-frame (which reads back). */
            int ww = hle_d3d_res_w > 0 ? hle_d3d_res_w : hle_d3d_widescreen ? (int)(h * 16 + 4) / 9 : (int)w;
            int wh = hle_d3d_res_h > 0 ? hle_d3d_res_h : (int)h;
            video_want_vulkan = !strcmp(hle_d3d_renderer, "vulkan") && !hle_d3d_dump_path && strcmp(hle_d3d_present, "readback");
            video_fullscreen = hle_d3d_fullscreen;
            video_open(hle_d3d_video, ww, wh, "WarriorsReborn");
            renderer_open(w, h);
            opened = 1;
        }
        clear_parity_check(&ops, px, w, h);
        hstat_end = (RendererTiming){0};
        double t0 = hle_d3d_stats ? now_ms() : 0;
        /* Frames without draws go through the renderer too when its output is scaled. */
        const uint32_t *shown = px;
        uint32_t sw = w, sh = h;
        int scaled = out_w != (int)w || out_h != (int)h;
        if (presenting && render_frame(&ops, px, w, NULL)) {   /* the swapchain failed: readback from now on */
            presenting = 0;
            printf("[video] present: readback through the %s backend (swapchain lost: %s)\n", hle_d3d_video, renderer_present_info());
            if (video_vulkan_fallback()) finish("no window");
        } else if (presenting) {
            shown = NULL;
        }
        if (shown && renderer_on && (ops.draws || scaled)) {
            if (!out) out = malloc((size_t)out_w * out_h * 4);
            if (out && !render_frame(&ops, px, w, out)) {
                shown = out;
                sw = (uint32_t)out_w;
                sh = (uint32_t)out_h;
            }
        }
        double t1 = hle_d3d_stats ? now_ms() : 0;
        if (shown) video_present(shown, (int)sw, (int)sh, (int)sw * 4);
        if (shown && hle_d3d_dump_path) keep_shown(shown, sw, sh, seq);
        if (shown && hle_d3d_dump_path && hle_d3d_dump_every > 0 && seq % (uint32_t)hle_d3d_dump_every == 0) {
            char name[1024];
            snprintf(name, sizeof name, "%s.%05u.bmp", hle_d3d_dump_path, seq);
            video_write_bmp(name, shown, (int)sw, (int)sh, (int)sw * 4);
        }
        if (hle_d3d_stats) host_stats(t1 - t0, now_ms() - t1, ops.draws, &ft);
        if (hle_spawn_report) hle_spawn_frame(seq);
        pthread_mutex_lock(&d3d_lock);
        st.presented++;
        pthread_mutex_unlock(&d3d_lock);
    }
    /* Window events every 4 ms (the kernel service loop calls this about every millisecond). */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t since = (int64_t)(now.tv_sec - last_pump.tv_sec) * 1000000000 + (now.tv_nsec - last_pump.tv_nsec);
    if (opened && (fresh || since >= 4000000)) {
        last_pump = now;
        if (!video_pump()) finish("window closed");
        if (hle_d3d_stats) __atomic_store_n(&last_pump_us, (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000, __ATOMIC_RELAXED);
    }
    if (done && !pending && !fresh) finish("frame limit reached");   /* the last frame has been presented */
}
