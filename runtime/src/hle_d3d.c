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
#include "colpatch.h"
#include "hle_input.h"
#include "input_map.h"
#include "kbm_glyphs.h"
#include "sharp_font.h"
#include "hle_spawn.h"
#include "hle_fx.h"
#include "hle_xapi.h"
#include "pacing.h"
#include "hle_sim60.h"
#include "rateprobe.h"
#include "geom.h"
#include "gpu_skin.h"
#include "tri_check.h"
#include "hud_glass.h"
#include "bloom.h"
#include "filmic.h"
#include "auto_exposure.h"
#include "dof.h"
#include "gamma_ramp.h"
#include "shadow_range.h"
#include "mem.h"
#include "movie_hd.h"
#include "diaglog.h"
#include "psh.h"
#include "renderer.h"
#include "renderer/vk_caps.h"
#include "meshpack.h"
#include "meshover.h"
#include "texlayer.h"
#include "builtin_tex.h"
#include "texpack.h"
#include "video.h"
#include "texload.h"
#include "normal_maps.h"
#include "tex_race.h"
#include "vsh.h"
#include "xtex.h"
#include "yuv.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fps_overlay.h"
#include "hle_modmenu.h"
#include "fsr.h"
#include "taa.h"
#include "dynres.h"
#include "bench.h"
#include "framegen.h"
#include "anim_interp.h"
#include "cinema_bars.h"
#include "combat_debug.h"
#include "vram_budget.h"
#include "perf_log.h"
#include "load_prof.h"
#include "platform.h"
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <wchar.h>   /* black_frames_warning's message box text */
#endif
#include "render_rec.h"   /* the renderer_* calls below go through its wrappers (--record-renderer) */
#include "render_wire.h"  /* rw_hash64: the weld tables' key (skin_adjacency) */

static void sharp_faces_summary(void);
static void neon_summary(void);
static void neon_index(void);
static void relief_summary(void);
static void relief_index(void);

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
/* --wall-doors: open door-panel triangles carried per frame (a level spawns at most 27 doors, two
 * triangles each; past this many the rest stay closed in the map). */
#define DOOR_OPEN_MAX 128

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
int hle_d3d_dump_from;    /* --dump-from: the periodic dumps start at this presented frame (0: from the first) */
const char *hle_d3d_video = "sdl";
const char *hle_d3d_renderer = "vulkan";
int hle_d3d_stats;
void (*hle_d3d_swap_hook)(uint32_t swap, uint32_t game_ms, int last);
int (*hle_d3d_pace_hook)(uint32_t swap);
void (*hle_d3d_host_frame_hook)(const uint32_t *px, uint32_t w, uint32_t h, uint32_t seq, uint32_t draws, uint32_t movie);
void (*hle_d3d_settings_hook)(void);
HudGlassConfig hle_d3d_glass = HUD_GLASS_DEFAULTS;
BloomConfig hle_d3d_bloom = BLOOM_DEFAULTS;
FilmicConfig hle_d3d_filmic = FILMIC_DEFAULTS;
HdrConfig hle_d3d_hdr = HDR_DEFAULTS;
AutoExposureConfig hle_d3d_auto_exposure = AUTO_EXPOSURE_DEFAULTS;
DofConfig hle_d3d_dof = DOF_DEFAULTS;
RainConfig hle_d3d_rain = RAIN_DEFAULTS;
SkyConfig hle_d3d_sky = SKY_DEFAULTS;
SharpFacesConfig hle_d3d_sharp_faces = SHARP_FACES_DEFAULTS;
NeonGlowConfig hle_d3d_neon_glow = NEON_GLOW_DEFAULTS;
NeonReflConfig hle_d3d_neon_refl = NEON_REFL_DEFAULTS;
SssrConfig hle_d3d_sssr = SSSR_DEFAULTS;
NormalMapsConfig hle_d3d_normal_maps = NORMAL_MAPS_DEFAULTS;
SsaoConfig hle_d3d_ssao = SSAO_DEFAULTS;
ContactConfig hle_d3d_contact = CONTACT_DEFAULTS;
LampLightsConfig hle_d3d_lamp = LAMP_LIGHTS_DEFAULTS;
LightShaftsConfig hle_d3d_shafts = LIGHT_SHAFTS_DEFAULTS;
VolfogConfig hle_d3d_volfog = VOLFOG_DEFAULTS;
WetStreaksConfig hle_d3d_wet_streaks = WET_STREAKS_DEFAULTS;
MaterialsConfig hle_d3d_materials = MATERIALS_DEFAULTS;
DampConfig hle_d3d_damp = DAMP_DEFAULTS;
CharLightConfig hle_d3d_char_light = CHAR_LIGHT_DEFAULTS;
HairConfig hle_d3d_hair = HAIR_DEFAULTS;
HatConfig hle_d3d_hats = HAT_DEFAULTS;
SkinConfig hle_d3d_skin = SKIN_DEFAULTS;
EyesConfig hle_d3d_eyes = EYES_DEFAULTS;
ClothConfig hle_d3d_cloth = CLOTH_DEFAULTS;
WorldLightConfig hle_d3d_world_light = WORLD_LIGHT_DEFAULTS;
FireLightConfig hle_d3d_fire = FIRE_LIGHT_DEFAULTS;
LevelLightsConfig hle_d3d_level = LEVEL_LIGHTS_DEFAULTS;
GpuSkinConfig hle_d3d_gpu_skin = GPU_SKIN_DEFAULTS;
int hle_d3d_direct_geometry = 1;   /* --direct-geometry (default) / --no-direct-geometry: RendererConfig.direct_geometry */
SoftConfig hle_d3d_soft = SOFT_DEFAULTS;
ShadowRangeConfig hle_d3d_shadow_range = SHADOW_RANGE_DEFAULTS;
LampShadowConfig hle_d3d_lamp_shadow = LAMP_SHADOW_DEFAULTS;
WorldShadowConfig hle_d3d_world_shadow = WORLD_SHADOW_DEFAULTS;
_Static_assert(BLOOM_DRAW_DEPTH_TEST == RENDERER_DEPTH_TEST, "bloom_scene_draw reads FrameOp.draw_flags");
_Static_assert(SSAO_DRAW_DEPTH_TEST == RENDERER_DEPTH_TEST, "ssao_opaque_draw reads FrameOp.draw_flags");
int hle_d3d_render_scale, hle_d3d_res_w, hle_d3d_res_h, hle_d3d_msaa, hle_d3d_aniso, hle_d3d_widescreen;
int hle_d3d_supersample, hle_d3d_shadow_scale, hle_d3d_shadow_soft;
int hle_d3d_sharpen;
int hle_d3d_fps_overlay;
int hle_d3d_fps_overlay_corner;
int hle_d3d_fps_overlay_items;
int hle_d3d_pipeline_cache = 1;
int hle_d3d_pipeline_async = 1;
int hle_d3d_vrs;   /* --vrs: RENDERER_VRS_* (RendererConfig.vrs) */
int hle_d3d_dynamic_state = 1;
const char *hle_d3d_pipeline_cache_dir;
const char *hle_d3d_present = "auto";
int hle_d3d_frames_in_flight = 2;
const char *hle_d3d_dump_textures, *hle_d3d_texture_packs[HLE_D3D_MAX_PACKS];
int hle_d3d_texture_pack_count;
const char *hle_d3d_mesh_pack;
int hle_d3d_meshover_log;   /* --mesh-log: print each new draw's content hash (finding a mesh to override) */
int hle_d3d_fullscreen;
int hle_d3d_render_targets = 1;
int hle_d3d_frame_blur = 1;
int hle_d3d_title_shadow_full;
int hle_d3d_button_icons;
int hle_d3d_pad_icons;
int hle_d3d_sharp_fonts = 1;
static KbmRect glyph_rects[KBM_RECTS_MAX], big_glyph_rects[KBM_RECTS_MAX];
static int glyph_nrects, big_glyph_nrects;

int hle_d3d_glyphs_load(const char *disc_dir) {
    int n = kbm_rects_load(disc_dir, glyph_rects, KBM_RECTS_MAX);
    glyph_nrects = n > KBM_RECTS_MAX ? KBM_RECTS_MAX : n > 0 ? n : 0;
    int b = n > 0 ? kbm_big_rects_load(disc_dir, big_glyph_rects, KBM_RECTS_MAX) : -1;
    big_glyph_nrects = b > KBM_RECTS_MAX ? KBM_RECTS_MAX : b > 0 ? b : 0;
    if (n > 0 && b < 0) printf("[glyphs] no big font table in paks\\global.pak: the pulsing D-pad icons stay Xbox\n");
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
static double process_cpu_ms(void) {   /* CPU time of every thread of the process */
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static struct {
    double frame_start, record;   /* guest thread only */
    double pace;                  /* guest thread: the last 60 Hz pacing sleep (ms) */
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
static int hstat_valid;            /* hstat_end is from a renderer frame (the [hitch] wait field) */
/* A presented frame's guest times (taken from the queue entry; g_ms / hle_ms -1 = not measured). */
typedef struct { double t_read, read_age, t_swap, t_take; float g_ms, hle_ms, pace_ms; } FrameTimes;

#define LAMP_FRAME_SPRITES 256   /* --lamp-lights: halo quads kept per frame (the probe saw at most ~70) */
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
    uint8_t prev_mark;                         /* renderer_prev_frame_mark before it: the first back-buffer draw sampling a blur_fed target */
    uint8_t halo;                              /* bloom_halo_draw: drawn after the bloom split (--bloom only) */
    uint8_t rain;                              /* a rain layer of the title (rain_layer; --rain-hd only): replaced */
    uint8_t character;                         /* drawn by a character's vertex program (VShader.character) */
    uint8_t hat;                               /* --hat-fix: a hat on a character's head (hat_record): no title shadow, lit as a character */
    float eye_shut;                            /* --eyes: 1 - eyes_lid_open of its bone palette (0: open, or not a character) */
    WorldLitOp wl;                             /* --world-lighting: its view normals in FrameList.wl and its box (world_light_record) */
    uint32_t skin;                             /* --gpu-skinning: its record (FrameList.skin index + 1; 0 = none) */
    uint8_t meshpack;                          /* --mesh-pack: the replacement mesh was drawn instead (--tri-check logs it) */
    uint16_t meshover;                         /* a mod's mesh override with its own stage-0 texture (meshover index + 1) */
    uint16_t texlayer;                         /* a mod's texture layers over stage 0 (texlayer.h combination id) */
    uint8_t vsh;                               /* its vertex program (vshaders index + 1; 0 = fixed function) */
    uint8_t fit_pos;                           /* skin draw whose first DEPTH_FIT_VERTS vertices hold CPU clip positions (vp_positions) */
    uint8_t neon_pos;                          /* --neon-reflections: likewise, for a sign's draw (nr_key_has) */
    uint8_t movie;                             /* a --movie-hd movie quad: its planes skip the texture pack and dumps */
    uint8_t cam3d;                             /* photo mode: 1 a back-buffer draw through the perspective camera, 2 its sky (no view translation), 0 2D */
    RainLayer rain_layer;                      /* its parameters, and its scroll so far (rain_scroll_layer) */
    float rain_scroll[2];
    float ssao_scale[2];                       /* --ssao: projection x/y scale of an opaque draw (0: unknown) */
    uint8_t particle;                          /* --soft-particles: a world particle draw (soft_particle_draw) */
    uint8_t world_particle;                    /* the world particle pass's signature (soft_particle_draw): RENDERER_PARTICLE (--upscaler fsr3's reactive mask) */
    SkyOp sky;                                 /* --sky-hd: the draw as the sky-stage classifier sees it (sky_classify) */
    uint8_t sky_world;                         /* --sky-hd: a skyline candidate whose tex[2].xyz holds world positions */
    SoftView soft_view;                        /* its camera (soft_classify) */
    int has_ps;                                /* ps: renderer_set_pixel_shader argument (else NULL) */
    RPixelShader ps;
    RGlass glass;                              /* OP_GLASS: renderer_glass argument (before the bar's draw) */
} FrameOp;
/* --gpu-skinning: a draw whose vertex program runs on the device (renderer_draw_skinned) or, without
 * it, on the host thread before the frame is drawn (skin_cpu_fill). Its vertices are reserved in
 * the list but not written at recording; its stream bytes (the draw's vertices, from vertex 0)
 * and its 192 constants (gsk_pack_constants) are copied into FrameList.skin_bytes. */
typedef struct {
    uint32_t vsh;                       /* vshaders index */
    GeomLayout layout;
    uint32_t fog_mode;                  /* GSK_FOG_OFF or the D3DRS_FOGTABLEMODE (DrawFog) */
    float fog_start, fog_end, fog_density;
    float tscale[RENDERER_STAGES][2];
    uint32_t data, data_bytes, consts;  /* byte offsets in skin_bytes */
    uint32_t sbase[4], stride[4];       /* stream k's vertex 0 at data + sbase[k] */
    uint64_t id;                        /* --taa: the draw's identity across frames (program, streams, first vertex, count) */
    int own_motion;                     /* --taa: drawn with its identity (characters, and objects drawn as a mod's mesh override) */
} SkinRec;
typedef struct {
    FrameOp *ops;
    uint32_t nops, cap;
    SkinRec *skin;
    uint32_t nskin, skincap;
    uint8_t *skin_bytes;
    uint32_t skin_nbytes, skin_bcap;
    RVertex *v;
    uint32_t nv, vcap;
    uint32_t *idx;
    uint32_t ni, icap;
    uint32_t *base;       /* back buffer copied at the first draw (CPU clears, movie quads so far) */
    uint32_t *spare_base; /* a recycled list's base buffer (base_bytes), reused by the first draw */
    size_t base_bytes;
    uint32_t first_draw;  /* index of the first OP_DRAW */
    uint32_t draws;
    uint32_t movie_draws; /* of the draws, movie quads (the [hitch] lines' loading frames) */
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
    /* --sky-hd: the camera of the frame's first draw with the dome's signature (sky_is_dome; op
     * sky_cam_op),
     * the shadow direction c_vSunDir (c90) of its last vertex-program draw, and the title's rain
     * layers seen in it (rain_layer). */
    float sky_view[16], sky_proj[16], sun[4];
    uint8_t has_sky_cam, has_sun;
    uint32_t sky_cam_op;
    uint32_t sky_rain;
    /* --lamp-lights: the halo quads of the frame as light sprites (world space; lamp_sprite_quad)
     * and the camera (view matrix) of the last halo draw. */
    LampSprite lamp_spr[LAMP_FRAME_SPRITES];
    uint32_t lamp_n;
    uint8_t has_lamp_view;
    float lamp_view[16];
    /* --fire-light: the flame quads of the frame as fire sprites (world space; fire_sprite_quad),
     * the camera of the last flame draw and the guest clock then (the flicker's time). */
    LampSprite fire_spr[FIRE_FRAME_SPRITES];
    uint32_t fire_n;
    uint8_t has_fire_view, has_fire_ms;
    float fire_view[16];
    uint32_t fire_ms;
    /* --level-lights: the level's lights read at the swap (level_lights_read; has_level: the read
     * succeeded) and the 3D camera (the view matrix of the last depth-tested perspective
     * fixed-function draw in a viewport of at least half the back buffer). */
    LevelLightSet level;
    uint8_t has_level, has_level_view;
    /* --cutscene-dof: the title's camera (its view matrix, DOF_CAM_PTR) read at the swap. */
    uint8_t has_game_cam;
    float game_cam[16];
    /* --cutscene-dof-speaker: the faces of the frame's character draws (dof_face_record). */
    DofFace faces[DOF_FACES_MAX];
    uint32_t nfaces;
    float level_view[16];
    WorldLitStore wl;     /* --world-lighting: the view normals of the frame's world-lit draws */
    /* --lamp-shadows: the depth stage's projection of the frame's first world-program receiver
     * (c_mTexPrj with the stage's coordinate scale; rows s, t, r, q over the world position). */
    float lsh_prj[4][4];
    uint8_t has_lsh_prj;
    /* --world-shadows: the world-to-clip matrix (c69-c72) and light (c90 c_vSunDir) of the frame's
     * first world-program receiver. */
    float ws_wvp[4][4], ws_sun[3];
    uint8_t has_ws;
    /* --taa: the 3D camera's world-to-clip matrix (rows x y z w, clip = m p) and viewport: a world
     * program's c69-c72 at the frame's first world-program back-buffer draw (taa_cam 1), else the
     * view x projection of the last depth-tested perspective fixed-function draw in a viewport of at
     * least half the back buffer (taa_cam 2); 0 = none. */
    float taa_wvp[4][4], taa_vp[4];
    uint8_t taa_cam;
    /* --gamma-ramp: the title's latest gamma ramp when the frame was swapped (set_gamma_ramp): 1 =
     * gamma_lut (gamma_ramp_lut), 0 = none or the identity (the frame as rendered). */
    uint8_t gamma;
    uint32_t gamma_lut[256];
    /* --wet-light-streaks: the fog colour (D3DRS_FOGCOLOR, ARGB) of the frame's last fogged
     * depth-tested 3D draw (the miss fallback's colour). */
    uint8_t has_fog;
    uint32_t fog_color;
    /* --volfog: the fog (DrawFog: on, D3DRS_FOGTABLEMODE, start, end, density) of the frame's last
     * depth-tested 3D draw, the one the title fogs its world with (volfog_far, volfog_fog_keep). */
    uint8_t has_vf_fog, vf_fog_on;
    uint32_t vf_fog_mode;
    float vf_fog_start, vf_fog_end, vf_fog_density;
    /* --wall-doors: the collision triangles the title has opened (door
     * panels; materials.h mat_tri_open), read at the swap (door_frame). has_doors: the title's
     * collision matched the wall map of level load door_gen. */
    uint8_t has_doors;
    uint16_t door_n;
    uint32_t door_gen, door_swap;   /* door_swap: the frame's swap number (the log) */
    uint16_t door_open[DOOR_OPEN_MAX];
    /* --combat-debug (combat_debug.h): the world-to-clip matrix (c69-c72) and viewport of the frame's
     * first world-program draw, and its hit/hurt volumes (cd_ring slot cd_slot - 1; 0 = none). */
    float cd_wvp[4][4];
    RViewport cd_vp;
    uint8_t has_cd_wvp;
    uint16_t cd_slot, cd_n;
    /* Photo mode (hle_photo.h): the world-to-clip matrix (c69-c72) of the frame's first world-program
     * back-buffer draw, the camera the free camera starts from. */
    float photo_wvp[4][4];
    uint8_t has_photo_wvp;
} FrameList;
static void frame_list_free(FrameList *l) {
    free(l->ops);
    free(l->skin);
    free(l->skin_bytes);
    free(l->v);
    free(l->idx);
    free(l->base);
    free(l->spare_base);
    world_lit_store_free(&l->wl);
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
    s->skin = l->skin;
    s->skincap = l->skincap;
    s->skin_bytes = l->skin_bytes;
    s->skin_bcap = l->skin_bcap;
    s->v = l->v;
    s->vcap = l->vcap;
    s->idx = l->idx;
    s->icap = l->icap;
    s->spare_base = l->base ? l->base : l->spare_base;
    s->base_bytes = l->base_bytes;
    s->wl = l->wl;
    s->wl.nn = 0;
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
void (*hle_d3d_frame_queued)(void);
#define MAX_LIGHTS 32   /* light indices kept (the title uses a few; D3D allows any index) */
static struct {
    uint32_t surf[3], depth, rt, zs;
    uint32_t w, h;
    uint32_t clear_color, frames, fence, scratch;
    uint32_t tex[4], ps_def, vshader;   /* bound textures, pixel shader definition, vertex shader/FVF */
    uint32_t psc[PSH_CONSTANTS];        /* SetPixelShaderConstant values (ARGB8888) */
    uint32_t fog_color;                 /* SetRenderState_FogColor (ARGB) */
    uint32_t movie_quads;               /* Bink frames drawn (CPU YUV quad or, with --movie-hd, the renderer) */
    uint32_t movie_gpu;                 /* of those, recorded for the renderer (--movie-hd) */
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
    uint32_t depth_fit_frame, depth_fit_ok;   /* --gpu-vertex-programs: frame_depth_fit successes of this frame's sliced draws (vp_positions) */
    /* --rain-hd: the title's rain layers seen in the frame being recorded, and their scroll. */
    int rain_layers;
    RainScroll rain_scroll;
    uint8_t target_c, target_z;   /* current render target when not the back buffer (rt_surf index + 1) */
    uint32_t blur_fed;            /* this frame's offscreen targets drawn from the back buffer (RT_BACK_PREV), directly or not: bit per rt_surf index */
    int blur_marked;              /* this frame has a draw with prev_mark */
    uint32_t alpha_test, alpha_func, alpha_ref;   /* NV2A alpha test state from SetRenderState_Simple */
    RState rs;            /* blend, color mask, depth write/func (SetRenderState_Simple), cull (CullMode) */
    GeomLight light[MAX_LIGHTS];          /* SetLight by index */
    uint8_t light_on[MAX_LIGHTS];         /* LightEnable */
    GeomMaterial material;                /* SetMaterial */
    uint32_t drawn, skipped_target, skipped_fmt, skipped_up;
    uint32_t tex_draws[256], tex_unsupported[256];   /* draws per stage-0 texture format (decoded / not) */
    /* Swapped frames waiting for the host main thread (FIFO): pixels, operations, frame_seq. */
    struct { uint32_t *px; uint32_t w, h, seq; FrameList ops; double t_read, read_age, t_swap; float g_ms, hle_ms, pace_ms; } queue[FRAME_QUEUE];
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
 * recorded in queued frames (published under d3d_lock), never removed; only guest_size changes. */
#define MAX_RT_SURFACES 32
typedef struct {
    uint32_t pa, kind, w, h;
    /* Set once (atomically, by the guest thread while recording the frame that first feeds it)
     * when the surface is a color target of the blur chain (fed from the back buffer,
     * st.blur_fed): its host surface keeps the guest size (RENDERER_SURFACE_GUEST_SIZE). */
    uint8_t guest_size;
} RtSurface;
static RtSurface rt_surf[MAX_RT_SURFACES];
/* FrameOp.tex_surf of a stage that samples the previous frame (renderer_prev_frame), not a registry
 * entry: the front buffer bound as a texture. */
#define RT_PREV_FRAME 0xFF
/* The same for the back buffer sampled while drawing into an offscreen target (the title's blur
 * mode, 0xbc1c0): offscreen passes run before the frame's main pass, so the previous frame. */
#define RT_BACK_PREV 0xFE
#define RT_IS_PREV(t) ((t) == RT_PREV_FRAME || (t) == RT_BACK_PREV)
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
    rt_surf[n_rt_surf] = (RtSurface){pa, kind, w, h, 0};
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
/* (pResource, pBase): Data += base; masked to a physical address except for push buffers. A
 * texture registered while a texture pack prefetches is offered to its loaders. */
static int tp_prefetch_on;   /* set by the render thread (tp_start_loaders), read here */
static void tp_prefetch_register(uint32_t res);
static uint32_t register_res(D3dFrame *f, const uint32_t *a) {
    (void)f;
    if (!a[0]) return 0;
    uint32_t d = U32(a[0], 4) + a[1];
    if ((U32(a[0], 0) & COMMON_TYPE_MASK) != COMMON_PUSHBUFFER) d &= 0x0FFFFFFFu;
    U32(a[0], 4) = d;
    if ((U32(a[0], 0) & COMMON_TYPE_MASK) == COMMON_TEXTURE && __atomic_load_n(&tp_prefetch_on, __ATOMIC_ACQUIRE)) tp_prefetch_register(a[0]);
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
    int character;   /* a skinned character's program (rain HD's character mask, RENDERER_CHARACTER) */
    int gpu;         /* --gpu-skinning: a character program the device runs (gpu_skin.h matcher) */
    int world;       /* --world-lighting: a world or object program (world_light_program) */
    int gpu_world;   /* --gpu-vertex-programs: a world or object program the device runs (gsk_world_program_eligible) */
    VshProgram prog;
    uint32_t words[VSH_MAX_INSTRUCTIONS][4];   /* gsk_pack_program of prog (gpu, gpu_world) */
    VshProgram pos;       /* gpu_world: prog's oPos slice (vsh_slice), for the depth fit's vertices */
    uint32_t pos_reads;   /* the input registers pos reads */
    uint64_t hash;        /* gsk_program_hash of the microcode (0 without one; --tri-check logs it) */
    int pal_base;         /* a character's bone palette: its first constant row (vsh_palette_base; C_BONES for the
                             skinned declaration without a program; -1: none) */
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
        /* Characters: the skinned declaration (bone indices and weights in stream 1) or a program
         * that reads a bone palette (vsCharacter: arl and relative constant reads). */
        v->character = v->skinned || (v->has_prog && vsh_bone_palette(&v->prog));
        const uint64_t hash = v->has_prog ? gsk_program_hash(G(a[1]), 1 + 4 * n) : 0;
        v->hash = hash;
        const int runs = v->has_prog && gsk_program_supported(&v->prog) && gsk_layout_supported(&v->layout);
        v->gpu = v->character && runs && gsk_program_allowed(hash);
        /* World and object programs: by what the shader implements (gsk_world_program_eligible); one
         * line per program that stays on the CPU, and why. */
        char why[160];
        v->gpu_world = v->has_prog && !v->character && gsk_world_program_eligible(&v->prog, &v->layout, why, sizeof why);
        if (v->has_prog && !v->character && !v->gpu_world && hle_d3d_gpu_skin.world && gsk_log_once(hash))
            printf("[gpu-vp] vertex program %016llx (%u instructions) stays on the CPU: %s\n", (unsigned long long)hash, v->prog.n, why);
        if (v->gpu || v->gpu_world) gsk_pack_program(&v->prog, v->words);
        v->pal_base = !v->character ? -1 : v->has_prog ? vsh_palette_base(&v->prog) : -36 + 96 /* C_BONES */;
        if (v->gpu_world || (v->character && v->has_prog)) {   /* character: --cutscene-dof-speaker's vertex depths (dof_face_record) */
            vsh_slice(&v->prog, 1u << VSH_OPOS, &v->pos);
            v->pos_reads = vsh_inputs_read(&v->pos);
        }
        v->world = v->has_prog && !v->character ? world_light_program(&v->prog) : WORLD_PROG_NONE;
        if (hle_d3d_world_light.on && v->world)
            printf("[world-light] vertex program %d (%u instructions): %s\n", nvshaders, v->prog.n, v->world == WORLD_PROG_WORLD ? "world" : "object");
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
int hle_d3d_game_clock_ms(uint32_t *ms) { return guest_clock_ms(ms); }
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
    /* The title restarts this clock on a level load: a step back is a reset, not a speed. */
    const int32_t dms = (int32_t)(ms - pst.guest_ms);
    if (have && pst.have_ms && t > pst.host_ms && dms < 0)
        snprintf(speed, sizeof speed, " | guest clock reset (%d ms) in %.0f ms host", dms, t - pst.host_ms);
    else if (have && pst.have_ms && t > pst.host_ms)
        snprintf(speed, sizeof speed, " | guest clock +%d ms in %.0f ms host (x%.4f)", dms, t - pst.host_ms, dms / (t - pst.host_ms));
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
static void pipeline_lines(void) {   /* the renderer's [pipeline] lines (renderer_pipeline_report) */
    char line[512];
    while (renderer_pipeline_report(line, sizeof line)) printf("%s\n", line);
}
/* D3DDevice_SetGammaRamp(Flags, pRamp) (src/gamma_ramp.h): the title sets one about every frame
 * (levels, menus, movies, the Brightness screen). With --gamma-ramp (default) the latest one goes
 * with each swapped frame (gamma_frame) and the host thread hands it to the renderer (render_frame),
 * which applies it to the finished frame; none while it is the identity. --no-gamma-ramp ignores
 * them as before. The guest device's own copy is not kept: nothing in the title calls
 * D3DDevice_GetGammaRamp (0x3611a0). */
static uint8_t gamma_on;          /* guest thread: the latest ramp is not the identity */
static uint32_t gamma_lut[256];   /* its table (gamma_ramp_lut) */
static uint32_t set_gamma_ramp(D3dFrame *f, const uint32_t *a) {
    (void)f;
    GammaRamp g;
    if (!gamma_ramp_enabled || !readable(a[1], GAMMA_RAMP_BYTES) || gamma_ramp_parse(G(a[1]), GAMMA_RAMP_BYTES, &g)) return S_OK;
    gamma_on = !gamma_ramp_is_identity(&g);
    if (gamma_on) gamma_ramp_lut(&g, gamma_lut);
    return S_OK;
}
/* --level-lights: guest reads for level_lights_read, each page checked once per frame. */
typedef struct {
    uint32_t page[64];
    uint32_t n;
} LevelPages;
static int level_read(uint32_t va, void *dst, uint32_t len, void *ctx) {
    LevelPages *c = ctx;
    if (va < 0x10000u || !len || va + len < va) return -1;
    for (uint32_t pg = va >> 12, last = (va + len - 1) >> 12; pg <= last; pg++) {
        uint32_t i = 0;
        while (i < c->n && c->page[i] != pg) i++;
        if (i < c->n) continue;
        if (!mem_is_mapped(pg << 12)) return -1;
        if (c->n < sizeof c->page / sizeof c->page[0]) c->page[c->n++] = pg;
    }
    memcpy(dst, G(va), len);
    return 0;
}
/* At the swap (the frame's game logic done): the level's lights into the frame's list. */
static void level_frame(FrameList *l) {
    if (!hle_d3d_level.on || !l->draws) return;
    const double t0 = now_ms();
    LevelPages pages = {{0}, 0};
    l->has_level = !level_lights_read(level_read, &pages, LEVEL_LIGHTS_MGR_VA, &l->level);
    static double ms;
    static uint32_t calls, logged;
    ms += now_ms() - t0;
    calls++;
    const LevelLightSet *s = &l->level;
    if (l->has_level && s->n && !logged++)
        printf("[level-lights] %u point lights read from the level (%u listed, %u off or glow-only, %u invalid), %u directional\n", s->n,
               s->listed, s->off, s->rejected, s->ndir);
    if (hle_d3d_stats && !(st.frames % 600))
        printf("[level-lights] frame %u: %s, %u point lights (%u listed, %u off, %u invalid, %u over %u), %u directional; %.1f us/frame read\n",
               st.frames, l->has_level ? "read" : "no light manager", s->n, s->listed, s->off, s->rejected, s->dropped, LEVEL_POINTS_MAX, s->ndir,
               1000 * ms / calls);
}
/* --cutscene-dof and --auto-exposure (their camera cuts): the title's game camera object (research
 * note dof-motion-blur.md 1.2; the sim60 and cutscene-sweep probes read it): its view matrix (16
 * floats, row vectors) at +0x1c. */
#define DOF_CAM_PTR 0x004cf13cu
#define DOF_CAM_VIEW 0x1cu
static void dof_cam_frame(FrameList *l) {
    l->has_game_cam = 0;
    if ((!hle_d3d_dof.on && !hle_d3d_auto_exposure.on) || !l->draws || !readable(DOF_CAM_PTR, 4)) return;
    const uint32_t cam = *(const uint32_t *)G(DOF_CAM_PTR);
    if (cam < 0x10000u || cam + DOF_CAM_VIEW < cam || !readable(cam + DOF_CAM_VIEW, sizeof l->game_cam)) return;
    memcpy(l->game_cam, G(cam + DOF_CAM_VIEW), sizeof l->game_cam);
    l->has_game_cam = 1;
}
/* --wall-doors: the title's collision world (pointer at 0x005af590; the
 * door behaviours' triangle-flag helpers 0x2b4040 / 0x2b4070 reach it as *(*(*(0x5af590) + 0x10) +
 * 4)), its triangle count (u16 at +0x78) and records (+0x7c, the .lev's 10-byte records). Read at the
 * swap on the guest thread; used only when the count and the corners' checksum match the level's wall
 * map (door_ntri, door_sum, set at the level load by mat_level), checked again when the pointers
 * change and every DOOR_RECHECK frames while they do not match. */
#define DOOR_WORLD_PTR 0x005af590u
#define DOOR_RECHECK 60
static uint32_t door_ntri, door_sum, door_gen;   /* the wall map's mesh (0: none), its level load */
static uint32_t door_w, door_tri, door_seen;
static int door_ok;
static int doors_wanted(void);
static void door_frame(FrameList *l) {
    l->has_doors = 0;
    l->door_n = 0;
    if (!door_ntri || (!doors_wanted() && !hle_d3d_audio_doors) || !readable(DOOR_WORLD_PTR, 4)) return;
    const uint32_t g = U32(DOOR_WORLD_PTR, 0);
    if (g < 0x10000u || !readable(g + 0x10, 4)) return;
    const uint32_t a = U32(g, 0x10);
    if (a < 0x10000u || !readable(a + 4, 4)) return;
    const uint32_t w = U32(a, 4);
    if (w < 0x10000u || !readable(w + 0x78, 8)) return;
    const uint32_t base = colpatch_base_ntri();   /* a mod's collision patch appends records: compare the level's own */
    const uint32_t n = base ? base : *(const uint16_t *)G(w + 0x78), tri = U32(w, 0x7c);
    const int moved = w != door_w || tri != door_tri;
    if (moved || (!door_ok && ++door_seen >= DOOR_RECHECK)) {
        door_w = w, door_tri = tri, door_seen = 0;
        const int was = door_ok;
        door_ok = n == door_ntri && tri >= 0x10000u && readable(tri, n * 10) && mat_tri_sum(G(tri), n) == door_sum;
        if (door_ok != was) printf("[wall-doors] the title's collision (%u triangles at 0x%08x) %s the level's wall map\n", n, tri, door_ok ? "matches" : "no longer matches");
    }
    if (!door_ok || n != door_ntri) return;
    const uint32_t k = mat_tri_open(G(tri), n, l->door_open, DOOR_OPEN_MAX);
    l->door_n = (uint16_t)(k < DOOR_OPEN_MAX ? k : DOOR_OPEN_MAX);
    l->door_gen = door_gen;
    l->door_swap = st.frames + 1;
    l->has_doors = 1;
    if (hle_d3d_audio_doors) hle_d3d_audio_doors(door_gen, l->door_open, l->door_n);
}
/* At the swap: the latest ramp into the frame's list. */
/* --combat-debug: the characters' volumes at the swap (guest thread) into a ring slot the frame's
 * list names; allocated at the first use, so nothing exists without the option. */
#define CD_RING 8
static CdVolume (*cd_ring)[CD_MAX_VOLUMES];
static uint32_t cd_ring_next;
static void combat_frame(FrameList *l) {
    l->cd_slot = l->cd_n = 0;
    if (!combat_debug_on || !l->has_cd_wvp) return;
    if (!cd_ring && !(cd_ring = calloc(CD_RING, sizeof *cd_ring))) return;
    const uint32_t k = cd_ring_next++ % CD_RING;
    l->cd_n = (uint16_t)combat_debug_snapshot(cd_ring[k], CD_MAX_VOLUMES);
    l->cd_slot = (uint16_t)(k + 1);
}
static void gamma_frame(FrameList *l) {
    l->gamma = gamma_on;
    if (gamma_on) memcpy(l->gamma_lut, gamma_lut, sizeof gamma_lut);
}

/* Photo mode (hle_d3d.h hle_d3d_photo_*): the guest's render thread waits at the end of Swap while
 * frozen; the title's clock is held meanwhile. Returns 1 when it waited. */
static pthread_mutex_t photo_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t photo_cv = PTHREAD_COND_INITIALIZER;
static int photo_want, photo_blocked;
void hle_d3d_photo_freeze(int on) {
    pthread_mutex_lock(&photo_mx);
    photo_want = on != 0;
    if (!on) pthread_cond_broadcast(&photo_cv);
    pthread_mutex_unlock(&photo_mx);
}
static int photo_wait(void) {
    if (!__atomic_load_n(&photo_want, __ATOMIC_ACQUIRE)) return 0;
    pthread_mutex_lock(&photo_mx);
    const int waited = photo_want;
    if (waited) {
        hle_xapi_clock_hold(1);
        __atomic_store_n(&photo_blocked, 1, __ATOMIC_RELEASE);
        printf("[photo] game frozen at swap %u\n", st.frames);
        fflush(stdout);
        while (photo_want) pthread_cond_wait(&photo_cv, &photo_mx);
        __atomic_store_n(&photo_blocked, 0, __ATOMIC_RELEASE);
        hle_xapi_clock_hold(0);
        printf("[photo] game resumed\n");
        fflush(stdout);
    }
    pthread_mutex_unlock(&photo_mx);
    return waited;
}

/* A viewport of at least half the back buffer each way (the 3D view, not an inset). */
static int photo_big_viewport(void) {
    uint32_t bw = 0, bh = 0, bp;
    if (st.surf[0]) res_dims(st.surf[0], &bw, &bh, &bp);
    return bw && st.vp.w * 2 >= bw && st.vp.h * 2 >= bh;
}
static uint32_t swap(D3dFrame *f, const uint32_t *a) {
    (void)f;
    uint32_t flags = a[0] ? a[0] : 5;   /* 0 = D3DSWAP_DEFAULT */
    if (flags & 4) U32(D3D_DIRTY_FLAGS, 0) |= 0xff1000u;
    uint32_t bb = st.surf[0], w = 0, h = 0, pitch = 0;
    if (bb) res_dims(bb, &w, &h, &pitch);
    vp_wait_all();   /* the frame's vertex program outputs are complete before it is handed over */
    level_frame(&st.rec);
    dof_cam_frame(&st.rec);
    colpatch_frame(readable);   /* mod collision patches (src/colpatch.h), before the doors read the world */
    door_frame(&st.rec);
    combat_frame(&st.rec);
    double t_swap = now_ms(), waited = 0;   /* always: the [hitch] game time */
    pthread_mutex_lock(&d3d_lock);
    uint32_t n = ++st.frames;
    st.blur_fed = 0;
    st.blur_marked = 0;
    if (bb) U32(D3D_DEVICE, DEV_SWAP_COUNT) = n;
    if (dlog_debug_crash_frame && n == dlog_debug_crash_frame) {   /* XBRUN_DEBUG_CRASH (diaglog.h) */
        pthread_mutex_unlock(&d3d_lock);
        dlog_debug_crash();
    }
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
            gamma_frame(&st.rec);
            st.queue[i].ops = st.rec;
            st.queue[i].t_read = gstat.read;
            st.queue[i].read_age = gstat.read_age;
            st.queue[i].t_swap = t_swap;
            st.queue[i].g_ms = gstat.frame_start ? (float)(t_swap - gstat.frame_start) : -1;
            st.queue[i].hle_ms = hle_d3d_stats && gstat.frame_start ? (float)gstat.record : -1;
            st.queue[i].pace_ms = gstat.frame_start ? (float)gstat.pace : -1;
            st.q_count++;
            if (hle_d3d_frame_queued) hle_d3d_frame_queued();
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
    pipeline_lines();
    if (n <= 3 || n % 60 == 0) printf("[d3d] frame %u presented (%ux%u, clear color %08x)\n", n, w, h, color);
    if (n == 2) list_frame_calls();
    if (photo_wait()) st.next_vblank.tv_sec = 0;   /* photo mode held the game here: a new pacing grid */
    /* 60 Hz pacing (D3DPRESENT_INTERVAL_ONE). */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (st.next_vblank.tv_sec == 0 || now.tv_sec > st.next_vblank.tv_sec + 1) st.next_vblank = now;
    st.next_vblank.tv_nsec += 16666667;
    if (st.next_vblank.tv_nsec >= 1000000000) { st.next_vblank.tv_sec++; st.next_vblank.tv_nsec -= 1000000000; }
    /* With the frame lock, a frame a whole period late or more skips the whole periods it is late
     * instead of presenting the following frames back to back to catch up (each would advance
     * the frame-locked clock by a period). The schedule keeps its grid, the one the frame-locked
     * clock skips by (pacing_clock_present, same tolerance): a frame less than a period late (a
     * pacing sleep that woke late, a slow frame) presents now and the next one waits less, so
     * the rate stays at 60 and no period of game time is skipped and repeated. */
    const int64_t late = (int64_t)(now.tv_sec - st.next_vblank.tv_sec) * 1000000000 + (now.tv_nsec - st.next_vblank.tv_nsec);
    if (hle_xapi_frame_lock && late >= PACING_PERIOD_60HZ_NS - PACING_LATE_TOLERANCE(PACING_PERIOD_60HZ_NS)) {
        const int64_t skip = (late + PACING_LATE_TOLERANCE(PACING_PERIOD_60HZ_NS)) / PACING_PERIOD_60HZ_NS * PACING_PERIOD_60HZ_NS;
        st.next_vblank.tv_sec += skip / 1000000000;
        st.next_vblank.tv_nsec += skip % 1000000000;
        if (st.next_vblank.tv_nsec >= 1000000000) { st.next_vblank.tv_sec++; st.next_vblank.tv_nsec -= 1000000000; }
    }
    const double t_pace = now_ms();
    if (hle_d3d_pace_hook && hle_d3d_pace_hook(n)) st.next_vblank.tv_sec = 0;   /* unpaced (src/gamectl.h): the next frame starts a new grid */
    else plat_sleep_until_precise(&st.next_vblank);
    gstat.pace = now_ms() - t_pace;   /* the [hitch] pace of the next frame */
    hle_xapi_frame_presented();   /* the frame-locked QueryPerformanceCounter advances one period */
    if (rateprobe_active) rate_probe_frame(n, hle_d3d_frame_limit && n >= (uint32_t)hle_d3d_frame_limit);
    if (hle_d3d_swap_hook) {
        uint32_t ms;
        if (guest_clock_ms(&ms)) hle_d3d_swap_hook(n, ms, hle_d3d_frame_limit && n >= (uint32_t)hle_d3d_frame_limit);
    }
    gstat.frame_start = now_ms();
    if (hle_d3d_stats) pacing_frame(n, hle_d3d_frame_limit && n >= (uint32_t)hle_d3d_frame_limit);
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
        for (int s = 0; s < RENDERER_STAGES; s++) {
            const int prev = RT_IS_PREV(op->tex_surf[s]);
            d->stage_surface[s] = op->tex_surf[s] && !prev ? rt_surf[op->tex_surf[s] - 1].pa : 0;
            d->stage_prev_frame |= (uint32_t)prev << s;
        }
        d->prev_mark = op->prev_mark;
        d->target_guest_size = op->target_c && rt_surf[op->target_c - 1].guest_size;
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
    int character;   /* a character's program (--stats splits the pool's time by it) */
} VpJob;
static uint64_t vp_now_ns(void);
/* --stats: vertex-program CPU time (every thread) and vertices, character programs and others. */
static struct { _Alignas(8) uint64_t ns[2]; _Alignas(8) uint64_t verts[2]; } vp_prog_stat;

/* --stats: vertices of world and object program draws run on the CPU, by why (record_draw_body). */
enum { VP_CPU_UNSUPPORTED, VP_CPU_OFF, VP_CPU_DEBUG_COLOR, VP_CPU_HAT, VP_CPU_SHADOW_FIT, VP_CPU_WHY };
static _Alignas(8) uint64_t vp_cpu_why[VP_CPU_WHY];

static void vp_batch_run(const VpJob *jb, uint32_t i0);
static void vp_batch(const VpJob *jb, uint32_t i0) {
    if (!hle_d3d_stats) { vp_batch_run(jb, i0); return; }
    const uint64_t t0 = vp_now_ns();
    vp_batch_run(jb, i0);
    const int c = jb->character != 0;
    __atomic_fetch_add(&vp_prog_stat.ns[c], vp_now_ns() - t0, __ATOMIC_RELAXED);
    __atomic_fetch_add(&vp_prog_stat.verts[c], jb->nv - i0 < VSH_BATCH ? jb->nv - i0 : VSH_BATCH, __ATOMIC_RELAXED);
}
static void vp_batch_run(const VpJob *jb, uint32_t i0) {
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

int hle_d3d_vp_threads = -1;   /* worker threads; -1 = auto (hle_d3d_vp_default_workers), 0 = off */
/* Idle polls before a worker blocks: about 5-30 us, enough to catch the next batch of a draw
 * being queued, not enough to burn a core between draws (4000 polls kept every worker spinning
 * through busy frames: about 65% of the pool's CPU time). */
#define VP_SPIN 256
#define VP_MAX_JOBS 8192
#define VP_MAX_ITEMS 65536
#define VP_ARENA (24u << 20)         /* per-frame copies of each draw's inputs and constants */
/* With workers, a draw of two or more batches does not wait for its vertex programs: its inputs
 * (the stream bytes it reads, the constants, layout, fog and texture scales) are copied into
 * vp_arena and its batches are queued; the outputs land in the recording list's vertex array,
 * which is only read at Swap (vp_wait_all) and never reallocated while batches are pending. A
 * draw of one batch runs on the recording thread (the copy and the hand-off cost more than the
 * batch). vp_claim and vp_pub pack a generation (high half, bumped at every vp_wait_all) with an
 * item index, so a worker can never claim an item of a newer generation it has not seen published.
 * Workers poll briefly (VP_SPIN), then block on vp_go; vp_wake wakes one only while more items
 * are pending than workers are awake, and each worker that claims an item calls it again, so a
 * large draw wakes the pool in a chain and the recording thread makes at most one wake-up call
 * per draw. */
typedef struct { uint32_t job, batch; } VpItem;
static VpJob vp_jobs[VP_MAX_JOBS];
static VpItem vp_items[VP_MAX_ITEMS];
static uint8_t *vp_arena;
static uint32_t vp_njobs, vp_arena_used, vp_gen = 1, vp_finished;
static uint64_t vp_claim = 1ull << 32, vp_pub = 1ull << 32;
static pthread_mutex_t vp_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vp_go = PTHREAD_COND_INITIALIZER;
/* vp_sleepers: workers blocked on vp_go that no wake-up was sent to (changed under vp_mu, read
 * without it); vp_wakeups: wake-ups sent and not yet taken (under vp_mu). */
static int vp_sleepers, vp_wakeups, vp_workers = -1;
/* --stats: the pool's share of the frame's CPU (__atomic; host_stats reads and clears them). busy:
 * batches run by workers; spin: workers polling for work (not sleeping); draws queued to the pool
 * and draws whose vertex programs ran on the recording thread. */
static struct { _Alignas(8) uint64_t busy_ns; _Alignas(8) uint64_t spin_ns; uint32_t queued, inline_draws; } vp_stat;
static uint64_t vp_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static void vp_wake(void);
/* Runs one published item; 0 when none is available. *t_claim (workers, --stats): when the item
 * was claimed; its run time is counted as pool busy time. */
static int vp_take(uint64_t *t_claim) {
    uint64_t c = __atomic_load_n(&vp_claim, __ATOMIC_ACQUIRE);
    for (;;) {
        uint64_t p = __atomic_load_n(&vp_pub, __ATOMIC_ACQUIRE);
        if ((uint32_t)(c >> 32) != (uint32_t)(p >> 32) || (uint32_t)c >= (uint32_t)p) return 0;
        if (__atomic_compare_exchange_n(&vp_claim, &c, c + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
    }
    const VpItem it = vp_items[(uint32_t)c];
    vp_wake();   /* more pending than awake workers: pass a wake-up on before running this one */
    if (t_claim) *t_claim = vp_now_ns();
    vp_batch(&vp_jobs[it.job], it.batch * VSH_BATCH);
    if (t_claim) __atomic_fetch_add(&vp_stat.busy_ns, vp_now_ns() - *t_claim, __ATOMIC_RELAXED);
    __atomic_fetch_add(&vp_finished, 1, __ATOMIC_RELEASE);
    return 1;
}

static uint32_t vp_pending(void) {   /* published items nobody has claimed yet */
    uint64_t c = __atomic_load_n(&vp_claim, __ATOMIC_SEQ_CST), p = __atomic_load_n(&vp_pub, __ATOMIC_SEQ_CST);
    return (uint32_t)(c >> 32) != (uint32_t)(p >> 32) || (uint32_t)c >= (uint32_t)p ? 0 : (uint32_t)p - (uint32_t)c;
}

/* Wakes one blocked worker when more items are pending than workers are awake (polling or running
 * a batch). Called after a publish (seq_cst: pairs with the sleeper's count-then-check in
 * vp_worker, so a publish is never missed by a worker about to block) and by workers after each
 * claim. */
static void vp_wake(void) {
    int sleepers = __atomic_load_n(&vp_sleepers, __ATOMIC_SEQ_CST);
    if (!sleepers || vp_pending() <= (uint32_t)(vp_workers - sleepers)) return;
    pthread_mutex_lock(&vp_mu);
    if (vp_sleepers > 0) {
        __atomic_sub_fetch(&vp_sleepers, 1, __ATOMIC_SEQ_CST);
        vp_wakeups++;
        pthread_cond_signal(&vp_go);
    }
    pthread_mutex_unlock(&vp_mu);
}

static void *vp_worker(void *arg) {
    (void)arg;
    const int stats = hle_d3d_stats;
    for (;;) {
        int idle = 0;
        uint64_t spin_from = 0, claimed = 0;
        while (!vp_take(stats ? &claimed : NULL)) {
            if (stats && !idle) spin_from = vp_now_ns();
            if (++idle < VP_SPIN) {
                __builtin_ia32_pause();
                continue;
            }
            if (stats) __atomic_fetch_add(&vp_stat.spin_ns, vp_now_ns() - spin_from, __ATOMIC_RELAXED);
            pthread_mutex_lock(&vp_mu);
            __atomic_add_fetch(&vp_sleepers, 1, __ATOMIC_SEQ_CST);
            if (vp_pending()) {   /* published while we were giving up: no wake-up will come for it */
                __atomic_sub_fetch(&vp_sleepers, 1, __ATOMIC_SEQ_CST);
            } else {
                while (!vp_wakeups) pthread_cond_wait(&vp_go, &vp_mu);   /* vp_wake took us off vp_sleepers */
                vp_wakeups--;
            }
            pthread_mutex_unlock(&vp_mu);
            idle = 0;
        }
        if (stats && idle) __atomic_fetch_add(&vp_stat.spin_ns, claimed - spin_from, __ATOMIC_RELAXED);
    }
    return NULL;
}

/* Runs every queued batch to completion (the recording thread helps) and starts a new generation. */
static void vp_wait_all(void) {
    if (vp_workers <= 0) return;
    uint32_t n = (uint32_t)__atomic_load_n(&vp_pub, __ATOMIC_ACQUIRE);
    if (!n) return;
    while (vp_take(NULL)) {}
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

/* Physical cores - 2 (the recording thread and the host thread keep a core each; SMT siblings
 * add little to the interpreter's vector code), at least 1, at most 7. */
int hle_d3d_vp_default_workers(int physical_cores) {
    int n = physical_cores - 2;
    return n < 1 ? 1 : n > 7 ? 7 : n;
}

static void vp_start(void) {
    int n = hle_d3d_vp_threads;
    if (n < 0) n = hle_d3d_vp_default_workers(plat_physical_cores());
    vp_workers = 0;
    if (n > 0 && !(vp_arena = malloc(VP_ARENA))) n = 0;
    for (int i = 0; i < n; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, vp_worker, NULL) == 0) vp_workers++;
    }
    printf("[d3d] vertex programs: %d worker thread%s%s (%d physical cores)\n", vp_workers, vp_workers == 1 ? "" : "s",
           hle_d3d_vp_threads < 0 ? ", auto" : "", plat_physical_cores());
}

/* The bytes nv vertices of stream k occupy up to the end of the last vertex's elements: a
 * declaration may place an element past the stride (it then reads into the next vertex, and the last
 * vertex reads the bytes after the draw's vertices, as the Xbox does). */
static uint32_t stream_bytes(const GeomLayout *l, int k, uint32_t stride, uint32_t nv) {
    uint32_t ext = stride;
    for (int e = 0; e < l->n; e++) {
        const int sz = geom_type_size(l->e[e].type);
        if (l->e[e].stream == k && sz > 0 && l->e[e].offset + (uint32_t)sz > ext) ext = l->e[e].offset + (uint32_t)sz;
    }
    return nv ? (nv - 1) * stride + ext : 0;
}
static void vp_run(const VpJob *jb) {
    uint32_t nb = (jb->nv + VSH_BATCH - 1) / VSH_BATCH;
    if (vp_workers < 0) vp_start();
    if (vp_workers == 0 || nb < 2) {   /* no pool, or a single batch: here, now */
        for (uint32_t b = 0; b < nb; b++) vp_batch(jb, b * VSH_BATCH);
        __atomic_fetch_add(&vp_stat.inline_draws, 1, __ATOMIC_RELAXED);
        return;
    }
    uint32_t bytes[4] = {0}, need = sizeof(GeomLayout) + sizeof(DrawFog) + RENDERER_STAGES * 2 * sizeof(float) + VSH_NCONST * 16 + 16 * 8;
    for (int k = 0; k < 4; k++)
        if (jb->base[k]) need += bytes[k] = (stream_bytes(jb->l, k, jb->stride[k], jb->nv) + 15) & ~15u;
    uint32_t items = (uint32_t)__atomic_load_n(&vp_pub, __ATOMIC_RELAXED);
    if (need > VP_ARENA) {   /* too large to copy: run it here, in place */
        for (uint32_t b = 0; b < nb; b++) vp_batch(jb, b * VSH_BATCH);
        __atomic_fetch_add(&vp_stat.inline_draws, 1, __ATOMIC_RELAXED);
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
            uint8_t *d = vp_alloc(bytes[k]);   /* with the bytes the last vertex reads past its stride */
            memcpy(d, jb->base[k] + (size_t)jb->lo * jb->stride[k], stream_bytes(jb->l, k, jb->stride[k], jb->nv));
            q->base[k] = d;
        }
    q->lo = 0;
    items = (uint32_t)__atomic_load_n(&vp_pub, __ATOMIC_RELAXED);
    for (uint32_t b = 0; b < nb; b++) vp_items[items + b] = (VpItem){vp_njobs, b};
    vp_njobs++;
    __atomic_fetch_add(&vp_stat.queued, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&vp_pub, ((uint64_t)vp_gen << 32) | (items + nb), __ATOMIC_SEQ_CST);
    vp_wake();
}

void hle_d3d_vp_test_draw(const VshProgram *prog, const uint8_t *stream, uint32_t stride, uint32_t nv, const float (*c)[4], RVertex *out) {
    static const GeomLayout l = {{{GEOM_REG_POSITION, 0, GEOM_FLOAT4, 0}}, 1, {16, 0, 0, 0}};
    static const DrawFog fog;   /* off */
    static const float tscale[RENDERER_STAGES][2] = {{1, 1}, {1, 1}, {1, 1}, {1, 1}};
    VpJob jb = {prog, &l, 1u << GEOM_REG_POSITION, 0, nv, {stream, NULL, NULL, NULL}, {stride, 0, 0, 0}, c, out, 1, NULL, &fog, tscale, 0};
    vp_run(&jb);
}
/* As hle_d3d_vp_test_draw, with the position (FLOAT4, v0) at byte offset pos_offset of each vertex:
 * past stride - 16 it reaches into the next vertex, as some of the title's declarations do. */
void hle_d3d_vp_test_draw_at(const VshProgram *prog, const uint8_t *stream, uint32_t stride, uint32_t pos_offset, uint32_t nv, const float (*c)[4],
                             RVertex *out) {
    const GeomLayout l = {{{GEOM_REG_POSITION, 0, GEOM_FLOAT4, (uint16_t)pos_offset}}, 1, {(uint16_t)(pos_offset + 16), 0, 0, 0}};
    static const DrawFog fog;   /* off */
    static const float tscale[RENDERER_STAGES][2] = {{1, 1}, {1, 1}, {1, 1}, {1, 1}};
    VpJob jb = {prog, &l, 1u << GEOM_REG_POSITION, 0, nv, {stream, NULL, NULL, NULL}, {stride, 0, 0, 0}, c, out, 1, NULL, &fog, tscale, 0};
    vp_run(&jb);
}
void hle_d3d_vp_test_wait(void) { vp_wait_all(); }
void hle_d3d_vp_test_counts(uint32_t *queued, uint32_t *inline_draws) {
    *queued = __atomic_exchange_n(&vp_stat.queued, 0, __ATOMIC_RELAXED);
    *inline_draws = __atomic_exchange_n(&vp_stat.inline_draws, 0, __ATOMIC_RELAXED);
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

/* --movie-hd: nonzero while draw_movie_gpu records a movie quad. Its planes are not cached
 * textures: resolve_texture copies them into the renderer's movie plane images every frame. */
static int movie_recording;
/* The coordinate set of each stage for the movie quad: TexCoordIndex is not replayed (the title
 * sets it only for the movie, 0x249589: stage 0 <- set 0, stages 1 and 2 <- set 1). */
static const uint8_t movie_coord_set[RENDERER_STAGES] = {0, 1, 1, 3};
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
    /* The front buffer as a texture (the title's frame-feedback quad, FVF 0x144: the motion blur
     * of QueueMotionBlurEffect and the blinding haze): the previous frame the host rendered. */
    if (hle_d3d_frame_blur && !(fmt & FMT_CUBEMAP) && st.surf[1] && (U32(t, 4) & 0x0FFFFFFFu) == (U32(st.surf[1], 4) & 0x0FFFFFFFu))
        surf = RT_PREV_FRAME;
    else if (hle_d3d_frame_blur && !(fmt & FMT_CUBEMAP) && st.rt != st.surf[0] && (U32(t, 4) & 0x0FFFFFFFu) == (U32(st.surf[0], 4) & 0x0FFFFFFFu))
        surf = RT_BACK_PREV;
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
    return op->tex_key[stage] = tex_race_key(data, fmt, size, G(data), xtex_total_bytes(ti));   /* never 0; checked again at replay (tex_race.h) */
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
#define C_SUN_DIR (90 + 96)   /* c_vSunDir: the shadow direction (lighting census) */
#define C_TEX_PRJ (65 + 96)   /* c_mTexPrj: the shadow map's projection */
static int record_draw_body(uint32_t prim, uint32_t count, uint32_t idx_addr, uint32_t up_data, uint32_t up_stride);
/* --hud-glass: the target health bars (hud_glass_find_targets; the rage meter is left alone) of a just-recorded draw (v, its nv vertices; the n indices at
 * rl->idx + rl->ni) get a glass panel operation each, recorded before the draw, and their quads
 * are faded to the configured opacity. Only small textured draws without a vertex program are
 * looked at (the HUD batches: FVF 0x142, a few dozen quads). */
#define GLASS_MAX_VERTS 1024
/* --lamp-lights: the quads (6 indices each: two triangles) of a fixed-function halo draw as light
 * sprites in rl (world positions from the vertex stream and the world matrix, the recorded diffuse
 * colours), and its camera. idx: the draw's n indices into out (vertex lo + i). */
static void lamp_record(FrameList *rl, const RVertex *out, const uint32_t *idx, uint32_t n, const GeomElement *pos, const uint8_t *const base[4],
                        const uint32_t stride[4], uint32_t lo) {
    for (uint32_t q = 0; q + 6 <= n && rl->lamp_n < LAMP_FRAME_SPRITES; q += 6) {
        float w[6][3];
        uint32_t c[6];
        for (int k = 0; k < 6; k++) {
            const uint32_t v = idx[q + k];
            float p[4], wp[4];
            geom_fetch(pos->type, base[pos->stream] + (lo + v) * stride[pos->stream] + pos->offset, p);
            p[3] = 1;
            geom_row_xform(p, st.world, wp);
            memcpy(w[k], wp, sizeof w[k]);
            c[k] = out[v].color;
        }
        if (!lamp_sprite_quad((const float (*)[3])w, c, 6, &rl->lamp_spr[rl->lamp_n])) rl->lamp_n++;
    }
    memcpy(rl->lamp_view, st.view, sizeof rl->lamp_view);
    rl->has_lamp_view = 1;
}
/* --fire-light: a flame draw's quads as fire sprites in rl (as lamp_record), its camera and the
 * guest clock. */
static void fire_record(FrameList *rl, const RVertex *out, const uint32_t *idx, uint32_t n, const GeomElement *pos, const uint8_t *const base[4],
                        const uint32_t stride[4], uint32_t lo) {
    for (uint32_t q = 0; q + 6 <= n && rl->fire_n < FIRE_FRAME_SPRITES; q += 6) {
        float w[6][3];
        uint32_t c[6];
        for (int k = 0; k < 6; k++) {
            const uint32_t v = idx[q + k];
            float p[4], wp[4];
            geom_fetch(pos->type, base[pos->stream] + (lo + v) * stride[pos->stream] + pos->offset, p);
            p[3] = 1;
            geom_row_xform(p, st.world, wp);
            memcpy(w[k], wp, sizeof w[k]);
            c[k] = out[v].color;
        }
        if (!fire_sprite_quad((const float (*)[3])w, c, 6, &rl->fire_spr[rl->fire_n])) rl->fire_n++;
    }
    memcpy(rl->fire_view, st.view, sizeof rl->fire_view);
    rl->has_fire_view = 1;
    if (!rl->has_fire_ms) rl->has_fire_ms = (uint8_t)guest_clock_ms(&rl->fire_ms);
}
static uint64_t texture_hash(const FrameOp *op, int stage, const XTexInfo *t, int *race);
/* --fire-light: the draw samples a flame texture (fire_light_texture) at stage 0. */
static int fire_draw(const FrameOp *op) {
    XTexInfo ti;
    if (!op->tex_key[0] || op->tex_surf[0] || xtex_info(op->tex_format[0], op->tex_size[0], &ti)) return 0;
    return fire_light_texture(texture_hash(op, 0, &ti, NULL));   /* record time: the bytes the key was taken from */
}
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
/* --mesh-pack (src/meshpack.h): the replacement for a character draw (an indexed triangle list
 * from streams 0 and 1 in the character vertex format), or NULL. Each draw identity (buffer
 * addresses, count, index range and a fingerprint of the first bytes) is content-hashed once. */
/* --tri-check: the bone palette slots (index / 3 of every weighted influence) of nv vertices of
 * stream 1, as a bit set of up to 64 slots; bit 63 also holds every slot past it. */
static uint64_t bone_slots(const GeomLayout *l, const uint8_t *s1, uint32_t stride1, uint32_t nv) {
    const GeomElement *bi = geom_find(l, 4), *bw = geom_find(l, 5);
    if (!bi || !bw || bi->stream != 1 || bw->stream != 1) return 0;
    uint64_t m = 0;
    for (uint32_t v = 0; v < nv; v++) {
        float i[4], w[4];
        geom_fetch(bi->type, s1 + (size_t)v * stride1 + bi->offset, i);
        geom_fetch(bw->type, s1 + (size_t)v * stride1 + bw->offset, w);
        for (int k = 0; k < 3; k++)
            if (w[k] != 0) {
                const int slot = i[k] >= 0 ? (int)i[k] / 3 : 63;
                m |= 1ull << (slot < 63 ? slot : 63);
            }
    }
    return m;
}
static const MeshPackMesh *mesh_pack_swap(const GeomLayout *l, uint32_t prim, uint32_t count, uint32_t n, uint32_t idx_addr,
                                          const uint16_t *ix, uint32_t lo, uint32_t hi, const uint8_t *const base[4],
                                          const uint32_t stride[4]) {
    if (prim != 5 || n != count || stride[0] != MESHPACK_STRIDE0 || stride[1] != MESHPACK_STRIDE1) return NULL;
    for (int k = 0; k < l->n; k++)
        if (l->e[k].stream > 1) return NULL;
    uint32_t id[8 + 8 + 8] = {idx_addr, count, lo, hi, st.stream[0].vb ? U32(st.stream[0].vb, 4) : 0,
                              st.stream[1].vb ? U32(st.stream[1].vb, 4) : 0};
    memcpy(&id[8], ix, (count < 16 ? count : 16) * 2);
    memcpy(&id[16], base[0] + (size_t)lo * stride[0], 32);
    uint64_t key = meshpack_hash((const uint16_t *)id, sizeof id / 2, 0, 0, base[0], 0, base[1], 0) | 1, hash;
    if (!meshpack_memo_get(key, &hash)) {
        hash = meshpack_hash(ix, count, lo, hi, base[0], stride[0], base[1], stride[1]);
        meshpack_memo_set(key, hash);
        const MeshPackMesh *m = meshpack_find(hash);
        if (m && hle_d3d_tri_check) {   /* a replacement must only use the original's palette slots */
            const uint64_t was = bone_slots(l, base[1] + (size_t)lo * stride[1], stride[1], hi - lo + 1), now = bone_slots(l, m->s1, MESHPACK_STRIDE1, m->nv);
            if (now & ~was)
                printf("[tri-check] mesh pack mesh %016llx uses bone palette slots the original does not (original %016llx, replacement %016llx)\n",
                       (unsigned long long)hash, (unsigned long long)was, (unsigned long long)now);
        }
        static int logged;
        if (m) dlog_crumb("mesh pack replacement %016llx", (unsigned long long)hash);
        if (m) printf("[meshpack] character mesh %016llx: %u -> %u triangles, %u -> %u vertices\n", (unsigned long long)hash,
                      count / 3, m->ni / 3, hi - lo + 1, m->nv);
        else if (logged++ < 64)
            printf("[meshpack] character mesh %016llx (%u triangles, %u vertices): no replacement\n", (unsigned long long)hash,
                   count / 3, hi - lo + 1);
        return m;
    }
    return meshpack_find(hash);
}
/* Mod mesh overrides (src/meshover.h): the replacement for an indexed triangle-list draw of one or
 * two vertex streams with the same strides, or NULL. Each draw identity is content-hashed once
 * (meshover's memo); the stage-0 texture is hashed only for a mesh that has a texture-limited
 * override (once per texture: the memo keyed by its header). */
static uint64_t ovr_last_hash;   /* the content hash of the last draw mesh_override looked at (--mesh-log hat lines) */
/* Whether hash is logged for the first time (up to 4096 hashes; later ones are never logged). */
static int log_once(uint64_t hash) {
    static uint64_t seen[4096];
    static int n;
    for (int i = 0; i < n; i++)
        if (seen[i] == hash) return 0;
    if (n == 4096) return 0;
    seen[n++] = hash;
    return 1;
}
/* The texture-pack content hash (texpack_hash) of the bound stage-0 texture, memoized by its
 * address, format and size like the draw hashes: 1 when known (mesh overrides of one skin, texture
 * layers), else 0. */
static int stage0_tex_hash(uint64_t *tex) {
    uint32_t t = st.tex[0];
    if (!t || !readable(t, 0x14) || (U32(t, 0) & COMMON_TYPE_MASK) != COMMON_TEXTURE || !U32(t, 4)) return 0;
    uint32_t fmt = U32(t, 0xc), size = U32(t, 0x10), data = VA(U32(t, 4));
    uint32_t tid[4] = {data, fmt, size, 0x74657821u};
    uint64_t tkey = meshpack_hash((const uint16_t *)tid, sizeof tid / 2, 0, 0, NULL, 0, NULL, 0) | 1;
    XTexInfo ti;
    if (meshover_memo_get(tkey, tex)) return 1;
    if (xtex_info(fmt, size, &ti) || !readable(data, xtex_total_bytes(&ti))) return 0;
    *tex = texpack_hash(&ti, G(data));
    meshover_memo_set(tkey, *tex);
    return 1;
}
static uint32_t inst_groups_off(const VShader *vs, uint64_t hash, const GeomLayout *l, const uint8_t *const base[4], const uint32_t stride[4],
                                uint32_t lo, uint32_t nv);
static uint32_t placed_groups_off(const VShader *vs, uint64_t hash);
static const MeshOverride *mesh_override(const VShader *vs, const GeomLayout *l, uint32_t prim, uint32_t count, uint32_t n, uint32_t idx_addr,
                                         const uint16_t *ix, uint32_t lo, uint32_t hi, const uint8_t *const base[4],
                                         const uint32_t stride[4]) {
    const int character = vs->character;
    if (!ix || !n) return NULL;
    for (int k = 0; k < l->n; k++)
        if (l->e[k].stream > 1) return NULL;
    uint32_t id[8 + 8 + 8] = {idx_addr, count, lo, hi, st.stream[0].vb ? U32(st.stream[0].vb, 4) : 0,
                              st.stream[1].vb ? U32(st.stream[1].vb, 4) : 0, stride[0], stride[1] | prim << 16};
    memcpy(&id[8], ix, (count < 16 ? count : 16) * 2);
    memcpy(&id[16], base[0] + (size_t)lo * stride[0], stride[0] < 32 ? stride[0] : 32);
    uint64_t key = meshpack_hash((const uint16_t *)id, sizeof id / 2, 0, 0, base[0], 0, base[1], 0) | 1, hash;
    if (!meshover_memo_get(key, &hash)) {
        hash = meshover_draw_hash(prim, ix, count, lo, hi, base[0], stride[0], base[1], base[1] ? stride[1] : 0);
        meshover_memo_set(key, hash);
        int needs;
        uint64_t tx;
        if (!meshover_has_mesh(hash, &needs) && hle_d3d_meshover_log && log_once(hash)) {
            char ts[40] = "";
            if (stage0_tex_hash(&tx)) snprintf(ts, sizeof ts, ", texture %016llx", (unsigned long long)tx);
            printf("[meshover] mesh %016llx (%u triangles, %u vertices, strides %u/%u, primitive %u%s%s)\n", (unsigned long long)hash,
                   n / 3, hi - lo + 1, stride[0], base[1] ? stride[1] : 0, prim, character ? ", character" : "", ts);
        }
    }
    ovr_last_hash = hash;
    int needs_tex;
    if (!meshover_has_mesh(hash, &needs_tex)) return NULL;
    uint64_t tex = 0;
    const int tex_known = needs_tex && stage0_tex_hash(&tex);
    const MeshOverride *o =
        meshover_find_inst(hash, tex, tex_known, inst_groups_off(vs, hash, l, base, stride, lo, hi - lo + 1) | placed_groups_off(vs, hash));
    if (!o) return NULL;
    if (o->hide) {
        if (meshover_first_use(o)) printf("[meshover] mod mesh %016llx hidden (%u triangles, %u vertices)\n", (unsigned long long)hash, n / 3, hi - lo + 1);
        return o;
    }
    uint32_t want1 = base[1] ? stride[1] : 0;
    if (o->mesh.stride0 != stride[0] || o->mesh.stride1 != want1) {
        static int warned;
        if (!warned++)
            printf("[meshover] mod mesh %016llx: strides %u/%u do not match the draw's %u/%u; not used\n", (unsigned long long)hash,
                   o->mesh.stride0, o->mesh.stride1, stride[0], want1);
        return NULL;
    }
    if (meshover_first_use(o)) printf("[meshover] mod mesh %016llx drawn (%u triangles, %u vertices)\n", (unsigned long long)hash, n / 3, hi - lo + 1);
    return o;
}
/* --hat-fix, --hat-subdiv (src/hats.h): the heads of this frame's character draws in view space
 * (right-handed, the camera at the origin), and the hat meshes seen so far, each rebuilt once
 * (smoothed normals, PN subdivision) in the layout of its draw with its streams interleaved. The
 * rebuilt meshes are never freed: vertex program batches on the pool and the GPU records read them
 * after the draw. */
#define HAT_HEADS 64
static float hat_heads[HAT_HEADS][3];
static int hat_heads_n;
static uint32_t hat_heads_frame;
/* A model-space point through the draw's c69-c72 (clip = rows . (p, 1)) into view space (x / px,
 * y / py, -w: right-handed, the camera looks down -z). 0 when the projection is unusable. */
static int hat_view(const float p[3], float out[3]) {
    float c[4], s[2];
    for (int r = 0; r < 4; r++) c[r] = st.vsc[C_WVP + r][0] * p[0] + st.vsc[C_WVP + r][1] * p[1] + st.vsc[C_WVP + r][2] * p[2] + st.vsc[C_WVP + r][3];
    if (ssao_proj_scale(st.vsc[C_WVP], st.vsc[C_WVP + 1], st.vsc[C_WVP + 3], s) || !(c[3] > 1e-4f)) return 0;
    out[0] = c[0] / s[0], out[1] = c[1] / s[1], out[2] = -c[3];
    return isfinite(out[0]) && isfinite(out[1]);
}
/* A character draw's head: the centre of its bind-pose vertices weighted most to the head joint
 * (palette slot DOF_PAL_HEAD; SHORT3 bone indices hold slot x 3), posed by that slot's palette rows
 * (the title's vsCharacter: r5 = rows . p, then c69-c72). */
static int char_head_point(const VShader *vs, const GeomLayout *l, const uint8_t *const base[4], const uint32_t stride[4], uint32_t lo,
                           uint32_t nv, float ph[3], float vw[3]) {
    const GeomElement *ep = geom_find(l, 0), *ei = geom_find(l, 4), *ew = geom_find(l, 5);
    const int h = vs->pal_base + 3 * DOF_PAL_HEAD;
    if (vs->pal_base < 0 || !ep || !ei || !ew || h < 0 || h + 3 > (int)(sizeof st.vsc / sizeof st.vsc[0])) return 0;
    double sum[3] = {0, 0, 0};
    uint32_t cnt = 0;
    for (uint32_t v = 0; v < nv; v++) {
        float bi[4], bw[4], p[4];
        geom_fetch(ei->type, base[ei->stream] + (size_t)(lo + v) * stride[ei->stream] + ei->offset, bi);
        geom_fetch(ew->type, base[ew->stream] + (size_t)(lo + v) * stride[ew->stream] + ew->offset, bw);
        const int k = bw[0] >= bw[1] && bw[0] >= bw[2] ? 0 : bw[1] >= bw[2] ? 1 : 2;
        if ((int)bi[k] != 3 * DOF_PAL_HEAD || !(bw[k] > 0)) continue;
        geom_fetch(ep->type, base[ep->stream] + (size_t)(lo + v) * stride[ep->stream] + ep->offset, p);
        sum[0] += p[0], sum[1] += p[1], sum[2] += p[2];
        cnt++;
    }
    if (!cnt) return 0;
    const float c[3] = {(float)(sum[0] / cnt), (float)(sum[1] / cnt), (float)(sum[2] / cnt)};
    for (int i = 0; i < 3; i++) ph[i] = st.vsc[h + i][0] * c[0] + st.vsc[h + i][1] * c[1] + st.vsc[h + i][2] * c[2] + st.vsc[h + i][3];
    return hat_view(ph, vw);
}
static void hat_head_add(const VShader *vs, const GeomLayout *l, const uint8_t *const base[4], const uint32_t stride[4], uint32_t lo, uint32_t nv) {
    if (hat_heads_frame != st.frames) hat_heads_frame = st.frames, hat_heads_n = 0;
    float ph[3], vw[3];
    if (hat_heads_n >= HAT_HEADS || !char_head_point(vs, l, base, stride, lo, nv, ph, vw)) return;
    for (int k = 0; k < hat_heads_n; k++)   /* a character drawn in several parts: one head */
        if (fabsf(hat_heads[k][0] - vw[0]) + fabsf(hat_heads[k][1] - vw[1]) + fabsf(hat_heads[k][2] - vw[2]) < 1e-3f) return;
    memcpy(hat_heads[hat_heads_n++], vw, sizeof vw);
}
/* An object-program draw of a small mesh whose origin is near a head drawn earlier this frame. */
static int hat_draw(const GeomElement *pos, const uint8_t *const base[4], const uint32_t stride[4], uint32_t lo, uint32_t nv) {
    if (hat_heads_frame != st.frames || !hat_heads_n || pos->type != GEOM_FLOAT3 || nv > HAT_MAX_VERTS) return 0;
    float o[3];
    if (!hat_view((const float[3]){0, 0, 0}, o) || !hat_near_head(o, (const float (*)[3])hat_heads, hat_heads_n, HAT_HEAD_REACH)) return 0;
    float mn[3] = {INFINITY, INFINITY, INFINITY}, mx[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (uint32_t v = 0; v < nv; v++) {
        float p[3];
        memcpy(p, base[pos->stream] + (size_t)(lo + v) * stride[pos->stream] + pos->offset, sizeof p);
        for (int k = 0; k < 3; k++) mn[k] = fminf(mn[k], p[k]), mx[k] = fmaxf(mx[k], p[k]);
    }
    return hat_is_small(mn, mx);
}
/* Instance groups of the mods' mesh overrides (meshover.h): which character wears which marker.
 * Each character draw that has an instance-group override adds its head (char_head_point: its
 * view-space point; the palette's own point is the character's local frame) to this frame's list; a marker draw
 * (an object draw, e.g. a hat) whose origin lies within INST_REACH of a head of this frame in view
 * space is worn by it. A character draw is decided from the previous frame (it is drawn before its
 * hat): the head there within INST_SAME of its own in view space and what it wore. A head keeps
 * wearing a marker while no draw of that marker lies within INST_NEAR of it off any head (the hat
 * not drawn: outside the view, an opening scene), and loses it in the frame such a draw does (the
 * hat flying off). A head not seen in the previous frame wears every marker (drawn as the game
 * draws it). */
#define INST_HEADS 64
#define INST_SAME 0.3f     /* m (view space): the same head one frame later (a running character or the camera moves ~0.1 m) */
#define INST_REACH 0.2f    /* m: a worn marker's origin to its head's point (measured: 0.13 for Vargas) */
#define INST_NEAR 1.0f     /* m: a marker off a head this close to a head takes it off that head */
typedef struct {
    float world[3], view[3];
    uint32_t worn;      /* markers drawn on it this frame */
    uint32_t carry;     /* markers it wore in the previous frame (or every one: not seen then) */
    uint32_t near;      /* markers drawn off a head within INST_NEAR of it this frame */
    uint32_t applied;   /* the instance groups its draw used (--mesh-log: changes are printed) */
} InstHead;
static InstHead inst_cur[INST_HEADS], inst_prev[INST_HEADS];
static int inst_ncur, inst_nprev;
static uint32_t inst_frame;
static float dist3(const float a[3], const float b[3]) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}
/* The nearest head of list within reach of p (world or view space), or -1. */
static int inst_nearest(const InstHead *list, int n, const float p[3], int view, float reach) {
    int best = -1;
    float bd = reach;
    for (int k = 0; k < n; k++) {
        const float d = dist3(view ? list[k].view : list[k].world, p);
        if (d < bd) bd = d, best = k;
    }
    return best;
}
/* Points of the mods' mesh groups (meshover_point_set: ModMeshPoint) are world positions; the
 * frame's camera (inst_cam: a world program's c69-c72, world to clip, taken at the first such
 * back-buffer draw of a frame while any group holds a point; the previous frame's until then) takes
 * them to view space as hat_view takes a draw's own points. */
#define INST_POINT_REACH 0.6f   /* m (view space): a head to the segment from a character's point (his feet) up 1.8 m */
#define INST_POINT_TALL 1.8f
#define PLACED_REACH 0.3f       /* m: an object draw's origin to a point of a placed group */
static float inst_cam[4][4];
static int inst_cam_ok;
static uint32_t inst_cam_frame;
static int world_view(const float p[3], float out[3]) {
    float c[4], s[2];
    if (!inst_cam_ok) return 0;
    for (int r = 0; r < 4; r++) c[r] = inst_cam[r][0] * p[0] + inst_cam[r][1] * p[1] + inst_cam[r][2] * p[2] + inst_cam[r][3];
    if (ssao_proj_scale(inst_cam[0], inst_cam[1], inst_cam[3], s) || !(c[3] > 1e-4f)) return 0;
    out[0] = c[0] / s[0], out[1] = c[1] / s[1], out[2] = -c[3];
    return isfinite(out[0]) && isfinite(out[1]);
}
/* Whether a head (view space) belongs to a character standing at one of group g's points: within
 * INST_POINT_REACH of the segment from the point up INST_POINT_TALL (world z). Such a head does not
 * wear the group's marker, whatever the previous frames said: the mod's Lua knows (the head of a
 * character who left the view and came back is new to the runtime). */
static int inst_head_at_point(int g, const float head[3]) {
    for (int i = 0, n = meshover_point_count(g); i < n; i++) {
        const float *p = meshover_point(g, i), top[3] = {p[0], p[1], p[2] + INST_POINT_TALL};
        float a[3], b[3], d[3], e[3];
        if (!world_view(p, a) || !world_view(top, b)) continue;
        for (int k = 0; k < 3; k++) d[k] = b[k] - a[k], e[k] = head[k] - a[k];
        const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        float t = dd > 0 ? (e[0] * d[0] + e[1] * d[1] + e[2] * d[2]) / dd : 0;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        const float q[3] = {a[0] + t * d[0], a[1] + t * d[1], a[2] + t * d[2]};
        if (dist3(q, head) < INST_POINT_REACH) return 1;
    }
    return 0;
}
/* The placed groups (meshover.h) overriding an object draw that are off for it: those with no point
 * within PLACED_REACH of its origin. */
static uint32_t placed_groups_off(const VShader *vs, uint64_t hash) {
    const uint32_t mask = vs->character ? 0 : meshover_placed_mask(hash);
    if (!mask) return 0;
    float o[3];
    if (!hat_view((const float[3]){0, 0, 0}, o)) return mask;
    uint32_t off = mask;
    float best = INFINITY;
    for (int g = 0; g < 32; g++) {
        if (!(mask >> g & 1)) continue;
        for (int i = 0, n = meshover_point_count(g); i < n; i++) {
            float v[3];
            if (!world_view(meshover_point(g, i), v)) continue;
            const float d = dist3(v, o);
            if (d < best) best = d;
            if (d < PLACED_REACH) off &= ~(1u << g);
        }
    }
    if (hle_d3d_meshover_log) {
        static int logged;
        if (logged++ < 40)
            printf("[meshover] placed %016llx: nearest point %.3f m (view), %s (frame %u)\n", (unsigned long long)hash, best,
                   off == mask ? "not placed" : "placed", st.frames);
    }
    return off;
}
static uint32_t inst_groups_off(const VShader *vs, uint64_t hash, const GeomLayout *l, const uint8_t *const base[4], const uint32_t stride[4],
                                uint32_t lo, uint32_t nv) {
    const uint32_t mask = meshover_instance_mask(hash), marker = vs->character ? 0 : meshover_marker_mask(hash);
    if (!mask && !marker) return 0;
    if (inst_frame != st.frames) {
        inst_frame = st.frames;
        memcpy(inst_prev, inst_cur, sizeof inst_cur);
        inst_nprev = inst_ncur;
        inst_ncur = 0;
    }
    if (vs->character) {   /* off: the groups of the markers this character wore in the previous frame */
        InstHead h = {{0}, {0}, 0, 0, 0, 0};
        if (!char_head_point(vs, l, base, stride, lo, nv, h.world, h.view)) return mask;
        int c = inst_nearest(inst_cur, inst_ncur, h.view, 1, 1e-3f);   /* the same head drawn in parts */
        if (c < 0 && inst_ncur < INST_HEADS) inst_cur[c = inst_ncur++] = h;
        const int k = inst_nearest(inst_prev, inst_nprev, h.view, 1, INST_SAME);
        uint32_t wore = ~0u;
        if (k >= 0) {
            uint32_t worn = 0, carry = 0, near = 0;
            for (int i = 0; i < inst_nprev; i++)   /* every pass that drew this head */
                if (dist3(inst_prev[i].view, inst_prev[k].view) < 1e-3f)
                    worn |= inst_prev[i].worn, carry |= inst_prev[i].carry, near |= inst_prev[i].near;
            wore = worn | (carry & ~near);
        }
        if (mask && meshover_points_any())   /* a point says he lost it (ModMeshPoint): also after leaving the view */
            for (int g = 0; g < 32; g++)
                if (((wore & mask) >> g & 1) && meshover_point_count(g) && inst_head_at_point(g, h.view)) {
                    wore &= ~(1u << g);
                    if (hle_d3d_meshover_log) {
                        static int logged;
                        if (logged++ < 40)
                            printf("[meshover] instance group %d: the character with the head at (%.2f %.2f %.2f) (view) lost its marker (point, %s), frame %u\n",
                                   g, h.view[0], h.view[1], h.view[2], k < 0 ? "back in view" : "in view", st.frames);
                    }
                }
        if (c >= 0) inst_cur[c].carry |= wore;
        const uint32_t off = mask & wore;
        if (c >= 0) inst_cur[c].applied |= mask & ~off;
        if (hle_d3d_meshover_log && k >= 0 && c >= 0 && inst_cur[c].applied != inst_prev[k].applied) {
            static int logged;
            if (logged++ < 64)
                printf("[meshover] instance groups %#x -> %#x for the character with the head at (%.2f %.2f %.2f) (view), frame %u\n",
                       inst_prev[k].applied, inst_cur[c].applied, h.view[0], h.view[1], h.view[2], st.frames);
        }
        return off;
    }
    /* a marker: on a head of this frame (else of the previous one: drawn before its wearer) */
    float o[3];
    if (!hat_view((const float[3]){0, 0, 0}, o)) return 0;
    int k = inst_nearest(inst_cur, inst_ncur, o, 1, INST_REACH);
    if (hle_d3d_meshover_log) {
        static int logged[2];
        int j = inst_nearest(inst_cur, inst_ncur, o, 1, 1e9f);
        if (j >= 0 && logged[k < 0] < 40) {
            logged[k < 0]++;
            printf("[meshover] marker %016llx: nearest head %.3f m (view), %s (frame %u, %d heads)\n", (unsigned long long)hash,
                   dist3(inst_cur[j].view, o), k >= 0 ? "worn" : "not worn", st.frames, inst_ncur);
        }
    }
    if (k >= 0) {
        for (int i = 0; i < inst_ncur; i++)
            if (dist3(inst_cur[i].view, inst_cur[k].view) < 1e-3f) inst_cur[i].worn |= marker;
        return marker | mask;
    }
    if (inst_nearest(inst_prev, inst_nprev, o, 1, INST_REACH) >= 0) return marker | mask;
    for (int i = 0; i < inst_ncur; i++)   /* off a head: it takes the marker off the heads near it */
        if (dist3(inst_cur[i].view, o) < INST_NEAR) inst_cur[i].near |= marker;
    return 0;
}
typedef struct {
    uint64_t key;
    uint8_t *v;             /* NULL: this mesh cannot be rebuilt (drawn as it is) */
    uint16_t *idx;
    uint32_t nv, ni, stride;
    uint32_t off[4];        /* each stream's bytes within a vertex */
} HatEntry;
#define HAT_CACHE 512
static HatEntry hat_cache[HAT_CACHE];
static uint32_t hat_cache_n, hat_logged;
/* The rebuilt mesh of a hat draw (n triangle-list indices tri of the draw, through ix when indexed),
 * or NULL when it cannot be rebuilt: the streams' elements must be float vectors or D3DCOLORs inside
 * their strides, with a FLOAT3 normal. */
static const HatEntry *hat_mesh(const GeomLayout *l, const GeomElement *pos, const GeomElement *nrm, const uint8_t *const base[4],
                                const uint32_t stride[4], const uint16_t *ix, const uint32_t *tri, uint32_t n, uint32_t lo, uint32_t nv, int subdiv) {
    uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)subdiv;
#define HAT_MIX(p, len) for (size_t q_ = 0; q_ < (size_t)(len); q_++) h = (h ^ ((const uint8_t *)(p))[q_]) * 0x100000001b3ull
    HAT_MIX(l->e, sizeof l->e[0] * (size_t)l->n);
    HAT_MIX(stride, sizeof(uint32_t) * 4);
    for (uint32_t i = 0; i < n; i++) {
        const uint16_t v = (uint16_t)((ix ? ix[tri[i]] : tri[i]) - lo);
        HAT_MIX(&v, 2);
    }
    for (int k = 0; k < 4; k++)
        if (base[k]) HAT_MIX(base[k] + (size_t)lo * stride[k], (size_t)stride[k] * nv);
#undef HAT_MIX
    h |= 1;
    for (uint32_t i = 0; i < hat_cache_n; i++)
        if (hat_cache[i].key == h) return hat_cache[i].v ? &hat_cache[i] : NULL;
    if (hat_cache_n >= HAT_CACHE) return NULL;
    HatEntry *e = &hat_cache[hat_cache_n++];
    memset(e, 0, sizeof *e);
    e->key = h;
    HatMesh m;
    memset(&m, 0, sizeof m);
    int ok = nrm && nrm->type == GEOM_FLOAT3 && n >= 3 && n % 3 == 0;
    for (int k = 0; k < 4; k++) {
        e->off[k] = m.stride;
        if (base[k]) m.stride += stride[k];
    }
    for (int k = 0; k < l->n && ok; k++) {
        const GeomElement *g = &l->e[k];
        const int sz = geom_type_size(g->type);
        if (sz <= 0 || !base[g->stream] || g->offset + (uint32_t)sz > stride[g->stream]) ok = 0;
        else if (g == pos) m.pos_off = e->off[g->stream] + g->offset;
        else if (g == nrm) m.nrm_off = e->off[g->stream] + g->offset;
        else if (g->type == GEOM_D3DCOLOR && m.nattr < HAT_MAX_ATTRS) m.attr[m.nattr++] = (HatAttr){e->off[g->stream] + g->offset, HAT_ATTR_BYTES, 4};
        else if ((g->type == GEOM_FLOAT1 || g->type == GEOM_FLOAT2 || g->type == GEOM_FLOAT3 || g->type == GEOM_FLOAT4) && m.nattr < HAT_MAX_ATTRS)
            m.attr[m.nattr++] = (HatAttr){e->off[g->stream] + g->offset, HAT_ATTR_FLOAT, (uint8_t)(sz / 4)};
        else ok = 0;
    }
    if (ok) {
        m.nv = nv, m.ni = n;
        m.v = malloc((size_t)nv * m.stride);
        m.idx = malloc((size_t)n * sizeof *m.idx);
        ok = m.v && m.idx;
    }
    if (ok) {
        for (uint32_t v = 0; v < nv; v++)
            for (int k = 0; k < 4; k++)
                if (base[k]) memcpy(m.v + (size_t)v * m.stride + e->off[k], base[k] + (size_t)(lo + v) * stride[k], stride[k]);
        for (uint32_t i = 0; i < n; i++) m.idx[i] = (uint16_t)((ix ? ix[tri[i]] : tri[i]) - lo);
        ok = !hat_smooth_normals(&m, HAT_SMOOTH_DEG);
    }
    if (ok && subdiv) {
        HatMesh s;
        if (hat_subdivide(&m, HAT_SUBDIV_LEVEL, &s)) ok = 0;
        else {
            hat_mesh_free(&m);
            m = s;
        }
    }
    if (!ok) {
        hat_mesh_free(&m);
        if (hat_logged++ < 16) printf("[hats] hat mesh %016llx (%u vertices, %u triangles): drawn as it is (vertex format)\n", (unsigned long long)h, nv, n / 3);
        return NULL;
    }
    e->v = m.v, e->idx = m.idx, e->nv = m.nv, e->ni = m.ni, e->stride = m.stride;
    if (hat_logged++ < 16)
        printf("[hats] hat mesh %016llx: %u -> %u triangles, %u -> %u vertices, normals smoothed%s\n", (unsigned long long)h, n / 3, m.ni / 3, nv,
               m.nv, subdiv ? ", PN-subdivided" : "");
    return e;
}
/* --gpu-skinning: 0 once the host has opened a renderer without it (the programs then run on the CPU
 * as before); written by the host thread, read by the recording thread. */
static int gsk_live = 1;
/* frame_depth_fit's inputs: the first DEPTH_FIT_VERTS clip positions of each depth-tested draw on
 * the back buffer; the passes that use the fit (rain, SSAO, lamps and character lighting, contact
 * shadows, soft particles). */
#define DEPTH_FIT_VERTS 256
#define DEPTH_FIT_DRAWS 256   /* frame_depth_fit stops after this many fitted draws */
/* --wet-light-streaks in use: with --rain-hd and its wet ground. */
static int wet_streaks_used(void) { return hle_d3d_wet_streaks.on && hle_d3d_rain.on && hle_d3d_rain.wet; }
/* --neon-reflections: the texture keys of the sign draws seen so far (frame_neon adds them on the host
 * thread; record_draw_body reads them on the guest threads, relaxed atomics: a key read late only
 * costs a frame), so a sign drawn by a device-run program gets CPU clip positions (FrameOp.neon_pos).
 * Open addressing, NR_PROBES slots per key; cleared at each level load. */
#define NR_KEYS 512
#define NR_PROBES 16
static uint64_t nr_keys[NR_KEYS];
static uint32_t nr_slot(uint64_t key) { return (uint32_t)((key ^ key >> 29) * 0x9E3779B97F4A7C15ull >> 40) % NR_KEYS; }
static int nr_key_has(uint64_t key) {
    if (!key) return 0;
    for (uint32_t j = 0, i = nr_slot(key); j < NR_PROBES; j++, i = (i + 1) % NR_KEYS) {
        const uint64_t v = __atomic_load_n(&nr_keys[i], __ATOMIC_RELAXED);
        if (v == key) return 1;
        if (!v) return 0;
    }
    return 0;
}
static void nr_key_add(uint64_t key) {
    if (!key) return;
    for (uint32_t j = 0, i = nr_slot(key); j < NR_PROBES; j++, i = (i + 1) % NR_KEYS) {
        const uint64_t v = __atomic_load_n(&nr_keys[i], __ATOMIC_RELAXED);
        if (v == key) return;
        if (!v) {
            __atomic_store_n(&nr_keys[i], key, __ATOMIC_RELAXED);
            return;
        }
    }
}
static int depth_fit_used(void) {
    return hle_d3d_rain.on || hle_d3d_damp.on || hle_d3d_ssao.on || hle_d3d_lamp.on || hle_d3d_char_light.on || hle_d3d_contact.on || hle_d3d_soft.on || hle_d3d_shafts.on ||
           hle_d3d_volfog.on || taa_options.on;   /* --wet-light-streaks needs --rain-hd */
}
/* --gpu-vertex-programs: the clip positions (RVertex x, y, z, w; nothing else is read of a GPU
 * draw's list vertices) of its first n vertices into out, from the program's oPos slice
 * (VShader.pos) over the inputs it reads, filled as vp_batch_run fills them: bit-identical to the
 * positions the whole program gives on the CPU. */
static void vp_positions_step(const VShader *vs, const GeomLayout *l, const uint8_t *const base[4], const uint32_t stride[4], uint32_t lo,
                              uint32_t n, uint32_t step, const float (*vsc)[4], RVertex *out);
static void vp_positions(const VShader *vs, const GeomLayout *l, const uint8_t *const base[4], const uint32_t stride[4], uint32_t lo,
                         uint32_t n, const float (*vsc)[4], RVertex *out) {
    vp_positions_step(vs, l, base, stride, lo, n, 1, vsc, out);
}
/* The same for n vertices `step` apart from vertex lo (vertex lo + i * step into out[i]). */
static void vp_positions_step(const VShader *vs, const GeomLayout *l, const uint8_t *const base[4], const uint32_t stride[4], uint32_t lo,
                              uint32_t n, uint32_t step, const float (*vsc)[4], RVertex *out) {
    static __thread float in[16][4][VSH_BATCH], vo[VSH_NOUT][4][VSH_BATCH];
    for (uint32_t i0 = 0; i0 < n; i0 += VSH_BATCH) {
        const int cnt = n - i0 < VSH_BATCH ? (int)(n - i0) : VSH_BATCH;
        for (int r = 0; r < 16; r++)
            for (int j = 0; j < cnt && (vs->pos_reads >> r & 1); j++) { in[r][0][j] = in[r][1][j] = in[r][2][j] = 0; in[r][3][j] = 1; }
        for (int k = 0; k < l->n; k++) {
            const GeomElement *e = &l->e[k];
            if (e->reg >= 16 || !(vs->pos_reads >> e->reg & 1)) continue;
            geom_fetch_rows(e->type, base[e->stream] + (size_t)(lo + i0 * step) * stride[e->stream] + e->offset, stride[e->stream] * step, cnt,
                            &in[e->reg][0][0], VSH_BATCH);
        }
        vsh_run_batch(&vs->pos, cnt, (const float (*)[4][VSH_BATCH])in, vsc, vo);
        for (int j = 0; j < cnt; j++) {
            RVertex *o = &out[i0 + j];
            float pos4[4], c[4];
            for (int q = 0; q < 4; q++) pos4[q] = vo[VSH_OPOS][q][j];
            vsh_clip(pos4, c);
            memcpy(&o->x, c, 16);
        }
    }
}
/* --cutscene-dof-speaker: set by the replay while the depth of field runs (dof_frame), so the
 * character draws record their faces only in cutscenes. */
static int dof_faces_on;
static double dof_face_ms;   /* --stats: recording time of the faces (guest thread) */
#define DOF_FACE_VERTS 24
/* --hair: a character draw adds its bind-pose mesh (streams as recorded: position v0, normal v1,
 * the skin's coordinates, bone indices v4 and weights v5; idx: its triangles from vertex lo) to its
 * skin's hair mask (hair.h), with the skin's own colours, once per skin and mesh (hair_cache_wants:
 * a lookup per draw afterwards). --skin: the same meshes and colours give the skin's skin mask. */
static uint32_t hair_logged, skin_logged, mat_logged;
static void hair_record(const FrameOp *rec, const GeomLayout *l, int uv_reg, const uint8_t *const base[4], const uint32_t stride[4], uint32_t lo,
                        uint32_t nv, const uint32_t *idx, uint32_t ni) {
    static int skin_set;
    if (!skin_set) {
        skin_set = 1;
        hair_cache_set_skin(hle_d3d_skin.on);
        hair_cache_set_mat(hle_d3d_eyes.on, hle_d3d_cloth.on);
    }
    const GeomElement *ep = geom_find(l, 0), *en = geom_find(l, 1), *et = geom_find(l, uv_reg), *ei = geom_find(l, 4), *ew = geom_find(l, 5);
    if (!ep || !et || !ei || !ew || nv < 3 || ni < 3) return;
    uint64_t sig = 0xcbf29ce484222325ull ^ ((uint64_t)nv << 32 | ni);   /* the mesh: its size, first indices and first positions */
    const uint8_t *p0 = base[ep->stream] + (size_t)lo * stride[ep->stream];
    for (uint32_t i = 0; i < ni && i < 48; i++) sig = (sig ^ idx[i]) * 0x100000001b3ull;
    for (uint32_t i = 0; i < 4 * stride[ep->stream] && i < 256; i++) sig = (sig ^ p0[i]) * 0x100000001b3ull;
    const uint64_t key = rec->tex_key[0];
    if (!hair_cache_wants(key, sig)) return;
    XTexInfo ti;
    if (xtex_info(rec->tex_format[0], rec->tex_size[0], &ti)) return;
    float (*pos)[3] = malloc((size_t)nv * sizeof *pos), (*nrm)[3] = en ? malloc((size_t)nv * sizeof *nrm) : NULL;
    float (*uv)[2] = malloc((size_t)nv * sizeof *uv), (*w)[3] = malloc((size_t)nv * sizeof *w);
    int (*sl)[3] = malloc((size_t)nv * sizeof *sl);
    uint32_t *px = ti.w <= 1024 && ti.h <= 1024 && readable(rec->tex_data[0], xtex_total_bytes(&ti)) ? malloc((size_t)ti.w * ti.h * 4) : NULL;
    if (px && xtex_decode_level(&ti, 0, G(rec->tex_data[0]), px)) {
        free(px);
        px = NULL;
    }
    if (pos && uv && w && sl && (nrm || !en)) {
        for (uint32_t v = 0; v < nv; v++) {
            float t[4];
#define HAIR_AT(e) (base[(e)->stream] + (size_t)(lo + v) * stride[(e)->stream] + (e)->offset)
            geom_fetch(ep->type, HAIR_AT(ep), t);
            memcpy(pos[v], t, sizeof pos[v]);
            if (nrm) {
                geom_fetch(en->type, HAIR_AT(en), t);
                memcpy(nrm[v], t, sizeof nrm[v]);
            }
            geom_fetch(et->type, HAIR_AT(et), t);
            uv[v][0] = t[0], uv[v][1] = t[1];
            geom_fetch(ei->type, HAIR_AT(ei), t);
            for (int k = 0; k < 3; k++) sl[v][k] = t[k] >= 0 ? (int)t[k] / 3 : -1;   /* SHORT3 palette offsets: slot x 3 */
            geom_fetch(ew->type, HAIR_AT(ew), t);
            memcpy(w[v], t, sizeof w[v]);
#undef HAIR_AT
        }
        const HairMesh m = {nv, (const float (*)[3])pos, (const float (*)[3])nrm, (const float (*)[2])uv, (const int (*)[3])sl, (const float (*)[3])w, idx, ni};
        const uint32_t n = hair_cache_add(key, sig, &m, px, ti.w, ti.h);
        if ((hle_d3d_eyes.on || hle_d3d_cloth.on) && mat_logged < 12) {
            uint32_t ne, nc;
            hair_cache_mat_texels(key, &ne, &nc);
            mat_logged++;
            printf("[eyes/cloth] skin %016llx (%ux%u%s): %u eye and %u cloth texels in its masks after a mesh of %u vertices\n", (unsigned long long)key, ti.w,
                   ti.h, px ? "" : ", colours unreadable", ne, nc, nv);
        }
        if (hle_d3d_skin.on && skin_logged < 12) {
            skin_logged++;
            printf("[skin] skin %016llx (%ux%u%s): %u skin texels in its mask after a mesh of %u vertices\n", (unsigned long long)key, ti.w, ti.h,
                   px ? "" : ", colours unreadable", hair_cache_skin_texels(key), nv);
        }
        if (hle_d3d_hair.on && hair_logged < 12) {
            hair_logged++;
            printf("[hair] skin %016llx (%ux%u%s): %u hair texels in its mask, from a mesh of %u vertices (head print %016llx%s)\n",
                   (unsigned long long)key, ti.w, ti.h, px ? "" : ", colours unreadable", n, nv, (unsigned long long)hair_head_print(&m),
                   hair_is_headgear(&m) ? ": modeled headgear, no hair" : "");
        }
    }
    free(pos), free(nrm), free(uv), free(w), free(sl), free(px);
}
/* --cutscene-dof-speaker: a character draw's face (dof_face_palette over its bone palette) and the
 * view distance (clip w, median) and screen centre of DOF_FACE_VERTS of its vertices spread evenly
 * over the draw: from the program's oPos slice in one batch (vp_positions_step), or the CPU positions
 * already in out (no program: the skinned declaration). */
static void dof_face_record(FrameList *rl, const VShader *vs, const GeomLayout *l, const uint8_t *const base[4], const uint32_t stride[4],
                            uint32_t lo, uint32_t nv, const RVertex *out) {
    if (rl->nfaces >= DOF_FACES_MAX || nv < 3) return;
    DofFace *f = &rl->faces[rl->nfaces];
    if (!dof_face_palette((const float (*)[4])st.vsc, vs->pal_base, f)) return;
    float w[DOF_FACE_VERTS], sx = 0, sy = 0;
    int n = 0;
    const uint32_t k = nv < DOF_FACE_VERTS ? nv : DOF_FACE_VERTS, step = nv / k;
    RVertex t[DOF_FACE_VERTS];
    if (vs->has_prog) vp_positions_step(vs, l, base, stride, lo, k, step, (const float (*)[4])st.vsc, t);
    for (uint32_t i = 0; i < k; i++) {
        const RVertex *c = vs->has_prog ? &t[i] : &out[i * step];
        if (!(c->w > 0.01f) || !isfinite(c->x) || !isfinite(c->y)) continue;
        int j = n++;
        while (j > 0 && w[j - 1] > c->w) {   /* insertion: w sorted */
            w[j] = w[j - 1];
            j--;
        }
        w[j] = c->w;
        sx += c->x / c->w;
        sy += c->y / c->w;
    }
    if (n) {
        f->dist = w[n / 2];
        f->ndc[0] = sx / n;
        f->ndc[1] = sy / n;
    }
    rl->nfaces++;
}
/* Records a GPU-skinned draw (SkinRec) of nv vertices from vertex lo of the streams; returns its
 * FrameOp.skin value, or 0 when out of memory (the caller runs the program on the CPU). */
static uint32_t skin_record(FrameList *rl, const VShader *vs, const GeomLayout *l, const uint8_t *const base[4], const uint32_t stride[4],
                            uint32_t lo, uint32_t nv, const DrawFog *fog, const float (*tscale)[2]) {
    /* Each stream's bytes up to the end of its last vertex's elements (stream_bytes), as the CPU
     * path reads them. */
    uint32_t bytes = 0, off[4] = {0}, len[4] = {0};
    for (int k = 0; k < 4; k++)
        if (base[k]) {
            len[k] = stream_bytes(l, k, stride[k], nv);
            off[k] = bytes;
            bytes += (len[k] + 3) & ~3u;
        }
    uint32_t need = rl->skin_nbytes + VSH_NCONST * 16 + bytes;
    if (need < rl->skin_nbytes || !grow((void **)&rl->skin_bytes, &rl->skin_bcap, need, 1) ||
        !grow((void **)&rl->skin, &rl->skincap, rl->nskin + 1, sizeof(SkinRec)))
        return 0;
    SkinRec *r = &rl->skin[rl->nskin];
    memset(r, 0, sizeof *r);
    r->vsh = (uint32_t)(vs - vshaders);
    r->layout = *l;
    uint64_t id = 0xcbf29ce484222325ull ^ r->vsh;   /* FNV-1a over the identity's words */
    const uint64_t words[6] = {(uint64_t)(uintptr_t)base[0], (uint64_t)(uintptr_t)base[1], (uint64_t)(uintptr_t)base[2],
                               (uint64_t)(uintptr_t)base[3], lo, nv};
    for (int k = 0; k < 6; k++) id = (id ^ words[k]) * 0x100000001b3ull;
    r->id = id ? id : 1;
    r->fog_mode = fog->on ? fog->mode : GSK_FOG_OFF;
    r->fog_start = fog->start;
    r->fog_end = fog->end;
    r->fog_density = fog->density;
    memcpy(r->tscale, tscale, sizeof r->tscale);
    r->consts = rl->skin_nbytes;
    gsk_pack_constants((const float (*)[4])st.vsc, (float (*)[4])(rl->skin_bytes + r->consts));
    /* The same constants as the frame's previous record (consecutive world draws often differ only
     * in textures): that record's copy, which renderer_draw_skinned then uploads once. */
    if (rl->nskin && !memcmp(rl->skin_bytes + r->consts, rl->skin_bytes + r[-1].consts, VSH_NCONST * 16)) {
        r->consts = r[-1].consts;
        need -= VSH_NCONST * 16;
        r->data = rl->skin_nbytes;
    } else {
        r->data = r->consts + VSH_NCONST * 16;
    }
    r->data_bytes = bytes;
    for (int k = 0; k < 4; k++)
        if (base[k]) {
            memcpy(rl->skin_bytes + r->data + off[k], base[k] + (size_t)lo * stride[k], len[k]);
            r->sbase[k] = off[k];
            r->stride[k] = stride[k];
        }
    rl->skin_nbytes = need;
    return ++rl->nskin;
}
/* --tri-check: once per program and stream, a declaration whose elements reach past the stride (the
 * draw's last vertex then reads the bytes after it). */
static void tri_check_layout(const VShader *vs, const GeomLayout *l, const uint8_t *const base[4], const uint32_t stride[4], uint32_t nv) {
    static uint8_t logged[MAX_VSHADERS];
    const int id = (int)(vs - vshaders);
    for (int k = 0; k < 4; k++) {
        if (!base[k] || logged[id] >> k & 1) continue;
        const uint32_t ext = stream_bytes(l, k, stride[k], 1);
        if (ext <= stride[k]) continue;
        logged[id] |= 1u << k;
        char els[256];
        int at = 0;
        for (int e = 0; e < l->n && at < (int)sizeof els - 32; e++)
            if (l->e[e].stream == k) at += snprintf(els + at, sizeof els - at, " v%d@%u+%d", l->e[e].reg, l->e[e].offset, geom_type_size(l->e[e].type));
        printf("[tri-check] program %d %016llx%s%s: stream %d elements reach %u bytes per vertex past its stride %u (first draw: %u vertices;%s)\n", id,
               (unsigned long long)vs->hash, vs->character ? " (character)" : "", vs->gpu_world ? " (gpu-world)" : "", k, ext, stride[k], nv, els);
    }
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
    for (int i = 0; l && i < RENDERER_STAGES; i++)
        uv[i] = geom_find(l, vs ? (i ? 0xFF : vs->skinned ? 2 : 3) : GEOM_REG_TEX0 + (movie_recording ? movie_coord_set[i] : i));
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
        if (!stride[sn] || !readable(addr + lo * stride[sn], stream_bytes(l, sn, stride[sn], nv))) { st.skipped_fmt++; return 0; }
        base[sn] = G(addr);
    }
    if (hle_d3d_tri_check && vs) tri_check_layout(vs, l, base, stride, nv);
    ovr_last_hash = 0;
    const MeshOverride *ovr = (meshover_count() || hle_d3d_meshover_log) && vs && ix && !up_data
                                  ? mesh_override(vs, l, prim, count, n, idx_addr, ix, lo, hi, base, stride)
                                  : NULL;
    if (ovr && ovr->hide) return 1;   /* a mod's .hide override: not drawn */
    const MeshPackMesh *mesh = !ovr && meshpack_count() && vs && vs->character && ix && !up_data
                                   ? mesh_pack_swap(l, prim, count, n, idx_addr, ix, lo, hi, base, stride)
                                   : NULL;
    const MeshPackMesh *swap = ovr ? &ovr->mesh : mesh;
    if (swap) {   /* same layout (and palette for a character): the title's program draws the replacement */
        if (!grow((void **)&tri, &tri_cap, swap->ni, 4)) return 0;
        n = geom_triangulate(5, swap->ni, tri, swap->ni);   /* replacements are triangle lists */
        ix = swap->idx;
        lo = 0;
        hi = swap->nv - 1;
        nv = swap->nv;
        base[0] = swap->s0;
        if (base[1]) base[1] = swap->s1;
    }
    /* --hat-fix, --hat-subdiv: the heads of the character draws (a --mesh-pack replacement's too: same
     * joints and palette); a hat drawn after them takes its rebuilt mesh (hat_mesh) */
    const int hat_fix = hat_fix_on(&hle_d3d_hats), hat_sub = hat_subdiv_on(&hle_d3d_hats);
    int hat = 0;
    if ((hat_fix || hat_sub) && vs && !off && st.zenable && !up_data) {
        if (vs->character && vs->pal_base >= 0) hat_head_add(vs, l, base, stride, lo, nv);
        else if (!mesh && vs->world == WORLD_PROG_OBJECT && hat_draw(pos, base, stride, lo, nv)) {
            hat = 1;
            if (hle_d3d_meshover_log && ovr_last_hash && log_once(ovr_last_hash ^ 0x686174ull)) {
                char decl[256], hex[160];
                size_t w = 0;
                decl[0] = hex[0] = 0;
                for (int k = 0; k < l->n && w + 24 < sizeof decl; k++)
                    w += (size_t)snprintf(decl + w, sizeof decl - w, " r%u@%u:%u/t%02x", l->e[k].reg, l->e[k].stream, l->e[k].offset,
                                          l->e[k].type);
                for (uint32_t b = 0; b < stride[0] && b < 64; b++) snprintf(hex + 2 * b, 3, "%02x", base[0][(size_t)lo * stride[0] + b]);
                printf("[meshover] hat mesh %016llx (an object drawn on a character's head): elements%s; first vertex %s\n",
                       (unsigned long long)ovr_last_hash, decl, hex);
            }
            const HatEntry *e = hat_mesh(l, pos, nrm, base, stride, ix, tri, n, lo, nv, hat_sub);
            if (e) {
                if (!grow((void **)&tri, &tri_cap, e->ni, 4)) return 0;
                n = geom_triangulate(5, e->ni, tri, e->ni);
                ix = e->idx;
                lo = 0;
                hi = e->nv - 1;
                nv = e->nv;
                for (int k = 0; k < 4; k++)
                    if (base[k]) base[k] = e->v + e->off[k], stride[k] = e->stride;
            }
        }
    }
    /* --hat-fix: not a receiver of the title's character shadow; with --char-lighting its vertices on
     * the CPU (the lit draw reads them) and its view normals recorded */
    const int hat_fixed = hat && hat_fix, hat_cpu = hat_fixed && hle_d3d_char_light.on;
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
    rec.movie = movie_recording != 0;
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
    /* --sky-hd: the draw's state for the sky-stage classifier (sky_classify); a skyline candidate
     * (fixed function, depth writes, a far near plane) keeps its world positions in tex[2] when the
     * draw does not sample stage 2, for the skyline's windows (renderer_skyline). */
    /* --damp-streets reads the sky stage too (frame_damp_sky: outdoors at night), without the skyline's world positions. */
    const int sky_rec = (hle_d3d_sky.on || hle_d3d_damp.on || hle_d3d_volfog.on) && !off && !vs && !rhw;   /* --volfog: indoors (no dome) */
    float sky_radius = 0;
    if (sky_rec) {
        XTexInfo ti;
        rec.sky.kind = SKY_OP_DRAW;
        rec.sky.fvf = st.vshader;
        rec.sky.zwrite = st.zenable && st.rs.depth_write;
        rec.sky.blend = st.rs.blend_enable != 0;
        rec.sky.view_origin = fabsf(st.view[12]) + fabsf(st.view[13]) + fabsf(st.view[14]) < 1e-4f;
        rec.sky.near = st.proj[10] != 0 ? fabsf(st.proj[14] / st.proj[10]) : 0;
        float e = st.material.emissive[0];
        for (int k = 1; k < 3; k++) e = st.material.emissive[k] > e ? st.material.emissive[k] : e;
        rec.sky.emissive = lighting ? e : 0;
        if (rec.tex_key[0] && !xtex_info(rec.tex_format[0], rec.tex_size[0], &ti)) {
            rec.sky.tex_fmt = ti.code;
            rec.sky.tex_w = ti.w;
            rec.sky.tex_h = ti.h;
        }
        rec.sky_world = hle_d3d_sky.on && rec.sky.zwrite && rec.sky.near >= SKY_SKYLINE_NEAR_MIN && rec.sky.near <= SKY_SKYLINE_NEAR_MAX &&
                        (st.vshader == 0x152u || st.vshader == 0x252u) && (rec.has_ps ? psh_tex_mode(&rec.ps, 2) == 0 : 1);
    }
    int real_color = rec.has_ps || textured || (!vs && dif);   /* otherwise the debug color */
    RVertex *out = rl->v + rl->nv;
    int prog = vs && vs->has_prog;
    uint32_t skin_id = 0;   /* --gpu-skinning: the device runs it (real colours; no CPU reader of its vertices) */
    int fit_pos = 0;
    /* --gpu-vertex-programs: a world program's draw also when it samples a depth stage, once the
     * frame's --shadow-range fit (which reads its vertices below) is done. */
    const int world = prog && vs->gpu_world && hle_d3d_gpu_skin.world &&
                      (!depth_stages || !hle_d3d_shadow_range.on || st.shadow_fit_frame == st.frames + 1);
    if (prog && ((vs->gpu && !depth_stages) || world) && !hat_cpu && hle_d3d_gpu_skin.on && __atomic_load_n(&gsk_live, __ATOMIC_RELAXED) && real_color) {
        vsh_set_screen_constants(st.vsc);
        skin_id = skin_record(rl, vs, l, base, stride, lo, nv, &fog, (const float (*)[2])tscale);
        /* --taa: a character's own motion, and a mod mesh override's (a hat falling off moves
         * apart from the camera: without it the history smears it) */
        if (skin_id) rl->skin[skin_id - 1].own_motion = vs->character || ovr != NULL;
        /* What frame_depth_fit reads of it, until this frame's sliced draws alone have given it
         * DEPTH_FIT_DRAWS fits: the fit (in op order, over these and the CPU draws) stops there or
         * before, so it never reaches a later draw without positions. */
        if (st.depth_fit_frame != st.frames + 1) {
            st.depth_fit_frame = st.frames + 1;
            st.depth_fit_ok = 0;
        }
        if (skin_id && world && depth_fit_used() && !off && st.zenable && nv >= 3 && st.depth_fit_ok < DEPTH_FIT_DRAWS) {
            const uint32_t k = nv < DEPTH_FIT_VERTS ? nv : DEPTH_FIT_VERTS;
            float f[2];
            vp_positions(vs, l, base, stride, lo, k, (const float (*)[4])st.vsc, out);
            fit_pos = 1;
            st.depth_fit_ok += !rain_depth_fit(&out[0].z, sizeof(RVertex) / sizeof(float), k, f);
        }
        rec_mark(REC_VSH);
    }
    /* --neon-reflections: a sign's draw run on the device (its texture key seen by frame_neon): its
     * first vertices' clip positions, as for the depth fit (which may have written them already). */
    int neon_pos = 0;
    if (skin_id && !fit_pos && hle_d3d_neon_refl.on && !off && st.zenable && nv && !vs->character)
        for (int s = 0; s < RENDERER_STAGES && !neon_pos; s++)
            if (nr_key_has(rec.tex_key[s])) {
                vp_positions(vs, l, base, stride, lo, nv < DEPTH_FIT_VERTS ? nv : DEPTH_FIT_VERTS, (const float (*)[4])st.vsc, out);
                neon_pos = 1;
            }
    if (prog && !skin_id && !vs->character && hle_d3d_stats) {   /* --stats: why a world program's draw stays on the CPU */
        const int why = !vs->gpu_world                                                                                  ? VP_CPU_UNSUPPORTED
                        : !hle_d3d_gpu_skin.on || !hle_d3d_gpu_skin.world || !__atomic_load_n(&gsk_live, __ATOMIC_RELAXED) ? VP_CPU_OFF
                        : !real_color                                                                                   ? VP_CPU_DEBUG_COLOR
                        : hat_cpu                                                                                       ? VP_CPU_HAT
                                                                                                                        : VP_CPU_SHADOW_FIT;
        __atomic_fetch_add(&vp_cpu_why[why], nv, __ATOMIC_RELAXED);
    }
    if (prog && !skin_id) {   /* the title's vertex program, VSH_BATCH vertices per interpreter pass */
        vsh_set_screen_constants(st.vsc);   /* D3D-reserved; the library's SetViewport is replaced */
        uint32_t fed = 0;   /* input registers the declaration fills (geom_fetch writes all 4 components) */
        for (int k = 0; k < l->n; k++)
            if (l->e[k].reg < 16) fed |= 1u << l->e[k].reg;
        VpJob jb = {&vs->prog, l, fed, lo, nv, {base[0], base[1], base[2], base[3]}, {stride[0], stride[1], stride[2], stride[3]},
                    (const float (*)[4])st.vsc, out, real_color, nrm, &fog, (const float (*)[2])tscale, vs->character};
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
        if (sky_rec) {
            const float r = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            sky_radius = r > sky_radius ? r : sky_radius;
        }
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
        if (rec.sky_world) {
            float wp[4];
            p[3] = 1;
            geom_row_xform(p, st.world, wp);
            memcpy(o->tex[2], wp, 3 * sizeof(float));
        }
    }
    rec.sky.radius = sky_radius;
    rec.sky.nv = nv;
    rec_mark(REC_VERT);
    if (depth_stages && hle_d3d_shadow_range.on) {   /* --shadow-range: the light projection, once per frame */
        const int ds = __builtin_ctz(depth_stages);
        float ab[2];
        /* The fit reads this draw's outputs: a vertex program's batches may still be on the pool. */
        if (st.shadow_fit_frame != st.frames + 1 && prog && !skin_id) vp_wait_all();
        if (st.shadow_fit_frame != st.frames + 1 && !shadow_range_fit(&out[0].tex[ds][2], sizeof(RVertex) / sizeof(float), nv, ab)) {
            memcpy(st.shadow_ab, ab, sizeof ab);
            st.shadow_fit_frame = st.frames + 1;
        }
        memcpy(rec.ps.shadow_depth, st.shadow_ab, sizeof st.shadow_ab);
    }
    if (depth_stages && hle_d3d_lamp_shadow.on && !off && prog && vs->world == WORLD_PROG_WORLD && !rl->has_lsh_prj) {
        /* --lamp-shadows: the receivers' light projection, once per frame, from a world program (its
         * v0 is the world position, as the lamps'; an object program's may carry its own matrix). */
        const int ds = __builtin_ctz(depth_stages);
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) rl->lsh_prj[r][c] = st.vsc[C_TEX_PRJ + r][c] * (r < 2 ? tscale[ds][r] : 1.0f);
        rl->has_lsh_prj = 1;
        static int checked;   /* once: the projection reproduces the program's own stage coordinates */
        if (!checked && pos && !skin_id && nv) {
            checked = 1;
            vp_wait_all();
            float p[4];
            geom_fetch(pos->type, base[pos->stream] + lo * stride[pos->stream] + pos->offset, p);
            p[3] = 1;
            double err = 0, mag = 0;
            for (int r = 0; r < 4; r++) {
                const double v = rl->lsh_prj[r][0] * p[0] + rl->lsh_prj[r][1] * p[1] + rl->lsh_prj[r][2] * p[2] + rl->lsh_prj[r][3];
                err = fmax(err, fabs(v - out[0].tex[ds][r]));
                mag = fmax(mag, fabs(out[0].tex[ds][r]));
            }
            printf("[lamp-shadows] light projection from c%d-c%d, stage %d: vertex 0 off by %.3g of %.3g\n", C_TEX_PRJ - 96, C_TEX_PRJ - 93, ds,
                   err, mag);
        }
    }
    if (depth_stages && hle_d3d_world_shadow.on && !off && prog && vs->world == WORLD_PROG_WORLD && !rl->has_ws) {
        /* --world-shadows: the camera and light, once per frame, from a world program (its v0 is the
         * world position: c69-c72 take it to the clip position). */
        memcpy(rl->ws_wvp, st.vsc[C_WVP], sizeof rl->ws_wvp);
        memcpy(rl->ws_sun, st.vsc[C_SUN_DIR], sizeof rl->ws_sun);
        rl->has_ws = 1;
        static int checked;   /* once: the matrix reproduces the program's own clip position */
        if (!checked && pos && !skin_id && nv) {
            checked = 1;
            vp_wait_all();
            float p[4];
            geom_fetch(pos->type, base[pos->stream] + lo * stride[pos->stream] + pos->offset, p);
            const float *c = &out[0].x;
            double err = 0, mag = 0;
            for (int r = 0; r < 4; r++) {
                if (r == 2) continue;   /* z: not used */
                const double v = rl->ws_wvp[r][0] * p[0] + rl->ws_wvp[r][1] * p[1] + rl->ws_wvp[r][2] * p[2] + rl->ws_wvp[r][3];
                err = fmax(err, fabs(v - c[r]));
                mag = fmax(mag, fabs(c[r]));
            }
            printf("[world-shadows] camera from c%d-c%d: vertex 0 off by %.3g of %.3g; light c%d (%.3f %.3f %.3f)\n", C_WVP - 96, C_WVP - 93, err,
                   mag, C_SUN_DIR - 96, rl->ws_sun[0], rl->ws_sun[1], rl->ws_sun[2]);
        }
    }
    if (!off && prog && vs->world == WORLD_PROG_WORLD && st.zenable && inst_cam_frame != st.frames && meshover_count() && meshover_points_any()) {
        uint32_t bw, bh, bp;   /* the mods' mesh-group points: the frame's 3D camera (inst_cam) */
        res_dims(st.surf[0], &bw, &bh, &bp);
        if (st.vp.w * 2 >= bw && st.vp.h * 2 >= bh) {
            memcpy(inst_cam, st.vsc[C_WVP], sizeof inst_cam);
            inst_cam_ok = 1;
            inst_cam_frame = st.frames;
        }
    }
    if ((taa_options.on || anim_interp_options.on || framegen_options.on) && !off && st.zenable && rl->taa_cam != 1) {   /* --taa, --anim-interp, --frame-gen: the frame's 3D camera (FrameList.taa_wvp) */
        uint32_t bw, bh, bp;
        res_dims(st.surf[0], &bw, &bh, &bp);
        const int big = st.vp.w * 2 >= bw && st.vp.h * 2 >= bh;
        if (prog && vs->world == WORLD_PROG_WORLD) {
            memcpy(rl->taa_wvp, st.vsc[C_WVP], sizeof rl->taa_wvp);
            rl->taa_cam = 1;
        } else if (!vs && !rhw && big && st.proj[15] == 0 && fabsf(st.proj[11]) == 1) {
            for (int j = 0; j < 4; j++)   /* D3D row vectors: clip_j = sum_i [p 1]_i (view proj)_ij */
                for (int i = 0; i < 4; i++) {
                    double v = 0;
                    for (int k = 0; k < 4; k++) v += (double)st.view[4 * i + k] * st.proj[4 * k + j];
                    rl->taa_wvp[j][i] = (float)v;
                }
            rl->taa_cam = 2;
        }
        if (rl->taa_cam) {
            rl->taa_vp[0] = st.vp.x;
            rl->taa_vp[1] = st.vp.y;
            rl->taa_vp[2] = st.vp.w;
            rl->taa_vp[3] = st.vp.h;
        }
    }
    if (combat_debug_on && !off && prog && vs->world == WORLD_PROG_WORLD && !rl->has_cd_wvp) {   /* --combat-debug: the camera */
        memcpy(rl->cd_wvp, st.vsc[C_WVP], sizeof rl->cd_wvp);
        rl->cd_vp = st.vp;
        rl->has_cd_wvp = 1;
    }
    if (vs && vs->character && vs->pal_base >= 0 && !off && st.zenable && hle_d3d_dof.speaker && __atomic_load_n(&dof_faces_on, __ATOMIC_RELAXED)) {
        const double t0 = hle_d3d_stats ? now_ms() : 0;
        if (prog) vsh_set_screen_constants(st.vsc);
        dof_face_record(rl, vs, l, base, stride, lo, nv, out);
        if (hle_d3d_stats) dof_face_ms += now_ms() - t0;
    }
    for (uint32_t i = 0; i < n; i++) rl->idx[rl->ni + i] = (ix ? ix[tri[i]] : tri[i]) - lo;
    if ((hle_d3d_hair.on || hle_d3d_skin.on || hle_d3d_eyes.on || hle_d3d_cloth.on) && hle_d3d_char_light.on && vs && vs->character && !off && st.zenable && rec.tex_key[0] && !rec.tex_surf[0])
        hair_record(&rec, l, vs->skinned ? 2 : 3, base, stride, lo, nv, rl->idx + rl->ni, n);
    if (hle_d3d_eyes.on && hle_d3d_char_light.on && vs && vs->character && vs->pal_base >= 0 && !off)   /* a blink: the lid over the eye */
        rec.eye_shut = 1 - eyes_lid_open((const float (*)[4])st.vsc, vs->pal_base, (int)(sizeof st.vsc / sizeof st.vsc[0]));
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
    op->character = vs && vs->character;
    /* Photo mode (hle_photo.h): a back-buffer draw through a perspective camera (its clip positions
     * move with the free camera; 2 = a view without translation, the sky dome), else 2D (the HUD) */
    if (!off && prog && (st.vsc[C_WVP + 3][0] != 0 || st.vsc[C_WVP + 3][1] != 0 || st.vsc[C_WVP + 3][2] != 0)) op->cam3d = 1;
    else if (!off && !vs && !rhw && st.zenable && st.proj[15] == 0 && fabsf(st.proj[11]) == 1)   /* the HUD: perspective too, but no depth test */
        op->cam3d = fabsf(st.view[12]) + fabsf(st.view[13]) + fabsf(st.view[14]) < 1e-4f ? 2 : 1;
    if (!off && prog && vs->world == WORLD_PROG_WORLD && rl->has_photo_wvp != 1) {   /* photo mode: the frame's camera */
        memcpy(rl->photo_wvp, st.vsc[C_WVP], sizeof rl->photo_wvp);
        rl->has_photo_wvp = 1;
    } else if (op->cam3d == 1 && !vs && !rl->has_photo_wvp && st.zenable && photo_big_viewport()) {
        for (int j = 0; j < 4; j++)   /* else the first full-view fixed-function camera: (view x projection) transposed */
            for (int i = 0; i < 4; i++) {
                double x = 0;
                for (int k = 0; k < 4; k++) x += (double)st.view[4 * i + k] * st.proj[4 * k + j];
                rl->photo_wvp[j][i] = (float)x;
            }
        rl->has_photo_wvp = 2;
    }
    op->hat = (uint8_t)hat_fixed;
    for (int i = 0; i < RENDERER_STAGES && hat_fixed; i++)   /* the title's shadow stage reads white: lit */
        if (depth_stages >> i & 1) op->tex_surf[i] = 0, op->tex_key[i] = 0;
    op->skin = skin_id;
    op->meshpack = mesh != NULL;
    op->meshover = ovr && ovr->png ? (uint16_t)(meshover_index(ovr) + 1) : 0;
    op->texlayer = 0;
    if (texlayer_active() && vs && vs->character) {   /* src/texlayer.h: per skin texture */
        uint64_t th;
        if (stage0_tex_hash(&th)) op->texlayer = texlayer_select(th);
    }
    op->vsh = vs ? (uint8_t)(vs - vshaders + 1) : 0;
    op->fit_pos = (uint8_t)fit_pos;
    op->neon_pos = (uint8_t)neon_pos;
    op->target_c = off ? st.target_c : 0;
    op->target_z = off ? st.target_z : 0;
    if (hle_d3d_frame_blur) {   /* the blur mode's chain: back buffer -> offscreen targets -> composite (RT_BACK_PREV) */
        int fed = 0;
        for (int s = 0; s < RENDERER_STAGES; s++) {
            const uint8_t t = op->tex_surf[s];
            fed |= t == RT_BACK_PREV || (t && t <= MAX_RT_SURFACES && (st.blur_fed >> (t - 1) & 1));
        }
        if (fed && off && op->target_c) {
            st.blur_fed |= 1u << (op->target_c - 1);
            /* Its passes shift it by half a guest texel (0xbc1c0): at the guest size the blinding
             * blur keeps the console's width at any render scale (scaled, a fraction as wide). */
            __atomic_store_n(&rt_surf[op->target_c - 1].guest_size, 1, __ATOMIC_RELAXED);
        }
        if (fed && !off && !st.blur_marked) op->prev_mark = st.blur_marked = 1;
    }
    op->first_vertex = rl->nv;
    op->nv = nv;
    op->first_index = rl->ni;
    op->ni = n;
    if ((hle_d3d_lamp.on || hle_d3d_char_light.on || hle_d3d_world_light.on || hle_d3d_lamp_shadow.on || hle_d3d_shafts.on || hle_d3d_volfog.on || wet_streaks_used() || hle_d3d_damp.on) && op->halo && !off && !vs && !rhw) {   /* --lamp-lights, --char-lighting, --world-lighting, --lamp-shadows, --light-shafts, --volfog, --wet-light-streaks, --damp-streets: the halo quads as light sprites */
        /* Only the 3D scene's camera: the radar draws its markers with the same render state in a
         * small viewport with its own top-down view. */
        uint32_t bw, bh, bp;
        res_dims(st.surf[0], &bw, &bh, &bp);
        if (st.vp.w * 2 >= bw && st.vp.h * 2 >= bh) {
            if (hle_d3d_fire.on && fire_draw(op)) fire_record(rl, out, rl->idx + rl->ni, n, pos, base, stride, lo);   /* not a lamp too */
            else lamp_record(rl, out, rl->idx + rl->ni, n, pos, base, stride, lo);
        }
    }
    if (wet_streaks_used() && fog.on && !off && !rhw && st.zenable) {   /* --wet-light-streaks: the miss fallback's fog colour */
        rl->fog_color = st.fog_color;
        rl->has_fog = 1;
    }
    if (hle_d3d_volfog.on && !off && !rhw && st.zenable) {   /* --volfog: the fog of the frame's last depth-tested 3D draw (its far plane and fade) */
        rl->vf_fog_on = (uint8_t)(fog.on != 0);
        rl->vf_fog_mode = fog.mode;
        rl->vf_fog_start = fog.start;
        rl->vf_fog_end = fog.end;
        rl->vf_fog_density = fog.density;
        rl->has_vf_fog = 1;
    }
    if (hle_d3d_level.on && !off && !vs && !rhw && st.zenable && st.proj[15] == 0 && fabsf(st.proj[11]) == 1) {   /* --level-lights: the 3D camera */
        uint32_t bw, bh, bp;
        res_dims(st.surf[0], &bw, &bh, &bp);
        if (st.vp.w * 2 >= bw && st.vp.h * 2 >= bh) {
            memcpy(rl->level_view, st.view, sizeof rl->level_view);
            rl->has_level_view = 1;
        }
    }
    if ((hle_d3d_ssao.on || hle_d3d_lamp.on || hle_d3d_char_light.on || hle_d3d_world_light.on || hle_d3d_shafts.on || hle_d3d_volfog.on || wet_streaks_used()) && !off && st.zenable && st.rs.depth_write &&   /* --ssao, --lamp-lights, --char-/world-lighting, --light-shafts, --volfog, --wet-light-streaks: the camera's x/y scale */
        (prog ? ssao_proj_scale(st.vsc[C_WVP], st.vsc[C_WVP + 1], st.vsc[C_WVP + 3], op->ssao_scale)
              : ssao_proj_scale((const float[4]){st.proj[0], st.proj[4], st.proj[8], 0}, (const float[4]){st.proj[1], st.proj[5], st.proj[9], 0},
                                (const float[4]){st.proj[3], st.proj[7], st.proj[11], 0}, op->ssao_scale)))
        op->ssao_scale[0] = op->ssao_scale[1] = 0;
    /* --world-lighting: a world or object program's draw into the 3D scene, opaque or an alpha-blended
     * decal (additive and multiplying layers are not lit): its view normals and box, from the raw
     * inputs (also for --gpu-vertex-programs draws, whose outputs the device computes). */
    if ((hle_d3d_world_light.on || hat_cpu) && prog && vs->world && !off && st.zenable && nrm &&
        (!st.rs.blend_enable || (st.rs.blend_src == 0x302 && st.rs.blend_dst == 0x303)))
        world_light_record(&rl->wl, &op->wl, (const float (*)[4])st.vsc[C_WVP], pos->type, base[pos->stream] + lo * stride[pos->stream] + pos->offset,
                           stride[pos->stream], nrm->type, base[nrm->stream] + lo * stride[nrm->stream] + nrm->offset, stride[nrm->stream], nv);
    {
        const SoftDraw sd = {off, st.vshader & 1 ? 0 : st.vshader, prim, stride[0], idx_addr != 0, st.zenable != 0, st.rs.depth_write,
                             st.rs.blend_enable, st.rs.blend_src, st.rs.blend_dst, st.rs.blend_eq};
        op->world_particle = (uint8_t)soft_particle_draw(&sd);
    }
    if (hle_d3d_soft.on && op->world_particle && st.proj[15] == 0 && fabsf(st.proj[11]) == 1 && st.proj[0] > 0 && st.proj[5] > 0) {   /* --soft-particles */
        const float *u = &st.view[8];   /* world up (+z) in view space */
        op->particle = 1;
        op->soft_view = (SoftView){st.proj[0], st.proj[5], st.proj[11], {u[0], u[1], u[2]}, 1};
    }
    if ((hle_d3d_shafts.on || hle_d3d_volfog.on) && !hle_d3d_sky.on && !off && !vs) {   /* --light-shafts, --volfog: the title's rain layers thicken the air (as --sky-hd counts them) */
        const RainDraw rd = {st.vshader & 1 ? 0 : st.vshader, prim, nv, n, st.zenable != 0, st.rs.blend_enable, st.rs.blend_src, st.rs.blend_dst, out,
                             st.vp.h > 0 ? st.vp.w / st.vp.h : 0};
        RainLayer layer;
        if (rain_layer(&rd, &layer)) rl->sky_rain++;
    }
    if ((hle_d3d_rain.on || hle_d3d_sky.on || hle_d3d_damp.on || hle_d3d_volfog.on) && !rl->has_frame_ms) rl->has_frame_ms = (uint8_t)guest_clock_ms(&rl->frame_ms);
    if (hle_d3d_sky.on && !off) {   /* --sky-hd: the dome's camera, the shadow direction, the rain layers */
        if (sky_rec && !rl->has_sky_cam && sky_is_dome(&op->sky)) {
            rl->sky_cam_op = rl->nops - 1;
            memcpy(rl->sky_view, st.view, sizeof rl->sky_view);
            memcpy(rl->sky_proj, st.proj, sizeof rl->sky_proj);
            rl->has_sky_cam = 1;
        }
        if (prog) {
            memcpy(rl->sun, st.vsc[C_SUN_DIR], sizeof rl->sun);
            rl->has_sun = 1;
        }
        RainLayer layer;
        const RainDraw rd = {st.vshader & 1 ? 0 : st.vshader, prim, nv, n, st.zenable != 0, st.rs.blend_enable, st.rs.blend_src, st.rs.blend_dst, out,
                             st.vp.h > 0 ? st.vp.w / st.vp.h : 0};
        if (!vs && rain_layer(&rd, &layer)) rl->sky_rain++;
    }
    if ((hle_d3d_rain.on || hle_d3d_contact.on) && !off && !(st.vshader & 1)) {   /* fixed-function: out is complete here */
        const RainDraw rd = {st.vshader, prim, nv, n, st.zenable != 0, st.rs.blend_enable, st.rs.blend_src, st.rs.blend_dst, out,
                             st.vp.h > 0 ? st.vp.w / st.vp.h : 0};
        if (hle_d3d_rain.on && rain_layer(&rd, &op->rain_layer)) {
            op->rain = 1;
            rain_scroll_layer(&st.rain_scroll, st.rain_layers++, &op->rain_layer, op->rain_scroll);
            if (!rl->has_clock) rl->has_clock = (uint8_t)guest_clock_ms(&rl->clock_ms);
        } else if ((hle_d3d_rain.splash || hle_d3d_contact.on) && st.zenable && st.proj[15] == 0 && fabsf(st.proj[11]) == 1) {
            memcpy(rl->cam_view, st.view, sizeof rl->cam_view);
            memcpy(rl->cam_proj, st.proj, sizeof rl->cam_proj);
            rl->has_cam = 1;
        }
    }
    if (!rl->draws++) rl->first_draw = rl->nops - 1;
    rl->movie_draws += op->movie;
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
/* --movie-hd: the screen rectangle of a 16:9 HD movie (the _hd files, movie_hd.h). The title
 * centres the quad at the movie's own size, (-320,-120)-(960,600) for 1280x720: on an HDTV the
 * back buffer is 1280x720. Here it is fitted to the whole 640x480 frame at the movie's aspect on
 * the display: letterboxed at 4:3, the whole frame at --widescreen (16:9 display, anamorphic
 * pixels). Other movies keep the title's rectangle. */
static void movie_rect(const YuvPlane *luma, const float *tl, const float *br, float r[4]) {
    r[0] = tl[0], r[1] = tl[1], r[2] = br[0], r[3] = br[1];
    if (!movie_hd_on || !st.w || !st.h || (uint64_t)luma->w * 9 != (uint64_t)luma->h * 16) return;
    float frame[4] = {0, 0, (float)st.w, (float)st.h};
    float par = (hle_d3d_widescreen ? 16.f / 9.f : (float)st.w / st.h) / ((float)st.w / st.h);
    yuv_fit_rect(frame, 16.f / 9.f, par, r);
}

/* The constants of a program set with SetPixelShaderProgram are its definition's own
 * (PSConstant0/1, the final combiner's); its PSC0/PSC1Mapping only say which D3D constant a later
 * SetPixelShaderConstant would overwrite. psh_from_def reads mapped stages from the D3D constants,
 * which the title never sets for the Bink program, so for the movie quad they are loaded from the
 * definition (D3DPIXELSHADERDEF dwords: PSConstant0 10, PSConstant1 18, final constants 43/44,
 * PSC0Mapping 57, PSC1Mapping 58, PSFinalCombinerConstants 59). */
static void movie_constants(const uint32_t *def, uint32_t psc[PSH_CONSTANTS]) {
    for (int i = 0; i < 8; i++) {
        uint32_t n0 = def[57] >> (4 * i) & 0xF, n1 = def[58] >> (4 * i) & 0xF;
        if (n0 < PSH_CONSTANTS && n0 != 0xF) psc[n0] = def[10 + i];
        if (n1 < PSH_CONSTANTS && n1 != 0xF) psc[n1] = def[18 + i];
    }
    for (int i = 0; i < 2; i++) {
        uint32_t n = def[59] >> (4 * i) & 0xF;
        if (n < PSH_CONSTANTS && n != 0xF) psc[n] = def[43 + i];
    }
}

/* --movie-hd with a renderer: the movie quad is recorded as a regular draw, so the renderer runs
 * the title's own YUV combiner program at output resolution with bilinear sampling of all three
 * planes (the chroma planes interpolated). The title's vertices (its stack copy for this call)
 * are rewritten for the record (rectangle per movie_rect, texture coordinates per yuv_gpu_coord)
 * and restored before the title sees them again. The planes go to the renderer's persistent
 * movie images (renderer_movie_plane), outside the texture cache. */
static int draw_movie_gpu(const uint32_t *a, const YuvPlane pl[3], const float *tl, const float r[4]) {
    float *v = G(a[2]), saved[4 * MOVIE_VERTEX_DWORDS], left = tl[0], top = tl[1];   /* tl, br point into v */
    memcpy(saved, v, sizeof saved);
    for (int i = 0; i < 4; i++) {
        float *q = v + i * MOVIE_VERTEX_DWORDS;
        q[0] = q[0] == left ? r[0] : r[2];
        q[1] = q[1] == top ? r[1] : r[3];
        for (int set = 0; set < 2; set++) {
            const YuvPlane *p = &pl[set];   /* set 0 feeds the luma, set 1 both chroma planes */
            q[5 + 2 * set] = yuv_gpu_coord(q[5 + 2 * set], p->u0, p->u1);
            q[6 + 2 * set] = yuv_gpu_coord(q[6 + 2 * set], p->v0, p->v1);
        }
    }
    uint32_t psc[PSH_CONSTANTS];
    memcpy(psc, st.psc, sizeof psc);
    movie_constants(G(st.ps_def), st.psc);
    movie_recording = 1;
    int ok = record_draw(a[0], a[1], 0, a[2], a[3]);
    movie_recording = 0;
    memcpy(st.psc, psc, sizeof psc);
    memcpy(v, saved, sizeof saved);
    return ok;
}

static int renderer_on;   /* set by the host main thread once Vulkan is up (hle_d3d_service) */
/* (PrimitiveType, VertexCount, pVertexStreamZeroData, VertexStreamZeroStride). Returns 0 when
 * the draw is not the movie quad, 1 when drawn on the CPU, 2 when recorded for the renderer. */
static int draw_movie_quad(const uint32_t *a) {
    static uint32_t logged_w, logged_h;
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
    float r[4];
    movie_rect(&pl[0], tl, br, r);
    int drawn = 0;
    /* Only once the renderer runs (until then, or without Vulkan, frames show the CPU-drawn back
     * buffer). Swizzled planes (none in this title) are unswizzled into host buffers: CPU only. */
    if (movie_hd_on && renderer_on && FMT_CODE(U32(st.tex[0], 0xc)) == 0x13 && draw_movie_gpu(a, pl, tl, r))
        drawn = 2;
    if (!drawn) {
        uint32_t w, h, pitch;
        if (xtex_fmt_bpp(FMT_CODE(U32(st.rt, 0xc))) != 32 || !U32(st.rt, 4)) return 0;
        res_dims(st.rt, &w, &h, &pitch);
        int x0 = (int)lroundf(r[0]), y0 = (int)lroundf(r[1]), x1 = (int)lroundf(r[2]), y1 = (int)lroundf(r[3]);
        (movie_hd_on ? yuv_draw_quad_bilinear : yuv_draw_quad)(G(VA(U32(st.rt, 4))), pitch, w, h, x0, y0, x1, y1, pl, &c);
        drawn = 1;
    }
    if (pl[0].w != logged_w || pl[0].h != logged_h) {   /* the first quad of each movie size */
        logged_w = pl[0].w;
        logged_h = pl[0].h;
        printf("[d3d] movie quad (%.0f,%.0f)-(%.0f,%.0f) %s: planes", r[0], r[1], r[2], r[3],
               drawn == 2 ? "on the renderer" : movie_hd_on ? "on the CPU (bilinear)" : "on the CPU");
        for (int i = 0; i < 3; i++)
            printf(" %ux%u fmt %02x pitch %u uv %.1f,%.1f-%.1f,%.1f;", pl[i].w, pl[i].h, FMT_CODE(U32(st.tex[i], 0xc)),
                   pl[i].pitch, pl[i].u0, pl[i].v0, pl[i].u1, pl[i].v1);
        printf(" shader %08x: off %.4f %.4f %.4f, R %.4f %.4f %.4f, G %.4f %.4f %.4f, B %.4f %.4f %.4f\n", st.ps_def,
               c.off[0], c.off[1], c.off[2], c.m[0][0], c.m[0][1], c.m[0][2], c.m[1][0], c.m[1][1], c.m[1][2], c.m[2][0],
               c.m[2][1], c.m[2][2]);
    }
    return drawn;
}
static uint32_t draw_vertices_up(D3dFrame *f, const uint32_t *a) {
    (void)f;
    int movie = draw_movie_quad(a);
    if (movie) {
        __atomic_add_fetch(&st.movie_quads, 1, __ATOMIC_SEQ_CST);
        if (movie == 2) {
            __atomic_add_fetch(&st.movie_gpu, 1, __ATOMIC_SEQ_CST);
        } else {
            st.rec.cpu_draws++;
            if (st.rec.draws) st.rec.cpu_after_draw++;
        }
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
    E("D3DDevice_SetGammaRamp", 0x361110, 8, set_gamma_ramp, 0x53, 0x55, 0x8b, 0x6c, 0x24),
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
    if (n == 1 && e->fn == ok) dlog_missing_first(e->name, f->ret_addr, &e->calls);   /* accepted, not emulated */
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

/* Texture replay race (tex_race.h): the render thread's checks, the replayed frame they log, and
 * the draws the guard skipped. tex_race_skip is set by resolve_texture for the draw being bound. */
static TexRaceStats tex_race;
static double tex_race_ms;
static TexRaceRecent tex_race_recent;
static uint32_t tex_race_frame, tex_race_skipped, tex_race_drawn;
static int tex_race_skip;
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
    printf("[d3d] %u swaps (%u presented, %u dropped), last clear color %08x, %u movie quads drawn (%u on the renderer); %u draws recorded for the renderer "
           "(skipped: %u other render target, %u unsupported format/memory, %u DrawVerticesUP not recorded); D3D calls: %u total;%s\n",
           hle_d3d_frame_count(), st.presented, st.dropped, st.clear_color, st.movie_quads, st.movie_gpu, st.drawn, st.skipped_target, st.skipped_fmt, st.skipped_up,
           total, buf);
    len = 0;
    for (int c = 0; c < 256; c++)
        if ((st.tex_draws[c] || st.tex_unsupported[c]) && len < (int)sizeof buf - 40)
            len += snprintf(buf + len, sizeof buf - len, " %02x=%u%s", c, st.tex_draws[c] + st.tex_unsupported[c],
                            st.tex_unsupported[c] ? " (unsupported)" : "");
    buf[len] = 0;
    printf("[d3d] stage-0 texture formats of recorded draws:%s\n", len ? buf : " none");
    printf("[tex-race] %llu textures changed between record and replay in %llu replay checks (%.1f MiB checked, %.2f ms), %u log lines "
           "suppressed; guard %s: %u draws skipped, %u textures drawn with their current bytes\n",
           (unsigned long long)tex_race.races, (unsigned long long)tex_race.checks, tex_race.check_bytes / 1048576.0, tex_race_ms,
           tex_race.suppressed, tex_race_guard ? "on" : "off (--no-tex-race-guard)", tex_race_skipped, tex_race_drawn);
}

/* --dump-textures / --texture-pack (src/texpack.h): opened on the main thread with the first
 * texture; the manifest is rewritten every 600 frames and on exit. */
static struct {
    int init, dump, pack;
    uint32_t flushed_at, replaced, replaced_bc, missing, bad_dds;
    double load_ms;
} tp;
static int tp_loaders;   /* texture pack loader threads (see tp_loader) */
static int tp_masks;     /* --neon-glow masks indexed (neon_index) */
static int tp_relief;    /* --normal-maps companion maps indexed (relief_index) */
/* Load queue kinds of the --normal-maps companions (texload.h kinds are the caller's numbers): the
 * normal map and the specular map, read like replacements (texpack_load_image). */
#define TL_RELIEF (TL_MASK + 1)
#define TL_RELIEF_KIND(kind) ((kind) - TL_RELIEF)   /* -> NORMAL_MAPS_RELIEF / NORMAL_MAPS_SPECULAR */
static void tp_start_loaders(void);
static void tp_summary(void);
static void tp_budget_frame(void);
/* The texture-pack cache cap (src/vram_budget.h): 1 GiB or --texpack-budget, lowered to what the
 * driver's memory budget leaves when it reports one (VK_EXT_memory_budget). */
static VramBudget tpb;
unsigned hle_d3d_texpack_budget_mb;

void (*hle_d3d_on_exit)(void);

/* ---- always-on [hitch] and [level-summary] lines (perf_log.h), host thread ---- */
int (*hle_d3d_level_pending)(void);
static uint32_t perf_tex_misses;   /* pack replacements not loaded yet when drawn (render_frame) */
static PerfHitchGate perf_gate;
static PerfLevel perf_lvl;
static char perf_level[PERF_LEVEL_CHARS];
/* Level names from the guest thread: written into the slot after the published one, then published. */
static char level_req[2][PERF_LEVEL_CHARS];
static uint32_t level_gen;
/* --load-profile (load_prof.h): the request's time, process CPU and counters, beside its name. */
static struct { double t, cpu; uint64_t c[LOAD_COUNTERS][3]; } level_req_at[2];
static LoadWindow load_win;
static double load_pack_ms;   /* texture pack replacements created in this frame (render thread) */
/* --materials: the level's material map, and --lamp-shadows: its wall map (lamp_shadow.h walls),
 * built on the guest thread at the level load from one read of the disc (about 15 ms) and handed to
 * the render thread, which uploads them (mat_take, wall_take). */
static char mat_disc[1024];
typedef struct {
    MatMap map;        /* texels NULL: the level has none or it was not wanted (the old one is dropped) */
    MatWallMap walls;  /* likewise */
    uint32_t gen;      /* door_gen of this load */
    char level[PERF_LEVEL_CHARS];
    double ms;
} MatPending;
static MatPending *mat_pending;   /* exchanged atomically */
void hle_d3d_materials_disc(const char *disc_dir) { snprintf(mat_disc, sizeof mat_disc, "%s", disc_dir ? disc_dir : ""); }
static int mat_wanted(void) { return (hle_d3d_materials.on && hle_d3d_rain.on && hle_d3d_rain.wet) || hle_d3d_damp.on; }   /* --damp-streets needs the map too */
/* --volfog: the ground mist lies on the map's surfaces (read on the CPU only; the renderer's copy stays mat_wanted's). */
static int mat_build_wanted(void) { return mat_wanted() || hle_d3d_volfog.on; }
/* --light-walls: the port's lights are on and the walls stop them; also the
 * lamps' streaks on wet and damp ground (no streak of a light behind a wall). */
static int light_walls_wanted(void) {
    return hle_d3d_lamp.walls && (hle_d3d_lamp.on || hle_d3d_char_light.on || hle_d3d_world_light.on || wet_streaks_used() || hle_d3d_damp.on);
}
/* --volfog: the walls stop the light in the air too (with --light-walls, on by default). */
static int volfog_walls_wanted(void) { return hle_d3d_volfog.on && hle_d3d_lamp.walls; }
static int render_walls_wanted(void) { return (hle_d3d_lamp_shadow.on && hle_d3d_lamp_shadow.walls) || light_walls_wanted() || volfog_walls_wanted(); }
/* --audio-occlusion: the sound reads the same wall map (hle_d3d_audio_walls). */
int hle_d3d_audio_occlusion;
void (*hle_d3d_audio_level)(const char *disc, const char *level, uint32_t gen);
void (*hle_d3d_audio_doors)(uint32_t gen, const uint16_t *open, uint32_t n);
static int walls_wanted(void) { return render_walls_wanted() || hle_d3d_audio_occlusion || hle_d3d_audio_level; }   /* the doors' checksum needs the wall map */
/* --wall-doors: doors the title opens leave the wall map (while the renderer uses it). */
static int doors_wanted(void) { return hle_d3d_lamp_shadow.doors && render_walls_wanted(); }
static void mat_level(const char *name) {
    const int want_mat = mat_build_wanted(), want_walls = walls_wanted();
    if ((!want_mat && !want_walls) || !mat_disc[0]) return;
    MatPending *p = calloc(1, sizeof *p);
    if (!p) return;
    snprintf(p->level, sizeof p->level, "%s", name ? name : "");
    const double t0 = now_ms();
    mat_level_maps(mat_disc, p->level, want_mat ? &p->map : NULL, want_walls ? &p->walls : NULL);   /* a map not built stays zeroed */
    p->ms = now_ms() - t0;
    p->gen = ++door_gen;   /* the guest thread's door reads (door_frame) follow this map */
    if (hle_d3d_audio_level) hle_d3d_audio_level(mat_disc, p->level, p->gen);
    door_ntri = p->walls.texels ? p->walls.ntri : 0;
    door_sum = p->walls.tri_sum;
    door_ok = 0;
    door_w = door_tri = 0;
    MatPending *old = __atomic_exchange_n(&mat_pending, p, __ATOMIC_ACQ_REL);
    if (old) {
        mat_map_free(&old->map);
        mat_wall_map_free(&old->walls);
        free(old);
    }
}
/* Render thread, between frames: the level's maps last handed over (mat_level), and whether each is
 * new since its last upload. */
static MatPending *mat_cur;
static pthread_mutex_t walls_lock = PTHREAD_MUTEX_INITIALIZER;   /* mat_cur's walls: hle_d3d_audio_walls reads them */
static int mat_new, walls_new;
static void mat_poll(void) {
    MatPending *p = __atomic_exchange_n(&mat_pending, NULL, __ATOMIC_ACQ_REL);
    if (!p) return;
    pthread_mutex_lock(&walls_lock);
    if (mat_cur) {
        mat_map_free(&mat_cur->map);
        mat_wall_map_free(&mat_cur->walls);
        free(mat_cur);
    }
    mat_cur = p;
    pthread_mutex_unlock(&walls_lock);
    mat_new = walls_new = 1;
}
/* --audio-occlusion: the walls between a listener and a sound source, both in the title's audio axes
 * (DirectSound: y up; the level's world is (x, z, y) of them, logged in a mission 7 run). A source below the listener is lifted up to 1 unit, never above it
 * (footsteps lie on the ground, so curbs and low walls do not muffle them), and the first and last
 * 0.75 units of the segment are left out. Any thread: the
 * render thread swaps the map (mat_poll) and opens doors in it (door_take) under walls_lock. */
#define AUDIO_WALL_LIFT 1.0f
#define AUDIO_WALL_SKIP 0.75f
int hle_d3d_audio_walls(const float lis[3], const float src[3]) {
    const float lift = src[1] < lis[1] ? fminf(AUDIO_WALL_LIFT, lis[1] - src[1]) : 0;
    const float a[3] = {lis[0], lis[2], lis[1]}, b[3] = {src[0], src[2], src[1] + lift};
    pthread_mutex_lock(&walls_lock);
    const int n = mat_cur ? mat_wall_crossings(&mat_cur->walls, a, b, AUDIO_WALL_SKIP) : 0;
    pthread_mutex_unlock(&walls_lock);
    return n;
}
/* Render thread, between frames: a new level's material map to the renderer (once; again after the
 * renderer was recreated). Returns 1 with a map loaded. */
static int mat_take(void) {
    static int failed;
    mat_poll();
    MatPending *cur = mat_cur;
    if (mat_new && cur && mat_wanted()) {
        mat_new = 0;
        failed = 0;
        if (!cur->map.texels) {
            renderer_material_map(NULL);
            printf("[materials] %s: no material map (no collision mesh found on the disc)\n", cur->level);
            return 0;
        }
        uint32_t n[MAT_CLASSES] = {0}, tot = 0;
        for (size_t i = 0; i < (size_t)cur->map.w * cur->map.h; i++)
            if (cur->map.texels[2 * i] & 0xfff) n[cur->map.texels[2 * i] >> 12 < MAT_CLASSES ? cur->map.texels[2 * i] >> 12 : 0]++, tot++;
        char mix[256];
        int k = 0;
        for (int c = 1; c < MAT_CLASSES && k < (int)sizeof mix - 24; c++)
            if (n[c] * 100 >= tot) k += snprintf(mix + k, sizeof mix - (size_t)k, " %s %u%%", mat_class_name(c), (unsigned)(n[c] * 100 / (tot ? tot : 1)));
        printf("[materials] %s: %dx%d map (%.2f m cells), %u surfaces, built in %.1f ms; top surfaces:%s\n", cur->level, cur->map.w, cur->map.h,
               cur->map.cell, cur->map.surfaces, cur->ms, k ? mix : " none");
    }
    if (!cur || !cur->map.texels || failed) return 0;
    if (!renderer_material_map_on() && renderer_rain_wet_on()) {
        const RMaterialMap rm = {cur->map.w, cur->map.h, cur->map.x0, cur->map.y0, cur->map.cell, cur->map.z0, cur->map.zstep, cur->map.texels};
        renderer_material_map(&rm);
        failed = !renderer_material_map_on();
    }
    return renderer_material_map_on();
}
/* Render thread, between frames: a new level's wall map to the renderer (--lamp-shadows walls,
 * --light-walls; again after the renderer was recreated), or none (a level without one keeps no
 * earlier level's walls). */
/* --wall-doors: the door-panel triangles open in the map now (increasing). */
static uint16_t door_applied[DOOR_OPEN_MAX];
static uint32_t door_applied_n;
static void door_log(const char *what, const uint16_t *t, uint32_t n) {
    printf(" %s", what);
    for (uint32_t i = 0; i < n; i++) printf(" %u", t[i]);
}
/* Render thread, between frames: the frame's open door panels (door_frame) into the level's wall map:
 * the triangles opened or closed since the last frame, their cells rebuilt (mat_wall_map_set_open) and
 * re-uploaded. */
static void door_take(MatPending *cur, const RLampWallMap *rm, const FrameList *l) {
    if (!l->has_doors || l->door_gen != cur->gen || !renderer_lamp_wall_map_on()) return;
    uint16_t opened[DOOR_OPEN_MAX], closed[DOOR_OPEN_MAX];
    uint32_t no = 0, nc = 0, i = 0, j = 0;
    while (i < l->door_n || j < door_applied_n) {   /* both lists increase */
        if (j >= door_applied_n || (i < l->door_n && l->door_open[i] < door_applied[j])) opened[no++] = l->door_open[i++];
        else if (i >= l->door_n || door_applied[j] < l->door_open[i]) closed[nc++] = door_applied[j++];
        else i++, j++;
    }
    if (!no && !nc) return;
    const double t0 = now_ms();
    MatWallRect a, b;
    pthread_mutex_lock(&walls_lock);
    const int ch = mat_wall_map_set_open(&cur->walls, opened, no, 1, &a) + mat_wall_map_set_open(&cur->walls, closed, nc, 0, &b);
    pthread_mutex_unlock(&walls_lock);
    if (b.x1 > b.x0) {
        if (a.x1 <= a.x0) a = b;
        else {
            if (b.x0 < a.x0) a.x0 = b.x0;
            if (b.y0 < a.y0) a.y0 = b.y0;
            if (b.x1 > a.x1) a.x1 = b.x1;
            if (b.y1 > a.y1) a.y1 = b.y1;
        }
    }
    const double t1 = now_ms();
    const int up = a.x1 > a.x0 ? renderer_lamp_wall_map_region(rm, a.x0, a.y0, a.x1, a.y1) : 0;
    const double t2 = now_ms();
    memcpy(door_applied, l->door_open, l->door_n * sizeof *door_applied);
    door_applied_n = l->door_n;
    printf("[wall-doors] %s frame %u:", cur->level, l->door_swap);
    if (no) door_log("opened triangles", opened, no);
    if (no && nc) printf(";");
    if (nc) door_log("closed triangles", closed, nc);
    if (a.x1 > a.x0)
        printf("; %d walls changed, cells x %d-%d y %d-%d (%dx%d) rebuilt in %.2f ms, uploaded in %.2f ms%s\n", ch, a.x0, a.x1 - 1, a.y0, a.y1 - 1,
               a.x1 - a.x0, a.y1 - a.y0, t1 - t0, t2 - t1, up ? " (upload failed)" : "");
    else printf("; not walls of the map, nothing to rebuild\n");
}
static void wall_take(const FrameList *l) {
    static int failed;
    mat_poll();
    MatPending *cur = mat_cur;
    if (!cur || (!renderer_lamp_shadows_on() && !light_walls_wanted() && !(volfog_walls_wanted() && renderer_volfog_on()))) return;
    const MatWallMap *w = &cur->walls;
    const RLampWallMap rm = {w->w, w->h, w->x0, w->y0, w->cell, w->z0, w->zstep, w->texels};
    if (walls_new) {
        walls_new = 0;
        door_applied_n = 0;   /* a new map: every door closed in it */
        renderer_lamp_wall_map(w->texels ? &rm : NULL);
        failed = w->texels && !renderer_lamp_wall_map_on();
        const char *tag = renderer_lamp_shadows_on() ? "lamp-shadows" : "light-walls";
        if (!w->texels) printf("[%s] %s: no wall map (no collision mesh found on the disc): shadows and lights pass walls\n", tag, cur->level);
        else
            printf("[%s] %s: wall map %dx%d (%.2f m cells, %.1f MiB), %u wall triangles, built in %.1f ms%s\n", tag, cur->level, w->w, w->h, w->cell,
                   w->w * (double)w->h * 4 / 1048576.0, w->walls, cur->ms, failed ? ": upload failed, shadows and lights pass walls" : "");
        return;
    }
    if (w->texels && !failed && !renderer_lamp_wall_map_on()) {
        renderer_lamp_wall_map(&rm);   /* the texels hold the doors applied so far */
        failed = !renderer_lamp_wall_map_on();
    }
    if (w->texels && !failed && doors_wanted()) door_take(cur, &rm, l);
}
void hle_d3d_level_loaded(const char *name) {
    mat_level(name);
    for (uint32_t i = 0; i < NR_KEYS; i++) __atomic_store_n(&nr_keys[i], 0, __ATOMIC_RELAXED);   /* --neon-reflections */
    const uint32_t g = __atomic_load_n(&level_gen, __ATOMIC_RELAXED) + 1;
    snprintf(level_req[g & 1], PERF_LEVEL_CHARS, "%s", name ? name : "");
    if (load_prof_on) {
        level_req_at[g & 1].t = now_ms();
        level_req_at[g & 1].cpu = plat_process_cpu_ms();
        load_prof_snapshot(level_req_at[g & 1].c);
    }
    __atomic_store_n(&level_gen, g, __ATOMIC_RELEASE);
}
/* --stats: the GPU time per pass (renderer_gpu_passes) of a window: 0 every GPU_PASSES_FRAMES
 * presented frames (what = the swap), 1 at each level summary (what = the level). */
#define GPU_PASSES_FRAMES 600
static void gpu_passes_line(int window, const char *what) {
    char text[1536];
    if (!hle_d3d_stats || !renderer_on) return;
    const uint32_t n = renderer_gpu_passes(window, text, sizeof text);
    if (n) printf("[gpu-passes] %s %s, %u frames, ms/frame %s\n", window ? "level" : "swap", what, n, text);
}
/* The [level-summary] of the frames since the last one (and the pending suppressed hitches). */
static struct {
    int open, area;
    char level[PERF_LEVEL_CHARS];
    double until, first, worst;
    uint32_t frames, slow, game_slow, misses;
    uint32_t lvl_slow, lvl_game_slow;   /* the whole level: frames over 25 ms; of them with the game's own frame over 20 ms */
    uint32_t regs_seen, reg_hist[16];   /* texture registrations per frame, last 16 frames */
} first_win;   /* --load-profile: the "[load] <level> first 2 s" and "area streamed" lines */
static uint32_t tp_registrations;      /* guest thread: textures registered (tp_prefetch_register) */
static void perf_summary(void) {
    char line[512];
    const uint32_t n = perf_hitch_flush(&perf_gate);
    if (n) {
        perf_suppressed_line(line, sizeof line, n);
        printf("%s\n", line);
    }
    if (perf_lvl.frames || perf_lvl.loading_frames) {
        perf_level_line(line, sizeof line, &perf_lvl, perf_level);
        printf("%s\n", line);
        if (load_prof_on)
            printf("[load] %s slow frames: %u over 25 ms, %u of them with the game's own frame over 20 ms\n", perf_level[0] ? perf_level : "?",
                   first_win.lvl_slow, first_win.lvl_game_slow);
    }
    first_win.lvl_slow = first_win.lvl_game_slow = 0;
    gpu_passes_line(1, perf_level[0] ? perf_level : "?");
    perf_level_reset(&perf_lvl);
}
/* --dynres (src/dynres.h): the controller, started at renderer start when temporal upscaling is on. */
static Dynres dynres;
static int dynres_live, dynres_out_w, dynres_out_h;
static void dynres_start(int out_w, int out_h) {
    int uw = 0, uh = 0;
    if (!dynres_options.on) return;
    if (!renderer_taa_upscale(&uw, &uh) || out_w <= 0 || out_h <= 0) {
        printf("[dynres] unavailable: temporal upscaling is not running; the render size stays fixed\n");
        return;
    }
    /* --taa-upscale native: its own auto target and ultra's floor (dynres.h) */
    const int native = taa_options.upscale == TAA_UP_NATIVE;
    dynres_init(&dynres, dynres_target_for(dynres_options.target_ms, native), dynres_floor(dynres_options.min_ratio, dynres_options.min_set, native),
                (float)uw / out_w);
    dynres_out_w = out_w;
    dynres_out_h = out_h;
    dynres_live = 1;
    int lw, lh;
    dynres_size(dynres.scale_min, out_w, out_h, uw, uh, &lw, &lh);
    printf("[dynres] dynamic resolution: GPU target %.1f ms, render %dx%d down to %dx%d\n", dynres.target_ms, uw, uh, lw, lh);
}
/* A rendered frame's GPU time (ms, < 0 none) outside loading: the next render size. */
static void dynres_frame(float gpu_ms) {
    if (!dynres_live || !dynres_update(&dynres, gpu_ms)) return;
    int rw, rh;
    dynres_size(dynres.scale, dynres_out_w, dynres_out_h, dynres_out_w, dynres_out_h, &rw, &rh);
    renderer_taa_render_size(rw, rh);   /* clamped to the starting size */
    if (!taa_options.sharpness_set) renderer_taa_sharpness(taa_auto_sharpness((float)dynres_out_w / rw));
    if (hle_d3d_stats) printf("[dynres] render %dx%d (GPU %.2f ms filtered, target %.1f)\n", rw, rh, dynres.ema_ms, dynres.target_ms);
}
/* The in-game settings menu's live changes (hle_d3d.h hle_d3d_live_*), host thread. */
static int live_out_w, live_out_h;   /* the output size temporal upscaling started with */
static float live_sharp0 = -2;       /* --taa-sharpness's value at start (the default when left out) */
static int live_upscale_size(TaaUpscale m, float ratio) {
    int rw, rh;
    taa_upscale_size_k(m == TAA_UP_CUSTOM ? ratio : taa_upscale_scale(m), live_out_w, live_out_h, &rw, &rh);
    renderer_taa_render_size(rw, rh);   /* clamped to the starting size */
    if (!taa_options.sharpness_set) renderer_taa_sharpness(taa_auto_sharpness((float)live_out_w / rw));
    return 0;
}
int hle_d3d_live_upscale_ok(void) { return live_out_w > 0 && renderer_taa_upscale(NULL, NULL); }
int hle_d3d_live_taa_ok(void) { return renderer_taa_on(); }
int hle_d3d_live_upscale_mode(const char *mode) {
    TaaUpscale m;
    float ratio = 0;
    if (!hle_d3d_live_upscale_ok() || taa_parse_upscale(mode, &m, &ratio) || m == TAA_UP_OFF) return -1;
    taa_options.upscale = m;
    if (m == TAA_UP_CUSTOM) taa_options.upscale_ratio = ratio;
    return dynres_live ? 0 : live_upscale_size(m, ratio);   /* with --dynres the controller keeps the size */
}
int hle_d3d_live_dynres(int on) {
    if (!hle_d3d_live_upscale_ok()) return -1;
    dynres_options.on = on != 0;
    if (on) {
        if (!dynres_live) dynres_start(live_out_w, live_out_h);
        return dynres_live ? 0 : -1;
    }
    if (!dynres_live) return 0;
    dynres_live = 0;
    printf("[dynres] dynamic resolution off: the upscaling mode's render size\n");
    return live_upscale_size(taa_options.upscale, taa_options.upscale_ratio);
}
void hle_d3d_live_saturation(float s) {
    hle_d3d_hdr.saturation = s;
    renderer_saturation(s);
}
void hle_d3d_live_hdr_levels(float peak, float paper) {
    hle_d3d_hdr.peak = peak;
    hle_d3d_hdr.paper = paper;
    renderer_hdr_levels(peak, paper);
}
int hle_d3d_live_taa_sharpness(float s) {
    if (!renderer_taa_on()) return -1;
    if (live_sharp0 < -1) live_sharp0 = taa_options.sharpness;
    int rw = 0;
    if (s < 0) {
        taa_options.sharpness_set = 0;
        taa_options.sharpness = live_sharp0;
        renderer_taa_sharpness(renderer_taa_upscale(&rw, NULL) && rw > 0 && live_out_w ? taa_auto_sharpness((float)live_out_w / rw) : live_sharp0);
    } else {
        taa_options.sharpness_set = 1;
        taa_options.sharpness = s;
        renderer_taa_sharpness(s);
    }
    return 0;
}
/* --bench (src/bench.h): waits for the level's first frame, plays BENCH_WARMUP_FRAMES, pans the
 * camera for BENCH_FRAMES while recording each frame's time, GPU time and render size, then writes
 * the report and ends the run (the frame limit). A load during the window starts it again. */
#ifdef XBRUN_VERSION
#define XBRUN_VERSION_TEXT XBRUN_VERSION
#else
#define XBRUN_VERSION_TEXT "unknown"
#endif
static struct {
    int phase;   /* 0 waiting for a level, 1 warm-up, 2 measuring, 3 done */
    uint32_t count, total;
    float *ms, *gpu;
    int wmin, wmax, hmin, hmax;
    double wsum;
} bench;
static void bench_finish(const BenchSummary *sum, const char *failure, uint32_t seq) {
    static char passes[1536], text[4096];
    int ow = 0, oh = 0;
    input_set_auto_stick(0, 0);
    bench.phase = 3;
    if (renderer_on) {
        renderer_output_size(&ow, &oh);
        if (!renderer_gpu_passes(1, passes, sizeof passes)) passes[0] = 0;
    }
    const BenchReport r = {XBRUN_VERSION_TEXT, renderer_on ? renderer_device_name() : "none", bench_options.settings, passes, perf_level, ow, oh,
                           bench.wmin, bench.wmax, bench.hmin, bench.hmax, bench.count ? bench.wsum / bench.count : 0, dynres_live, sum,
                           failure};
    bench_report(text, sizeof text, &r);
    const char *path = bench_options.out ? bench_options.out : "bench-report.txt";
    FILE *fp = fopen(path, "w");
    const int wrote = fp && fputs(text, fp) >= 0;
    if (fp && fclose(fp)) printf("[bench] could not finish writing %s\n", path);
    for (char *line = text, *nl; *line; line = nl + 1) {
        nl = strchr(line, '\n');
        if (!nl) break;
        printf("[bench] %.*s\n", (int)(nl - line), line);
    }
    printf(wrote ? "[bench] report written to %s\n" : "[bench] ERROR: could not write the report to %s\n", path);
    __atomic_store_n(&hle_d3d_frame_limit, (int)seq + 2, __ATOMIC_RELEASE);   /* the run ends after this frame */
}
static void bench_frame(double frame_ms, float gpu_ms, int loading, uint32_t seq) {
    if (!bench_options.on || bench.phase == 3) return;
    if (!bench.ms) {
        bench.ms = calloc(BENCH_FRAMES, sizeof *bench.ms);
        bench.gpu = calloc(BENCH_FRAMES, sizeof *bench.gpu);
        if (!bench.ms || !bench.gpu) {
            bench_finish(NULL, "out of memory", seq);
            return;
        }
    }
    if (bench.phase < 2 && ++bench.total > BENCH_TIMEOUT_FRAMES) {
        bench_finish(NULL, "the level did not start (menus not passed or warp failed)", seq);
        return;
    }
    if (loading) {   /* waiting, or a load in the window: start again */
        if (bench.phase) input_set_auto_stick(0, 0);
        bench.phase = bench.phase ? 1 : 0;
        bench.count = 0;
        return;
    }
    if (bench.phase == 0) {
        if (!perf_level[0]) return;   /* the title screen and menus: no level requested yet */
        bench.phase = 1;
    }
    if (bench.phase == 1) {
        if (++bench.count < BENCH_WARMUP_FRAMES) return;
        bench.phase = 2;
        bench.count = 0;
        bench.wmin = bench.hmin = 1 << 30;
        bench.wmax = bench.hmax = 0;
        bench.wsum = 0;
        char drop[64];
        if (renderer_on) renderer_gpu_passes(1, drop, sizeof drop);   /* the pass window starts here */
        input_set_auto_stick(BENCH_PAN_STICK, 0);
        printf("[bench] measuring %d frames from frame %u (%s)\n", BENCH_FRAMES, seq, perf_level);
        return;
    }
    int rw = 0, rh = 0;
    if (!renderer_on || !renderer_taa_upscale(&rw, &rh)) {
        if (renderer_on) renderer_fsr(&rw, &rh);
    }
    if (rw < bench.wmin) bench.wmin = rw;
    if (rw > bench.wmax) bench.wmax = rw;
    if (rh < bench.hmin) bench.hmin = rh;
    if (rh > bench.hmax) bench.hmax = rh;
    bench.wsum += rw;
    bench.ms[bench.count] = (float)frame_ms;
    bench.gpu[bench.count] = gpu_ms;
    if (++bench.count < BENCH_FRAMES) return;
    BenchSummary sum;
    bench_summarize(bench.ms, bench.gpu, bench.count, &sum);
    bench_finish(&sum, NULL, seq);
}
/* One presented frame: render = host replay and present time (ms). A few clock reads, one
 * process-CPU read and, once a second, the driver's memory numbers; a line only for a hitch. */
static void perf_frame(double render, const FrameTimes *ft, uint32_t draws, uint32_t movie, uint32_t seq) {
    static double last, cpu_last, vram_next;
    static uint32_t seen_gen;
    const double t = now_ms(), cpu = plat_process_cpu_ms();
    if (!last) {   /* the first frame has no interval */
        last = t;
        cpu_last = cpu;
        return;
    }
    const uint32_t gen = __atomic_load_n(&level_gen, __ATOMIC_ACQUIRE);
    const int changed = gen != seen_gen;
    if (changed) {   /* a level load: the previous level's summary */
        perf_summary();
        snprintf(perf_level, sizeof perf_level, "%s", level_req[gen & 1]);
        if (load_prof_on) load_window_begin(&load_win, level_req[gen & 1], level_req_at[gen & 1].t, level_req_at[gen & 1].cpu, level_req_at[gen & 1].c);
        seen_gen = gen;
    }
    PerfFrame f = {t - last, ft->g_ms, ft->hle_ms, render, hstat_end.gpu_busy_ms, hstat_end.upload_ms, hstat_end.pipeline_ms, cpu - cpu_last, 0,
                   ft->pace_ms, hstat_valid ? hstat_end.gpu_ms + hstat_end.present_ms : -1};
    /* Loading: a frame without draws (the CPU path, black screens) or with a movie quad, the frame
     * that saw the load request, and frames while the load is pending. */
    f.loading = !draws || movie || changed || (hle_d3d_level_pending && hle_d3d_level_pending());
    if (!f.loading) dynres_frame(hstat_end.gpu_busy_ms);
    bench_frame(f.total, hstat_end.gpu_busy_ms, f.loading, seq);
    last = t;
    cpu_last = cpu;
    if (load_win.open) {   /* --load-profile: the level's first frame closes the window */
        uint64_t c[LOAD_COUNTERS][3];
        char line[1024];
        load_prof_snapshot(c);
        const LoadFrame lf = {t, cpu, f.total, ft->g_ms, hstat_end.upload_ms, load_pack_ms, hstat_end.pipeline_ms, draws, movie != 0};
        if (load_window_frame(&load_win, &lf, c, line, sizeof line)) {
            printf("%s\n", line);
            snprintf(first_win.level, sizeof first_win.level, "%s", perf_level);
            first_win.open = 1;   /* the level's first frame: its first 2 s follow */
            first_win.area = 0;
            first_win.until = t + 2000;
            first_win.first = f.total;
            first_win.frames = first_win.slow = first_win.game_slow = first_win.misses = 0;
            first_win.worst = 0;
        }
    }
    if (load_prof_on) {   /* an area streaming in within a level (64+ registrations in 16 frames): its 2 s too */
        const uint32_t regs = __atomic_load_n(&tp_registrations, __ATOMIC_RELAXED), now = regs - first_win.regs_seen;
        uint32_t recent = now;
        first_win.regs_seen = regs;
        for (int i = 15; i > 0; i--) recent += first_win.reg_hist[i] = first_win.reg_hist[i - 1];
        first_win.reg_hist[0] = now;
        if (!first_win.open && !load_win.open && !f.loading && perf_level[0] && recent >= 64) {
            snprintf(first_win.level, sizeof first_win.level, "%s", perf_level);
            first_win.open = first_win.area = 1;
            first_win.until = t + 2000;
            first_win.first = f.total;
            first_win.frames = first_win.slow = first_win.game_slow = first_win.misses = 0;
            first_win.worst = 0;
        }
        if (!f.loading && f.total > 25) {
            first_win.lvl_slow++;
            first_win.lvl_game_slow += ft->g_ms > 20;
        }
    }
    if (first_win.open) {   /* --load-profile: the first 2 s of the level from its first frame, or of an area */
        first_win.frames++;
        first_win.slow += f.total > 25;
        first_win.game_slow += f.total > 25 && ft->g_ms > 20;
        first_win.misses += perf_tex_misses;
        if (f.total > first_win.worst) first_win.worst = f.total;
        if (t >= first_win.until || changed) {
            first_win.open = 0;
            if (first_win.area)
                printf("[load] %s area streamed: next 2 s %u frames, %u over 25 ms (%u game-side, worst %.1f ms), %u draws with a pack texture not "
                       "loaded yet, %u textures cached (%llu MiB)\n", first_win.level, first_win.frames, first_win.slow, first_win.game_slow,
                       first_win.worst, first_win.misses, renderer_on ? renderer_texture_count() : 0,
                       renderer_on ? (unsigned long long)(renderer_texture_bytes() >> 20) : 0ull);
            else
                printf("[load] %s first 2 s: first frame %.1f ms, %u frames, %u over 25 ms (%u game-side, worst %.1f ms), %u draws with a pack "
                       "texture not loaded yet, %u textures cached (%llu MiB)\n", first_win.level, first_win.first, first_win.frames,
                       first_win.slow, first_win.game_slow, first_win.worst, first_win.misses, renderer_on ? renderer_texture_count() : 0,
                       renderer_on ? (unsigned long long)(renderer_texture_bytes() >> 20) : 0ull);
        }
    }
    load_pack_ms = 0;
    if (perf_is_hitch(f.total) && !movie) {   /* movies run at their own rate: no line, not counted */
        uint32_t flush;
        char line[512];
        const int print = perf_hitch_gate(&perf_gate, t, &flush);
        if (flush) {
            perf_suppressed_line(line, sizeof line, flush);
            printf("%s\n", line);
        }
        if (print) {
            perf_hitch_line(line, sizeof line, seq, &f, perf_level);
            printf("%s\n", line);
        }
    }
    perf_level_frame(&perf_lvl, &f);
    perf_level_tex_misses(&perf_lvl, perf_tex_misses, tp.pack);
    perf_tex_misses = 0;
    if (renderer_on && t >= vram_next) {   /* device-local memory in use, once a second */
        uint64_t budget, usage, now[RENDERER_VRAM_CATS], sum = 0;
        vram_next = t + 1000;
        if (renderer_memory_budget(&budget, &usage, NULL)) {
            perf_level_vram(&perf_lvl, usage);
        } else {
            renderer_vram(now, NULL);
            for (int c = 0; c < RENDERER_VRAM_CATS; c++) sum += now[c];
            perf_level_vram(&perf_lvl, sum);
        }
    }
}

static int presenting;   /* the renderer presents to the window (renderer_present) */

/* --stats: the renderer's device-local memory at exit and its peak, per category (renderer.h
 * renderer_vram), the numbers the launcher's video-memory meter is calibrated against. */
static void vram_report(void) {
    static const char *const names[RENDERER_VRAM_CATS] = {"scene", "surfaces", "textures", "effects", "skin", "other"};
    uint64_t now[RENDERER_VRAM_CATS], peak[RENDERER_VRAM_CATS], total = 0;
    uint64_t peak_total = renderer_vram(now, peak);
    char line[512];
    int n = 0;
    for (int c = 0; c < RENDERER_VRAM_CATS; c++) {
        total += now[c];
        n += snprintf(line + n, sizeof line - (size_t)n, "%s %.1f (peak %.1f)%s", names[c], now[c] / 1048576.0, peak[c] / 1048576.0,
                      c + 1 < RENDERER_VRAM_CATS ? ", " : "");
    }
    printf("[vram] device-local MiB: total %.1f (peak %.1f); %s; swapchain %.1f\n", total / 1048576.0, peak_total / 1048576.0, line,
           renderer_swapchain_bytes() / 1048576.0);
    uint64_t budget, usage;
    int heap;
    if (renderer_memory_budget(&budget, &usage, &heap))
        printf("[vram] driver (VK_EXT_memory_budget, heap %d): process usage %.1f MiB of a %.1f MiB budget (%.0f%%)\n", heap, usage / 1048576.0,
               budget / 1048576.0, 100.0 * usage / budget);
    else
        printf("[vram] driver: no memory budget (VK_EXT_memory_budget unavailable)\n");
}
static uint32_t readback_failures;   /* readback frames render_frame failed on (host thread); the CPU frame was shown */
static int quit_code = -1;   /* hle_d3d_request_quit: the exit code, -1 = none */
void hle_d3d_request_quit(int exit_code) {
    int none = -1;
    __atomic_compare_exchange_n(&quit_code, &none, exit_code < 0 ? 0 : exit_code, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
/* VK_ERROR_DEVICE_LOST (renderer_device_lost, in the render64 helper too): the GPU hung or its driver
 * reset it (a TDR on Windows). Every later frame fails, so the picture would stay frozen while the game
 * and its sound play on. The host thread then closes the game (HLE_D3D_EXIT_DEVICE_LOST), said in the
 * log and, on Windows, in a message box once the window is closed (both languages, as
 * black_frames_warning; none for headless runs or with XBRUN_NO_DIALOGS). */
static int device_lost_seen;
static void device_lost_box(void) {
#ifdef _WIN32
    if (!strcmp(hle_d3d_video, "null") || hle_d3d_dump_path || getenv("XBRUN_NO_DIALOGS")) return;
    typedef int(__stdcall * MessageBoxWFn)(void *, const wchar_t *, const wchar_t *, unsigned);
    void *user32 = plat_dlopen("user32.dll");
    MessageBoxWFn box = user32 ? (MessageBoxWFn)plat_dlsym(user32, "MessageBoxW") : NULL;
    if (box)   /* MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST */
        box(NULL,
            L"La tarjeta gr\u00e1fica dej\u00f3 de responder (el controlador la reinici\u00f3), as\u00ed que la imagen no "
            L"pod\u00eda volver y el juego se cerr\u00f3. Se pierde lo jugado desde el \u00faltimo guardado. Vuelve a "
            L"abrir el juego; si se repite, actualiza el controlador de la tarjeta gr\u00e1fica o baja la calidad "
            L"gr\u00e1fica. Detalles en xbrun.log.\n\n"
            L"The graphics card stopped responding (its driver reset it), so the picture could not come back and the "
            L"game closed. Progress since the last save is lost. Start the game again; if it happens again, update "
            L"the graphics card driver or lower the graphics quality. Details in xbrun.log.",
            L"The Warriors", 0x10 | 0x10000 | 0x40000);
#endif
}
static void device_lost_check(void);

static void finish(const char *why) {
    printf("[d3d] exit: %s\n", why);
    if (hle_d3d_on_exit) hle_d3d_on_exit();
    if (renderer_on) renderer_pipeline_cache_save();   /* also without renderer_shutdown (offscreen) */
    if (presenting) renderer_shutdown();   /* release the swapchain before its window */
    pipeline_lines();
    if (hle_d3d_dump_path) {
        int w = 0, h = 0;
        uint32_t *px = hle_d3d_last_frame(&w, &h);
        if (px && !video_write_bmp(hle_d3d_dump_path, px, w, h, w * 4))
            printf("[d3d] frame %u written to %s (%dx%d)\n", hle_d3d_frame_count(), hle_d3d_dump_path, w, h);
        else
            printf("[d3d] no frame written to %s\n", hle_d3d_dump_path);
        free(px);
    }
    perf_summary();
    hle_d3d_summary();
    dlog_exit_summary();
    if (renderer_on)
        printf("[renderer] %u movie planes copied, %u texture cache drops\n", renderer_movie_upload_count(), renderer_texture_drop_count());
    if (__atomic_load_n(&readback_failures, __ATOMIC_RELAXED))
        printf("[renderer] %u readback frames failed to render (the CPU back buffer was shown instead)\n",
               __atomic_load_n(&readback_failures, __ATOMIC_RELAXED));
    if (renderer_on && hle_d3d_stats) vram_report();
    if (tp.dump) printf("[texpack] %d textures listed in %s/manifest.csv\n", texpack_dump_close(), hle_d3d_dump_textures);
    sharp_faces_summary();
    neon_summary();
    relief_summary();
    tp_summary();
    render_rec_exit();
    if (hle_d3d_stats || load_prof_on) plat_report_memory("exit");
    video_close();
    plat_flush_output();
    if (device_lost_seen) device_lost_box();   /* the window (and the render64 helper's) is closed by now */
    const int code = __atomic_load_n(&quit_code, __ATOMIC_ACQUIRE);
    _exit(code > 0 ? code : 0);   /* guest threads are still running; skip atexit handlers of the host libraries */
}

/* Host main thread: creates the renderer next to the video output. --renderer none (or a failed
 * initialization) keeps the CPU-only path. */
static int renderer_on, out_w, out_h;   /* renderer target size (renderer_output_size) */
/* After a failed frame (host thread): a lost device closes the game (device_lost_box). */
static void device_lost_check(void) {
    if (device_lost_seen || !renderer_on || !renderer_device_lost()) return;
    device_lost_seen = 1;
    printf("[renderer] FATAL: the GPU stopped responding (Vulkan device lost: %s); the picture cannot come back on this "
           "device, so the game closes (exit code %d) instead of playing on behind a frozen picture. Progress since the last "
           "save is lost. A GPU hang the driver reset (a TDR on Windows), an overheating or unstable GPU or a driver fault; "
           "update the graphics driver or lower the graphics preset if it happens again\n",
           renderer_error(), HLE_D3D_EXIT_DEVICE_LOST);
    plat_flush_output();
    hle_d3d_request_quit(HLE_D3D_EXIT_DEVICE_LOST);
}
static int present_mode(void) {
    return !strcmp(hle_d3d_present, "fifo") ? RENDERER_PRESENT_FIFO : !strcmp(hle_d3d_present, "immediate") ? RENDERER_PRESENT_IMMEDIATE
           : !strcmp(hle_d3d_present, "mailbox")                    ? RENDERER_PRESENT_MAILBOX
                                                                    : RENDERER_PRESENT_AUTO;
}
/* After video_open: the renderer, presenting to the window when the video backend made a Vulkan
 * window, else offscreen; logs the present path. */
/* Vulkan did not start although it was asked for (not --renderer none): only the Bink movies are
 * drawn without it (draw_movie's CPU path), so the menus and the game stay black while their sound
 * plays. Said once in the log and, on Windows, in a message box before the game carries on; both
 * languages, since the runtime has no UI language of its own (--language is the title's). No box
 * for headless runs (--video null, --dump-frame) or with XBRUN_NO_DIALOGS set. */
static void black_frames_warning(const char *why) {
    printf("[renderer] ERROR: 3D frames will be black: %s\n", why && *why ? why : "unknown reason");
    plat_flush_output();
#ifdef _WIN32
    if (!strcmp(hle_d3d_video, "null") || hle_d3d_dump_path || getenv("XBRUN_NO_DIALOGS")) return;
    typedef int(__stdcall * MessageBoxWFn)(void *, const wchar_t *, const wchar_t *, unsigned);
    void *user32 = plat_dlopen("user32.dll");
    MessageBoxWFn box = user32 ? (MessageBoxWFn)plat_dlsym(user32, "MessageBoxW") : NULL;
    /* UTF-16 text (wchar_t on Windows); MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST, so it shows over the game's window */
    if (box)
        box(NULL,
            L"El juego no pudo usar Vulkan en tu tarjeta gr\u00e1fica, as\u00ed que el men\u00fa y el juego se ver\u00e1n "
            L"en negro (los videos y el sonido s\u00ed funcionan). Actualiza el controlador de tu tarjeta gr\u00e1fica desde la "
            L"p\u00e1gina de NVIDIA, AMD o Intel.\n\n"
            L"The game could not use Vulkan on your graphics card, so the menu and the game will look black (movies and sound "
            L"still work). Update your graphics card driver from the NVIDIA, AMD or Intel website.",
            L"The Warriors", 0x30 | 0x10000 | 0x40000);
#endif
}

extern const int *hle_d3d_photo_prepare_hook;
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
    /* --filmic: the shader parameters and the grade LUT (a --filmic-lut .cube, else the procedural
     * one), kept for renderer_init (which copies the LUT). */
    static RFilmic filmic;
    static uint8_t *filmic_lut;
    const char *grade = "procedural 70s grade";
    if (hle_d3d_filmic.on) {
        filmic_params(hle_d3d_filmic.strength, hle_d3d_filmic.grain, hle_d3d_filmic.vignette, &filmic);
        int n = 0;
        free(filmic_lut);
        filmic_lut = NULL;
        if (hle_d3d_filmic.lut_path) {
            char err[192];
            filmic_lut = filmic_cube_load(hle_d3d_filmic.lut_path, &n, err, sizeof err);
            if (filmic_lut) grade = hle_d3d_filmic.lut_path;
            else printf("[renderer] --filmic-lut: %s; using the procedural grade\n", err);
        }
        if (!filmic_lut && (filmic_lut = malloc((size_t)FILMIC_LUT_SIZE * FILMIC_LUT_SIZE * FILMIC_LUT_SIZE * 4))) {
            n = FILMIC_LUT_SIZE;
            filmic_lut_build(n, filmic_lut);
        }
        filmic.lut = filmic_lut;
        filmic.lut_size = n;
    }
    static RAutoExposure auto_exposure;
    static RDof dof;
    if (hle_d3d_dof.on) dof_params(hle_d3d_dof.strength, &dof);
    if (hle_d3d_auto_exposure.on) auto_exposure_params(hle_d3d_auto_exposure.strength, &auto_exposure);
    RendererConfig c = {.samples = hle_d3d_msaa > 1 ? hle_d3d_msaa : 1, .aniso = hle_d3d_aniso, .present_mode = present_mode(),
                        .frames_in_flight = hle_d3d_frames_in_flight, .supersample = hle_d3d_supersample,
                        .shadow_scale = hle_d3d_shadow_scale, .shadow_soft = hle_d3d_shadow_soft, .sharpen = hle_d3d_sharpen,
                        .glass = hle_d3d_glass.on || (hle_d3d_glass.hd && hle_d3d_glass.hd_frost > 0), .bloom = hle_d3d_bloom.on,
                        .bloom_strength = hle_d3d_bloom.strength, .bloom_threshold = hle_d3d_bloom.threshold,
                        .shadow_range = hle_d3d_shadow_range.on ? hle_d3d_shadow_range.dist : 0, .rain = hle_d3d_rain.on,
                        .rain_wet = hle_d3d_rain.on && hle_d3d_rain.wet, .rain_cover = hle_d3d_rain.on && hle_d3d_rain.cover, .damp = hle_d3d_damp.on, .sssr = sssr_renderer_steps(&hle_d3d_sssr),
                        .sssr_rays = sssr_rays(hle_d3d_sssr.quality), .sssr_glass = sssr_renderer_glass(&hle_d3d_sssr) && hle_d3d_normal_maps.on && hle_d3d_normal_maps.dry, .sssr_full = hle_d3d_sssr.full,
                        .ssao = hle_d3d_ssao.on, .ssao_strength = hle_d3d_ssao.strength, .ssao_radius = hle_d3d_ssao.radius,
                        .ssao_mode = hle_d3d_ssao.mode, .gtao_quality = hle_d3d_ssao.quality,
                        .ssgi = hle_d3d_ssao.on && hle_d3d_ssao.mode == SSAO_MODE_GTAO ? hle_d3d_ssao.ssgi : 0, .ssgi_strength = hle_d3d_ssao.ssgi_strength,
                        .soft = hle_d3d_soft.on, .filmic = hle_d3d_filmic.on && filmic_lut ? &filmic : NULL,
                        .auto_exposure = hle_d3d_auto_exposure.on ? &auto_exposure : NULL, .dof = hle_d3d_dof.on ? &dof : NULL,
                        .sky = hle_d3d_sky.on, .lamp = hle_d3d_lamp.on, .lamp_shadow = hle_d3d_lamp_shadow.on, .light_walls = light_walls_wanted(),
                        .world_shadow = hle_d3d_world_shadow.on ? hle_d3d_world_shadow.size : 0, .contact = hle_d3d_contact.on,
                        .shafts = hle_d3d_shafts.on && !hle_d3d_volfog.on, .shafts_cheap = hle_d3d_shafts.cheap,
                        .volfog = hle_d3d_volfog.on, .volfog_quality = hle_d3d_volfog.quality,
                        .char_light = hle_d3d_char_light.on, .hair = hle_d3d_hair.on && hle_d3d_char_light.on ? hle_d3d_hair.strength : 0,
                        .skin = hle_d3d_skin.on && hle_d3d_char_light.on ? hle_d3d_skin.strength : 0,
                        .eyes = hle_d3d_eyes.on && hle_d3d_char_light.on ? hle_d3d_eyes.strength : 0,
                        .cloth = hle_d3d_cloth.on && hle_d3d_char_light.on ? hle_d3d_cloth.strength : 0,
                        .gpu_skin = hle_d3d_gpu_skin.on, .world_light = hle_d3d_world_light.on,
                        .direct_geometry = hle_d3d_direct_geometry, .fps_overlay = hle_d3d_fps_overlay,
                        .fps_overlay_corner = hle_d3d_fps_overlay_corner,
                        .fps_overlay_gpu = hle_d3d_fps_overlay && (hle_d3d_fps_overlay_items & FPS_ITEM_GPU),
                        .pipeline_cache_dir = hle_d3d_pipeline_cache ? hle_d3d_pipeline_cache_dir : NULL,
                        .no_pipeline_cache = !hle_d3d_pipeline_cache, .pipeline_stats = hle_d3d_stats, .gpu_timestamps = 1,
                        .pipeline_async = hle_d3d_pipeline_async, .no_dynamic_state = !hle_d3d_dynamic_state,
                        .pass_stats = hle_d3d_stats || bench_options.on, .texture_streaming = hle_d3d_level_prefetch,
                        .hdr = hle_d3d_hdr.mode, .hdr_peak = hle_d3d_hdr.peak, .hdr_paper = hle_d3d_hdr.paper,
                        .saturation = hle_d3d_hdr.saturation};
    hle_d3d_output_size((int)w, (int)h, &c.out_w, &c.out_h);
    if (hle_d3d_photo_prepare_hook && *hle_d3d_photo_prepare_hook) {   /* photo mode (hle_photo.h --photo-prepare): the passes it turns on */
        const char *made = "";
        if (!c.ssao) c.ssao = 1, c.ssao_mode = SSAO_MODE_GTAO, c.gtao_quality = GTAO_QUALITY_HIGH, c.ssgi = SSGI_HIGH, made = " ambient occlusion + bounce light,";
        else if (c.ssao_mode == SSAO_MODE_GTAO && !c.ssgi) c.ssgi = SSGI_HIGH;
        if (!c.contact) c.contact = 1;
        if (!c.volfog) c.volfog = 1, c.volfog_quality = VOLFOG_HIGH;
        if (!c.world_shadow) c.world_shadow = 4096;
        printf("[photo] --photo-prepare: renderer passes built for photo mode (off in play):%s contact shadows, volumetric fog, sun shadows\n", made);
    }
    if (taa_options.upscaler == TAA_UPSCALER_FSR3 && taa_options.upscale == TAA_UP_OFF && !dynres_options.on) {   /* --upscaler fsr3 upscales */
        taa_options.upscale = TAA_UP_QUALITY;
        printf("[taa] --taa-upscale quality (--upscaler fsr3 rebuilds the output from a smaller render size)\n");
    }
    if (dynres_options.on && taa_options.upscale == TAA_UP_OFF) {   /* --dynres: the render size it varies is temporal upscaling's */
        taa_options.upscale = TAA_UP_ULTRA;
        printf("[dynres] --taa-upscale ultra (dynamic resolution varies the temporal upscaling's render size)\n");
    }
    if (taa_options.upscale != TAA_UP_OFF) {   /* --taa-upscale: the scene below the output size, rebuilt by the TAA resolve */
        if (fsr_options.mode != FSR_OFF) printf("[taa] warning: --fsr %s ignored: --taa-upscale upscales instead\n", fsr_mode_name(fsr_options.mode));
        if (c.supersample > 1) printf("[taa] warning: --supersample %d ignored: --taa-upscale renders below the output size instead\n", c.supersample);
        fsr_options.mode = FSR_OFF;
        c.supersample = 0;
        taa_options.on = 1;
        taa_upscale_size_k(taa_options.upscale == TAA_UP_CUSTOM ? taa_options.upscale_ratio : taa_upscale_scale(taa_options.upscale), c.out_w,
                           c.out_h, &c.taa_up_w, &c.taa_up_h);
    }
    if (fsr_options.mode != FSR_OFF) {   /* --fsr: the scene below the output size, then EASU + RCAS */
        const int dropped = fsr_resolve(fsr_options.mode, &c.supersample, &c.sharpen);
        if (dropped & FSR_DROPPED_SUPERSAMPLE)
            printf("[fsr] warning: --supersample %d ignored: --fsr renders below the output size instead\n", hle_d3d_supersample);
        if (dropped & FSR_DROPPED_SHARPEN)
            printf("[fsr] warning: --sharpen %d ignored: RCAS sharpens instead (--fsr-sharpness)\n", hle_d3d_sharpen);
        fsr_render_size(fsr_options.mode, c.out_w, c.out_h, &c.fsr_w, &c.fsr_h);
        c.fsr_sharpness = fsr_options.sharpness;
    }
    c.taa = taa_options.on;   /* --taa */
    c.taa_upscaler = taa_options.upscaler == TAA_UPSCALER_FSR3 && c.taa_up_w > 0 ? (taa_options.fsr3_masks_off ? 2 : 1) : 0;   /* --upscaler fsr3, --fsr3-masks */
    c.taa_lite = (int)taa_options.lite;   /* --taa-lite */
    c.vrs = hle_d3d_vrs;   /* --vrs */
    /* --taa-sharpness left out: more sharpening the lower the render size (taa_auto_sharpness) */
    c.taa_sharpness = taa_options.sharpness_set || c.taa_up_w <= 0 ? taa_options.sharpness : taa_auto_sharpness((float)c.out_w / c.taa_up_w);
    c.frame_gen = framegen_options.on ? (framegen_options.of ? 2 : 1) : 0;   /* --frame-gen (2: --frame-gen-of) */
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
        black_frames_warning(renderer_error());
        return;
    }
    renderer_on = 1;
    renderer_output_size(&out_w, &out_h);
    printf("[renderer] Vulkan on %s, %dx%d (back buffer %ux%u), %d samples, anisotropy %.0f\n", renderer_device_name(), out_w, out_h, w,
           h, renderer_samples(), renderer_anisotropy());
    printf("[renderer] %s\n", renderer_vk_caps());   /* Vulkan version, loader, device type, features */
    if (renderer_supersample() > 1 || hle_d3d_shadow_scale > 1 || hle_d3d_shadow_soft)
        printf("[renderer] supersample %dx (internal %dx%d), shadow maps %dx, soft shadows %s\n", renderer_supersample(),
               out_w * renderer_supersample(), out_h * renderer_supersample(), hle_d3d_shadow_scale > 1 ? hle_d3d_shadow_scale : 1,
               hle_d3d_shadow_soft ? "on" : "off");
    if (hle_d3d_shadow_range.on) printf("[renderer] shadow range %.2f (character shadows fade out from %.2f to %.2f below the caster)\n",
                                        hle_d3d_shadow_range.dist, hle_d3d_shadow_range.dist, 1.5f * hle_d3d_shadow_range.dist);
    if (fsr_options.mode != FSR_OFF) {
        int rw = 0, rh = 0;
        if (renderer_fsr(&rw, &rh))
            printf("[fsr] mode=%s render=%dx%d output=%dx%d sharpness=%.2f\n", fsr_mode_name(fsr_options.mode), rw, rh, out_w, out_h,
                   c.fsr_sharpness);
        else
            printf("[fsr] mode=%s unavailable at this output size or on this device; rendering at %dx%d without upscaling\n",
                   fsr_mode_name(fsr_options.mode), out_w, out_h);
    }
    if (c.taa) {
        int uw = 0, uh = 0;
        if (renderer_taa_on() && renderer_taa_upscale(&uw, &uh))
            printf("[taa] temporal %s %s (%s): render %dx%d, output %dx%d (%d samples, sharpness %.2f, texture LOD bias %.2f, anisotropy %.0f)\n",
                   taa_options.upscale == TAA_UP_NATIVE ? "anti-aliasing" : "upscaling", taa_upscale_name(taa_options.upscale), renderer_taa_fsr3() ? "AMD FSR 3.1" : "TAA", uw, uh, out_w, out_h, renderer_samples(), c.taa_sharpness, renderer_texture_lod_bias(),
                   renderer_anisotropy());
        else if (renderer_taa_on()) printf("[taa] temporal anti-aliasing on (%d samples, sharpness %.2f)\n", renderer_samples(), c.taa_sharpness);
        else printf("[taa] temporal anti-aliasing unavailable on this device (the depth target cannot be sampled); frames are not jittered\n");
        live_out_w = out_w, live_out_h = out_h;   /* the settings menu's live upscaling changes */
        dynres_start(out_w, out_h);
    }
    if (c.frame_gen && !renderer_frame_gen_on())
        printf("[frame-gen] unavailable on this device (no sampled depth or storage image atomics); frames are shown as rendered\n");
    if (c.sharpen > 0) {
        if (renderer_sharpen()) printf("[renderer] sharpening (contrast-adaptive) %d\n", renderer_sharpen());
        else printf("[renderer] sharpening unavailable on this device; frames are not sharpened\n");
    }
    if (hle_d3d_fps_overlay) {
        if (renderer_fps_overlay_on()) {
            const int items = hle_d3d_fps_overlay_items ? hle_d3d_fps_overlay_items : FPS_ITEMS_DEFAULT;
            char what[64] = "";
            if (items != FPS_ITEMS_DEFAULT) {   /* --fps-overlay alone: the line as before */
                static const char *names[] = {"fps", "ms", "cpu", "gpu"};
                strcpy(what, ":");
                for (int i = 0; i < 4; i++)
                    if (items & 1 << i) strcat(strcat(what, " "), names[i]);
            }
            double busy;
            int frames;
            if ((items & FPS_ITEM_GPU) && !renderer_fps_overlay_gpu(&busy, &frames)) strcat(what, " (no GPU timestamps on this queue: GPU --)");
            printf("[renderer] fps overlay on (%s corner)%s\n", (const char *[]){"top left", "top right", "bottom left", "bottom right"}[hle_d3d_fps_overlay_corner & 3], what);
        }
        else printf("[renderer] fps overlay unavailable on this device\n");
    }
    if (hle_d3d_bloom.on) {
        if (renderer_bloom_on()) printf("[renderer] bloom on (strength %.2f, threshold %.2f)\n", hle_d3d_bloom.strength, hle_d3d_bloom.threshold);
        else printf("[renderer] bloom unavailable on this device; frames are drawn without it\n");
    }
    if (hle_d3d_filmic.on) {
        if (renderer_filmic_on())
            printf("[renderer] filmic look on (strength %.2f, grain %.2f, vignette %.2f, %s; bloom %s)\n", hle_d3d_filmic.strength,
                   hle_d3d_filmic.grain, hle_d3d_filmic.vignette, grade, renderer_bloom_on() ? "6 levels, halation" : "off: no halation");
        else printf("[renderer] filmic look unavailable on this device; frames are drawn without it\n");
    }
    if (hle_d3d_hdr.saturation != 1.0f) printf("[renderer] saturation %.2f in the final grade\n", hle_d3d_hdr.saturation);
    if (hle_d3d_hdr.mode != HDR_OFF && !c.create_surface)
        printf("[hdr] --hdr %s: SDR output (no window swapchain: readback or offscreen present)\n", hdr_mode_name(hle_d3d_hdr.mode));
    if (hle_d3d_auto_exposure.on) {
        if (renderer_auto_exposure_on())
            printf("[renderer] auto exposure on (strength %.2f: %+.1f to %+.1f stop, %s)\n", hle_d3d_auto_exposure.strength, auto_exposure.ev_min,
                   auto_exposure.ev_max, renderer_filmic_on() && hle_d3d_filmic.on ? "in the filmic pass" : "exposure-only pass");
        else printf("[renderer] auto exposure unavailable on this device; frames are drawn without it\n");
    }
    if (hle_d3d_dof.on) {
        if (renderer_dof_on())
            printf("[renderer] cutscene depth of field on (strength %.2f: far blur %.1f%% of the height, %s)\n", hle_d3d_dof.strength,
                   100 * dof.coc, renderer_filmic_on() && (hle_d3d_filmic.on || hle_d3d_auto_exposure.on) ? "in the filmic pass" : "own pass");
        else printf("[renderer] cutscene depth of field unavailable (the depth cannot be read): off\n");
    }
    if (hle_d3d_contact.on) {
        if (renderer_contact_on()) printf("[renderer] contact shadows on (strength %.2f)\n", hle_d3d_contact.strength);
        else printf("[renderer] contact shadows unavailable (the depth or the character mask cannot be read): off\n");
    }
    if (hle_d3d_ssao.on) {
        static const char *const gq[] = {"low", "medium", "high"};
        if (renderer_ssao_gtao_on())
            printf("[renderer] ambient occlusion on (XeGTAO %s, strength %.2f, radius %.2f)\n", gq[hle_d3d_ssao.quality % 3], hle_d3d_ssao.strength,
                   hle_d3d_ssao.radius);
        else if (renderer_ssao_on()) {
            if (hle_d3d_ssao.mode == SSAO_MODE_GTAO) printf("[renderer] XeGTAO unavailable on this device (compute or storage formats): classic occlusion\n");
            printf("[renderer] ambient occlusion on (strength %.2f, radius %.2f)\n", hle_d3d_ssao.strength, hle_d3d_ssao.radius);
        } else printf("[renderer] ambient occlusion unavailable on this device; frames are drawn without it\n");
    }
    if (hle_d3d_ssao.ssgi) {   /* --ssgi: rides on XeGTAO's passes */
        static const char *const sq[] = {"off", "low", "medium", "high"};
        if (renderer_ssgi_on()) printf("[renderer] bounce lighting on (SSGI %s, strength %.2f)\n", sq[hle_d3d_ssao.ssgi & 3], hle_d3d_ssao.ssgi_strength);
        else if (!hle_d3d_ssao.on || hle_d3d_ssao.mode != SSAO_MODE_GTAO) printf("[renderer] bounce lighting (--ssgi) needs --ssao --ssao-mode gtao: off\n");
        else printf("[renderer] bounce lighting unavailable on this device (XeGTAO not running): off\n");
    }
    if (hle_d3d_lamp.on) {
        if (renderer_lamp_lights_on()) printf("[renderer] lamp lights on (strength %.2f)\n", hle_d3d_lamp.strength);
        else printf("[renderer] lamp lights unavailable on this device; frames are drawn without them\n");
    }
    if (hle_d3d_lamp_shadow.on)
        printf("[renderer] lamp shadows %s (strength %.2f, min angle %.0f)\n", renderer_lamp_shadows_on() ? "on" : "unavailable on this device",
               hle_d3d_lamp_shadow.strength, hle_d3d_lamp_shadow.min_angle);
    if (hle_d3d_world_shadow.on)
        printf("[renderer] world shadows %s (strength %.2f, %d x %d map over %.0f m)\n", renderer_world_shadow_on() ? "on" : "unavailable on this device",
               hle_d3d_world_shadow.strength, hle_d3d_world_shadow.size, hle_d3d_world_shadow.size, hle_d3d_world_shadow.range);
    if (hle_d3d_volfog.on) {
        if (renderer_volfog_on()) {
            const VolfogGrid g = volfog_grid(hle_d3d_volfog.quality);
            printf("[renderer] volumetric fog on (%s: %dx%dx%d froxels, up to %d lights, strength %.2f)%s\n", volfog_quality_name(hle_d3d_volfog.quality), g.w,
                   g.h, g.d, g.lights, hle_d3d_volfog.strength, hle_d3d_shafts.on ? "; it replaces the light shafts" : "");
        } else printf("[renderer] volumetric fog unavailable on this device; frames are drawn without it\n");
    }
    if (hle_d3d_shafts.on && !hle_d3d_volfog.on) {
        if (renderer_light_shafts_on())
            printf("[renderer] light shafts on (strength %.2f%s)\n", hle_d3d_shafts.strength, hle_d3d_shafts.cheap ? ", cheap quarter-size path" : "");
        else printf("[renderer] light shafts unavailable on this device; frames are drawn without them\n");
    }
    if (hle_d3d_wet_streaks.on) {
        if (!wet_streaks_used()) printf("[renderer] wet light streaks need --rain-hd and its wet ground; they do nothing alone\n");
        else if (renderer_rain_wet_on())
            printf("[renderer] wet light streaks on (strength %.2f, up to %d lights)\n", hle_d3d_wet_streaks.strength, hle_d3d_wet_streaks.cap);
        else printf("[renderer] wet light streaks unavailable on this device (no wet ground); frames are drawn without them\n");
    }
    if (hle_d3d_damp.on) {
        if (renderer_rain_wet_on())
            printf("[renderer] damp streets on (strength %.2f): dry night streets outdoors, asphalt and tar from the level's collision map\n",
                   hle_d3d_damp.strength);
        else printf("[renderer] damp streets unavailable on this device (no wet ground pass); frames are drawn without them\n");
    }
    if (hle_d3d_char_light.on) {
        if (renderer_char_lights_on()) printf("[renderer] character lighting on (lamp lights on characters per pixel%s)\n",
                                              hle_d3d_lamp.on ? "" : "; the world is not lit: --lamp-lights is off");
        else printf("[renderer] character lighting unavailable on this device; characters are drawn without it\n");
    }
    if (hle_d3d_hair.on) {
        if (!hle_d3d_char_light.on) printf("[hair] needs --char-lighting: off\n");
        else if (renderer_char_lights_on())
            printf("[renderer] hair on (strength %.2f): strand highlights and darker under-layers on each skin's hair texels\n", hle_d3d_hair.strength);
        else printf("[renderer] hair unavailable on this device (no character lighting); characters are drawn without it\n");
    }
    if (hle_d3d_eyes.on || hle_d3d_cloth.on) {
        if (!hle_d3d_char_light.on) printf("[eyes/cloth] --eyes and --cloth need --char-lighting: off\n");
        else if (renderer_char_lights_on())
            printf("[renderer] eyes %s, cloth %s: wet glints on the eyes, leather, denim and cotton on the clothes, from the scene's lights\n",
                   hle_d3d_eyes.on ? "on" : "off", hle_d3d_cloth.on ? "on" : "off");
        else printf("[renderer] eyes and cloth unavailable on this device (no character lighting); characters are drawn without them\n");
    }
    if (hle_d3d_skin.on) {
        if (!hle_d3d_char_light.on) printf("[skin] needs --char-lighting: off\n");
        else if (renderer_char_lights_on())
            printf("[renderer] skin on (strength %.2f): light through thin parts, a red terminator and a sweat sheen on each skin's skin texels\n",
                   hle_d3d_skin.strength);
        else printf("[renderer] skin unavailable on this device (no character lighting); characters are drawn without it\n");
    }
    if (hle_d3d_world_light.on) {
        if (renderer_char_lights_on()) printf("[renderer] world lighting on (lamp lights on the world per pixel, strength %.2f)\n", hle_d3d_world_light.strength);
        else printf("[renderer] world lighting unavailable on this device; the world is drawn without it\n");
    }
    if (hle_d3d_fire.on) {
        if (hle_d3d_world_light.on || hle_d3d_char_light.on || hle_d3d_lamp.on || hle_d3d_shafts.on || hle_d3d_volfog.on)
            printf("[renderer] fire light on (up to %d flickering fire lights, strength %.2f)\n", FIRE_MAX_LIGHTS, hle_d3d_fire.strength);
        else printf("[renderer] fire light needs --world-lighting, --char-lighting or --lamp-lights; it does nothing alone\n");
    }
    if (hle_d3d_soft.on) {
        if (renderer_soft_on()) printf("[renderer] soft particles on (distance %.2f)\n", hle_d3d_soft.distance);
        else printf("[renderer] soft particles unavailable on this device; particles are drawn as is\n");
    }
    if (hle_d3d_sky.on) {
        if (renderer_sky_on()) printf("[renderer] enhanced sky on\n");
        else printf("[renderer] enhanced sky unavailable on this device; the title's sky is drawn as is\n");
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
    const double t_mesh = now_ms();
    if (hle_d3d_mesh_pack) {
        int n = meshpack_open(hle_d3d_mesh_pack);
        if (n < 0) printf("[meshpack] cannot read the mesh pack %s\n", hle_d3d_mesh_pack);
        else printf("[meshpack] %d replacement character meshes in %s\n", n, hle_d3d_mesh_pack);
        dlog_crumb("mesh pack load: %d meshes", n);
    }
    const double t_tex = now_ms();
    int total = 0;
    texpack_set_dds(1);   /* counted below; offered only when the device samples BC blocks */
    for (int i = 0; i < hle_d3d_texture_pack_count; i++) {
        const char *dir = hle_d3d_texture_packs[i];
        int n = i ? texpack_add(dir) : texpack_open(dir);
        dlog_crumb("texture pack load %d: %d textures", i + 1, n);
        if (n < 0) printf("[texpack] cannot read the texture pack %s\n", dir);
        else {
            printf("[texpack] %d replacement textures in %s%s\n", n, dir, i ? " (not in an earlier pack)" : "");
            total += n;
        }
    }
    if (load_prof_on && (hle_d3d_mesh_pack || hle_d3d_texture_pack_count))   /* once, at the first frame */
        printf("[load] packs at start: mesh pack %.0f ms, texture pack index %.0f ms\n", t_tex - t_mesh, now_ms() - t_tex);
    neon_index();
    if (hle_d3d_texture_pack_count) {
        tp.pack = total > 0;
        if (texpack_pack_count() > 1) {
            printf("[texpack] %d packs, first match wins:", texpack_pack_count());
            for (int i = 0; i < hle_d3d_texture_pack_count; i++) printf(" %s", hle_d3d_texture_packs[i]);
            printf(" (%d textures)\n", total);
        }
        int dds = texpack_dds_count();
        texpack_set_dds(renderer_has_bc());
        if (dds)
            printf("[texpack] %d replacements have a .dds or .ktx2 (BC1/BC3/BC7): %s\n", dds,
                   renderer_has_bc() ? "uploaded as is" : "ignored, the device does not sample BC formats (the .png when present)");
        /* PNG replacements are uncompressed and typically 4x larger: a larger budget keeps the
         * renderer from dropping (and the pack from reloading) them every few frames. */
        if (tp.pack) {
            vb_init(&tpb, hle_d3d_texpack_budget_mb ? (uint64_t)hle_d3d_texpack_budget_mb << 20 : TEXPACK_BUDGET);
            renderer_set_texture_budget(tpb.cap);
        }
    }
    relief_index();
    if ((tp.pack || tp_masks || tp_relief) && !hle_d3d_texpack_sync) tp_start_loaders();
    else if (tp.pack || tp_masks || tp_relief) printf("[texpack] --texpack-sync: replacements and masks load on the render thread when drawn\n");
}

/* Content hash of a draw's stage texture (memoized per cache key); a texture seen for the first
 * time is dumped (level 0 decoded on the CPU). race: NULL at record time; at replay the
 * tex_race_check state, and a hash of bytes that changed since record time is neither memoized
 * nor dumped (0 with the guard on). */
static int tex_race_check(const FrameOp *op, int stage, const XTexInfo *t, int *race);
static uint64_t texture_hash(const FrameOp *op, int stage, const XTexInfo *t, int *race) {
    uint64_t key = op->tex_key[stage], hash;
    if (texpack_key_hash(key, &hash)) return hash;
    const uint8_t *src = G(op->tex_data[stage]);
    hash = texpack_hash(t, src);
    if (race && tex_race_check(op, stage, t, race)) return 0;   /* checked after the read: a change during it counts too */
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

/* Replacements and --neon-glow emission masks are read and decoded (a .dds parsed as is, a .ktx2
 * transcoded to BC blocks, a PNG decoded with its mip chain: tens of ms for an upscaled pack, more
 * while the disk is busy) on
 * TP_LOADERS low-priority loader threads, so a texture seen for the first time never stalls the
 * frame: until its replacement is ready the draw samples the guest's own texture, cached under
 * TP_FALLBACK(key) so it never takes the replacement's key, and the replacement takes the guest
 * key when it is uploaded. The queue (src/texload.h) shares one slot per hash and kind, loads
 * demand requests before prefetches and caps the images waiting in RAM; this thread uploads at
 * most TP_BUDGET_BYTES / TP_BUDGET_MS per frame (always at least one image). The guest's resource
 * registrations queue prefetches of the pack's textures (tp_prefetch_register), so a level load
 * reads them before they are drawn. --texpack-sync loads everything on this thread when drawn. */
#define TP_SLOTS 256
#define TP_LOADERS 2
#define TP_STAGED_CAP (160u << 20)      /* RAM for images waiting to be uploaded (a 32-bit process) */
/* --level-prefetch (on by default): a level's pack textures reach the
 * GPU during its load. More slots and loaders, LP_PREFETCH_PAR prefetch reads at once (one loader
 * stays free for draws), ready prefetched images uploaded ahead of their first draw (tp_preupload)
 * under TP_HASH_KEY(hash), the content hash, which every draw of that content finds and the LRU
 * cache (RendererConfig.texture_streaming) keeps across levels; the frame's upload budget is
 * charged with the frame before's measured upload time (guest textures and waits included) and
 * is larger on loading frames; the level's texture list (lp_*) starts the reads at the level
 * request. Off: exactly the queue above. */
#define LP_SLOTS 2048
#define LP_LOADERS 4
#define LP_PREFETCH_PAR 3
#define LP_STAGED_CAP (256u << 20)
#define LP_LOAD_BUDGET_BYTES (48u << 20)   /* loading frames: the game's own frames are short then */
#define LP_LOAD_BUDGET_MS 10.0
#define TP_HASH_KEY(hash) ((hash) ^ 0x1badb00c5eed1234ull)
#define TP_KEEP_FRAMES 600              /* a loaded image no draw asks for is dropped after 10 s */
#define TP_BUDGET_BYTES (16u << 20)     /* per-frame upload budget */
#define TP_BUDGET_MS 3.0
#define TP_INBOX 1024                   /* prefetch hashes waiting for this thread */
#define TP_FALLBACK(key) ((key) ^ 0x5bd1e9955bd1e995ull)
int hle_d3d_texpack_sync, hle_d3d_texpack_prefetch = 1, hle_d3d_level_prefetch = 1;
static TlSlot tp_slot[LP_SLOTS];
static TexLoadQueue tpq;
static pthread_mutex_t tp_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tp_cv = PTHREAD_COND_INITIALIZER;
static uint64_t tp_inbox[TP_INBOX];
static int tp_inbox_n;
static struct {
    uint32_t prefetched, inbox_full;   /* prefetch requests queued; registrations lost to a full inbox */
    double max_upload_ms;
    uint32_t preup, preup_skip;        /* --level-prefetch: images uploaded ahead of their draw; already resident */
    double preup_ms, frame_ms;         /* their upload time; this frame's measured pack uploads (carry) */
} tps;
static int lp_on;   /* --level-prefetch in use: loaders running, a pack, prefetch allowed */

/* Bytes of a loaded chain: a replacement (TEXPACK_* format) or an L8 mask. */
static size_t tp_image_bytes(int mask, uint32_t format, uint32_t w, uint32_t h, uint32_t levels) {
    size_t n = 0;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        n += mask ? (size_t)lw * lh : texpack_level_bytes(format, lw, lh);
    }
    return n;
}

static void *tp_loader(void *arg) {
    (void)arg;
    plat_thread_low_priority();
    pthread_mutex_lock(&tp_mx);
    for (;;) {
        int i = tl_next(&tpq);
        if (i < 0) {
            pthread_cond_wait(&tp_cv, &tp_mx);
            continue;
        }
        char *path = tpq.slot[i].path;
        const int kind = tpq.slot[i].kind;
        tpq.slot[i].path = NULL;
        pthread_mutex_unlock(&tp_mx);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        TlImage img = {.why = "no file"};
        int failed = 1;
        if (path && (kind == TL_BASE || kind >= TL_RELIEF)) {   /* a replacement or a companion map */
            TexpackImage ti;
            if (!(failed = texpack_load_image(path, &ti, &img.why)))
                img = (TlImage){ti.format, ti.w, ti.h, ti.levels, ti.data, tp_image_bytes(0, ti.format, ti.w, ti.h, ti.levels), NULL, 0};
        } else if (path) {
            uint32_t w = 0, h = 0, levels = 0;
            uint8_t *chain = neon_glow_load(path, &w, &h, &levels, &img.why);
            if (chain) {
                img = (TlImage){RENDERER_TEX_L8, w, h, levels, chain, tp_image_bytes(1, 0, w, h, levels), NULL, 0};
                failed = 0;
            }
        }
        free(path);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        img.ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        if (load_prof_on && !failed) load_prof_count(LOAD_PACK_READ, img.bytes, (uint64_t)(img.ms * 1e6));
        pthread_mutex_lock(&tp_mx);
        tl_done(&tpq, i, &img, failed, hle_d3d_frame_count());
    }
    return NULL;
}

static void tp_start_loaders(void) {
    const int lp = hle_d3d_level_prefetch && hle_d3d_texpack_prefetch && tp.pack;
    tl_init(&tpq, tp_slot, lp ? LP_SLOTS : TP_SLOTS, lp ? LP_STAGED_CAP : TP_STAGED_CAP, TP_KEEP_FRAMES, TP_BUDGET_BYTES, TP_BUDGET_MS);
    if (lp) tpq.prefetch_par = LP_PREFETCH_PAR;
    for (int i = 0; i < (lp ? LP_LOADERS : TP_LOADERS); i++) {
        pthread_t th;
        if (pthread_create(&th, NULL, tp_loader, NULL)) break;
        pthread_detach(th);
        tp_loaders++;
    }
    if (!tp_loaders) printf("[texpack] cannot start the loader threads: replacements load when drawn\n");
    __atomic_store_n(&tp_prefetch_on, tp_loaders && hle_d3d_texpack_prefetch && tp.pack, __ATOMIC_RELEASE);
    lp_on = lp && tp_loaders;
    if (lp_on)
        printf("[level-prefetch] on: %d loader threads (%d prefetch reads at once), %d queue slots, %u MB staging, pack textures cached by "
               "content hash with LRU eviction, level texture lists in %s/cache/levels\n", tp_loaders, LP_PREFETCH_PAR, LP_SLOTS,
               LP_STAGED_CAP >> 20, hle_d3d_pipeline_cache_dir ? hle_d3d_pipeline_cache_dir : "(no hdd: not saved)");
}

/* Asks the loaders for (hash, kind) (tp_mx held); returns the slot or -1. A new slot gets its file
 * (NULL: absent, which fails in the loader). */
static int tp_request(uint64_t hash, int kind, int prio, uint32_t frame) {
    int created, i = tl_request(&tpq, hash, kind, prio, frame, &created);
    if (i >= 0 && created) {
        tpq.slot[i].path = kind == TL_BASE ? texpack_path(hash) : kind == TL_MASK ? neon_glow_mask_path(hash) : normal_maps_path(hash, TL_RELIEF_KIND(kind));
        pthread_cond_signal(&tp_cv);
    }
    return i;
}

/* Guest thread, D3DResource_Register of a texture whose texels are in memory: its content hash
 * goes to the inbox; the render thread queues the pack's file (tp_frame). */
static void tp_prefetch_register(uint32_t res) {
    uint32_t fmt = U32(res, 0xc), size = U32(res, 0x10), data = VA(U32(res, 4));
    XTexInfo t;
    if (xtex_info(fmt, size, &t) || !readable(data, xtex_total_bytes(&t))) return;
    const uint64_t hash = texpack_hash(&t, G(data));
    __atomic_add_fetch(&tp_registrations, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&tp_mx);
    if (tp_inbox_n < TP_INBOX) tp_inbox[tp_inbox_n++] = hash;
    else tps.inbox_full++;
    pthread_mutex_unlock(&tp_mx);
}

/* --level-prefetch: the level texture list (texload.h TlList) of the level being played, learnt
 * while it is drawn and kept in <hdd>/cache/levels/<level>.txt (content hashes only). At the level
 * request the list's early part (drawn in the first LP_EARLY_FRAMES frames that drew anything) is
 * queued for the loaders, at most as many as the queue takes each frame; once the level has run
 * that long and nothing loads, its late part (drawn later on earlier visits: the hub's Coney
 * streets past the hideout door) follows, LP_LATE_INFLIGHT at a time, uploaded only while the cache
 * stays below 4/5 of its cap. Render thread only. */
static void tp_uploaded(double ms);
#define LP_EARLY_FRAMES 300   /* ~5 s */
#define LP_LATE_INFLIGHT 16
#define LP_LIST_BYTES (TL_LIST_MAX * 19 + 256)
static TlList lp_list, lp_seen;   /* the level's list; hashes drawn in this visit */
static struct {
    uint32_t gen;                 /* level_gen seen */
    char level[PERF_LEVEL_CHARS];
    int have, next_f, next_l;     /* a level is being played; list cursors (prefetch requests made) */
    uint32_t frames;              /* frames with draws since the request */
    uint32_t loaded, added, queued, ready, at_draw, missed, preup0;   /* this visit's numbers */
    int dirty;
    double reg_recent;            /* registrations per frame, smoothed: an area streaming in */
    uint32_t levels, saved;
} lp;

/* <hdd>/cache/levels/<level>.txt for a level name made of [A-Za-z0-9_-] (others replaced), or 0. */
static int lp_path(char *out, size_t cap, int mkdirs) {
    const char *hdd = hle_d3d_pipeline_cache_dir;
    if (!hdd || !*hdd || !lp.level[0]) return 0;
    char name[PERF_LEVEL_CHARS];
    size_t n = 0;
    for (const char *c = lp.level; *c && n + 1 < sizeof name; c++)
        name[n++] = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-' ? *c : '_';
    name[n] = 0;
    if (mkdirs) {
        snprintf(out, cap, "%s/cache", hdd);
        plat_mkdir(out);
        snprintf(out, cap, "%s/cache/levels", hdd);
        plat_mkdir(out);
    }
    return snprintf(out, cap, "%s/cache/levels/%s.txt", hdd, name) < (int)cap;
}

static void lp_save(void) {
    char path[1024], tmp[1040];
    if (!lp.have || !lp.dirty || !lp_path(path, sizeof path, 1)) return;
    char *buf = malloc(LP_LIST_BYTES);
    const size_t n = buf ? tl_list_format(&lp_list, buf, LP_LIST_BYTES) : 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = buf && n < LP_LIST_BYTES ? fopen(tmp, "wb") : NULL;
    int ok = f && fwrite(buf, 1, n, f) == n;
    if (f && fclose(f)) ok = 0;
    if (ok) {
        remove(path);   /* Windows' rename does not replace */
        ok = !rename(tmp, path);
    }
    if (!ok) {
        if (f) remove(tmp);
        printf("[level-prefetch] cannot write %s\n", path);
    } else lp.saved++;
    free(buf);
    lp.dirty = 0;
}

/* The finished visit's line, then its list saved. */
static void lp_level_end(void) {
    if (!lp.have) return;
    int early = 0;
    for (int i = 0; i < lp_list.n; i++) early += !lp_list.late[i];
    printf("[level-prefetch] %s: list %d textures (%d early, %d late; %u loaded, %u new this visit), %u queued from it, %u uploaded "
           "ahead; first draws %u resident, %u uploaded when drawn, %u drawn with the original first\n",
           lp.level, lp_list.n, early, lp_list.n - early, lp.loaded, lp.added, lp.queued, tps.preup - lp.preup0, lp.ready, lp.at_draw,
           lp.missed);
    lp_save();
}

static void lp_level_begin(const char *name) {
    lp_level_end();
    memset(&lp_seen, 0, sizeof lp_seen);
    tl_list_clear(&lp_list);
    snprintf(lp.level, sizeof lp.level, "%s", name);
    lp.have = 1;
    lp.levels++;
    lp.next_f = lp.next_l = 0;
    lp.frames = lp.loaded = lp.added = lp.queued = lp.ready = lp.at_draw = lp.missed = 0;
    lp.preup0 = tps.preup;
    lp.dirty = 0;
    char path[1024];
    FILE *f = lp_path(path, sizeof path, 0) ? fopen(path, "rb") : NULL;
    if (!f) return;
    char *buf = malloc(LP_LIST_BYTES + 1);
    const size_t n = buf ? fread(buf, 1, LP_LIST_BYTES, f) : 0;
    fclose(f);
    if (buf) {
        buf[n] = 0;
        lp.loaded = (uint32_t)tl_list_parse(&lp_list, buf);
    }
    free(buf);
}

/* A draw of pack texture hash; how: 0 resident (found under its hash key), 1 uploaded now, 2 not
 * loaded yet (drawn with the original). The first draw of a visit is counted and listed. */
static void lp_drawn(uint64_t hash, int how) {
    if (!lp_on || !lp.have || tl_list_add(&lp_seen, hash, 0) != 1) return;
    if (how == 0) lp.ready++;
    else if (how == 1) lp.at_draw++;
    else lp.missed++;
    const int late = lp.frames > LP_EARLY_FRAMES, e = tl_list_find(&lp_list, hash);
    if (e >= 0) {
        if (!late && lp_list.late[e]) lp.dirty = 1;
        tl_list_add(&lp_list, hash, late);
    } else if (tl_list_add(&lp_list, hash, late) == 1) {
        lp.added++;
        lp.dirty = 1;
    }
}

/* A pack replacement for hash should be prefetched and pre-uploaded: the pack has it, no built-in
 * replacement wins over it, and it is not cached already. */
static int lp_wanted(uint64_t hash) {
    return texpack_has(hash) && builtin_texture_source(hash, tp.pack) == TEXSRC_PACK && !renderer_texture_find(TP_HASH_KEY(hash));
}

/* tp_mx held: feeds the list's early part (from the request) and late part (once the level runs
 * calmly) to the queue. */
static void lp_feed(uint32_t frame, int loading) {
    while (lp.next_f < lp_list.n) {
        const uint64_t hash = lp_list.hash[lp.next_f];
        if (!lp_list.late[lp.next_f] && lp_wanted(hash) && tl_find(&tpq, hash, TL_BASE) < 0) {
            if (tp_request(hash, TL_BASE, TL_PREFETCH, frame) < 0) return;   /* the queue is full: next frame */
            lp.queued++;
        }
        lp.next_f++;
    }
    if (loading || lp.frames <= LP_EARLY_FRAMES || tl_count(&tpq, TL_QUEUED) + tl_count(&tpq, TL_LOADING) >= LP_LATE_INFLIGHT) return;
    if (renderer_texture_bytes() >= tpb.cap / 5 * 4) return;
    for (int added = 0; lp.next_l < lp_list.n && added < LP_LATE_INFLIGHT; lp.next_l++) {
        const uint64_t hash = lp_list.hash[lp.next_l];
        if (!lp_list.late[lp.next_l] || !lp_wanted(hash) || tl_find(&tpq, hash, TL_BASE) >= 0) continue;
        if (tp_request(hash, TL_BASE, TL_PREFETCH, frame) < 0) return;
        lp.queued++;
        added++;
    }
}

/* Uploads ready prefetched images under their hash key within the frame's budget (tp_mx not held).
 * Outside loading frames only while the cache stays below 4/5 of its cap. */
static void tp_preupload(uint32_t frame, int loading) {
    static const uint32_t fmt[] = {[TEXPACK_BGRA8] = RENDERER_TEX_BGRA8, [TEXPACK_BC1] = RENDERER_TEX_BC1,
                                   [TEXPACK_BC3] = RENDERER_TEX_BC3, [TEXPACK_BC7] = RENDERER_TEX_BC7};
    for (;;) {
        if (!loading && renderer_texture_bytes() >= tpb.cap / 5 * 4) return;
        TlImage img;
        uint64_t hash;
        pthread_mutex_lock(&tp_mx);
        const int r = tl_take_prefetch(&tpq, TL_BASE, frame, &img, &hash);
        pthread_mutex_unlock(&tp_mx);
        if (!r) return;
        if (renderer_texture_find(TP_HASH_KEY(hash)) || builtin_texture_source(hash, tp.pack) != TEXSRC_PACK) {
            tps.preup_skip++;
            free(img.data);
            continue;
        }
        const double t0 = now_ms();
        const uint32_t h = renderer_texture_create(TP_HASH_KEY(hash), fmt[img.format], img.w, img.h, img.levels, img.data);
        const double ms = now_ms() - t0;
        free(img.data);
        tps.preup += h != 0;
        tps.preup_ms += ms;
        tp.load_ms += ms;
        load_pack_ms += ms;
        tp_uploaded(ms);
        if (!h) return;
    }
}

/* Once per replayed frame: the inbox's pack textures (and their masks) become prefetch requests,
 * images nobody takes are reclaimed, and loaders wake for prefetches that may start. With
 * --level-prefetch also the level list's requests, the frame's budget (larger while loading,
 * charged with the last frame's upload time beyond what this file measured) and pre-uploads. */
static void tp_frame(uint32_t draws) {
    static uint32_t last;
    const uint32_t frame = hle_d3d_frame_count();
    if (!tp_loaders || frame == last) return;
    last = frame;
    int loading = 0;
    if (lp_on) {
        const uint32_t gen = __atomic_load_n(&level_gen, __ATOMIC_ACQUIRE);
        if (gen != lp.gen) {
            lp.gen = gen;
            lp_level_begin(level_req[gen & 1]);
        }
        lp.frames += draws != 0;
    }
    pthread_mutex_lock(&tp_mx);
    if (lp_on) {   /* loading: the level's first frames, frames without draws, or many registrations lately (an area streaming in) */
        lp.reg_recent = lp.reg_recent * 0.875 + tp_inbox_n;
        loading = lp.have && (lp.frames < 30 || !draws || lp.reg_recent >= 64);
    }
    for (int k = 0; k < tp_inbox_n; k++) {
        const uint64_t hash = tp_inbox[k];
        /* --level-prefetch with the level's list read: only listed early textures are read ahead
         * (the list knows what the level draws first; reading every registration competes with the
         * game's own load), the rest when drawn or from the list's late part. */
        if (lp_on && lp.loaded) {
            const int e = tl_list_find(&lp_list, hash);
            if (e < 0 || lp_list.late[e]) continue;
        }
        if (texpack_has(hash) && tl_find(&tpq, hash, TL_BASE) < 0 && (!lp_on || lp_wanted(hash)) && tp_request(hash, TL_BASE, TL_PREFETCH, frame) >= 0) tps.prefetched++;
        if (tp_masks && neon_glow_has(hash)) tp_request(hash, TL_MASK, TL_PREFETCH, frame);
        for (int k = 0; tp_relief && k < NORMAL_MAPS_KINDS; k++)
            if (normal_maps_has(hash, k)) tp_request(hash, TL_RELIEF + k, TL_PREFETCH, frame);
    }
    tp_inbox_n = 0;
    if (lp_on) {
        lp_feed(frame, loading);
        tpq.budget_bytes = loading ? LP_LOAD_BUDGET_BYTES : TP_BUDGET_BYTES;
        tpq.budget_ms = loading ? LP_LOAD_BUDGET_MS : TP_BUDGET_MS;
        /* The real upload time of the frame before: guest textures, the batch's submit and waits. */
        const double extra = hstat_valid ? hstat_end.upload_ms - tps.frame_ms : 0;
        if (extra > 0) tl_charge_ms(&tpq, frame, extra);
        tps.frame_ms = 0;
    }
    tl_reclaim(&tpq, frame);
    /* Uploads and drops free staging room: a prefetch held at the cap may start now. */
    if (tpq.staged < tpq.staged_cap && tl_count(&tpq, TL_QUEUED)) pthread_cond_broadcast(&tp_cv);
    pthread_mutex_unlock(&tp_mx);
    if (lp_on) tp_preupload(frame, loading);
}

/* The loaded image of (hash, kind) for upload now: 1 (in *img), -1 failed (*img->why), 0 not yet
 * (requested when new; a full queue retries on a later draw). */
static int tp_fetch(uint64_t hash, int kind, TlImage *img) {
    const uint32_t frame = hle_d3d_frame_count();
    pthread_mutex_lock(&tp_mx);
    int i = tp_request(hash, kind, TL_DEMAND, frame), r = i >= 0 ? tl_take(&tpq, i, frame, img) : 0;
    if (kind == TL_BASE && i >= 0 && r == 0 && tp_masks && neon_glow_has(hash)) tp_request(hash, TL_MASK, TL_DEMAND, frame);
    for (int k = 0; kind == TL_BASE && i >= 0 && r == 0 && tp_relief && k < NORMAL_MAPS_KINDS; k++)
        if (normal_maps_has(hash, k)) tp_request(hash, TL_RELIEF + k, TL_DEMAND, frame);
    pthread_mutex_unlock(&tp_mx);
    return r;
}

/* With a pack, every ~2 s on the render thread: the driver's budget and usage set the cache cap
 * (renderer_begin frees the least recently used textures at the next frame when it no longer fits). */
static void tp_budget_frame(void) {
    static double next;
    if (!tp.pack) return;
    const double t = now_ms();
    if (t < next) return;
    next = t + 2000;
    uint64_t budget, usage;
    if (!renderer_memory_budget(&budget, &usage, NULL)) return;
    const uint64_t cache = renderer_texture_bytes(), old = tpb.cap;
    const int first = !tpb.samples;
    if (vb_update(&tpb, budget, usage, cache) == old && !first) return;
    const char *over = ": the least recently used textures are freed";
    renderer_set_texture_budget(tpb.cap);
    dlog_crumb("texture pack cache cap %llu MiB (budget %llu, usage %llu, cache %llu MiB)", (unsigned long long)(tpb.cap >> 20),
               (unsigned long long)(budget >> 20), (unsigned long long)(usage >> 20), (unsigned long long)(cache >> 20));
    printf("[texpack] cache cap %llu MiB (driver budget %llu MiB, usage %llu MiB, cache %llu MiB)%s\n", (unsigned long long)(tpb.cap >> 20),
           (unsigned long long)(budget >> 20), (unsigned long long)(usage >> 20), (unsigned long long)(cache >> 20),
           cache >= tpb.cap ? over : "");
}

static void tp_summary(void) {
    if (!tp.pack) return;
    if (lp_on) {
        lp_level_end();
        printf("[level-prefetch] %u levels, %u texture lists written, %u images uploaded ahead of their first draw (%.0f ms), %u already "
               "resident\n", lp.levels, lp.saved, tps.preup, tps.preup_ms, tps.preup_skip);
    }
    if (tpb.samples)
        printf("[texpack] cache cap %llu MiB of at most %llu (smallest %llu), %u memory budget samples, %u shrinks (%u dropping the cache), %u grows\n",
               (unsigned long long)(tpb.cap >> 20), (unsigned long long)(tpb.max_cap >> 20), (unsigned long long)(tpb.min_cap_seen >> 20),
               tpb.samples, tpb.shrinks, tpb.drop_shrinks, tpb.grows);
    else
        printf("[texpack] cache cap %llu MiB (fixed: no driver memory budget)\n", (unsigned long long)(tpb.cap >> 20));
    printf("[texpack] %u textures replaced, %u of them BC blocks from .dds/.ktx2, %u .dds/.ktx2 skipped (%.0f ms uploading, longest %.1f ms, %d loader threads%s)\n",
           tp.replaced, tp.replaced_bc, tp.bad_dds, tp.load_ms, tps.max_upload_ms, tp_loaders, tp_loaders ? "" : ", loaded when drawn");
    uint32_t k2;
    double k2_ms, k2_max;
    texpack_ktx2_stats(&k2, &k2_ms, &k2_max);
    if (k2)
        printf("[texpack] %u .ktx2 files transcoded (Basis Universal, replacements and companion maps): %.0f ms in all, %.2f ms each on "
               "average, longest %.1f ms (%s)\n", k2, k2_ms, k2_ms / k2, k2_max, tp_loaders ? "loader threads" : "render thread");
    if (tp_loaders)
        printf("[texpack] loader queue: %u prefetched (%u of them later drawn), %u uploads deferred to a later frame, %u images dropped unused, "
               "%u dropped over the %u MB staging cap, %u registrations lost to a full inbox\n",
               tps.prefetched, tpq.prefetch_hits, tpq.deferred, tpq.dropped_stale, tpq.dropped_cap, (unsigned)(tpq.staged_cap >> 20), tps.inbox_full);
}

/* Adds an upload's time to the frame's budget. */
static void tp_uploaded(double ms) {
    if (ms > tps.max_upload_ms) tps.max_upload_ms = ms;
    tps.frame_ms += ms;
    if (!tp_loaders) return;
    pthread_mutex_lock(&tp_mx);
    tl_charge_ms(&tpq, hle_d3d_frame_count(), ms);
    pthread_mutex_unlock(&tp_mx);
}

/* A replacement failed to load. A .dds or .ktx2 is dropped (logged once) and the hash's .png, if the
 * pack has one, is requested on a later draw (returns 1); otherwise the original is used from now on. */
static int tp_failed(uint64_t hash, const char *why) {
    char *path = texpack_path(hash);
    size_t n = path ? strlen(path) : 0;
    int dds = n > 4 && path[n - 4] == '.' && (path[n - 3] | 32) == 'd' && (path[n - 2] | 32) == 'd' && (path[n - 1] | 32) == 's';
    dds |= n > 5 && path[n - 5] == '.' && (path[n - 4] | 32) == 'k' && (path[n - 3] | 32) == 't' && (path[n - 2] | 32) == 'x' && path[n - 1] == '2';
    if (dds && !tp.bad_dds++)
        printf("[texpack] skipping %s: %s (the .png is used when the pack has one; further .dds/.ktx2 failures are not logged)\n", path,
               why ? why : "unreadable");
    free(path);
    if (dds && texpack_drop_dds(hash)) return 1;
    if (!dds && !tp.missing++) printf("[texpack] cannot load the replacement %016llx\n", (unsigned long long)hash);
    texpack_disable(hash);
    return 0;
}

static TlUploadFails tp_upfail;   /* replace_texture's failed uploads */

/* The pack replacement of a draw's texture (key, content hash) is on the GPU: under the guest key,
 * or with --level-prefetch under its hash key. */
static int tp_resident(uint64_t key, uint64_t hash) {
    return renderer_texture_find(key) || (lp_on && renderer_texture_find(TP_HASH_KEY(hash)));
}

/* Replacement from the pack, uploaded with its mip chain under the guest texture's cache key
 * (--level-prefetch: under TP_HASH_KEY(hash)).
 * Texture coordinates are normalized by then (LIN_* coordinates are divided by the guest size
 * when recorded), so a replacement of any size samples the same area. Returns 0 with *pending
 * set while the loaders still work on it (or it waits for the upload budget). */
static uint32_t replace_texture(uint64_t key, uint64_t hash, int *pending) {
    TexpackImage img = {0};
    const char *why = NULL;
    *pending = 0;
    if (!tp_loaders) {
        char *path = texpack_path(hash);
        int failed = !path || texpack_load_image(path, &img, &why);
        free(path);
        if (failed) {
            *pending = tp_failed(hash, why);
            return 0;
        }
    } else {
        TlImage ti;
        int r = tp_fetch(hash, TL_BASE, &ti);
        if (r <= 0) {
            *pending = r ? tp_failed(hash, ti.why) : 1;
            return 0;
        }
        img = (TexpackImage){ti.format, ti.w, ti.h, ti.levels, ti.data};
    }
    static const uint32_t fmt[] = {[TEXPACK_BGRA8] = RENDERER_TEX_BGRA8, [TEXPACK_BC1] = RENDERER_TEX_BC1,
                                   [TEXPACK_BC3] = RENDERER_TEX_BC3, [TEXPACK_BC7] = RENDERER_TEX_BC7};
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t handle = renderer_texture_create(lp_on ? TP_HASH_KEY(hash) : key, fmt[img.format], img.w, img.h, img.levels, img.data);
    free(img.data);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    tp.load_ms += ms;
    load_pack_ms += ms;
    tp_uploaded(ms);
    tp.replaced += handle != 0;
    tp.replaced_bc += handle && img.format != TEXPACK_BGRA8;
    /* A failed upload (the cache full, or device memory: renderer_begin frees the least recently
     * used textures at the next frame) draws the original under TP_FALLBACK meanwhile and tries
     * again on a later draw; not under the guest key, which would keep the original for good. */
    if (handle) {
        tl_upload_ok(&tp_upfail, hash);
    } else if (tl_upload_failed(&tp_upfail, hash)) {
        *pending = 1;
    } else {
        printf("[texpack] replacement %016llx (%ux%u) failed to upload %d times: the original is used\n", (unsigned long long)hash,
               img.w, img.h, TL_UPLOAD_TRIES);
        texpack_disable(hash);
    }
    return handle;
}

/* --sharp-fonts (src/sharp_font.h): the two font atlases, when no texture pack replaces them, are
 * upscaled SHARP_FONT_SCALE x on a worker thread from the guest's own level 0 the first time they
 * are drawn; until the result is ready the draw samples the original (cached under
 * TP_FALLBACK(key)), then the upscale with its mip chain takes the guest key. The main atlas's
 * chain is kept (the keyboard/mouse icons are painted on its level 0, and an evicted texture is
 * uploaded again); the big font's is freed after the upload and rebuilt if it is ever evicted.
 * States: 0 not started, 1 working, 2 ready, 3 uploaded, 4 failed. */
static struct {
    uint32_t *src, *chain, w, h, levels;
    double ms;
    int state;
} sf_job[2];
static pthread_mutex_t sf_mx = PTHREAD_MUTEX_INITIALIZER;

static void *sf_worker(void *arg) {
    int i = (int)(intptr_t)arg;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_mutex_lock(&sf_mx);
    uint32_t *src = sf_job[i].src, w = sf_job[i].w, h = sf_job[i].h;
    sf_job[i].src = NULL;
    pthread_mutex_unlock(&sf_mx);
    uint32_t *up = sharp_font_upscale(src, w, h, SHARP_FONT_SCALE), levels = 0, *chain = NULL;
    free(src);
    if (up) chain = texpack_mips(up, w * SHARP_FONT_SCALE, h * SHARP_FONT_SCALE, &levels);
    free(up);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    pthread_mutex_lock(&sf_mx);
    sf_job[i].chain = chain;
    sf_job[i].w = w * SHARP_FONT_SCALE;
    sf_job[i].h = h * SHARP_FONT_SCALE;
    sf_job[i].levels = levels;
    sf_job[i].ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    sf_job[i].state = chain ? 2 : 4;
    pthread_mutex_unlock(&sf_mx);
    return NULL;
}

static size_t sf_chain_bytes(uint32_t w, uint32_t h, uint32_t levels) {
    size_t n = 0;
    for (uint32_t l = 0; l < levels; l++, w = w > 1 ? w >> 1 : 1, h = h > 1 ? h >> 1 : 1) n += (size_t)w * h * 4;
    return n;
}

static int kbm_big_needs_sharp(void);

/* The sharp atlas for a draw: its handle, or 0 with *pending set while the worker runs (the caller
 * then uses the original under the fallback key), or 0 when it failed. */
static uint32_t sharp_font_texture(uint64_t key, uint64_t hash, const XTexInfo *t, const uint8_t *src, int *pending) {
    int i = hash == SHARP_FONT_MAIN_HASH ? 0 : 1;   /* sharp_font_atlas: one of the two */
    *pending = 0;
    pthread_mutex_lock(&sf_mx);
    int state = sf_job[i].state;
    pthread_mutex_unlock(&sf_mx);
    if (state == 4) return 0;
    if (state == 0) {
        uint32_t *px = malloc((size_t)t->w * t->h * 4);
        pthread_t th;
        if (!px || xtex_decode_level(t, 0, src, px)) {
            free(px);
            sf_job[i].state = 4;
            printf("[fonts] cannot read the font atlas %016llx: original kept\n", (unsigned long long)hash);
            return 0;
        }
        pthread_mutex_lock(&sf_mx);
        sf_job[i].src = px;
        sf_job[i].w = t->w;
        sf_job[i].h = t->h;
        sf_job[i].state = 1;
        pthread_mutex_unlock(&sf_mx);
        if (pthread_create(&th, NULL, sf_worker, (void *)(intptr_t)i)) {
            sf_worker((void *)(intptr_t)i);   /* no thread: upscale now */
        } else {
            pthread_detach(th);
            *pending = 1;
            return 0;
        }
    }
    pthread_mutex_lock(&sf_mx);
    state = sf_job[i].state;
    pthread_mutex_unlock(&sf_mx);
    if (state == 1) {
        *pending = 1;
        return 0;
    }
    if (state == 4) {
        printf("[fonts] sharp font %016llx failed: original kept\n", (unsigned long long)hash);
        return 0;
    }
    if (!sf_job[i].chain) {   /* the big font, uploaded before and evicted since: upscale it again */
        sf_job[i].state = 0;
        *pending = 1;
        return 0;
    }
    uint32_t handle = renderer_texture_create(key, RENDERER_TEX_BGRA8, sf_job[i].w, sf_job[i].h, sf_job[i].levels, sf_job[i].chain);
    if (state == 2)
        printf("[fonts] sharp font %016llx: %ux%u -> %ux%u in %.1f ms (worker thread), %.1f MB with mips\n",
               (unsigned long long)hash, sf_job[i].w / SHARP_FONT_SCALE, sf_job[i].h / SHARP_FONT_SCALE, sf_job[i].w, sf_job[i].h,
               sf_job[i].ms, sf_chain_bytes(sf_job[i].w, sf_job[i].h, sf_job[i].levels) / 1048576.0);
    sf_job[i].state = 3;
    if (i == 1 && !kbm_big_needs_sharp()) {   /* the big font's chain is not needed again */
        free(sf_job[i].chain);
        sf_job[i].chain = NULL;
    }
    return handle;
}

/* The sharp main atlas's job state (sf_job states above). */
static int sharp_font_state(int i) {
    pthread_mutex_lock(&sf_mx);
    int state = sf_job[i].state;
    pthread_mutex_unlock(&sf_mx);
    return state;
}

/* Sharp atlas i's level 0 (a copy to paint on) for the keyboard/mouse icons, or NULL when its
 * chain is gone. */
static uint32_t *sharp_font_copy(int i, uint32_t *w, uint32_t *h) {
    if (!sf_job[i].chain) return NULL;
    uint32_t *px = malloc((size_t)sf_job[i].w * sf_job[i].h * 4);
    if (px) {
        memcpy(px, sf_job[i].chain, (size_t)sf_job[i].w * sf_job[i].h * 4);
        *w = sf_job[i].w;
        *h = sf_job[i].h;
    }
    return px;
}

/* The pack's atlas level 0 as B8G8R8A8 texels to paint on: its .png, or its .dds decoded on the
 * CPU (a BC pack may ship the atlas as a .dds only); NULL when it cannot be read. */
static uint32_t *kbm_pack_atlas(uint64_t hash, uint32_t *w, uint32_t *h) {
    uint32_t levels;
    uint32_t *px = texpack_load(hash, w, h, &levels);   /* a chain: level 0 comes first */
    if (px) return px;
    char *path = texpack_path(hash);
    TexpackImage img;
    const char *why = "no file";
    if (path && !texpack_load_image(path, &img, &why)) {
        px = texpack_image_bgra(&img);
        *w = img.w;
        *h = img.h;
        free(img.data);
        if (!px) why = "cannot decode it";
    }
    if (!px) printf("[glyphs] cannot read the texture pack's font atlas (%s): painting on the game's atlas\n", why);
    free(path);
    return px;
}

/* Keyboard/mouse button icons (--button-icons kbm|auto): the font/icon atlas (slot 0) and the
 * big font atlas's pulsing D-pad sprites (1) painted with the current key map's icons, each
 * uploaded once under KBM_KEY(key) beside the original; each draw picks the key for the last used
 * device. Slots 2 and 3: the same atlases with the controller remap (input_map.h pad.* lines)
 * applied to the controller icons (kbm_remap_atlas: a prompt shows the button that now does its
 * action), under PADMAP_KEY(key), used for the controller icons while a remap is set. The
 * painted mip chains are kept, so the renderer dropping a texture costs one upload, not another
 * paint. */
#define KBM_KEY(key) ((key) ^ 0x6b626d5f69636f6eull)
#define PADMAP_KEY(key) ((key) ^ 0x7061645f72656d70ull)
static struct {
    uint32_t *chain, w, h, levels;
    int failed, pack_tried;
} kbm_atl[4];

static int kbm_icons_now(int i) {
    if (!(i ? big_glyph_nrects : glyph_nrects) || hle_d3d_button_icons == HLE_ICONS_XBOX) return 0;
    return hle_d3d_button_icons == HLE_ICONS_KBM || input_last_device() != INPUT_DEVICE_PAD;
}
/* Painted controller icons, whenever the keyboard/mouse ones are not shown: the PlayStation-style
 * icons (--pad-icons), or the remap moving a cell of that atlas (the big font's D-pad sprites only
 * follow the directions among themselves). The remap is fixed at start-up, so the answer is
 * computed once. */
static int pad_icons_now(int i) {
    static int moved[2] = {-1, -1};
    if (!(i ? big_glyph_nrects : glyph_nrects)) return 0;
    if (hle_d3d_pad_icons) return 1;
    if (moved[i] < 0) {
        moved[i] = 0;
        for (int c = 0; c < INPUT_PAD_INPUTS; c++) {
            int b = input_pad_button_for_control(c);
            if (b != c && (!i || (c <= IN_RIGHT && b >= IN_UP && b <= IN_RIGHT))) moved[i] = 1;
        }
    }
    return moved[i];
}

/* The sharp big font's chain is kept after its upload until the D-pad sprites are painted on it
 * (for each painted variant that will be needed). */
static int kbm_big_needs_sharp(void) {
    if (!big_glyph_nrects) return 0;
    return (hle_d3d_button_icons != HLE_ICONS_XBOX && !kbm_atl[1].chain && !kbm_atl[1].failed) ||
           (pad_icons_now(1) && !kbm_atl[3].chain && !kbm_atl[3].failed);
}

static uint32_t kbm_atlas(int slot, uint64_t key, uint64_t hash, const XTexInfo *t, const uint8_t *src) {
    const int i = slot & 1, pad = slot >= 2;   /* i: big font atlas */
    if (kbm_atl[slot].failed) return 0;
    if (!kbm_atl[slot].chain) {
        uint32_t w = t->w, h = t->h;
        uint32_t *px = NULL;
        const char *from = i ? "the big font's atlas" : "the game's atlas";
        if (!kbm_atl[slot].pack_tried && tp.pack && texpack_has(hash)) {
            kbm_atl[slot].pack_tried = 1;   /* read once: an unreadable file is not retried every frame */
            px = kbm_pack_atlas(hash, &w, &h);
        }
        /* --sharp-fonts upscales the atlas only when no pack replaces it: wait for that (Xbox
         * icons meanwhile), never for a job that will not start */
        int sharp_state = hle_d3d_sharp_fonts ? sharp_font_state(i) : 0;
        switch (kbm_atlas_base(px != NULL, builtin_texture_source(hash, tp.pack) != TEXSRC_GUEST, hle_d3d_sharp_fonts, sharp_state)) {
        case KBM_BASE_PACK: from = i ? "the texture pack's big font atlas" : "the texture pack's atlas"; break;
        case KBM_BASE_WAIT: return 0;
        case KBM_BASE_SHARP:
            if ((px = sharp_font_copy(i, &w, &h))) from = i ? "the sharp big font atlas" : "the sharp font atlas";
            break;
        default: break;
        }
        if (!px) {
            w = t->w;
            h = t->h;
        }
        if (!px && (px = malloc((size_t)t->w * t->h * 4)) && xtex_decode_level(t, 0, src, px)) {
            free(px);
            px = NULL;
        }
        if (!px) {
            kbm_atl[slot].failed = 1;
            printf("[glyphs] cannot read %s: Xbox button icons kept\n", i ? "the big font atlas" : "the font atlas");
            return 0;
        }
        if (pad) {
            if (hle_d3d_pad_icons) {   /* the PlayStation icons first, so the remap moves them */
                int n = i ? kbm_paint_ps_big_atlas(px, w, h, big_glyph_rects, big_glyph_nrects, hle_d3d_pad_icons)
                          : kbm_paint_ps_atlas(px, w, h, glyph_rects, glyph_nrects, hle_d3d_pad_icons);
                printf("[glyphs] PlayStation icons (%s): %d %s cells painted on %s (%ux%u)\n", kbm_ps_style_name(hle_d3d_pad_icons), n,
                       i ? "big font D-pad" : "button", from, w, h);
            }
            int shown[IN_COUNT];
            for (int c = 0; c < IN_COUNT; c++) shown[c] = input_pad_button_for_control(c);
            int n = i ? kbm_remap_big_atlas(px, w, h, big_glyph_rects, big_glyph_nrects, shown)
                      : kbm_remap_atlas(px, w, h, glyph_rects, glyph_nrects, shown);
            if (n || !hle_d3d_pad_icons)
                printf("[glyphs] controller remap: %d %s cells repainted on %s (%ux%u)\n", n, i ? "big font D-pad" : "button", from, w, h);
        } else {
            int keys[IN_COUNT];
            for (int c = 0; c < IN_COUNT; c++) keys[c] = input_key_for_control(c);
            if (i) {
                int n = kbm_paint_big_atlas(px, w, h, big_glyph_rects, big_glyph_nrects, keys);
                printf("[glyphs] keyboard/mouse D-pad icons: %d of %d big font cells painted on %s (%ux%u)\n", n,
                       kbm_big_cell_count(), from, w, h);
            } else {
                int n = kbm_paint_atlas(px, w, h, glyph_rects, glyph_nrects, keys);
                printf("[glyphs] keyboard/mouse icons: %d cells painted on %s (%ux%u)", n, from, w, h);
                for (int c = 0, first = 1; c < kbm_cell_count(); c++)
                    if (keys[kbm_cell(c)->control] < 0) {
                        printf("%s %s", first ? "; unbound, Xbox glyph kept:" : ",", kbm_cell(c)->what);
                        first = 0;
                    }
                printf("\n");
            }
        }
        kbm_atl[slot].chain = texpack_mips(px, w, h, &kbm_atl[slot].levels);
        free(px);
        if (!kbm_atl[slot].chain) {
            kbm_atl[slot].failed = 1;
            return 0;
        }
        kbm_atl[slot].w = w;
        kbm_atl[slot].h = h;
        if (i && sharp_font_state(1) == 3 && !kbm_big_needs_sharp()) {   /* painted: the uploaded sharp big font's chain can go */
            free(sf_job[1].chain);
            sf_job[1].chain = NULL;
        }
    }
    return renderer_texture_create(pad ? PADMAP_KEY(key) : KBM_KEY(key), RENDERER_TEX_BGRA8, kbm_atl[slot].w, kbm_atl[slot].h,
                                   kbm_atl[slot].levels, kbm_atl[slot].chain);
}

/* --sharp-faces (src/sharp_faces.h): a character skin's level 0 is decoded on this thread and
 * upscaled 2x (with its mip chain) on one worker thread, so no frame waits for the filter (~20 ms
 * a 256x256 skin, ~80 ms a 512x512 one). The result is cached under SF_KEY(key); until it is uploaded the draw samples
 * the guest texture under key. At most SHARP_FACES_PER_FRAME uploads per frame. Slots: 0 free,
 * 1 queued, 2 working, 3 ready, 4 failed. */
#define SF_SLOTS 32
#define SF_KEY(key) ((key) ^ 0x7368617270666163ull)
static struct {
    uint64_t key;
    uint32_t *px, w, h, levels;
    int state;
    double ms;
} sfc_req[SF_SLOTS];
static pthread_mutex_t sfc_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sfc_cv = PTHREAD_COND_INITIALIZER;
static struct {
    int worker;           /* 1 running, -1 could not start */
    uint32_t skins;       /* uploaded */
    uint32_t over_pack;   /* of them, skins the texture pack replaces (--sharp-faces-over-pack) */
    double filter_ms, upload_ms, max_ms;
    uint64_t bytes;       /* host bytes of the uploaded chains (B8G8R8A8) */
} sf;

static void *sfc_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&sfc_mx);
    for (;;) {
        int i = 0;
        while (i < SF_SLOTS && sfc_req[i].state != 1) i++;
        if (i == SF_SLOTS) {
            pthread_cond_wait(&sfc_cv, &sfc_mx);
            continue;
        }
        sfc_req[i].state = 2;
        uint32_t *px = sfc_req[i].px, w = sfc_req[i].w, h = sfc_req[i].h, levels = 0, *chain = NULL;
        sfc_req[i].px = NULL;
        pthread_mutex_unlock(&sfc_mx);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        uint32_t *up = sharp_faces_upscale(px, w, h);
        if (up) chain = texpack_mips(up, 2 * w, 2 * h, &levels);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        free(up);
        free(px);
        pthread_mutex_lock(&sfc_mx);
        sfc_req[i].px = chain;
        sfc_req[i].w = 2 * w;
        sfc_req[i].h = 2 * h;
        sfc_req[i].levels = levels;
        sfc_req[i].ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        sfc_req[i].state = chain ? 3 : 4;
    }
    return NULL;
}

/* The upscaled skin under key once ready (0 meanwhile, or when it cannot be made); pack_hash: the
 * pack hash it is drawn over (--sharp-faces-over-pack), 0 for a guest skin. */
static uint32_t sharp_skin(uint64_t key, const XTexInfo *t, const uint8_t *src, uint64_t pack_hash) {
    if (!sf.worker) {
        pthread_t th;
        sf.worker = pthread_create(&th, NULL, sfc_worker, NULL) ? -1 : 1;
        if (sf.worker > 0) pthread_detach(th);
        printf("[sharp-faces] %s\n", sf.worker > 0 ? "character skins are upscaled 2x on a worker thread"
                                                    : "cannot start the worker thread: skins are drawn as is");
    }
    if (sf.worker < 0) return 0;
    static uint32_t up_frame, ups;
    uint32_t frame = hle_d3d_frame_count();
    if (frame != up_frame) {
        up_frame = frame;
        ups = 0;
    }
    int i = 0, free_slot = -1;
    pthread_mutex_lock(&sfc_mx);
    for (; i < SF_SLOTS && !(sfc_req[i].state && sfc_req[i].key == key); i++)
        if (!sfc_req[i].state && free_slot < 0) free_slot = i;
    if (i == SF_SLOTS) {   /* not requested yet (a full queue retries on a later draw) */
        uint32_t *px = free_slot >= 0 ? malloc((size_t)t->w * t->h * 4) : NULL;
        if (px && !xtex_decode_level(t, 0, src, px)) {
            sfc_req[free_slot] = (typeof(sfc_req[0])){.key = key, .px = px, .w = t->w, .h = t->h, .state = 1};
            pthread_cond_signal(&sfc_cv);
        } else free(px);
        pthread_mutex_unlock(&sfc_mx);
        return 0;
    }
    int state = sfc_req[i].state;
    if (state < 3 || (state == 3 && ups >= SHARP_FACES_PER_FRAME)) {
        pthread_mutex_unlock(&sfc_mx);
        return 0;
    }
    uint32_t *px = sfc_req[i].px, w = sfc_req[i].w, h = sfc_req[i].h, levels = sfc_req[i].levels;
    double ms = sfc_req[i].ms;
    sfc_req[i].px = NULL;
    sfc_req[i].state = 0;
    pthread_mutex_unlock(&sfc_mx);
    if (!px) return 0;   /* failed: the next draw asks again */
    ups++;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t handle = renderer_texture_create(key, RENDERER_TEX_BGRA8, w, h, levels, px);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    free(px);
    if (handle) {
        uint64_t bytes = 0;
        for (uint32_t l = 0, lw = w, lh = h; l < levels; l++, lw = lw > 1 ? lw >> 1 : 1, lh = lh > 1 ? lh >> 1 : 1) bytes += (uint64_t)lw * lh * 4;
        sf.skins++;
        sf.bytes += bytes;
        if (pack_hash && !sf.over_pack++)
            printf("[sharp-faces] first skin over the texture pack: %016llx (the pack's version stays for other draws)\n",
                   (unsigned long long)pack_hash);
        sf.filter_ms += ms;
        sf.upload_ms += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        if (ms > sf.max_ms) sf.max_ms = ms;
        if (sf.skins == 1)
            printf("[sharp-faces] first skin %ux%u -> %ux%u: %.1f ms filter (worker), %.2f MB with mips\n", w / 2, h / 2, w, h, ms,
                   bytes / 1048576.0);
    }
    return handle;
}

static void sharp_faces_summary(void) {
    if (sf.skins)
        printf("[sharp-faces] %u skins upscaled (%u over the texture pack): %.1f ms filter each on average (max %.1f, worker thread), %.2f ms upload each, %.1f MB\n",
               sf.skins, sf.over_pack, sf.filter_ms / sf.skins, sf.max_ms, sf.upload_ms / sf.skins, sf.bytes / 1048576.0);
}

/* --neon-glow (src/neon_glow.h): the packs' emission masks are indexed with the packs (neon_index);
 * a draw whose stage texture has one (the first such stage) binds it for the renderer's emissive
 * variant (neon_bind). A mask is read and decoded by the texture pack loaders (tp_loader) like a
 * replacement, requested with its texture's replacement (or prefetched with it), so no frame waits
 * for the file; it is uploaded on this thread within the same per-frame budget and cached under
 * NEON_KEY(hash). Until then, and while the texture is still drawn from the guest's own image
 * (its replacement not uploaded yet), the draw is drawn as without a mask, so a sign never glows
 * with a mask that does not match the texels drawn. --texpack-sync reads masks when drawn. */
#define NEON_KEY(hash) ((hash) ^ 0x6e656f6e676c6f77ull)
static struct {
    int active;               /* --neon-glow with masks indexed */
    uint32_t loaded, failed;  /* masks uploaded / refused */
    uint32_t draws;           /* emissive draws bound */
    double load_ms, upload_ms;
} ng;

static void neon_index(void) {
    if (!hle_d3d_neon_glow.on) return;
    int total = 0;
    for (int i = 0; i < hle_d3d_texture_pack_count; i++) {
        int n = neon_glow_index_add(hle_d3d_texture_packs[i]);
        if (n > 0) total += n;
    }
    ng.active = tp_masks = total > 0;
    printf("[neon-glow] %d emission masks in %d pack%s%s\n", total, hle_d3d_texture_pack_count, hle_d3d_texture_pack_count == 1 ? "" : "s",
           total ? "" : ": nothing glows (add a pack with <hash>.emit.png / .emit.dds files)");
}

/* --neon-reflections: per sign texture (its content hash) the mask on a small grid, taken when the mask
 * is uploaded (neon_mask), and the colour of its lit parts, worked out once from the guest texture
 * when frame_neon first meets it (nr_colour). Host thread only. */
#define NR_SIGNS 128
typedef struct {
    uint64_t hash;
    uint8_t state;   /* 0 free, 1 grid only, 2 colour, 3 no colour (dark mask, unreadable texture) */
    uint8_t grid[NEON_REFL_GRID * NEON_REFL_GRID];
    float rgb[3], cover;
} NrSign;
static NrSign nr_sign[NR_SIGNS];
static NrSign *nr_find(uint64_t hash, int add) {
    for (uint32_t j = 0, i = (uint32_t)(hash % NR_SIGNS); j < NR_SIGNS; j++, i = (i + 1) % NR_SIGNS) {
        if (nr_sign[i].state && nr_sign[i].hash == hash) return &nr_sign[i];
        if (!nr_sign[i].state) {
            if (!add) return NULL;
            nr_sign[i].hash = hash;
            return &nr_sign[i];
        }
    }
    return NULL;
}
/* The colour of a sign drawn by op with stage s (hash: that stage's), or NULL. */
static const NrSign *nr_colour(const FrameOp *op, int s, uint64_t hash) {
    NrSign *e = nr_find(hash, 0);
    if (!e || e->state == 3) return NULL;
    if (e->state == 2) return e;
    XTexInfo t;
    e->state = 3;
    if (xtex_info(op->tex_format[s], op->tex_size[s], &t)) return NULL;
    uint32_t lv = 0;   /* the smallest level still at least the grid's size (cheap to decode) */
    while (lv + 1 < t.levels && xtex_level_w(&t, lv + 1) >= NEON_REFL_GRID && xtex_level_h(&t, lv + 1) >= NEON_REFL_GRID) lv++;
    const uint32_t w = xtex_level_w(&t, lv), h = xtex_level_h(&t, lv);
    uint32_t *px = w && h && (size_t)w * h <= 4096u * 4096u ? malloc((size_t)w * h * 4) : NULL;
    if (px && !xtex_decode_level(&t, lv, G(op->tex_data[s]), px) && !neon_refl_colour(e->grid, px, w, h, e->rgb, &e->cover)) e->state = 2;
    free(px);
    static int logged;
    if (e->state == 2 && logged++ < 8)
        printf("[neon-refl] sign %016llx: lit colour %.2f %.2f %.2f, %.0f%% lit\n", (unsigned long long)hash, e->rgb[0], e->rgb[1], e->rgb[2],
               100 * e->cover);
    return e->state == 2 ? e : NULL;
}

/* The renderer texture of a hash's mask (0 while it is being read, or when it cannot be). */
static uint32_t neon_mask(uint64_t hash) {
    uint32_t h = renderer_texture_find(NEON_KEY(hash));
    if (h) return h;
    TlImage img = {0};
    if (tp_loaders) {
        if (tp_fetch(hash, TL_MASK, &img) == 0) return 0;
    } else {   /* --texpack-sync (or no loader thread): read it now */
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        char *path = neon_glow_mask_path(hash);
        img.why = "no file";
        img.data = path ? neon_glow_load(path, &img.w, &img.h, &img.levels, &img.why) : NULL;
        free(path);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        img.ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    }
    if (!img.data) {
        if (ng.failed++ < 5) printf("[neon-glow] mask %016llx not used: %s\n", (unsigned long long)hash, img.why ? img.why : "unreadable");
        neon_glow_index_forget(hash);
        return 0;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (hle_d3d_neon_refl.on) {   /* --neon-reflections: the mask's grid for the sign's colour */
        NrSign *e = nr_find(hash, 1);
        if (e && !e->state && !neon_refl_grid(img.data, img.w, img.h, img.levels, e->grid)) e->state = 1;
    }
    h = renderer_texture_create(NEON_KEY(hash), RENDERER_TEX_L8, img.w, img.h, img.levels, img.data);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    free(img.data);
    const double up = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    tp_uploaded(up);
    if (!h) return 0;   /* the renderer refused it (logged there): the next draw reads it again */
    ng.load_ms += img.ms;
    ng.upload_ms += up;
    if (ng.loaded++ < 8)
        printf("[neon-glow] mask %016llx (%ux%u) applied: %.1f ms read (%s), %.1f ms upload\n", (unsigned long long)hash, img.w, img.h, img.ms,
               tp_loaders ? "loader" : "this thread", up);
    return h;
}

/* Binds the emission mask of a replayed draw (or none): the first stage whose content hash has a
 * mask; world draws on the frame target only (not offscreen, movies or characters). World draws
 * whose vertex program runs on the device (--gpu-vertex-programs: op->skin without op->character)
 * glow too, world-lit or not.
 * Strength and bloom weight carry the sign's flicker. */
static void neon_bind(const FrameOp *op, int off) {
    if (!ng.active) return;
    uint32_t mask = 0, stage = 0;
    uint64_t hash = 0;
    if (!off && !op->movie && !op->character)
        for (int s = 0; s < RENDERER_STAGES && !mask; s++) {
            if (!op->tex_key[s] || op->tex_surf[s] || !texpack_key_hash(op->tex_key[s], &hash) || !neon_glow_has(hash)) continue;
            /* The mask is drawn for the pack's replacement: not over the guest texture meanwhile. */
            if (tp.pack && texpack_has(hash) && !tp_resident(op->tex_key[s], hash)) continue;
            if ((mask = neon_mask(hash))) stage = (uint32_t)s;
        }
    if (!mask) {
        renderer_set_emission(0, 0, 0, 0);
        return;
    }
    const NeonGlowConfig *c = &hle_d3d_neon_glow;
    const float f = neon_glow_flicker(hash, hle_d3d_frame_count(), c->flicker);
    renderer_set_emission(mask, stage, c->strength * f, c->bloom * f);
    ng.draws++;
}

static void neon_summary(void) {
    if (ng.active)
        printf("[neon-glow] %u masks loaded (%.2f ms read + %.2f ms upload each on average), %u refused, %u emissive draws, %d masks indexed\n",
               ng.loaded, ng.loaded ? ng.load_ms / ng.loaded : 0.0, ng.loaded ? ng.upload_ms / ng.loaded : 0.0, ng.failed, ng.draws,
               neon_glow_index_count());
}

/* --normal-maps (src/normal_maps.h): the packs' companion maps (<hash>_n / <hash>_s) are indexed
 * after the packs (relief_index); a lit draw (--char-lighting characters, --world-lighting world,
 * skinned or not) whose stage texture has one (the first such stage) binds them for the renderer's
 * relief variant (relief_bind). The maps are read by the texture pack loaders like replacements,
 * requested with their texture's replacement (or prefetched with it), uploaded within the same
 * per-frame budget and cached under RELIEF_KEY(hash, kind); until then, and while the texture is
 * still drawn from the guest's own image (its replacement not uploaded yet), the draw is drawn as
 * without maps. Off (the default), or with no map indexed, relief_bind returns at its first test. */
#define RELIEF_KEY(hash, kind) ((hash) ^ ((kind) ? 0x7370656375006d61ull : 0x72656c6965666e6dull))
static struct {
    int active;                  /* --normal-maps with maps indexed */
    int bound;                   /* renderer_set_relief holds maps */
    uint32_t loaded, failed;     /* maps uploaded / refused */
    uint32_t draws;              /* relief draws bound */
    uint32_t heights;            /* --parallax: normal maps loaded with a height (normal_maps_image_height) */
    uint32_t glossy;             /* --reflections sssr: specular maps loaded glossy (normal_maps_image_glossy) */
    uint32_t parallax_draws;     /* world-lit relief draws bound with one */
    double load_ms, upload_ms;
} rm;

static void relief_index(void) {
    if (!hle_d3d_normal_maps.on) {
        if (hle_d3d_normal_maps.dry) printf("[dry-highlights] needs --normal-maps (and the relief pack): off\n");
        if (hle_d3d_normal_maps.parallax) printf("[parallax] needs --normal-maps (and the relief pack): off\n");
        return;
    }
    for (int i = 0; i < hle_d3d_texture_pack_count; i++) normal_maps_index_add(hle_d3d_texture_packs[i]);
    normal_maps_set_dds(renderer_has_bc());
    const int n = normal_maps_count(NORMAL_MAPS_RELIEF), m = normal_maps_count(NORMAL_MAPS_SPECULAR);
    rm.active = tp_relief = n + m > 0;
    printf("[normal-maps] %d textures with relief, %d with specular\n", n, m);
    if (!rm.active)
        printf("[normal-maps] no <hash>_n / <hash>_s maps in the texture packs (tools/normal_from_albedo.py makes them)\n");
    else if (!hle_d3d_char_light.on && !hle_d3d_world_light.on)
        printf("[normal-maps] needs --char-lighting and/or --world-lighting: only their per-pixel lights shade the relief\n");
    else if (!renderer_char_lights_on())
        printf("[normal-maps] the device runs no per-pixel lighting: the maps are not used\n");
    if (rm.active && hle_d3d_normal_maps.dry) {
        renderer_set_relief_dry(1);
        printf("[dry-highlights] material highlights from the specular maps' shine / gloss channels (tools/material_shine.py), strength %.2f, "
               "reach %.1f x the light radius\n", hle_d3d_normal_maps.dry_strength, RENDERER_DRY_REACH);
    }
    if (rm.active && hle_d3d_normal_maps.parallax) {
        renderer_set_parallax(hle_d3d_normal_maps.parallax_depth);
        printf("[parallax] depth %.3f of the texture, from the height in the normal maps' alpha (tools/parallax_height.py), on world-lit draws%s\n",
               hle_d3d_normal_maps.parallax_depth, hle_d3d_world_light.on ? "" : " (none: needs --world-lighting)");
    }
}
/* --dry-highlights: how far past a light's radius its highlights reach (1 = off), for the world
 * draws' light masks (world_light_mask_reach). */
static float relief_reach(void) { return rm.active && hle_d3d_normal_maps.dry ? RENDERER_DRY_REACH : 1.0f; }

/* The renderer texture of a hash's map of a kind (0 while it is being read, or when it cannot be). */
static uint32_t relief_map(uint64_t hash, int kind) {
    uint32_t h = renderer_texture_find(RELIEF_KEY(hash, kind));
    if (h) return h;
    TlImage img = {0};
    if (tp_loaders) {
        if (tp_fetch(hash, TL_RELIEF + kind, &img) == 0) return 0;
    } else {   /* --texpack-sync (or no loader thread): read it now */
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        char *path = normal_maps_path(hash, kind);
        TexpackImage ti;
        img.why = "no file";
        if (path && !texpack_load_image(path, &ti, &img.why)) img = (TlImage){ti.format, ti.w, ti.h, ti.levels, ti.data, 0, NULL, 0};
        free(path);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        img.ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    }
    if (!img.data) {
        if (rm.failed++ < 5)
            printf("[normal-maps] %s map %016llx not used: %s\n", kind ? "specular" : "normal", (unsigned long long)hash, img.why ? img.why : "unreadable");
        normal_maps_forget(hash, kind);   /* its other file (a .png under a failed .dds), if any, on a later draw */
        return 0;
    }
    static const uint32_t fmt[] = {[TEXPACK_BGRA8] = RENDERER_TEX_BGRA8, [TEXPACK_BC1] = RENDERER_TEX_BC1,
                                   [TEXPACK_BC3] = RENDERER_TEX_BC3, [TEXPACK_BC7] = RENDERER_TEX_BC7};
    /* --parallax: whether the normal map holds a height (its alpha), remembered per hash */
    if (kind == NORMAL_MAPS_RELIEF && hle_d3d_normal_maps.parallax) {
        const int ht = normal_maps_image_height(img.format, img.w, img.h, img.levels, img.data);
        normal_maps_set_height(hash, ht);
        rm.heights += ht != 0;
    }
    /* --reflections sssr's glass: whether the specular map mirrors the scene, remembered per hash */
    if (kind == NORMAL_MAPS_SPECULAR && hle_d3d_normal_maps.dry && renderer_wet_glass_on()) {
        const int g = normal_maps_image_glossy(img.format, img.w, img.h, img.levels, img.data);
        normal_maps_set_glossy(hash, g);
        rm.glossy += g != 0;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    h = renderer_texture_create(RELIEF_KEY(hash, kind), fmt[img.format], img.w, img.h, img.levels, img.data);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    free(img.data);
    const double up = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    tp_uploaded(up);
    if (!h) return 0;   /* the renderer refused it (logged there): the next draw reads it again */
    rm.load_ms += img.ms;
    rm.upload_ms += up;
    if (rm.loaded++ < 8)
        printf("[normal-maps] %s map %016llx (%ux%u) applied: %.1f ms read (%s), %.1f ms upload\n", kind ? "specular" : "normal",
               (unsigned long long)hash, img.w, img.h, img.ms, tp_loaders ? "loader" : "this thread", up);
    return h;
}

/* --hair: the mask of a lit character draw's skin (hair_record), uploaded once per version. */
#define HAIR_KEY(key, version) ((key) ^ 0x6861697200000000ull ^ (uint64_t)(version) * 0x9E3779B97F4A7C15ull)
static uint32_t hair_upload(void *ctx, uint32_t version, uint32_t mw, uint32_t mh, const uint32_t *mask) {
    const uint64_t k = HAIR_KEY(*(const uint64_t *)ctx, version);
    const uint32_t h = renderer_texture_find(k);
    return h ? h : renderer_texture_create(k, RENDERER_TEX_BGRA8, mw, mh, 1, mask);
}
static void hair_bind(const FrameOp *op, int lit) {
    uint32_t h = 0;
    if (lit && op->tex_key[0] && !op->tex_surf[0]) {
        uint64_t k = op->tex_key[0];
        h = hair_cache_with(k, hair_upload, &k);
    }
    renderer_set_hair(h);
    static int logged;
    if (h && !logged++) printf("[hair] hair shading in the lit character draws (strength %.2f)\n", hle_d3d_hair.strength);
}
/* --skin: likewise, the skin mask. */
#define SKIN_KEY(key, version) ((key) ^ 0x736b696e00000000ull ^ (uint64_t)(version) * 0x9E3779B97F4A7C15ull)
static uint32_t skin_upload(void *ctx, uint32_t version, uint32_t mw, uint32_t mh, const uint32_t *mask) {
    const uint64_t k = SKIN_KEY(*(const uint64_t *)ctx, version);
    const uint32_t h = renderer_texture_find(k);
    return h ? h : renderer_texture_create(k, RENDERER_TEX_BGRA8, mw, mh, 1, mask);
}
static void skin_bind(const FrameOp *op, int lit) {
    uint32_t h = 0;
    if (lit && op->tex_key[0] && !op->tex_surf[0]) {
        uint64_t k = op->tex_key[0];
        h = hair_cache_skin_with(k, skin_upload, &k);
    }
    renderer_set_skin(h);
    static int logged;
    if (h && !logged++) printf("[skin] skin shading in the lit character draws (strength %.2f)\n", hle_d3d_skin.strength);
}
/* --eyes, --cloth: the skin's three masks stacked (skin, eyes, cloth: hair_cache_mat_with) in place
 * of the skin mask. */
#define MAT_KEY(key, version) ((key) ^ 0x6d61740000000000ull ^ (uint64_t)(version) * 0x9E3779B97F4A7C15ull)
static uint32_t mat_upload(void *ctx, uint32_t version, uint32_t mw, uint32_t mh, const uint32_t *mask) {
    const uint64_t k = MAT_KEY(*(const uint64_t *)ctx, version);
    const uint32_t h = renderer_texture_find(k);
    return h ? h : renderer_texture_create(k, RENDERER_TEX_BGRA8, mw, mh, 1, mask);
}
static void mat_bind(const FrameOp *op, int lit) {
    uint32_t h = 0;
    if (lit && op->tex_key[0] && !op->tex_surf[0]) {
        uint64_t k = op->tex_key[0];
        h = hair_cache_mat_with(k, mat_upload, &k);
    }
    renderer_set_skin(h);
    renderer_set_eye_open(h ? 1 - op->eye_shut : 1);
    static int logged;
    if (h && !logged++)
        printf("[eyes/cloth] eye and cloth shading in the lit character draws (eyes %.2f, cloth %.2f%s)\n", hle_d3d_eyes.on ? hle_d3d_eyes.strength : 0,
               hle_d3d_cloth.on ? hle_d3d_cloth.strength : 0, hle_d3d_skin.on ? ", with the skin" : "");
}
/* Binds the relief maps of a replayed draw that the renderer lights per pixel (lit: a lit character
 * or world-lit draw on the frame target; world: the latter, whose normal map's height --parallax
 * marches), or none. */
static void relief_bind(const FrameOp *op, int off, int lit, int world) {
    if (!rm.active) return;
    uint32_t map[NORMAL_MAPS_KINDS] = {0, 0}, stage = 0;
    uint64_t mhash = 0;
    if (lit && !off && !op->movie)
        for (int s = 0; s < RENDERER_STAGES && !map[0] && !map[1]; s++) {
            uint64_t hash;
            if (!op->tex_key[s] || op->tex_surf[s] || !texpack_key_hash(op->tex_key[s], &hash)) continue;
            const int has_n = normal_maps_has(hash, NORMAL_MAPS_RELIEF), has_s = normal_maps_has(hash, NORMAL_MAPS_SPECULAR);
            if (!has_n && !has_s) continue;
            /* The maps are drawn for the pack's replacement: not over the guest texture meanwhile. */
            if (tp.pack && texpack_has(hash) && !tp_resident(op->tex_key[s], hash)) continue;
            if (has_n) map[0] = relief_map(hash, NORMAL_MAPS_RELIEF);
            if (has_s) map[1] = relief_map(hash, NORMAL_MAPS_SPECULAR);
            stage = (uint32_t)s;
            mhash = hash;
        }
    if (!map[0] && !map[1]) {
        if (rm.bound) renderer_set_relief(0, 0, 0, 0, 0, 0);
        rm.bound = 0;
        return;
    }
    const NormalMapsConfig *c = &hle_d3d_normal_maps;
    const int height = c->parallax && world && map[0] && normal_maps_height(mhash) == 1;
    renderer_set_relief_height(height);
    renderer_set_relief_glossy(map[1] && normal_maps_glossy(mhash) == 1);   /* --reflections sssr's glass */
    renderer_set_relief(map[0], map[1], stage, c->normal_strength, c->specular_strength * (c->dry ? c->dry_strength : 1.0f), c->baked);
    rm.bound = 1;
    rm.draws++;
    rm.parallax_draws += height;
}

static void relief_summary(void) {
    if (rm.active)
        printf("[normal-maps] %u maps loaded (%.2f ms read + %.2f ms upload each on average), %u refused, %u relief draws bound (%d relief, %d specular indexed)\n",
               rm.loaded, rm.loaded ? rm.load_ms / rm.loaded : 0.0, rm.loaded ? rm.upload_ms / rm.loaded : 0.0, rm.failed, rm.draws,
               normal_maps_count(NORMAL_MAPS_RELIEF), normal_maps_count(NORMAL_MAPS_SPECULAR));
    if (rm.active && hle_d3d_normal_maps.parallax)
        printf("[parallax] %u normal maps with a height loaded, %u parallax draws bound\n", rm.heights, rm.parallax_draws);
    if (rm.active && hle_d3d_normal_maps.dry && renderer_wet_glass_on())
        printf("[sssr] %u glossy specular maps loaded (glass, chrome, car paint: drawn into the glass's surface pass)\n", rm.glossy);
}

/* The sharp_faces_applies source of a stage texture (guest when nothing hashed it). */
static int sharp_faces_source(int hashed, uint64_t hash) {
    if (!hashed) return SHARP_FACES_SRC_GUEST;
    switch (builtin_texture_source(hash, tp.pack)) {
    case TEXSRC_BUILTIN: return SHARP_FACES_SRC_BUILTIN;
    case TEXSRC_PACK: return SHARP_FACES_SRC_PACK;
    default: return SHARP_FACES_SRC_GUEST;
    }
}

/* Whether the bytes of a recorded draw's stage texture changed since record time: 0 no, 1 yes
 * (skip the draw, or the guard is off), 2 yes and the same address changed on an earlier frame
 * too (draw the current bytes; tex_race_repeat). Counted, and logged within the rate limit. */
static int tex_race_detect(const FrameOp *op, int stage, const XTexInfo *t) {
    const double t0 = now_ms();
    int changed =
        tex_race_changed(op->tex_key[stage], op->tex_data[stage], op->tex_format[stage], op->tex_size[stage], G(op->tex_data[stage])) > 0;
    if (changed && tex_race_guard && tex_race_repeat(&tex_race_recent, op->tex_data[stage], tex_race_frame)) changed = 2;
    if (tex_race_note(&tex_race, changed, xtex_total_bytes(t), tex_race_frame))
        printf("[tex-race] frame %u: texture %08x (format %02x, %ux%u, %u levels, %u bytes) changed between record and replay: %s\n",
               tex_race_frame, op->tex_data[stage], t->code, t->w, t->h, t->levels, xtex_total_bytes(t),
               !tex_race_guard ? "cached anyway (--no-tex-race-guard)"
               : changed == 2  ? "changed again, drawn uncached under the recorded key (the current bytes, under their own key)"
                               : "not cached, skipped this frame");
    tex_race_ms += now_ms() - t0;
    return changed;
}
/* Called after (or right before) reading a texture's bytes for something cached by its key. *race:
 * 0 until a check of this lookup found the bytes changed, then tex_race_detect's verdict (not
 * checked again). Returns 1 when the caller must not cache what it read: changed, with the guard
 * on (the caller then returns tex_race_fallback). */
static int tex_race_check(const FrameOp *op, int stage, const XTexInfo *t, int *race) {
    if (!*race) *race = tex_race_detect(op, stage, t);
    return *race && tex_race_guard;
}
/* What a lookup whose texture changed under the replay binds: race 2 (changed again on a later
 * frame), the bytes there now under their own key (never the recorded one), resolved like a draw
 * recorded with them; otherwise, or when those bytes change again meanwhile, 0 with tex_race_skip
 * set (the draw is skipped this frame). */
static uint32_t resolve_texture(const FrameOp *op, int stage);
static uint64_t tex_race_drawn_key;   /* test hook: the key the last lookup used */
static uint32_t tex_race_fallback(const FrameOp *op, int stage, const XTexInfo *t, int race) {
    static int depth;
    static FrameOp cur;
    if (race != 2 || depth) {
        tex_race_skip = 1;
        return 0;
    }
    cur = *op;
    cur.tex_key[stage] = tex_race_key(op->tex_data[stage], op->tex_format[stage], op->tex_size[stage], G(op->tex_data[stage]), xtex_total_bytes(t));
    tex_race_drawn_key = cur.tex_key[stage];
    tex_race_drawn++;
    depth = 1;
    const uint32_t h = resolve_texture(&cur, stage);
    depth = 0;
    return h;
}

/* The stage-0 texture of a mod's mesh override (src/meshover.h): its PNG, read and uploaded with mips
 * once (key MO_KEY); 0 when it cannot be read (the draw keeps its own texture, reported once). */
#define MO_KEY(i) (0x6d6f645f6d657368ull ^ (uint64_t)(i))
static uint32_t meshover_texture(int i) {
    static uint8_t failed[64];
    uint32_t h = renderer_texture_find(MO_KEY(i));
    const MeshOverride *o = meshover_get(i);
    if (h || !o || !o->png || (i < 64 && failed[i])) return h;
    uint32_t w, hh, levels;
    uint32_t *px = texpack_png_read(o->png, &w, &hh);
    uint32_t *chain = px ? texpack_mips(px, w, hh, &levels) : NULL;
    if (chain) h = renderer_texture_create(MO_KEY(i), RENDERER_TEX_BGRA8, w, hh, levels, chain);
    if (!h) {
        if (i < 64) failed[i] = 1;
        printf("[meshover] cannot use the texture %s\n", o->png);
    }
    free(chain);
    free(px);
    return h;
}
/* Stage 0 of a draw with a mod's texture layers (src/texlayer.h): the draw's texture (a mesh
 * override's PNG, else the guest's level 0 decoded) with the layers blended over it, uploaded with
 * mips once per combination; 0 when it cannot be made (the draw keeps its texture without them). */
#define TL_KEY(k) (0x7465786c61796572ull ^ (k))
static uint32_t texlayer_texture(const FrameOp *op) {
    const uint64_t key = TL_KEY(texlayer_key(op->texlayer) * 31 + op->meshover);
    uint32_t h = renderer_texture_find(key);
    if (h) return h;
    uint32_t w = 0, hh = 0, levels;
    uint32_t *px = NULL;
    const MeshOverride *o = op->meshover ? meshover_get(op->meshover - 1) : NULL;
    if (o && o->png) {
        px = texpack_png_read(o->png, &w, &hh);
    } else {
        XTexInfo t;
        if (xtex_info(op->tex_format[0], op->tex_size[0], &t) || !op->tex_data[0]) return 0;
        w = t.w;
        hh = t.h;
        if (!(px = malloc((size_t)w * hh * 4))) return 0;
        int race = 0;
        if (xtex_decode_level(&t, 0, G(op->tex_data[0]), px) || tex_race_check(op, 0, &t, &race)) {
            free(px);
            return 0;
        }
    }
    if (!px) return 0;
    texlayer_compose(op->texlayer, px, w, hh, texpack_png_read);
    uint32_t *chain = texpack_mips(px, w, hh, &levels);
    if (chain) h = renderer_texture_create(key, RENDERER_TEX_BGRA8, w, hh, levels, chain);
    free(chain);
    free(px);
    return h;
}
/* Host texture of a recorded draw's stage: cached by key, otherwise the pack's replacement (by
 * content hash) or decoded from guest memory (DXT as BC blocks when the device samples them,
 * everything else to B8G8R8A8 on the CPU) and uploaded. When the guest bytes changed since the
 * draw was recorded (tex_race.h), nothing is cached and 0 is returned with tex_race_skip set. */
static uint32_t resolve_texture(const FrameOp *op, int stage) {
    static uint32_t *buf;
    static size_t cap;
    static int logged, failed;
    uint64_t key = op->tex_key[stage], hash = 0;
    if (!stage && op->texlayer) {   /* a mod's texture layers over the draw's texture */
        uint32_t h = texlayer_texture(op);
        if (h) return h;
    }
    if (!stage && op->meshover) {   /* a mod's mesh override drawn with its own texture */
        uint32_t h = meshover_texture(op->meshover - 1);
        if (h) return h;
    }
    if (!tp.init) texpack_init();
    XTexInfo t;
    int race = 0;
#define RACE_SKIP() return tex_race_fallback(op, stage, &t, race)
    int known = !xtex_info(op->tex_format[stage], op->tex_size[stage], &t);
    int hashed = known && !op->movie && (tp.dump || tp.pack || glyph_nrects || big_glyph_nrects || hle_d3d_sharp_fonts || ng.active);
    if (hashed) {
        hash = texture_hash(op, stage, &t, &race);
        if (race && tex_race_guard) RACE_SKIP();
        if (tp.dump) {
            texpack_dump_draw(hash);
            uint32_t frame = hle_d3d_frame_count();
            if (frame >= tp.flushed_at + 600) {
                tp.flushed_at = frame;
                texpack_dump_flush();
            }
        }
    }
    const int kbm_i = hash == KBM_ATLAS_HASH ? 0 : hash == KBM_BIG_ATLAS_HASH ? 1 : -1;
    const int kbm_slot = kbm_i < 0 ? -1 : kbm_icons_now(kbm_i) ? kbm_i : pad_icons_now(kbm_i) ? kbm_i + 2 : -1;
    if (known && kbm_slot >= 0) {
        uint32_t k = renderer_texture_find(kbm_slot >= 2 ? PADMAP_KEY(key) : KBM_KEY(key));
        if (!k && tex_race_check(op, stage, &t, &race)) RACE_SKIP();
        if (k || (k = kbm_atlas(kbm_slot, key, hash, &t, G(op->tex_data[stage])))) return k;
    }
    if (op->movie && known && t.code == 0x13 && t.pitch == t.w && stage < 3)   /* a movie plane (LIN_L8), uncached */
        return renderer_movie_plane(stage, t.w, t.h, G(op->tex_data[stage]));
    /* --sharp-faces: the upscaled skin has its own key, so a skin already cached for another draw
     * (or while the worker is busy) is still upgraded; until then the texture below (guest, or the
     * pack's). Skins are 64-512 DXT1 (never a built-in entry's 16x16), so only a pack can claim one
     * first, and with --sharp-faces-over-pack it does not: the upscale (from the guest texels) lives
     * under SF_KEY(key) while the pack version stays under key, so a world draw of the same texture
     * still gets the pack's. */
    int sf_source = known && !op->movie ? sharp_faces_source(hashed, hash) : SHARP_FACES_SRC_GUEST;
    if (known && !op->movie && sharp_faces_applies(&hle_d3d_sharp_faces, op->character, stage, sf_source, t.code, t.w, t.h)) {
        uint32_t k = renderer_texture_find(SF_KEY(key));
        if (!k && tex_race_check(op, stage, &t, &race)) RACE_SKIP();
        if (k || (k = sharp_skin(SF_KEY(key), &t, G(op->tex_data[stage]), sf_source == SHARP_FACES_SRC_PACK ? hash : 0))) return k;
    }
    uint32_t h = renderer_texture_find(key);
    if (h || !known) return h;
    /* --level-prefetch: a pack replacement is cached under its content hash (prefetched, or drawn
     * before under any key). */
    if (lp_on && hashed && (h = renderer_texture_find(TP_HASH_KEY(hash))) && builtin_texture_source(hash, tp.pack) == TEXSRC_PACK) {
        lp_drawn(hash, 0);
        return h;
    }
    /* Built-in replacements (src/builtin_tex.h) apply with no flag and win over the packs. With
     * nothing else hashing, only textures with an entry's format and size are hashed for them. */
    if (!hashed && builtin_texture_candidate(&t)) {
        hash = texture_hash(op, stage, &t, &race);
        if (race && tex_race_guard) RACE_SKIP();
        hashed = 1;
    }
    int source = hashed ? builtin_texture_source(hash, tp.pack) : TEXSRC_GUEST;
    if (source == TEXSRC_BUILTIN) {
        static int announced;
        const BuiltinTexture *b = builtin_texture_find(hash);
        uint32_t levels;
        const uint32_t *chain = builtin_texture_mips(b, &levels);
        if (chain && (h = renderer_texture_create(key, RENDERER_TEX_BGRA8, b->w, b->h, levels, chain))) {
            if (!announced) {
                announced = 1;
                printf("[texpack] built-in replacement %016llx (%ux%u) applied\n", (unsigned long long)hash, b->w, b->h);
            }
            return h;
        }
    }
    if (source == TEXSRC_GUEST && hle_d3d_sharp_fonts && sharp_font_atlas(hash)) {
        int pending;
        if (tex_race_check(op, stage, &t, &race)) RACE_SKIP();
        if ((h = sharp_font_texture(key, hash, &t, G(op->tex_data[stage]), &pending))) return h;
        if (pending) {
            key = TP_FALLBACK(key);
            if ((h = renderer_texture_find(key))) return h;
        }
    }
    if (source == TEXSRC_PACK) {
        int pending;
        if ((h = replace_texture(key, hash, &pending))) {
            lp_drawn(hash, 1);
            return h;
        }
        if (pending) {
            lp_drawn(hash, 2);
            perf_tex_misses++;   /* drawn with the guest texture while the replacement loads */
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
        if (tex_race_check(op, stage, &t, &race)) RACE_SKIP();   /* the upload reads src */
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
        if (tex_race_check(op, stage, &t, &race)) RACE_SKIP();   /* after the decode: a change during it counts too */
        h = renderer_texture_create(key, RENDERER_TEX_BGRA8, t.w, t.h, levels, buf);
    }
    if (!h && !failed++) printf("[renderer] texture upload failed: %s\n", renderer_error());
    if (h && !logged++) printf("[renderer] textures: DXT uploaded as %s\n", renderer_has_bc() ? "BC1-3 blocks" : "B8G8R8A8 (CPU decode)");
    return h;
#undef RACE_SKIP
}

/* Test hooks (tex_race.h): one recorded draw's stage 0, replayed on the calling thread. */
static FrameOp tex_race_probe;
uint64_t hle_d3d_tex_race_record(void) {
    XTexInfo ti;
    int ok;
    memset(&tex_race_probe, 0, sizeof tex_race_probe);
    return stage_texture(&tex_race_probe, 0, &ti, &ok);
}
int hle_d3d_tex_race_replay(uint32_t frame, uint64_t *drawn_key) {
    tex_race_skip = 0;
    tex_race_frame = frame;
    tex_race_drawn_key = tex_race_probe.tex_key[0];
    if (tex_race_probe.tex_key[0]) resolve_texture(&tex_race_probe, 0);
    *drawn_key = tex_race_drawn_key;
    return tex_race_skip;
}
void hle_d3d_tex_race_counts(uint64_t *checks, uint64_t *races) {
    *checks = tex_race.checks;
    *races = tex_race.races;
}

/* Replays a frame into out (the renderer's output size), or presents it when out is NULL: the
 * back buffer as it was at the first draw is the initial color (px, the posted frame, when it has
 * no draws), earlier clears only contribute their depth part, later operations run in order.
 * Returns 0 when out holds the frame (or it was presented). */
/* Host surface of a registry entry (created on first use; renderer_surface keeps it by key). */
static uint32_t rt_handle(uint8_t idx) {
    static uint32_t handle[MAX_RT_SURFACES][2];   /* render-scaled, guest size (a blur-chain target) */
    if (!idx) return 0;
    if (RT_IS_PREV(idx)) return renderer_prev_frame();
    const RtSurface *r = &rt_surf[idx - 1];
    const int g = __atomic_load_n(&r->guest_size, __ATOMIC_RELAXED) != 0;
    if (!handle[idx - 1][g])
        handle[idx - 1][g] = renderer_surface((uint64_t)r->pa << 8 | r->kind, r->kind | (g ? RENDERER_SURFACE_GUEST_SIZE : 0), (int)r->w, (int)r->h);
    return handle[idx - 1][g];
}
/* The depth projection of a frame (clip z = A w - B; ab = {A, B}) fitted from its depth-tested
 * back-buffer draws (rain_depth_fit per draw, rain_depth_pick of them); returns the draws fitted. */
static uint32_t frame_depth_fit(const FrameList *l, float ab[2]) {
    enum { FITS = DEPTH_FIT_DRAWS, FIT_VERTS = DEPTH_FIT_VERTS };
    static float fa[FITS], fb[FITS];
    uint32_t nf = 0;
    for (uint32_t i = 0; i < l->nops && nf < FITS; i++) {
        const FrameOp *op = &l->ops[i];
        if (op->kind != OP_DRAW || op->target_c || op->target_z || !(op->draw_flags & RENDERER_DEPTH_TEST) || op->nv < 3 || (op->skin && !op->fit_pos)) continue;
        float f[2];
        if (!rain_depth_fit(&l->v[op->first_vertex].z, sizeof(RVertex) / sizeof(float), op->nv < FIT_VERTS ? op->nv : FIT_VERTS, f)) {
            fa[nf] = f[0];
            fb[nf++] = f[1];
        }
    }
    rain_depth_pick(fa, fb, nf, ab);
    return nf;
}
/* frame_depth_fit at most once per replayed frame: rain, SSAO, lamp lights, splashes, wet ground and soft
 * particles all ask for it while render_frame prepares its passes, and the frame list does not
 * change in between (up to 256 draws x 256 vertices fitted each time). */
typedef struct {
    int done;
    uint32_t nf;
    float ab[2];
} DepthFit;
static uint32_t frame_depth(const FrameList *l, DepthFit *d, float ab[2]) {
    if (!d->done) {
        d->nf = frame_depth_fit(l, d->ab);
        d->done = 1;
    }
    ab[0] = d->ab[0];
    ab[1] = d->ab[1];
    return d->nf;
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
static int frame_rain(const FrameList *l, DepthFit *df, const RainLayer *layers, const float (*scroll)[2], int n, RRain *r) {
    if (!n) return 0;
    float ab[2];
    const uint32_t nf = frame_depth(l, df, ab);
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
/* The end of the opaque world (frame_world_end), or l->nops; p = the frame's depth projection
 * (frame_depth: fitted at most once per frame; also in ab, with the draws fitted in *nf_out) and the
 * camera's x/y scale (that draw's, else the nearest opaque draw before it that has one, else the
 * usual camera's). */
static uint32_t frame_world_proj(const FrameList *l, DepthFit *df, RSsao *p, float ab[2], uint32_t *nf_out) {
    const uint8_t *kind;
    const uint32_t at = frame_world_end(l, &kind);
    if (at == l->nops) return at;
    const uint32_t nf = frame_depth(l, df, ab);
    *nf_out = nf;
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
    return at;
}
/* --ssao: the operation after which the occlusion runs (frame_world_proj), or l->nops. */
static uint32_t frame_ssao(const FrameList *l, DepthFit *df, RSsao *p) {
    float ab[2];
    uint32_t nf = 0;
    const uint32_t at = frame_world_proj(l, df, p, ab, &nf);
    if (at == l->nops) return at;
    static int logged;
    if (!logged && nf && (logged = 1))   /* the first frame with a fitted projection */
        printf("[ssao] occlusion after the opaque world (op %u of %u); depth projection A %.5f B %.5f from %u draws, scale %.4f %.4f\n", at,
               l->nops, ab[0], ab[1], nf, p->px, p->py);
    return at;
}
typedef struct {
    const float *view;
    float px, py;
} LampCam;
/* lamp_track_update's in_view: the point lies in front of the camera and inside the screen. */
static int lamp_in_view(const float pos[3], void *ctx) {
    const LampCam *c = ctx;
    float v[3];
    for (int i = 0; i < 3; i++) v[i] = pos[0] * c->view[i] + pos[1] * c->view[4 + i] + pos[2] * c->view[8 + i] + c->view[12 + i];
    const float d = -v[2];
    return d > 0.1f && fabsf(v[0] * c->px) <= d && fabsf(v[1] * c->py) <= d;
}
/* --neon-reflections: the frame's glowing signs (the draws neon_bind gives a mask: on the frame target,
 * not movies or characters, depth-tested) as lights in the world: each draw's vertices (CPU clip
 * positions; a device-run draw only once its key is known, FrameOp.neon_pos) through the frame's
 * camera into a sprite of its lit colour (nr_colour), the sprites of one sign merged and followed
 * across frames by a tracker of their own (held while out of view, as the lamps). Returns the lights
 * in out (at most NEON_REFL_CAP), 0 without a camera. */
#define NR_FRAME_SPRITES 64
#define NR_MAX_VERTS 1024
static uint32_t nr_frame_signs;   /* --stats: sign draws of the last frame_neon */
static uint32_t frame_neon(const FrameList *l, LampLight *out) {
    static LampTracker track;
    static int reset;
    if (!reset++) lamp_tracker_reset(&track);
    if (!l->has_cam || !(l->cam_proj[15] == 0 && fabsf(l->cam_proj[11]) == 1)) return 0;
    LampSprite spr[NR_FRAME_SPRITES];
    uint32_t n = 0;
    nr_frame_signs = 0;
    for (uint32_t i = 0; i < l->nops && n < NR_FRAME_SPRITES; i++) {
        const FrameOp *op = &l->ops[i];
        if (op->kind != OP_DRAW || op->target_c || op->target_z || op->movie || op->character || !(op->draw_flags & RENDERER_DEPTH_TEST) || op->nv < 3)
            continue;
        uint64_t hash = 0;
        int s = 0;
        for (; s < RENDERER_STAGES; s++) {   /* as neon_bind, without loading the mask */
            if (!op->tex_key[s] || op->tex_surf[s] || !texpack_key_hash(op->tex_key[s], &hash) || !neon_glow_has(hash)) continue;
            if (tp.pack && texpack_has(hash) && !tp_resident(op->tex_key[s], hash)) continue;
            if (renderer_texture_find(NEON_KEY(hash))) break;
        }
        if (s == RENDERER_STAGES) continue;
        nr_key_add(op->tex_key[s]);
        nr_frame_signs++;
        if (op->skin && !op->fit_pos && !op->neon_pos) continue;   /* no CPU positions (yet) */
        const NrSign *e = nr_colour(op, s, hash);
        if (!e) continue;
        uint32_t nv = op->nv < NR_MAX_VERTS ? op->nv : NR_MAX_VERTS;
        if (op->skin && nv > DEPTH_FIT_VERTS) nv = DEPTH_FIT_VERTS;
        if (!neon_refl_sprite(&l->v[op->first_vertex].x, sizeof(RVertex) / sizeof(float), nv, l->cam_view, l->cam_proj, e->rgb, &spr[n])) n++;
    }
    n = lamp_cluster(spr, n);
    const float *v = l->cam_view;
    const float eye[3] = {-(v[12] * v[0] + v[13] * v[1] + v[14] * v[2]), -(v[12] * v[4] + v[13] * v[5] + v[14] * v[6]),
                          -(v[12] * v[8] + v[13] * v[9] + v[14] * v[10])};
    LampCam cam = {v, l->cam_proj[0], l->cam_proj[5]};
    return lamp_track_update(&track, spr, n, eye, out, NEON_REFL_CAP, lamp_in_view, &cam);
}
/* --lamp-lights: the operation after which the lamp lights are added (the end of the opaque world,
 * frame_world_end; drawn before the occlusion there), or l->nops when the frame has none or no
 * light is lit yet; p = the depth projection and camera scale (as frame_ssao) and the lights:
 * the frame's halo sprites merged per lamp (lamp_cluster) and followed across frames
 * (lamp_track_update: stable ids, fades, the cap), in the right-handed view space of the frame's
 * last halo draw. */
/* --level-lights: the point (radius r) lights the view: its sphere reaches in front of the camera
 * and into the screen's frustum (the side planes through the eye, in the view's x/z and y/z). */
static int level_in_view(const float pos[3], float r, void *ctx) {
    const LampCam *c = ctx;
    float v[3];
    for (int i = 0; i < 3; i++) v[i] = pos[0] * c->view[i] + pos[1] * c->view[4 + i] + pos[2] * c->view[8 + i] + c->view[12 + i];
    const float d = -v[2];
    if (d + r < 0.1f) return 0;
    const float sx = sqrtf(1 + c->px * c->px), sy = sqrtf(1 + c->py * c->py);   /* plane normals' lengths */
    return fabsf(v[0] * c->px) - d <= r * sx && fabsf(v[1] * c->py) - d <= r * sy;
}
/* --light-shafts with --level-lights: the halo lights (view space, as p->l) the shafts hang from. */
static RLamp frame_halo_lamp[RENDERER_LAMP_MAX];
static uint32_t frame_halo_n;
/* --lamp-shadows: frame_lamp's lights in the world (as p->l) and its camera position. */
static LampShadowLight frame_lamp_world[RENDERER_LAMP_MAX];
static float frame_lamp_eye[3];
static float frame_lamp_up[2][3];   /* world up in view space: the lamps' camera, the fires' (--light-shafts) */
static uint32_t frame_lamp_n;        /* p->l[0 .. frame_lamp_n - 1] are lamps, the rest fires */
static float frame_lamp_vw[16];      /* --volfog: the view p->l is in (the lamps', the fires' when only fires light the frame) */
/* --volfog with --level-lights: the frame's halo lights (world), for the lamps the level lists no light for. */
static LampLight frame_vf_halo[LAMP_MAX_LIGHTS];
static uint32_t frame_vf_halo_n;
/* --wet-light-streaks: frame_lamp's lights in the world with their colour (as p->l), the lamps
 * first (frame_lamp_n of them), then the fires. */
static LampLight frame_wet_light[RENDERER_LAMP_MAX];
static uint32_t frame_lamp(const FrameList *l, DepthFit *df, RLampLights *p) {
    static LampTracker track, fire_track;
    static int reset;
    if (!reset++) {
        lamp_tracker_reset(&track);
        lamp_tracker_reset(&fire_track);
    }
    /* --fire-light: the fires go the same way with their own tracker and camera (the flame draws'). */
    const int fire = hle_d3d_fire.on && (l->has_fire_view || fire_track.n);
    /* --level-lights: the level's own lights when the read found any (with the frame's 3D camera),
     * else the halos'. */
    static LevelTracker level_track;
    static int level_used;
    const int level = hle_d3d_level.on && l->has_level && l->level.n && l->has_level_view;
    /* The camera is the halo draws': a frame without any (no lamp on screen) adds no light, and its
     * update only fades or holds the tracks with the last camera. */
    if (!l->has_lamp_view && !track.n && !fire && !level && !level_track.n) return l->nops;
    static float view[16], fire_view[16];
    if (l->has_lamp_view) memcpy(view, l->lamp_view, sizeof view);
    if (l->has_fire_view) memcpy(fire_view, l->fire_view, sizeof fire_view);
    else if (l->has_lamp_view) memcpy(fire_view, l->lamp_view, sizeof fire_view);   /* no flame drawn: this frame's camera */
    RSsao sp;
    float ab[2];
    uint32_t nf = 0;
    const uint32_t at = frame_world_proj(l, df, &sp, ab, &nf);
    if (at == l->nops) return at;
    LampSprite spr[LAMP_FRAME_SPRITES];
    uint32_t n = l->lamp_n;
    memcpy(spr, l->lamp_spr, n * sizeof spr[0]);
    n = lamp_cluster(spr, n);
    /* The camera in the world: view = world x V (rows), so eye = -t R^T. */
    const float eye[3] = {-(view[12] * view[0] + view[13] * view[1] + view[14] * view[2]),
                          -(view[12] * view[4] + view[13] * view[5] + view[14] * view[6]),
                          -(view[12] * view[8] + view[13] * view[9] + view[14] * view[10])};
    LampLight lights[LAMP_MAX_LIGHTS];
    LampCam cam = {view, sp.px, sp.py};
    uint32_t nl = lamp_track_update(&track, spr, n, eye, lights, RENDERER_LAMP_MAX < LAMP_MAX_LIGHTS ? RENDERER_LAMP_MAX : LAMP_MAX_LIGHTS,
                                    lamp_in_view, &cam);
    if (!l->has_lamp_view) nl = 0;
    /* --level-lights: the lamps of the frame (else the halo lights above); the halo lights stay
     * the shafts'. A frame without a usable read fades the level lights out under the halos'. */
    static float lview[16];
    LampLight llights[LAMP_MAX_LIGHTS];
    uint32_t nlev = 0;
    if (hle_d3d_level.on && (level || level_track.n)) {
        if (level) memcpy(lview, l->level_view, sizeof lview);
        const float *v = lview;
        const float leye[3] = {-(v[12] * v[0] + v[13] * v[1] + v[14] * v[2]), -(v[12] * v[4] + v[13] * v[5] + v[14] * v[6]),
                               -(v[12] * v[8] + v[13] * v[9] + v[14] * v[10])};
        LampCam lcam = {lview, sp.px, sp.py};
        nlev = level_lights_select(&level_track, level ? &l->level : NULL, leye, level_in_view, &lcam, llights,
                                   hle_d3d_level.cap < RENDERER_LAMP_MAX ? hle_d3d_level.cap : RENDERER_LAMP_MAX);
        if (level && !level_used++)
            printf("[level-lights] %u of the level's %u point lights in use (cap %u); the halos are the fallback\n", nlev, l->level.n,
                   hle_d3d_level.cap);
        if (hle_d3d_stats && !(hle_d3d_frame_count() % 600))
            printf("[level-lights] frame %u: %u lights in use of %u (%u of the player's surroundings), %u halo lights %s\n", hle_d3d_frame_count(), nlev,
                   level ? l->level.n : 0, level ? l->level.in_lists : 0, nl, level ? "unused" : "in use");
    }
    const int use_level = level || (!nl && nlev);   /* fading out with no halo light to take over */
    frame_halo_n = 0;
    if (use_level && hle_d3d_shafts.on)
        for (uint32_t i = 0; i < nl; i++) {
            const LampLight *li = &lights[i];
            float v[3];
            for (int c = 0; c < 3; c++) v[c] = li->pos[0] * view[c] + li->pos[1] * view[4 + c] + li->pos[2] * view[8 + c] + view[12 + c];
            frame_halo_lamp[frame_halo_n++] = (RLamp){{v[0], v[1], v[2]}, li->radius, {li->rgb[0], li->rgb[1], li->rgb[2]}, li->intensity};
        }
    frame_vf_halo_n = 0;
    if (use_level && hle_d3d_volfog.on) {   /* --volfog: the halo lamps, deduped against the level's in frame_volfog */
        memcpy(frame_vf_halo, lights, nl * sizeof lights[0]);
        frame_vf_halo_n = nl;
    }
    if (use_level) {
        memcpy(lights, llights, nlev * sizeof lights[0]);
        nl = nlev;
    }
    const float *lamp_vw = use_level ? lview : view;
    LampLight flights[FIRE_MAX_LIGHTS];
    uint32_t nfire = 0;
    static double fire_ms_sum;   /* --stats: host time of the fire lights (cluster, track, shade) */
    static uint32_t fire_calls;
    float nearest = 0;   /* --stats: the nearest fire light's distance to the camera */
    if (fire) {
        const double t0 = now_ms();
        uint32_t fn = l->fire_n;
        memcpy(spr, l->fire_spr, fn * sizeof spr[0]);
        fn = fire_cluster(spr, fn);
        const float *v = fire_view;
        const float feye[3] = {-(v[12] * v[0] + v[13] * v[1] + v[14] * v[2]), -(v[12] * v[4] + v[13] * v[5] + v[14] * v[6]),
                               -(v[12] * v[8] + v[13] * v[9] + v[14] * v[10])};
        LampCam fcam = {fire_view, sp.px, sp.py};
        nfire = lamp_track_update(&fire_track, spr, fn, feye, flights, FIRE_MAX_LIGHTS, lamp_in_view, &fcam);
        nearest = 1e30f;
        for (uint32_t i = 0; i < nfire; i++) {
            const float dx = flights[i].pos[0] - feye[0], dy = flights[i].pos[1] - feye[1], dz = flights[i].pos[2] - feye[2];
            nearest = fminf(nearest, sqrtf(dx * dx + dy * dy + dz * dz));
        }
        /* The flicker's time: the guest clock (frame-rate independent, still while the game is
         * paused), else the host's. */
        static uint32_t last_ms;
        static int have_ms;
        if (l->has_fire_ms) last_ms = l->fire_ms, have_ms = 1;
        const double t = have_ms ? last_ms / 1000.0 : now_ms() / 1000.0;
        /* Every light is scaled by p->strength (the lamp strength): the fire's own strength instead. */
        fire_light_shade(flights, nfire, t, hle_d3d_fire.strength / hle_d3d_lamp.strength);
        if (!l->has_fire_view && !l->has_lamp_view) nfire = 0;   /* no camera this frame */
        fire_ms_sum += now_ms() - t0;
        fire_calls++;
    }
    if (!nl && !nfire && !frame_halo_n) return l->nops;
    p->depth_a = sp.depth_a;
    p->depth_b = sp.depth_b;
    p->px = sp.px;
    p->py = sp.py;
    p->strength = hle_d3d_lamp.strength;
    p->char_gain = LAMP_CHAR_GAIN;
    if (nl + nfire > RENDERER_LAMP_MAX) nl = RENDERER_LAMP_MAX - nfire;   /* the fires are the nearest lights: they stay */
    p->n = nl + nfire;
    frame_lamp_n = nl;
    for (int c = 0; c < 3; c++) frame_lamp_up[0][c] = view[8 + c], frame_lamp_up[1][c] = fire_view[8 + c];
    for (uint32_t i = 0; i < nl + nfire; i++) {
        const LampLight *li = i < nl ? &lights[i] : &flights[i - nl];
        const float *w = li->pos, *vw = i < nl ? lamp_vw : fire_view;
        float v[3];
        for (int c = 0; c < 3; c++)   /* the title's view space is right-handed already (the camera looks down -z) */
            v[c] = w[0] * vw[c] + w[1] * vw[4 + c] + w[2] * vw[8 + c] + vw[12 + c];
        p->l[i] = (RLamp){{v[0], v[1], v[2]}, li->radius, {li->rgb[0], li->rgb[1], li->rgb[2]}, li->intensity};
        frame_lamp_world[i] = (LampShadowLight){{w[0], w[1], w[2]}, li->radius, li->intensity};
        frame_wet_light[i] = *li;
    }
    /* --light-walls: the view back to the world for the walls' test (the lamps' camera; the flames'
     * when only fires light the frame). */
    p->walls = light_walls_wanted() && lamp_view_to_world(nl ? lamp_vw : fire_view, p->to_world) == 0;
    memcpy(frame_lamp_vw, nl ? lamp_vw : fire_view, sizeof frame_lamp_vw);
    for (int c = 0; c < 3; c++) p->up_view[c] = (nl ? lamp_vw : fire_view)[8 + c];   /* --hair: world up (z) in the lights' view space */
    if (use_level)
        for (int c = 0; c < 3; c++)
            frame_lamp_eye[c] = -(lview[12] * lview[4 * c] + lview[13] * lview[4 * c + 1] + lview[14] * lview[4 * c + 2]);
    else memcpy(frame_lamp_eye, eye, sizeof eye);
    static int logged, fire_logged;
    if (nl && !logged++)
        printf("[lamp] %u light%s from %u halo quads after the opaque world (op %u of %u); depth projection A %.5f B %.5f from %u draws\n", nl,
               nl == 1 ? "" : "s", l->lamp_n, at, l->nops, ab[0], ab[1], nf);
    if (nfire && !fire_logged++) printf("[fire-light] %u fire light%s from %u flame quads\n", nfire, nfire == 1 ? "" : "s", l->fire_n);
    if (nfire && hle_d3d_stats && !(hle_d3d_frame_count() % 600))
        printf("[fire-light] frame %u: %u fire lights (+%u lamp) from %u flame quads, %u fires tracked, nearest %.1f m; %.1f us/frame host\n",
               hle_d3d_frame_count(), nfire, nl, l->fire_n, fire_track.n, nearest, fire_calls ? 1000 * fire_ms_sum / fire_calls : 0.0);
    return at;
}
/* --light-shafts: the frame's lights (frame_lamp's p: lamps, then fires) as shaft volumes in out: a
 * lamp hangs a cone along world down, a fire a glow rising along world up, each light_shafts_length
 * of its radius long, colour x intensity; the fires are kept first when the list is longer than
 * RENDERER_SHAFT_MAX. The air's density follows the title's rain layers (light_shafts_rain_step,
 * eased over frames). Returns 1 with a light to draw. */
static int frame_shafts(const FrameList *l, const RLampLights *p, RLightShafts *out) {
    static float rain;
    rain = light_shafts_rain_step(rain, l->sky_rain > 0);
    /* --level-lights: the lamps are the halo lights (frame_halo_lamp), not the level's lights. */
    const int halo = hle_d3d_level.on && frame_halo_n;
    const uint32_t nlist = frame_lamp_n < p->n ? frame_lamp_n : p->n;
    const uint32_t lamps = halo ? frame_halo_n : nlist, fires = p->n - nlist;
    const uint32_t keep_fires = fires < RENDERER_SHAFT_MAX ? fires : RENDERER_SHAFT_MAX;
    const uint32_t keep_lamps = lamps < RENDERER_SHAFT_MAX - keep_fires ? lamps : RENDERER_SHAFT_MAX - keep_fires;
    out->depth_a = p->depth_a;
    out->depth_b = p->depth_b;
    out->px = p->px;
    out->py = p->py;
    out->density = light_shafts_density(hle_d3d_shafts.strength, rain);
    out->max_dist = 80;
    out->n = 0;
    for (uint32_t k = 0; k < keep_lamps + keep_fires; k++) {
        const int fire = k >= keep_lamps;
        const RLamp *li = !fire ? (halo ? &frame_halo_lamp[k] : &p->l[k]) : &p->l[nlist + (k - keep_lamps)];
        if (!(li->intensity > 0)) continue;
        RShaft *sh = &out->l[out->n++];
        const float *up = frame_lamp_up[fire];
        for (int c = 0; c < 3; c++) {
            sh->pos[c] = li->pos[c];
            sh->rgb[c] = li->rgb[c] * li->intensity;
            sh->axis[c] = fire ? up[c] : -up[c];
        }
        sh->length = light_shafts_length(li->radius);
        sh->kind = (uint32_t)fire;
    }
    static int logged, rain_logged;
    if (out->n && !logged++)
        printf("[shafts] %u light shaft%s (%u lamps, %u fires) after the opaque world; density %.3f\n", out->n, out->n == 1 ? "" : "s", keep_lamps,
               keep_fires, out->density);
    if (rain >= 1 && !rain_logged++) printf("[shafts] the title's rain thickens the air: density %.3f\n", out->density);
    return out->n > 0;
}
/* --volfog: the frame's volumetric fog from frame_lamp's lights (p: lamps, then fires, in
 * frame_lamp_vw's view) and the neon signs of frame_neon (world): colour x intensity x the lamp
 * strength x VOLFOG_LIGHT_GAIN, chosen by volfog_select up to the quality's cap. The density follows
 * the title's rain layers (light_shafts_rain_step), the damp streets' level and indoors (no sky
 * dome; eased over half a second); the far plane and fade follow the title's fog; the history is
 * kept while the camera moves smoothly between consecutive frames (volfog_reproject). Returns 1 with
 * a light to draw. */
static int frame_sky_stage(const FrameList *l, SkyStage *stage);
/* --volfog: the level's ground (world z, up) at world (x, y): the higher of the material map's two
 * surfaces there lying below zmax. 0 outside the map or without one. */
static int vf_ground_at(const MatMap *m, float x, float y, float zmax, float *hz) {
    if (!m || !m->texels || !(m->cell > 0)) return 0;
    const float fx = floorf((x - m->x0) / m->cell), fy = floorf((y - m->y0) / m->cell);
    if (!(fx >= 0 && fy >= 0 && fx < m->w && fy < m->h)) return 0;
    const uint16_t *t = m->texels + 2 * ((size_t)fy * m->w + (size_t)fx);
    int found = 0;
    for (int k = 0; k < 2; k++) {
        const int hb = t[k] & 0xfff;
        if (!hb) continue;
        const float z = m->z0 + (float)(hb - 1) * m->zstep;
        if (z < zmax && (!found || z > *hz)) *hz = z, found = 1;
    }
    return found;
}
/* --volfog: the ground under the camera (world z): the median of the level's surfaces below it at the
 * camera and VF_GROUND_AHEAD view units ahead of it (horizontally), the camera's (tw: view to world)
 * forward along -z. 0 without a map or a surface there. */
#define VF_GROUND_CLEAR 0.3f
static int vf_ground(const float tw[3][4], float *gz) {
    const MatPending *cur = mat_cur;
    if (!cur || !cur->map.texels) return 0;
    static const float ahead[4] = {0, 3, 6, 10};
    const float eye[3] = {tw[0][3], tw[1][3], tw[2][3]};
    float fx = -tw[0][2], fy = -tw[1][2];
    const float fl = sqrtf(fx * fx + fy * fy);
    if (fl > 1e-3f) fx /= fl, fy /= fl;
    else fx = fy = 0;
    float h[4];
    int n = 0;
    for (int i = 0; i < 4; i++)
        if (vf_ground_at(&cur->map, eye[0] + fx * ahead[i], eye[1] + fy * ahead[i], eye[2] - VF_GROUND_CLEAR, &h[n])) n++;
    if (!n) return 0;
    for (int i = 1; i < n; i++)   /* sort the few heights */
        for (int j = i; j > 0 && h[j] < h[j - 1]; j--) {
            const float t = h[j];
            h[j] = h[j - 1], h[j - 1] = t;
        }
    *gz = n & 1 ? h[n / 2] : 0.5f * (h[n / 2 - 1] + h[n / 2]);
    return 1;
}
static int frame_volfog(const FrameList *l, const RLampLights *p, const LampLight *signs, uint32_t nsigns, float damp_level, RVolfog *out) {
    static float rain, indoor, prev_view[16], prev_px, prev_py, ground_z;
    static uint32_t last_frame, jit_frame;
    static int have_prev, have_ground;
    const uint32_t frame = hle_d3d_frame_count();
    rain = light_shafts_rain_step(rain, l->sky_rain > 0);
    SkyStage sky;
    indoor += ((frame_sky_stage(l, &sky) ? 0.0f : 1.0f) - indoor) / 30.0f;
    const VolfogGrid g = volfog_grid(hle_d3d_volfog.quality);
    const int fog_on = l->has_vf_fog && l->vf_fog_on;
    const float far_z = volfog_far(fog_on, l->vf_fog_mode, l->vf_fog_start, l->vf_fog_end, l->vf_fog_density);
    VolfogLight in[RENDERER_LAMP_MAX + NEON_REFL_CAP], sel[VOLFOG_LIGHTS_MAX];
    uint32_t n = 0;
    const float gain = p->strength * VOLFOG_LIGHT_GAIN;
    for (uint32_t i = 0; i < p->n && i < RENDERER_LAMP_MAX; i++) {
        const RLamp *li = &p->l[i];
        in[n++] = (VolfogLight){{li->pos[0], li->pos[1], li->pos[2]}, li->radius, {li->rgb[0] * gain, li->rgb[1] * gain, li->rgb[2] * gain}, li->intensity,
                                i < frame_lamp_n ? VOLFOG_LAMP : VOLFOG_FIRE};
    }
    const float *vw = frame_lamp_vw;
    for (uint32_t i = 0; i < nsigns && i < NEON_REFL_CAP; i++) {
        const LampLight *li = &signs[i];
        float v[3];
        for (int c = 0; c < 3; c++) v[c] = li->pos[0] * vw[c] + li->pos[1] * vw[4 + c] + li->pos[2] * vw[8 + c] + vw[12 + c];
        in[n++] = (VolfogLight){{v[0], v[1], v[2]}, li->radius, {li->rgb[0] * gain, li->rgb[1] * gain, li->rgb[2] * gain}, li->intensity, VOLFOG_NEON};
    }
    /* With --level-lights: the halo lamps no level light stands at (the level lists no light for them). */
    VolfogLight halo[LAMP_MAX_LIGHTS];
    uint32_t nh = 0, nhalo = 0;
    for (uint32_t i = 0; i < frame_vf_halo_n && i < LAMP_MAX_LIGHTS; i++) {
        const LampLight *li = &frame_vf_halo[i];
        float v[3];
        for (int c = 0; c < 3; c++) v[c] = li->pos[0] * vw[c] + li->pos[1] * vw[4 + c] + li->pos[2] * vw[8 + c] + vw[12 + c];
        halo[nh++] = (VolfogLight){{v[0], v[1], v[2]}, li->radius, {li->rgb[0] * gain, li->rgb[1] * gain, li->rgb[2] * gain}, li->intensity, VOLFOG_LAMP};
    }
    if (nh) {
        const uint32_t nlamp = frame_lamp_n < p->n ? frame_lamp_n : p->n;
        const uint32_t room = (uint32_t)(sizeof in / sizeof in[0]) - n;
        nhalo = volfog_dedupe(halo, nh, in, nlamp, frame_lamp_up[0], &in[n], room);
        n += nhalo;
    }
    const uint32_t k = volfog_select(in, n, (uint32_t)g.lights, far_z, sel);
    /* The ground mist: on the level's surface under the camera (else the camera's height below it). */
    const int to_world = lamp_view_to_world(vw, out->to_world) == 0;   /* the walls' and the noise's world position */
    if (!to_world) memset(out->to_world, 0, sizeof out->to_world);
    float gz = 0;
    const int gmap = to_world && vf_ground(out->to_world, &gz);
    const float eye_z = out->to_world[2][3];
    ground_z = volfog_ground_step(ground_z, have_ground && to_world, gz, gmap, eye_z);
    have_ground = to_world;
    out->ground = to_world ? ground_z - eye_z : -VOLFOG_CAM_HEIGHT;
    const float *up = frame_lamp_up[0];
    out->n = 0;
    for (uint32_t i = 0; i < k && i < RENDERER_VOLFOG_MAX; i++) {
        const VolfogLight *li = &sel[i];
        RVolfogLight *o = &out->l[out->n++];
        for (int c = 0; c < 3; c++) {
            o->pos[c] = li->pos[c];
            o->rgb[c] = li->rgb[c] * li->intensity;
        }
        o->radius = li->kind == VOLFOG_LAMP ? volfog_lamp_reach(li->radius, li->pos[0] * up[0] + li->pos[1] * up[1] + li->pos[2] * up[2] - out->ground)
                                            : li->radius;   /* a lamp's cone reaches the ground */
        o->kind = (uint32_t)li->kind;
    }
    out->depth_a = p->depth_a;
    out->depth_b = p->depth_b;
    out->px = p->px;
    out->py = p->py;
    out->near_z = VOLFOG_NEAR;
    out->far_z = far_z;
    const float d_out = volfog_density(hle_d3d_volfog.strength, rain, damp_level, 0), d_in = volfog_density(hle_d3d_volfog.strength, rain, damp_level, 1);
    out->density = d_out + (d_in - d_out) * indoor;
    for (int c = 0; c < 3; c++) out->up[c] = up[c];
    out->time = (float)((l->has_frame_ms ? (double)l->frame_ms : now_ms()) / 1000.0);
    volfog_jitter(jit_frame++, out->jitter);
    out->fog_on = (uint32_t)fog_on;
    out->fog_mode = l->vf_fog_mode;
    out->fog_start = l->vf_fog_start;
    out->fog_end = l->vf_fog_end;
    out->fog_density = l->vf_fog_density;
    /* The history: the previous frame ran it and the camera did not cut. */
    out->history = have_prev && last_frame + 1 == frame && !volfog_reproject(prev_view, vw, out->prev);
    out->prev_px = prev_px;
    out->prev_py = prev_py;
    memcpy(prev_view, vw, sizeof prev_view);
    prev_px = p->px;
    prev_py = p->py;
    have_prev = 1;
    last_frame = frame;
    out->walls = volfog_walls_wanted() && to_world;
    out->mist = volfog_mist_density(1.0f, rain > damp_level ? rain : damp_level);
    out->rain = rain;
    static int logged, rain_logged, ground_logged;
    if (out->n && !logged++)
        printf("[volfog] %u light%s in the air (%u candidates, %u neon, %u halo lamps without a level light) after the opaque world; density %.3f, far %.0f%s\n",
               out->n, out->n == 1 ? "" : "s", n, nsigns, nhalo, out->density, far_z, fog_on ? " (the title's fog)" : "");
    if (out->n && !ground_logged++)
        printf("[volfog] ground mist %.2f on the %s, %.1f below the camera\n", out->mist, gmap ? "level's surface" : "camera's height (no surface)", -out->ground);
    if (rain >= 1 && !rain_logged++) printf("[volfog] the title's rain thickens the air: density %.3f\n", out->density);
    if (hle_d3d_stats && !(frame % 600))
        printf("[volfog] frame %u: %u lights (%u halo lamps), density %.3f (rain %.2f, damp %.2f, indoor %.2f), far %.0f, history %u, ground %.1f below (%s), mist %.2f\n",
               frame, out->n, nhalo, out->density, rain, damp_level, indoor, far_z, out->history, -out->ground, gmap ? "map" : "fallback", out->mist);
    return out->n > 0;
}
/* --gpu-skinning on the host: a GPU-skinned draw's program on the CPU from its recorded inputs,
 * exactly as at recording (vp_batch), into out (op->nv vertices). */
static void skin_cpu_run(const FrameList *l, const FrameOp *op, RVertex *out) {
    const SkinRec *r = &l->skin[op->skin - 1];
    const DrawFog fog = {r->fog_mode != GSK_FOG_OFF, 0, r->fog_mode == GSK_FOG_OFF ? 0 : r->fog_mode, r->fog_start, r->fog_end, r->fog_density};
    uint32_t fed = 0;
    for (int k = 0; k < r->layout.n; k++)
        if (r->layout.e[k].reg < 16) fed |= 1u << r->layout.e[k].reg;
    const uint8_t *d = l->skin_bytes + r->data;
    VpJob jb = {&vshaders[r->vsh].prog, &r->layout, fed, 0, op->nv,
                {r->stride[0] ? d + r->sbase[0] : NULL, r->stride[1] ? d + r->sbase[1] : NULL, r->stride[2] ? d + r->sbase[2] : NULL,
                 r->stride[3] ? d + r->sbase[3] : NULL},
                {r->stride[0], r->stride[1], r->stride[2], r->stride[3]}, (const float (*)[4])(l->skin_bytes + r->consts), out, 1, NULL, &fog,
                (const float (*)[2])r->tscale, 1};
    for (uint32_t b = 0; b < op->nv; b += VSH_BATCH) vp_batch(&jb, b);
}
/* Without device skinning at replay: every GPU-skinned draw's vertices into the list. */
static void skin_cpu_fill(FrameList *l) {
    for (uint32_t i = 0; i < l->nops; i++)
        if (l->ops[i].kind == OP_DRAW && l->ops[i].skin) skin_cpu_run(l, &l->ops[i], l->v + l->ops[i].first_vertex);
}
static int skin_force_cpu;   /* the same-frame A/B check: replay GPU-skinned draws from skin_cpu_fill */
static double skin_host_ms;  /* --stats: host time spent packing jobs and weld tables */

/* --char-lighting weld tables of GPU-skinned draws (gsk_adjacency_build), cached by a hash of the
 * bytes of the streams holding its position, bone index and weight inputs (registers 0, 4, 5) and
 * its indices; the groups come from those inputs' bytes. */
#define SKIN_ADJ_CACHE 256
static struct {
    uint64_t key;
    uint32_t nv, ni;
    GskAdjacency a;
} skin_adj[SKIN_ADJ_CACHE];
static uint64_t skin_key_mix(uint64_t key, uint64_t h) {
    key = (key ^ h) * 0x9E3779B97F4A7C15ull;
    return key ^ key >> 29;
}
static const GskAdjacency *skin_adjacency(const FrameList *l, const FrameOp *op, const SkinRec *r) {
    static uint8_t *keys;
    static uint32_t cap;
    const GeomElement *e[3];
    uint32_t ne = 0, kb = 0, streams = 0;
    for (int k = 0; k < r->layout.n; k++) {
        const GeomElement *x = &r->layout.e[k];
        if ((x->reg == 0 || x->reg == 4 || x->reg == 5) && ne < 3 && geom_type_size(x->type) > 0) {
            e[ne++] = x;
            kb += (uint32_t)geom_type_size(x->type);
            streams |= 1u << x->stream;
        }
    }
    if (!kb) return NULL;
    /* The key: the whole bytes of the streams holding those inputs (no per-vertex gather) and the indices,
     * each by rw_hash64 (render_wire.h, 64 bits, SSE2: ~8x the speed of gsk_hash_bytes' serial chain on the
     * ~5 MB/frame of skin streams in the busy street), chained with each stream's stride; a hit also needs
     * the same vertex and index counts. */
    const uint8_t *d = l->skin_bytes + r->data;
    const uint32_t *idx = l->idx + op->first_index;
    uint64_t key = (uint64_t)op->nv << 32 | op->ni;
    for (int k = 0; k < 4; k++)
        if (streams >> k & 1) key = skin_key_mix(key ^ (uint64_t)r->stride[k] << 40, rw_hash64(d + r->sbase[k], (size_t)op->nv * r->stride[k]));
    key = skin_key_mix(key, rw_hash64(idx, (size_t)op->ni * 4));
    const uint32_t slot = (uint32_t)(key % SKIN_ADJ_CACHE);
    if (skin_adj[slot].key == key && skin_adj[slot].nv == op->nv && skin_adj[slot].ni == op->ni && skin_adj[slot].a.group) return &skin_adj[slot].a;
    if (!grow((void **)&keys, &cap, op->nv * kb, 1)) return NULL;
    for (uint32_t v = 0; v < op->nv; v++) {
        uint8_t *o = keys + (size_t)v * kb;
        for (uint32_t k = 0; k < ne; k++) {
            const uint32_t sz = (uint32_t)geom_type_size(e[k]->type);
            memcpy(o, d + r->sbase[e[k]->stream] + (size_t)v * r->stride[e[k]->stream] + e[k]->offset, sz);
            o += sz;
        }
    }
    gsk_adjacency_free(&skin_adj[slot].a);
    skin_adj[slot].key = 0;
    if (gsk_adjacency_build(idx, op->ni, op->nv, keys, kb, &skin_adj[slot].a)) return NULL;
    skin_adj[slot].key = key;
    skin_adj[slot].nv = op->nv;
    skin_adj[slot].ni = op->ni;
    return &skin_adj[slot].a;
}
/* --stats: how often the GPU-skinned draws' constants change from frame to frame (anim_interp.h):
 * characters by their bone palette, other GPU vertex-program draws (world objects) by their
 * world-to-clip matrix (c69-c72: it moves with the object or the camera), and the 3D camera
 * (FrameList.taa_wvp, recorded with --taa or --anim-interp), all as shown (after --anim-interp).
 * One [anim] line per 300 rendered frames. Host thread. */
static AnimRate anim_chars, anim_objs;
static struct {
    uint64_t cam_hash;
    uint32_t frames, cam_frames, cam_new, have_cam;
    int cam_held;                        /* this frame's camera is last frame's */
    uint64_t still_draws, still_new;     /* objects drawn in such frames, and with a new matrix (they moved) */
} anim_st;
/* --anim-interp: every GPU vertex-program draw's motion rows (anim_poses_draw): a character's bone
 * palette and every draw's world-to-clip matrix (c69-c72). In the 30 FPS mode the whole 3D scene
 * (nearly all of it GPU vertex-program draws) then moves on every display frame. The blended
 * constants of a draw go to the renderer from one of two buffers in turn, so consecutive draws never
 * pass the same pointer with other contents (renderer_draw_skinned shares a copy of repeated
 * constants by pointer). The frame's 3D camera (FrameList.taa_wvp) follows the same cadence
 * (anim_cam_shown, for the TAA reprojection), and a camera cut (taa_camera_cut) between its states
 * is not blended across. */
static AnimPoses anim_poses, anim_cam;
static float anim_consts[2][VSH_NCONST][4], anim_tmp[VSH_NCONST][4];
static float anim_cam_shown[4][4], anim_cam_prev[4][4];
static int anim_pp, anim_cam_have;
static const float (*anim_interp_draw(const SkinRec *r, const VShader *vs, const float (*c)[4]))[4] {
    if (!anim_interp_options.on) return c;
    const int ch = vs->character && vs->pal_base >= 0;
    const AnimRows m = {ch ? vs->pal_base : -1, ch ? anim_palette_bones(vs->pal_base) : 0, C_WVP, 4};
    if (!anim_poses_draw(&anim_poses, r->id, c, &m, anim_tmp)) return c;
    float (*o)[4] = anim_consts[anim_pp];
    memcpy(o, c, sizeof anim_consts[0]);
    if (m.nbones) memcpy(o[m.base], anim_tmp[m.base], (size_t)(3 * m.nbones) * sizeof o[0]);
    memcpy(o[m.lin_first], anim_tmp[m.lin_first], (size_t)m.lin_n * sizeof o[0]);
    anim_pp ^= 1;
    return (const float (*)[4])o;
}
/* --anim-interp at the start of a frame: the cut test on the camera's last two states, the poses'
 * frame and the camera shown this frame. */
static void anim_interp_frame(const FrameList *l) {
    if (!anim_interp_options.on) return;
    int cut = 0;
    const int cam = l->taa_cam && l->draws;
    if (cam && anim_cam_have && memcmp(l->taa_wvp, anim_cam_prev, sizeof anim_cam_prev)) {
        float m[3][4];
        cut = taa_reprojection((const float (*)[4])l->taa_wvp, (const float (*)[4])anim_cam_prev, m) || taa_camera_cut((const float (*)[4])m);
    }
    anim_poses_frame(&anim_poses, cut);
    anim_poses_frame(&anim_cam, cut);
    if (cam) {
        memcpy(anim_cam_prev, l->taa_wvp, sizeof anim_cam_prev);
        memcpy(anim_cam_shown, l->taa_wvp, sizeof anim_cam_shown);
        memcpy(anim_tmp, l->taa_wvp, sizeof anim_cam_shown);
        const AnimRows m = {-1, 0, 0, 4};
        float o[VSH_NCONST][4];
        if (anim_poses_draw(&anim_cam, 1, (const float (*)[4])anim_tmp, &m, o)) memcpy(anim_cam_shown, o, sizeof anim_cam_shown);
    }
    anim_cam_have = cam;
}
static void anim_stats_frame(const FrameList *l) {
    anim_interp_frame(l);
    if (!hle_d3d_stats) return;
    if (anim_st.frames && anim_st.frames % 300 == 0) {
        char a[160], b[160], c[96] = "";
        anim_rate_format(&anim_chars, a, sizeof a);
        anim_rate_format(&anim_objs, b, sizeof b);
        if (anim_interp_options.on)
            snprintf(c, sizeof c, " | interpolated %llu of %llu GPU draws, %llu snaps", (unsigned long long)anim_poses.blended,
                     (unsigned long long)anim_poses.draws, (unsigned long long)anim_poses.snaps);
        printf("[anim] %u frames: characters %s | objects %s; with the camera still %llu of %llu new | camera new %u of %u%s\n", 300u, a, b,
               (unsigned long long)anim_st.still_new, (unsigned long long)anim_st.still_draws, anim_st.cam_new, anim_st.cam_frames, c);
        anim_rate_window(&anim_chars);
        anim_rate_window(&anim_objs);
        anim_poses_window(&anim_poses);
        anim_st.cam_new = anim_st.cam_frames = 0;
        anim_st.still_draws = anim_st.still_new = 0;
    }
    anim_st.frames++;
    anim_rate_frame(&anim_chars);
    anim_rate_frame(&anim_objs);
    anim_st.cam_held = 0;
    if (l->taa_cam && l->draws) {
        const uint64_t h = anim_hash_rows(anim_interp_options.on ? (const float (*)[4])anim_cam_shown : (const float (*)[4])l->taa_wvp, 4);
        anim_st.cam_held = anim_st.have_cam && h == anim_st.cam_hash;
        if (anim_st.have_cam) {
            anim_st.cam_frames++;
            anim_st.cam_new += h != anim_st.cam_hash;
        }
        anim_st.cam_hash = h;
        anim_st.have_cam = 1;
    } else {
        anim_st.have_cam = 0;
    }
}
static void anim_stats_draw(const SkinRec *r, const VShader *vs, const float (*c)[4]) {
    if (!hle_d3d_stats) return;
    if (vs->character && vs->pal_base >= 0) {
        anim_rate_draw(&anim_chars, r->id, anim_hash_rows(c + vs->pal_base, 3 * anim_palette_bones(vs->pal_base)));
        return;
    }
    const int n = anim_rate_draw(&anim_objs, r->id, anim_hash_rows(c + C_WVP, 4));
    if (anim_st.cam_held && n >= 0) {
        anim_st.still_draws++;
        anim_st.still_new += n == 1;
    }
}
/* XB_GSK_CHECK: the device's vertices (and lit normals) of the last frame against the CPU's. */
#define SKIN_CHECK_MAX 1024
static int32_t skin_check_at[SKIN_CHECK_MAX];
static uint32_t skin_check_op[SKIN_CHECK_MAX], skin_check_n;
static uint8_t skin_check_lit[SKIN_CHECK_MAX];
static float skin_check_px, skin_check_py;
/* A GPU-skinned draw through renderer_draw_skinned; lit: with --char-lighting's device normals;
 * wl: --world-lighting's lights for a world-lit draw (its recorded host normals, world_lit_draw's
 * mask), else NULL. */
static void skin_draw(const FrameList *l, const FrameOp *op, uint32_t fl, int lit, float px, float py, const RLampLights *wl) {
    const double t0 = hle_d3d_stats ? now_ms() : 0;
    const SkinRec *r = &l->skin[op->skin - 1];
    const VShader *vs = &vshaders[r->vsh];
    GskJob j;
    memset(&j, 0, sizeof j);
    gsk_job_layout(&j, &r->layout);
    j.nins = vs->prog.n;
    j.nv = op->nv;
    memcpy(j.sbase, r->sbase, sizeof j.sbase);
    memcpy(j.stride, r->stride, sizeof j.stride);
    j.fog_mode = r->fog_mode;
    j.fog_start = r->fog_start;
    j.fog_end = r->fog_end;
    j.fog_density = r->fog_density;
    memcpy(j.tscale, r->tscale, sizeof j.tscale);
    if (taa_options.on && r->own_motion) renderer_set_draw_identity(r->id);   /* --taa: its previous pose */
    const float (*consts)[4] = anim_interp_draw(r, vs, (const float (*)[4])(l->skin_bytes + r->consts));   /* --anim-interp */
    anim_stats_draw(r, vs, consts);   /* what is shown */
    RSkinNormals nm, *pn = NULL;
    const GskAdjacency *a = lit && fabsf(px) > 1e-6f && fabsf(py) > 1e-6f ? skin_adjacency(l, op, r) : NULL;
    if (a) {
        nm = (RSkinNormals){a->group, a->start, a->corner, a->ng, a->ncorner, px, py};
        pn = &nm;
    }
    const uint32_t wmask = wl && !pn && op->wl.first ? world_light_mask_reach(op->wl.box, wl, relief_reach()) : 0;
    static int wlogged;
    if (wmask && !wlogged++)
        printf("[world-light] GPU vertex program draws lit per pixel by the lamp lights (first: %u vertices, %d of %u lights reach it)\n", op->nv,
               __builtin_popcount(wmask), wl->n);
    const int32_t at = wl && !pn && op->wl.first
                           ? renderer_draw_skinned_world_lit(&op->vp, fl, &j, (const uint32_t (*)[4])vs->words, consts,
                                                             l->skin_bytes + r->data, r->data_bytes, l->idx + op->first_index, op->ni,
                                                             l->wl.n + (size_t)(op->wl.first - 1) * 3, wmask)
                           : renderer_draw_skinned(&op->vp, fl, &j, (const uint32_t (*)[4])vs->words, consts,
                                                   l->skin_bytes + r->data, r->data_bytes, l->idx + op->first_index, op->ni, pn);
    if (skin_check_n < SKIN_CHECK_MAX && at >= 0) {
        skin_check_at[skin_check_n] = at;
        skin_check_op[skin_check_n] = (uint32_t)(op - l->ops);
        skin_check_lit[skin_check_n++] = pn != NULL;
        skin_check_px = px;
        skin_check_py = py;
    }
    if (hle_d3d_stats) skin_host_ms += now_ms() - t0;
}
/* XB_GSK_CHECK: frame_depth_fit as the CPU path computes it, every world program's draw included
 * with its vertices from the CPU interpreter (characters stay out, as on the GPU path). */
static uint32_t depth_fit_cpu(const FrameList *l, float ab[2]) {
    static float fa[DEPTH_FIT_DRAWS], fb[DEPTH_FIT_DRAWS];
    static RVertex *cv;
    static uint32_t cap;
    uint32_t nf = 0;
    for (uint32_t i = 0; i < l->nops && nf < DEPTH_FIT_DRAWS; i++) {
        const FrameOp *op = &l->ops[i];
        if (op->kind != OP_DRAW || op->target_c || op->target_z || !(op->draw_flags & RENDERER_DEPTH_TEST) || op->nv < 3 || (op->skin && op->character)) continue;
        const RVertex *v = l->v + op->first_vertex;
        if (op->skin) {
            if (op->nv > cap) {
                RVertex *n = realloc(cv, (size_t)op->nv * sizeof *n);
                if (!n) return 0;
                cv = n;
                cap = op->nv;
            }
            skin_cpu_run(l, op, cv);
            v = cv;
        }
        float f[2];
        if (!rain_depth_fit(&v->z, sizeof(RVertex) / sizeof(float), op->nv < DEPTH_FIT_VERTS ? op->nv : DEPTH_FIT_VERTS, f)) {
            fa[nf] = f[0];
            fb[nf++] = f[1];
        }
    }
    rain_depth_pick(fa, fb, nf, ab);
    return nf;
}
static void skin_check(const FrameList *l) {
    static int on = -1;
    static double pos_err, tex_err, fog_err, nrm_dot = 1;
    static uint32_t col_off, draws, verts, frames, nrm_n, fit_n, fit_off, fit_frames_off;
    static RVertex *cv;
    static float *cn;
    static uint32_t cap;
    if (on < 0) on = getenv("XB_GSK_CHECK") != NULL;
    const RVertex *gv = renderer_debug_vertices();
    const float *gn = renderer_debug_normals();
    const uint32_t n = skin_check_n;
    skin_check_n = 0;
    if (!on || !gv) return;
    for (uint32_t c = 0; c < n; c++) {
        const FrameOp *op = &l->ops[skin_check_op[c]];
        if (op->nv > cap) {
            RVertex *v = realloc(cv, (size_t)op->nv * sizeof *v);
            float *m = realloc(cn, (size_t)op->nv * 3 * sizeof *m);
            if (v) cv = v;
            if (m) cn = m;
            if (!v || !m) return;
            cap = op->nv;
        }
        skin_cpu_run(l, op, cv);
        const RVertex *g = gv + skin_check_at[c];
        double dpos = 0, dtex = 0, dtex_rel = 0, dfog = 0;   /* this draw's, for its program's line */
        uint32_t dcol = 0;
        for (uint32_t i = 0; i < op->nv; i++) {
            const float sc = fabsf(cv[i].w) > 1 ? fabsf(cv[i].w) : 1;
            const float *a = &cv[i].x, *b = &g[i].x;
            for (int k = 0; k < 4; k++) dpos = fmax(dpos, fabs((double)a[k] - b[k]) / sc);
            for (int k = 0; k < 4 * RENDERER_STAGES; k++) {
                const double t = cv[i].tex[k / 4][k % 4], e = fabs(t - g[i].tex[k / 4][k % 4]);
                dtex = fmax(dtex, e);
                dtex_rel = fmax(dtex_rel, e / (fabs(t) > 1 ? fabs(t) : 1));
            }
            dfog = fmax(dfog, fabs((double)cv[i].fog - g[i].fog));
            for (int k = 0; k < 4; k++) {
                const int dc = (int)((cv[i].color >> (8 * k)) & 255) - (int)((g[i].color >> (8 * k)) & 255);
                const int ds = (int)((cv[i].specular >> (8 * k)) & 255) - (int)((g[i].specular >> (8 * k)) & 255);
                dcol += abs(dc) > 1 || abs(ds) > 1;
            }
        }
        pos_err = fmax(pos_err, dpos);
        tex_err = fmax(tex_err, dtex);
        fog_err = fmax(fog_err, dfog);
        col_off += dcol;
        gsk_check_note(vshaders[l->skin[op->skin - 1].vsh].hash, op->nv, dpos, dtex_rel, dfog, dcol);
        for (uint32_t i = 0; op->fit_pos && i < op->nv && i < DEPTH_FIT_VERTS; i++) {   /* vp_positions: the CPU's bits */
            fit_off += memcmp(&l->v[op->first_vertex + i].x, &cv[i].x, 4 * sizeof(float)) != 0;
            fit_n++;
        }
        if (skin_check_lit[c] && gn && !char_light_normals(cv, op->nv, l->idx + op->first_index, op->ni, skin_check_px, skin_check_py, cn)) {
            const float *m = gn + 3 * (size_t)skin_check_at[c];
            for (uint32_t i = 0; i < op->nv; i++) {
                if (!m[3 * i] && !m[3 * i + 1] && !m[3 * i + 2]) continue;   /* drawn plain after all */
                nrm_dot = fmin(nrm_dot, (double)cn[3 * i] * m[3 * i] + (double)cn[3 * i + 1] * m[3 * i + 1] + (double)cn[3 * i + 2] * m[3 * i + 2]);
                nrm_n++;
            }
        }
        draws++;
        verts += op->nv;
    }
    if (depth_fit_used()) {   /* the fit the passes use, against the CPU path's */
        float a[2], b[2];
        const uint32_t na = frame_depth_fit(l, a), nb = depth_fit_cpu(l, b);
        fit_frames_off += na != nb || memcmp(a, b, sizeof a) != 0;
    }
    if (++frames % 60 == 0) {
        printf("[gpu-skin] depth fit differs from the CPU path's in %u of %u frames\n", fit_frames_off, frames);
        printf("[gpu-skin] check over %u frames: %u draws, %u vertices: max position error %.3g (of max(|w|, 1)), texture %.3g, fog %.3g; "
               "%u colour channels off by more than 1; %u lit normals, min dot %.6f; %u of %u depth-fit positions differ from the CPU's\n",
               frames, draws, verts, pos_err, tex_err, fog_err, col_off, nrm_n, nrm_dot, fit_off, fit_n);
    }
    if (frames % 60 == 0) {   /* per program: the worst error of its draws so far */
        const GskCheckProg *cp;
        const uint32_t np = gsk_check_programs(&cp);
        for (uint32_t i = 0; i < np; i++)
            printf("[gpu-skin] program %016llx: %u draws, %u vertices: position %.3g, texture %.3g (relative), fog %.3g, %u colour channels off: %s\n",
                   (unsigned long long)cp[i].hash, cp[i].draws, cp[i].verts, cp[i].pos, cp[i].tex, cp[i].fog, cp[i].col_off,
                   gsk_check_within(&cp[i]) ? "within tolerance" : "OFF");
    }
}
/* --tri-check (XB_TRI_CHECK=1): every replayed draw's clip positions through tri_check_draw. Host
 * draws are checked before they are drawn; GPU draws (gpu-skin: characters, gpu-world: world and
 * object programs) over the device's own vertices, once their frame's fence has been waited
 * (renderer_set_vertex_check: a frame or two later on the swapchain path), from a ring of the draws'
 * details (tc_meta, tagged with renderer_set_draw_tag). A hit logs the path (cpu-vp: the
 * vertex-program interpreter, fixed: fixed function), whether it is a character, whether the mesh
 * pack replaced it, whether it went to an offscreen target, its program and mesh hashes, the worst
 * vertex and the robust box. At most TRI_CHECK_LOG lines, then a summary every 600 checked frames. */
int hle_d3d_tri_check;
#define TRI_CHECK_LOG 300
#define TRI_CHECK_RING 4     /* frames of tc_meta: more than the frames in flight */
#define TRI_CHECK_DRAWS 4096 /* GPU draws per frame with details */
typedef struct {
    uint32_t frame, op, nv, ni;
    uint64_t prog, mesh;
    int vsh;
    uint8_t character, meshpack, off;
    float vpw, vph;
} TriCheckMeta;
static TriCheckMeta tc_meta[TRI_CHECK_RING][TRI_CHECK_DRAWS];
static uint32_t tc_meta_n;
/* --tri-check samples one frame in TRI_CHECK_EVERY: reading the device's vertices back from mapped
 * (uncached, write-combined on NVIDIA) memory every frame cost the swapchain path tens of ms per
 * frame with the HD mesh pack. A spike that stays on screen long enough to be seen spans many frames. */
#define TRI_CHECK_EVERY 8
static struct { uint32_t frames, draws, device_draws, hit_draws, logged, sampled; uint64_t tris; int sample; } tc;
static void tri_check_log(const TriCheckMeta *m, const char *path, const RVertex *v, const TriCheckResult *r) {
    tc.hit_draws++;
    tc.tris += r->tris;
    if (tc.logged >= TRI_CHECK_LOG) return;
    tc.logged++;
    const RVertex *w = &v[r->worst];
    printf("[tri-check] frame %u op %u: %s%s%s%s program %d %016llx mesh %016llx (%u vertices, %u triangles, viewport %.0fx%.0f): "
           "%u non-finite, %u exploded vertices, %u spike triangles; worst vertex %u (%.3g x extent) at %g %g %g %g; box x %g..%g y %g..%g w %g..%g\n",
           m->frame, m->op, path, m->character ? " character" : "", m->meshpack ? " mesh-pack" : "", m->off ? " offscreen" : "", m->vsh,
           (unsigned long long)m->prog, (unsigned long long)m->mesh, m->nv, m->ni / 3, m->vpw, m->vph, r->nonfinite, r->outliers, r->tris, r->worst,
           r->ratio, w->x, w->y, w->z, w->w, r->lo[0], r->hi[0], r->lo[1], r->hi[1], r->lo[2], r->hi[2]);
}
static TriCheckMeta tri_check_meta(const FrameList *l, const FrameOp *op, uint32_t i) {
    const VShader *vs = op->vsh ? &vshaders[op->vsh - 1] : NULL;
    return (TriCheckMeta){tc.frames, i, op->nv, op->ni, vs ? vs->hash : 0,
                          gsk_hash_bytes((uint64_t)op->nv << 32 | op->ni, l->idx + op->first_index, (size_t)op->ni * 4), op->vsh - 1,
                          op->character, op->meshpack, op->target_c || op->target_z, op->vp.w, op->vp.h};
}
/* renderer_set_vertex_check: the device's vertices of a completed frame's GPU draws. */
static void tri_check_device(const RVertex *verts, const uint32_t *indices, const RVertexCheckDraw *d, uint32_t n) {
    for (uint32_t k = 0; k < n; k++) {
        TriCheckResult r;
        tc.draws++;
        tc.device_draws++;
        if (!tri_check_draw(&verts[d[k].first].x, sizeof(RVertex) / sizeof(float), d[k].nv, indices + d[k].first_index, d[k].ni, &r)) continue;
        const TriCheckMeta *m = &tc_meta[d[k].tag >> 16 & (TRI_CHECK_RING - 1)][d[k].tag & 0xFFFF];
        tri_check_log(m, m->character ? "gpu-skin" : "gpu-world", &verts[d[k].first], &r);
    }
}
/* Before op i is drawn: a host draw's list vertices, or the details of a GPU draw (its tag). */
static void tri_check_op(const FrameList *l, const FrameOp *op, uint32_t i, int skin_gpu) {
    if (!tc.sample || op->kind != OP_DRAW || !op->nv) return;
    if (op->skin && skin_gpu) {
        const uint32_t slot = tc.frames & (TRI_CHECK_RING - 1), at = tc_meta_n < TRI_CHECK_DRAWS ? tc_meta_n++ : TRI_CHECK_DRAWS - 1;
        tc_meta[slot][at] = tri_check_meta(l, op, i);
        renderer_set_draw_tag(slot << 16 | at);
        return;
    }
    TriCheckResult r;
    tc.draws++;
    if (!tri_check_draw(&l->v[op->first_vertex].x, sizeof(RVertex) / sizeof(float), op->nv, l->idx + op->first_index, op->ni, &r)) return;
    const TriCheckMeta m = tri_check_meta(l, op, i);
    tri_check_log(&m, op->skin ? (op->character ? "gpu-skin(host)" : "gpu-world(host)") : op->vsh ? "cpu-vp" : "fixed", l->v + op->first_vertex, &r);
}
static void tri_check_frame_begin(void) {
    static int installed;
    if (!installed++) renderer_set_vertex_check(tri_check_device);
    tc.sample = tc.frames % TRI_CHECK_EVERY == 0;
    tc.sampled += tc.sample;
    renderer_set_vertex_check_frame(tc.sample);
    tc_meta_n = 0;
}
static void tri_check_frame_end(void) {
    if (++tc.frames % 600 == 0)
        printf("[tri-check] %u frames (%u sampled, 1 in %d): %u draws checked (%u of them over the device's vertices), %u with spike triangles (%llu triangles)\n",
               tc.frames, tc.sampled, TRI_CHECK_EVERY, tc.draws, tc.device_draws, tc.hit_draws, (unsigned long long)tc.tris);
}
/* --char-lighting: a character draw with its smooth view normals (char_light_normals) through
 * renderer_draw_lit; a draw whose normals cannot be built is drawn plain. */
static void char_lit_draw(const FrameList *l, const FrameOp *op, uint32_t fl, float px, float py) {
    static float *nrm;
    static uint32_t cap;
    const RVertex *v = l->v + op->first_vertex;
    const uint32_t *idx = l->idx + op->first_index;
    if (op->nv > cap) {
        float *n = realloc(nrm, (size_t)op->nv * 3 * sizeof *n);
        if (n) {
            nrm = n;
            cap = op->nv;
        }
    }
    if (op->nv > cap || char_light_normals(v, op->nv, idx, op->ni, px, py, nrm)) {
        renderer_draw(&op->vp, fl, v, op->nv, idx, op->ni);
        return;
    }
    renderer_draw_lit(&op->vp, fl, v, op->nv, idx, op->ni, nrm);
    static int logged;
    if (!logged++) printf("[char-light] characters lit per pixel by the lamp lights (%u vertices in the first draw)\n", op->nv);
}
/* --hat-fix with --char-lighting: a worn hat lit per pixel as the characters (renderer_draw_lit),
 * with its own view normals (world_light_record of its smoothed normals: brims and rims stay
 * sharp, unlike char_light_normals' welded ones). */
static void hat_lit_draw(const FrameList *l, const FrameOp *op, uint32_t fl) {
    renderer_draw_lit(&op->vp, fl, l->v + op->first_vertex, op->nv, l->idx + op->first_index, op->ni, l->wl.n + (size_t)(op->wl.first - 1) * 3);
    static int logged;
    if (!logged++) printf("[hats] hats lit per pixel as the characters (%u vertices in the first draw)\n", op->nv);
}
/* --rain-hd splashes: the operation after which they are drawn (the end of the opaque world,
 * frame_world_end), or l->nops without layers (n of them, as for frame_rain), camera or opaque
 * world; p = their parameters (rain_splash_build: the layers, the camera, the title's clock, the
 * depth projection). */
static uint32_t frame_splash(const FrameList *l, DepthFit *df, const RainLayer *layers, int n, RRainSplash *p) {
    if (!n || !l->has_cam) return l->nops;
    const uint32_t at = frame_world_end(l, NULL);
    if (at == l->nops) return at;
    static uint32_t fallback_ms;   /* without the title's clock: 60 frames per second */
    fallback_ms += 17;
    float ab[2];
    frame_depth(l, df, ab);
    const uint32_t ms = l->has_clock ? l->clock_ms : l->has_frame_ms ? l->frame_ms : fallback_ms;
    if (!rain_splash_build(&hle_d3d_rain, layers, n, l->cam_view, l->cam_proj, ms, ab, p)) return l->nops;
    static int logged;
    if (!logged++)
        printf("[rain] splashes and ripples after the opaque world (op %u of %u): grid %.3f, chance %.2f, camera scale %.4f %.4f%s\n", at, l->nops,
               p->cell, p->chance, p->px, p->py, l->has_clock || l->has_frame_ms ? "" : ", no title clock");
    return at;
}
/* --contact-shadows: the operation after which they run (the end of the opaque world, as the
 * splashes), or l->nops without a 3D scene or camera; p = their parameters (contact_build). */
static uint32_t frame_contact(const FrameList *l, DepthFit *df, RContact *p) {
    if (!l->has_cam) return l->nops;
    const uint32_t at = frame_world_end(l, NULL);
    if (at == l->nops) return at;
    float ab[2];
    frame_depth(l, df, ab);
    if (!contact_build(&hle_d3d_contact, l->cam_view, l->cam_proj, ab, p)) return l->nops;
    static int logged;
    if (!logged++) printf("[contact] contact shadows after the opaque world (op %u of %u), strength %.2f, reach %.2f\n", at, l->nops, p->strength, p->radius);
    return at;
}
/* --neon-reflections: the signs of frame_neon (n of them) appended to the wet ground's lights. */
static void frame_neon_apply(const FrameList *l, const LampLight *signs, uint32_t n, RRainWet *p) {
    const uint32_t k = neon_refl_apply(&hle_d3d_neon_refl, signs, n, l->cam_view, l->cam_proj, p);
    static int logged;
    if (k && !logged++)
        printf("[neon-refl] %u neon sign%s mirrored in the %s ground (strength %.2f, %u sign draws)\n", k, k == 1 ? "" : "s", p->damp > 0 ? "damp" : "wet",
               hle_d3d_neon_refl.strength, nr_frame_signs);
    if (hle_d3d_stats && !(hle_d3d_frame_count() % 600))
        printf("[neon-refl] frame %u: %u of %u signs mirrored (%u sign draws)\n", hle_d3d_frame_count(), k, n, nr_frame_signs);
}
/* --damp-streets: 1 and the damp ground (damp_wet_build: the camera, the depth projection; the
 * light streaks of frame_lamp's nl lights at their damp share; the materials at their strength,
 * the material map being loaded) faded to level, else 0. */
static int frame_damp(const FrameList *l, DepthFit *df, uint32_t nl, const LampLight *signs, uint32_t nsigns, float level, RRainWet *p) {
    float ab[2];
    frame_depth(l, df, ab);
    if (!l->has_cam || !damp_wet_build(&hle_d3d_damp, l->cam_view, l->cam_proj, ab, p)) return 0;
    const WetStreaksConfig sc = {1, 1.0f, hle_d3d_wet_streaks.cap};
    const uint32_t nlamp = frame_lamp_n < nl ? frame_lamp_n : nl;
    const uint32_t k = wet_streaks_build(&sc, frame_wet_light, nl, nlamp, hle_d3d_lamp.strength, l->cam_view, l->cam_proj, 0, 0, p);
    damp_streaks_apply(p);
    frame_neon_apply(l, signs, nsigns, p);   /* --neon-reflections */
    const MaterialsConfig mc = {1, hle_d3d_materials.strength};
    if (!materials_wet_build(&mc, 1, l->cam_view, l->cam_proj, p)) return 0;
    rain_fade_apply(level, NULL, NULL, p);
    static int logged;
    if (!logged++)
        printf("[damp] damp streets: a dry night street (strength %.2f, reflection %.2f, darken %.2f, %u of %u lights streaked)\n", hle_d3d_damp.strength,
               p->reflect, p->darken, k, nl);
    return 1;
}
/* --reflections sssr's glass on a dry frame (no rain, no damp streets): 1 and the glass-only pass's
 * parameters (sssr_glass_frame: the camera, the depth projection; frame_lamp's nl lights and the
 * neon signs as the glass's mirrored lights, the streaks themselves off) with a camera and the glass
 * on, else 0. */
static int frame_glass(const FrameList *l, DepthFit *df, uint32_t nl, const LampLight *signs, uint32_t nsigns, RRainWet *p) {
    if (!l->has_cam || !sssr_renderer_glass(&hle_d3d_sssr) || !renderer_wet_glass_on()) return 0;
    float ab[2];
    frame_depth(l, df, ab);
    if (!sssr_glass_frame(&hle_d3d_sssr, l->cam_view, l->cam_proj, ab, p)) return 0;
    const WetStreaksConfig sc = {1, 1.0f, hle_d3d_wet_streaks.cap};
    const uint32_t nlamp = frame_lamp_n < nl ? frame_lamp_n : nl;
    wet_streaks_build(&sc, frame_wet_light, nl, nlamp, hle_d3d_lamp.strength, l->cam_view, l->cam_proj, 0, 0, p);
    frame_neon_apply(l, signs, nsigns, p);   /* --neon-reflections */
    p->streaks = 0;   /* the lights for the glass alone: the dry ground has no streaks */
    p->fallback = 0;
    static int logged;
    if (!logged++) printf("[sssr] glass, shop windows and car paint reflect the street (strength %.2f)\n", hle_d3d_sssr.glass);
    return 1;
}
/* --rain-hd wet ground: 1 and its parameters (rain_wet_build: the layers, n of them as for
 * frame_rain, the camera, the depth projection) with layers and a camera, else 0. */
static int frame_wet(const FrameList *l, DepthFit *df, const RainLayer *layers, int n, RRainWet *p) {
    if (!n || !l->has_cam) return 0;
    float ab[2];
    frame_depth(l, df, ab);
    if (!rain_wet_build(&hle_d3d_rain, layers, n, l->cam_view, l->cam_proj, ab, p)) return 0;
    if (hle_d3d_rain.puddles) {   /* --rain-hd-puddles: the title's clock as for the splashes */
        static uint32_t fallback_ms;
        fallback_ms += 17;
        const uint32_t ms = l->has_clock ? l->clock_ms : l->has_frame_ms ? l->frame_ms : fallback_ms;
        if (rain_puddle_build(&hle_d3d_rain, l->cam_view, l->cam_proj, ms, p)) {
            static int logged;
            if (!logged++) printf("[rain] puddles on the wet ground (strength %.2f)\n", p->puddles);
        }
    }
    static int logged;
    if (!logged++)
        printf("[rain] wet ground where the 3D scene ends: reflection %.2f, darken %.2f, up (view) %.3f %.3f %.3f\n", p->reflect, p->darken, p->up_view[0],
               p->up_view[1], p->up_view[2]);
    return 1;
}
/* --soft-particles: the operation after which the particles may be soft (the end of the opaque
 * world, frame_world_end), or l->nops; p = the frame's depth projection (frame_depth_fit) and the
 * fade distance. */
static uint32_t frame_soft(const FrameList *l, DepthFit *df, RSoft *p) {
    const uint32_t at = frame_world_end(l, NULL);
    if (at == l->nops) return at;
    float ab[2];
    const uint32_t nf = frame_depth(l, df, ab);
    p->depth_a = ab[0];
    p->depth_b = ab[1];
    p->distance = hle_d3d_soft.distance;
    static int logged;
    if (!logged && nf && (logged = 1))
        printf("[soft] soft particles after the opaque world (op %u of %u): distance %.2f, depth projection A %.5f B %.5f from %u draws\n", at,
               l->nops, p->distance, ab[0], ab[1], nf);
    return at;
}
/* --world-lighting: a world or object draw with its recorded view normals through
 * renderer_draw_world_lit, lit by the lights that reach its box (none: drawn plain). */
static void world_lit_draw(const FrameList *l, const FrameOp *op, uint32_t fl, const RLampLights *p) {
    const uint32_t mask = world_light_mask_reach(op->wl.box, p, relief_reach());
    static int logged;
    if (mask && !logged++)
        printf("[world-light] world draws lit per pixel by the lamp lights (first: %u vertices, %d of %u lights reach it)\n", op->nv,
               __builtin_popcount(mask), p->n);
    renderer_draw_world_lit(&op->vp, fl, l->v + op->first_vertex, op->nv, l->idx + op->first_index, op->ni, l->wl.n + (size_t)(op->wl.first - 1) * 3, mask);
}
/* --soft-particles: a world particle draw after renderer_soft. Its view-plane billboards
 * (soft_classify_sprites) are drawn with RENDERER_SOFT, the others (ground decals, wall tags, also a tag
 * seen head-on once its sprite has been seen off the view plane) without; runs of each go out as
 * separate draws in their recorded order, so the back-to-front blending is unchanged. */
static void soft_draw(const FrameList *l, const FrameOp *op, uint32_t flags) {
    static uint8_t *q;
    static uint32_t *ix, qcap, ixcap;
    static uint64_t quads, soft_quads;
    static SoftSprites sprites;   /* the world-fixed sprites seen so far (host thread only) */
    const RVertex *v = l->v + op->first_vertex;
    const uint32_t *idx = l->idx + op->first_index;
    const uint32_t nq = grow((void **)&q, &qcap, op->ni / 6 + 1, 1) && grow((void **)&ix, &ixcap, op->ni, sizeof *ix)
                            ? soft_classify_sprites(&v->x, sizeof(RVertex) / sizeof(float), (offsetof(RVertex, tex) - offsetof(RVertex, x)) / sizeof(float), idx,
                                                    op->ni, &op->soft_view, op->tex_key[0], &sprites, q)
                            : 0;
    if (!nq) {
        renderer_draw(&op->vp, flags, v, op->nv, idx, op->ni);
        return;
    }
    for (uint32_t a = 0, b; a < nq; a = b) {
        for (b = a + 1; b < nq && q[b] == q[a]; b++) {}
        const uint32_t fl = flags | (q[a] ? RENDERER_SOFT : 0);
        quads += b - a;
        soft_quads += q[a] ? b - a : 0;
        if (!a && b == nq) {
            renderer_draw(&op->vp, fl, v, op->nv, idx, op->ni);
            break;
        }
        const uint32_t *ri = idx + a * 6, n = (b - a) * 6;
        uint32_t lo = ri[0], hi = ri[0];
        for (uint32_t i = 1; i < n; i++) {
            lo = ri[i] < lo ? ri[i] : lo;
            hi = ri[i] > hi ? ri[i] : hi;
        }
        for (uint32_t i = 0; i < n; i++) ix[i] = ri[i] - lo;
        renderer_draw(&op->vp, fl, v + lo, hi - lo + 1, ix, n);
    }
    static uint64_t next_log = 1;
    if (quads >= next_log) {   /* a few lines over the run: how many particle quads are softened */
        printf("[soft] %llu of %llu particle quads soft (view-plane billboards), %u world-fixed sprites\n", (unsigned long long)soft_quads,
               (unsigned long long)quads, sprites.n);
        next_log = next_log < 1000000 ? next_log * 10 : next_log * 4;
    }
}
/* --sky-hd: the palette of a dome texture (sky_palette of its level 0, decoded from guest memory),
 * cached by texture key; NULL when it cannot be read. */
static const SkyPalette *dome_palette(const FrameOp *op) {
    enum { CACHE = 8 };
    static struct { uint64_t key; SkyPalette p; } cache[CACHE];
    static uint32_t next, *buf;
    static size_t cap;
    for (int i = 0; i < CACHE; i++)
        if (cache[i].key && cache[i].key == op->tex_key[0]) return &cache[i].p;
    XTexInfo t;
    if (!op->tex_key[0] || xtex_info(op->tex_format[0], op->tex_size[0], &t)) return NULL;
    const size_t need = (size_t)t.w * t.h;
    if (need > cap) {
        uint32_t *n = realloc(buf, need * 4);
        if (!n) return NULL;
        buf = n;
        cap = need;
    }
    SkyPalette p;
    if (xtex_decode_level(&t, 0, G(op->tex_data[0]), buf)) return NULL;
    /* The dome's bytes changed since record time: no palette is cached and the procedural sky is
     * off for this frame, so the title's dome draw goes through resolve_texture, which counts the
     * change (once) and applies the guard. Not counted here. */
    if (tex_race_guard && tex_race_changed(op->tex_key[0], op->tex_data[0], op->tex_format[0], op->tex_size[0], G(op->tex_data[0])) > 0) return NULL;
    if (sky_palette(buf, t.w, t.h, &p)) return NULL;
    const uint32_t k = next++ % CACHE;
    cache[k].key = op->tex_key[0];
    cache[k].p = p;
    printf("[sky] dome %ux%u: horizon %06x, zenith %06x, silhouette %06x, stars %.2f\n", t.w, t.h, p.ramp[0] & 0xFFFFFFu,
           p.ramp[SKY_RAMP - 1] & 0xFFFFFFu, p.silhouette & 0xFFFFFFu, p.stars);
    return &cache[k].p;
}
/* The frame's sky stage: sky_classify over its back-buffer operations (1 with a dome). */
static int frame_sky_stage(const FrameList *l, SkyStage *stage) {
    static SkyOp *ops;
    static uint32_t cap;
    if (!grow((void **)&ops, &cap, l->nops, sizeof *ops)) return 0;
    for (uint32_t i = 0; i < l->nops; i++) {
        const FrameOp *op = &l->ops[i];
        const int off = op->target_c || op->target_z;
        ops[i] = op->kind == OP_DRAW && !off ? op->sky : (SkyOp){0};
        if (op->kind == OP_CLEAR && !off)
            ops[i].kind = op->flags & RENDERER_CLEAR_COLOR ? SKY_OP_CLEAR_COLOR : op->flags & RENDERER_CLEAR_DEPTH ? SKY_OP_CLEAR_DEPTH : SKY_OP_OTHER;
    }
    return sky_classify(ops, l->nops, stage);
}
/* --sky-hd: the frame's sky stage (sky_classify over its back-buffer operations) and, with a dome,
 * the procedural sky that replaces dome and cap (sky_build: the dome's palette, camera and lit
 * colour, the cap, the title's rain layers eased in and out over about a second, c_vSunDir, the
 * title's clock) and the skyline's windows and haze (line, when the skyline draw kept its world
 * positions). Returns 1 with a sky to draw. */
static int frame_sky(const FrameList *l, SkyStage *stage, RSky *sky, RSkyline *line, int *has_line) {
    static uint32_t fallback_frames;
    static float rain;
    rain += ((l->sky_rain ? 1.0f : 0.0f) - rain) * 0.05f;
    fallback_frames++;
    if (!frame_sky_stage(l, stage) || !l->has_sky_cam || l->sky_cam_op != (uint32_t)stage->dome) return 0;
    const FrameOp *dome = &l->ops[stage->dome];
    const SkyPalette *pal = dome_palette(dome);
    if (!pal) return 0;
    const float seconds = l->has_frame_ms ? (float)(l->frame_ms % 3600000u) / 1000.0f : (float)(fallback_frames % 216000u) / 60.0f;
    const uint32_t shade = l->v[dome->first_vertex].color;
    /* A cap whose sheet is a tiny transparent texture (8 x 8: "no clouds") adds no cover. */
    const int clouds = stage->cap >= 0 && l->ops[stage->cap].sky.tex_w >= 64;
    if (!sky_build(pal, l->sky_view, l->sky_proj, shade, clouds, rain, l->has_sun ? l->sun : NULL, seconds, sky)) return 0;
    *has_line = stage->skyline >= 0 && l->ops[stage->skyline].sky_world;
    if (*has_line) sky_skyline_build(pal, shade, rain, seconds, line);
    static int logged;
    if (!logged++)
        printf("[sky] sky stage: dome op %d, cap %d, skyline %d%s, far glows %d x %d; dome and cap replaced by the procedural sky (moon %s)\n",
               stage->dome, stage->cap, stage->skyline, *has_line ? " (windows and haze)" : "", stage->glow_first, stage->glow_count,
               sky->moon_size > 0 ? "on" : "off");
    return 1;
}
/* --damp-streets: 1 when the frame is outdoors at night: it draws the title's sky dome (interiors draw
 * none, or a smaller one that is no dome) and the dome's zenith is dark (damp_night). */
static int frame_damp_sky(const FrameList *l) {
    SkyStage st;
    if (!frame_sky_stage(l, &st)) return 0;
    const SkyPalette *pal = dome_palette(&l->ops[st.dome]);
    return pal && damp_night(pal->ramp[SKY_RAMP - 1]);
}
/* --cutscene-dof: this frame's depth of field (renderer_dof) from the letterbox bars (cinema_bar_draw,
 * the gate's 0.3 s fade over the host clock), the depth projection (frame_depth; the last good one
 * when this frame has none) and the snap (a new cutscene, a cut of the title's camera: dof_cut on
 * the matrix read at the swap). p->weight stays 0 outside the cutscenes. */
static int frame_shows_bars(const FrameList *l) {
    for (uint32_t i = 0; i < l->nops; i++) {
        const FrameOp *op = &l->ops[i];
        if (op->kind == OP_DRAW && !op->target_c && !op->target_z && !op->skin &&
            cinema_bar_draw(l->v + op->first_vertex, op->nv, op->tex_key[0] || op->tex_surf[0], op->vsh != 0, op->rs.blend_enable != 0))
            return 1;
    }
    return 0;
}
static void dof_frame(const FrameList *l, DepthFit *df, RDofFrame *p) {
    static DofGate gate;
    static double last_ms = -1;
    static float last_ab[2], prev_cam[16];
    static int have_ab, have_cam;
    const double now = now_ms();
    const float dt = last_ms < 0 ? 0 : (float)((now - last_ms) / 1000);
    last_ms = now;
    const int bars = frame_shows_bars(l);
    int started;
    const float w = dof_gate_step(&gate, bars, dt, &started);
    const int cut = l->has_game_cam && have_cam && dof_cut(prev_cam, l->game_cam);
    if (l->has_game_cam) memcpy(prev_cam, l->game_cam, sizeof prev_cam);
    have_cam = l->has_game_cam;
    float ab[2];
    if (w > 0 && frame_depth(l, df, ab) && ab[1] > 0) {
        memcpy(last_ab, ab, sizeof ab);
        have_ab = 1;
    }
    /* --cutscene-dof-speaker: the faces of this frame's character draws (dof_face_record) pick the
     * speaker; the tracker starts over with each cutscene. */
    static DofSpeaker spk;
    DofSpeakerOut so = {0};
    if (started || !(w > 0)) dof_speaker_reset(&spk);
    if (w > 0 && hle_d3d_dof.speaker) {
        dof_speaker_step(&spk, l->faces, (int)l->nfaces, dt, &so);
        static uint32_t changes;
        if (so.changed) {
            changes++;
            if (so.id) printf("[dof] speaker %u: character %u at %.2f view units (mouth %.2f/s)\n", changes, so.id, so.dist, so.speed);
            else printf("[dof] speaker %u: none (the characters' range)\n", changes);
        }
        static uint32_t frames;
        if (hle_d3d_stats && ++frames % 30 == 0) {
            printf("[dof] frame %u: faces %u (%.1f us/frame), speaker weight %.2f:", hle_d3d_frame_count(), l->nfaces, 1000 * dof_face_ms / 30,
                   so.weight);
            dof_face_ms = 0;
            for (int t = 0; t < DOF_SPK_TRACKS; t++)
                if (spk.t[t].used)
                    printf(" #%u%s %.2fu (%.2f %.2f) mouth %.2f;", spk.t[t].id, t == spk.speaker ? "*" : "", spk.t[t].dist, spk.t[t].ndc[0], spk.t[t].ndc[1],
                           spk.t[t].speed);
            printf("\n");
        }
    }
    __atomic_store_n(&dof_faces_on, w > 0 && hle_d3d_dof.speaker, __ATOMIC_RELAXED);
    if (!(w > 0) || !have_ab) return;
    p->weight = w;
    p->speaker = so.weight;
    p->speaker_dist = so.dist;
    p->depth_a = last_ab[0];
    p->depth_b = last_ab[1];
    p->dt = dt;
    p->snap = started || cut;
    static uint32_t cuts;
    cuts += cut;
    static int logged;
    if (!logged++)
        printf("[dof] cutscene bars: depth of field fading in (depth projection A %.5f B %.5f, camera %s)\n", p->depth_a, p->depth_b,
               l->has_game_cam ? "read" : "not readable: no cut snaps");
    if (hle_d3d_stats && cut) printf("[dof] camera cut %u: focus snapped\n", cuts);
}
/* --auto-exposure: a cut of the title's camera (dof_cut on the matrix read at the swap, as the
 * depth of field's) snaps the adaptation to the new shot; nothing else does, however far the meter
 * moves (a camera turn in the hideout snapped the tone by 0.9 stop). */
static void ae_cut_frame(const FrameList *l) {
    static float prev_cam[16];
    static int have_cam;
    static uint32_t cuts;
    const int cut = l->has_game_cam && have_cam && dof_cut(prev_cam, l->game_cam);
    if (l->has_game_cam) memcpy(prev_cam, l->game_cam, sizeof prev_cam);
    have_cam = l->has_game_cam;
    if (!cut) return;
    renderer_auto_exposure_snap();
    cuts++;
    if (hle_d3d_stats) printf("[exposure] camera cut %u: adaptation snapped\n", cuts);
}
/* --taa: the frame's renderer_taa parameters. The reprojection map from this frame's camera
 * (FrameList.taa_wvp) to the last one seen (taa_reprojection), the depth projection (frame_depth)
 * and the camera's viewport; reset on a cut (taa_camera_cut) and when the cutscene bars appear or
 * go. Without a camera the renderer assumes no motion; without one last frame (the first frame, after
 * the menus or a load) there is nothing to reproject either, and the renderer drops the history by
 * itself after a frame without TAA. */
static void frame_taa(const FrameList *l, DepthFit *df, int bars, RTaa *t) {
    static float prev[4][4];
    static int have_prev, prev_bars, logged;
    memset(t, 0, sizeof *t);
    float ab[2] = {0, 0};
    const uint32_t nf = l->taa_cam ? frame_depth(l, df, ab) : 0;
    t->depth_a = ab[0];
    t->depth_b = ab[1];
    memcpy(t->vp, l->taa_vp, sizeof t->vp);
    /* --anim-interp: the camera the frame's draws show (between its last two states) */
    const float (*cam)[4] = anim_interp_options.on ? (const float (*)[4])anim_cam_shown : (const float (*)[4])l->taa_wvp;
    t->valid = l->taa_cam && have_prev && nf && ab[1] > 0 && !taa_reprojection(cam, (const float (*)[4])prev, t->reproject);
    t->reset = (t->valid && taa_camera_cut((const float (*)[4])t->reproject)) || bars != prev_bars;
    if (!logged && l->taa_cam) {
        logged = 1;
        printf("[taa] camera from %s, viewport %.0f,%.0f %.0fx%.0f; depth projection A %.5f B %.5f from %u draws\n",
               l->taa_cam == 1 ? "the world program (c69-c72)" : "the fixed-function view x projection", t->vp[0], t->vp[1], t->vp[2], t->vp[3],
               ab[0], ab[1], nf);
    }
    if (hle_d3d_stats && t->reset && t->valid) printf("[taa] camera cut: history dropped (motion %.2f NDC)\n", taa_camera_motion((const float (*)[4])t->reproject, TAA_CUT_DIST));
    have_prev = l->taa_cam != 0;
    if (have_prev) memcpy(prev, cam, sizeof prev);
    prev_bars = bars;
}

/* --frame-gen: the frame's renderer_frame_gen parameters, as frame_taa's but from its own last frame
 * (every frame-gen frame, with or without --taa): the reprojection map from this frame's camera to the
 * last one's, the depth projection and the viewport; reset on a cut and when the cutscene bars appear
 * or go (the renderer itself takes the real frame after a frame without it). */
static void frame_fg(const FrameList *l, DepthFit *df, int bars, RFrameGen *g) {
    static float prev[4][4];
    static int have_prev, prev_bars, logged;
    memset(g, 0, sizeof *g);
    float ab[2] = {0, 0};
    const uint32_t nf = l->taa_cam ? frame_depth(l, df, ab) : 0;
    g->depth_a = ab[0];
    g->depth_b = ab[1];
    memcpy(g->vp, l->taa_vp, sizeof g->vp);
    const float (*cam)[4] = anim_interp_options.on ? (const float (*)[4])anim_cam_shown : (const float (*)[4])l->taa_wvp;
    g->valid = l->taa_cam && have_prev && nf && ab[1] > 0 && !taa_reprojection(cam, (const float (*)[4])prev, g->reproject);
    const int cut = g->valid && taa_camera_cut((const float (*)[4])g->reproject);
    g->reset = cut || bars != prev_bars;
    if (!logged && l->taa_cam) {
        logged = 1;
        printf("[frame-gen] camera from %s, viewport %.0f,%.0f %.0fx%.0f; depth projection A %.5f B %.5f\n",
               l->taa_cam == 1 ? "the world program (c69-c72)" : "the fixed-function view x projection", g->vp[0], g->vp[1], g->vp[2], g->vp[3],
               ab[0], ab[1]);
    }
    if (hle_d3d_stats && cut)
        printf("[frame-gen] camera cut: no interpolation (motion %.2f NDC)\n", taa_camera_motion((const float (*)[4])g->reproject, TAA_CUT_DIST));
    else if (hle_d3d_stats && g->reset)
        printf("[frame-gen] cutscene bars %s: no interpolation\n", bars ? "appear" : "go");
    have_prev = l->taa_cam != 0;
    if (have_prev) memcpy(prev, cam, sizeof prev);
    prev_bars = bars;
}

/* --frame-gen: generated frames are presented (the host's framegen_decide and slow-source guard), so
 * the frames build them; readback frames (dumps) always do. */
static int fg_show;

/* --combat-debug: the frame's hit/hurt volumes (combat_debug_snapshot) as wireframes drawn over the
 * finished frame without depth (visible through walls and bodies): three great circles per sphere
 * (and per capsule end, with four lines along its sides) of 2-pixel lines, projected with the
 * frame's world-to-clip matrix. Hurt volumes green, strike volumes red, weapon spheres orange,
 * contacts (the last 20 steps) yellow. */
#define CD_SEGS 16
static void cd_line(RVertex *v, uint32_t *ix, uint32_t *nv, uint32_t *ni, const float m[4][4], const RViewport *vp, const float a[3],
                    const float b[3], uint32_t color) {
    float c[2][4];
    for (int e = 0; e < 2; e++) {
        const float *p = e ? b : a;
        for (int r = 0; r < 4; r++) c[e][r] = m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3];
        if (c[e][3] < 0.05f) return;   /* behind the camera */
    }
    /* Screen direction of the line, its perpendicular one pixel long in clip units per w. */
    const float dx = (c[1][0] / c[1][3] - c[0][0] / c[0][3]) * vp->w, dy = (c[1][1] / c[1][3] - c[0][1] / c[0][3]) * vp->h;
    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    const float px = -dy / len * 2.0f / vp->w, py = dx / len * 2.0f / vp->h;
    const uint32_t base = *nv;
    for (int e = 0; e < 2; e++)
        for (int side = -1; side <= 1; side += 2) {
            RVertex *o = &v[(*nv)++];
            memset(o, 0, sizeof *o);
            o->x = c[e][0] + side * px * c[e][3];
            o->y = c[e][1] + side * py * c[e][3];
            o->z = 0;
            o->w = c[e][3];
            o->color = color;
        }
    static const uint8_t quad[6] = {0, 1, 2, 2, 1, 3};
    for (int k = 0; k < 6; k++) ix[(*ni)++] = base + quad[k];
}
static void cd_circles(RVertex *v, uint32_t *ix, uint32_t *nv, uint32_t *ni, const float m[4][4], const RViewport *vp, const float c[3],
                       float r, uint32_t color) {
    for (int plane = 0; plane < 3; plane++) {
        float prev[3];
        for (int k = 0; k <= CD_SEGS; k++) {
            const float t = 6.2831853f * (float)k / CD_SEGS, u = cosf(t) * r, w = sinf(t) * r;
            float p[3] = {c[0], c[1], c[2]};
            p[plane] += u;
            p[(plane + 1) % 3] += w;
            if (k) cd_line(v, ix, nv, ni, m, vp, prev, p, color);
            memcpy(prev, p, sizeof prev);
        }
    }
}
static void combat_draw(const FrameList *l) {
    static uint32_t *ix;
    static RVertex *v;
    const CdVolume *vol = cd_ring[l->cd_slot - 1];
    const uint32_t per = 3 * CD_SEGS * 2 + 4, maxv = (uint32_t)l->cd_n * per * 4, maxi = (uint32_t)l->cd_n * per * 6;
    static uint32_t cap;
    if (cap < maxv) {
        free(v);
        free(ix);
        v = malloc(maxv * sizeof *v);
        ix = malloc(maxi * sizeof *ix);
        cap = v && ix ? maxv : 0;
        if (!cap) return;
    }
    const uint64_t key = 0xc0b1a7deb0000001ull;   /* a 1x1 white texture: the default program gives the vertex colour */
    const uint32_t white = 0xffffffffu;
    uint32_t white_tex = renderer_texture_find(key);
    if (!white_tex) white_tex = renderer_texture_create(key, RENDERER_TEX_BGRA8, 1, 1, 1, &white);
    if (!white_tex) return;
    uint32_t nv = 0, ni = 0;
    for (uint32_t i = 0; i < l->cd_n; i++) {
        const CdVolume *q = &vol[i];
        static const uint32_t colors[] = {0xc040ff40u, 0xffff3030u, 0xffff9020u, 0xffffff30u};
        const uint32_t color = colors[q->kind & 3];
        cd_circles(v, ix, &nv, &ni, l->cd_wvp, &l->cd_vp, q->a, q->r, color);
        if (memcmp(q->a, q->b, sizeof q->a)) {
            cd_circles(v, ix, &nv, &ni, l->cd_wvp, &l->cd_vp, q->b, q->r, color);
            for (int s = 0; s < 4; s++) {
                float a[3], b[3];
                memcpy(a, q->a, sizeof a);
                memcpy(b, q->b, sizeof b);
                a[s >> 1] += s & 1 ? q->r : -q->r;
                b[s >> 1] += s & 1 ? q->r : -q->r;
                cd_line(v, ix, &nv, &ni, l->cd_wvp, &l->cd_vp, a, b, color);
            }
        }
    }
    if (!ni) return;
    RPixelShader ps;
    renderer_default_pixel_shader(&ps);
    renderer_set_pixel_shader(&ps);
    renderer_set_alpha_test(0, 0);
    for (uint32_t s = 0; s < RENDERER_STAGES; s++) renderer_set_stage_texture(s, s ? 0 : white_tex);
    RState rs;
    renderer_state_default(&rs);
    rs.blend_enable = 1;
    rs.blend_src = 0x302;   /* SRC_ALPHA */
    rs.blend_dst = 0x303;   /* ONE_MINUS_SRC_ALPHA */
    renderer_set_state(&rs);
    renderer_draw(&l->cd_vp, 0, v, nv, ix, ni);
}

/* A mod's menu over the frame (src/hle_modmenu.h): the quads of modmenu_layout in the guest's back
 * buffer pixels, with three textures: white, the game's font atlas and the cover, both decoded
 * from the player's disc (kept on the GPU under their own keys). */
static uint32_t modmenu_h;   /* back-buffer height of the frame being rendered */
int (*hle_d3d_menu_view)(ModMenu *out, HleMenuArt *art);
static int photo_no_overlay;   /* photo mode: a capture replay (photo_replay) has no menu overlay */
static void modmenu_draw(uint32_t w, uint32_t h) {
    static ModMenu m;
    static HleMenuArt art;
    static ModQuad q[MODMENU_QUADS_MAX];
    static RVertex v[MODMENU_QUADS_MAX * 4];
    static uint32_t ix[MODMENU_QUADS_MAX * 6];
    if (!w || !h || !((hle_d3d_menu_view && hle_d3d_menu_view(&m, &art)) || hle_modmenu_view(&m, &art))) return;
    const uint64_t white_key = 0xc0b1a7deb0000001ull, font_key = 0xc0b1a7deb0000002ull, cover_key = 0xc0b1a7deb0010000ull + art.cover_serial;
    const uint32_t white = 0xffffffffu;
    uint32_t tex[3] = {renderer_texture_find(white_key), art.font_px ? renderer_texture_find(font_key) : 0,
                       art.cover_px ? renderer_texture_find(cover_key) : 0};
    if (!tex[0]) tex[0] = renderer_texture_create(white_key, RENDERER_TEX_BGRA8, 1, 1, 1, &white);
    if (!tex[1] && art.font_px)
        tex[1] = renderer_texture_create(font_key, RENDERER_TEX_BGRA8, (uint32_t)art.font.atlas_w, (uint32_t)art.font.atlas_h, 1, art.font_px);
    if (!tex[2] && art.cover_px)
        tex[2] = renderer_texture_create(cover_key, RENDERER_TEX_BGRA8, (uint32_t)art.cover_w, (uint32_t)art.cover_h, 1, art.cover_px);
    if (!tex[0]) return;
    /* the pixels' shape on screen: the anamorphic 16:9 output stretches them */
    const float aspect = hle_d3d_widescreen ? 16.0f * (float)h / (9.0f * (float)w) : 1.0f;
    const int n = modmenu_layout(&m, (int)w, (int)h, aspect, tex[1] ? &art.font : NULL, tex[2] != 0, art.cover_uv, q, MODMENU_QUADS_MAX);
    RPixelShader ps;
    renderer_default_pixel_shader(&ps);
    renderer_set_target(0, 0);
    renderer_set_pixel_shader(&ps);
    renderer_set_alpha_test(0, 0);
    RState rs;
    renderer_state_default(&rs);
    rs.blend_enable = 1;
    rs.blend_src = 0x302;   /* SRC_ALPHA */
    rs.blend_dst = 0x303;   /* ONE_MINUS_SRC_ALPHA */
    rs.cull = 0;
    renderer_set_state(&rs);
    const RViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    for (int i = 0; i < n;) {   /* runs of quads with the same texture, in order */
        const int t = q[i].tex;
        uint32_t nv = 0, ni = 0;
        for (; i < n && q[i].tex == t; i++) {
            const ModQuad *a = &q[i];
            const float x0 = a->x0 / (float)w * 2 - 1, x1 = a->x1 / (float)w * 2 - 1, y0 = 1 - a->y0 / (float)h * 2, y1 = 1 - a->y1 / (float)h * 2;
            const float px[4] = {x0, x1, x1, x0}, py[4] = {y0, y0, y1, y1}, pu[4] = {a->u0, a->u1, a->u1, a->u0}, pv[4] = {a->v0, a->v0, a->v1, a->v1};
            for (int k = 0; k < 4; k++) {
                RVertex *o = &v[nv + k];
                memset(o, 0, sizeof *o);
                o->x = px[k], o->y = py[k], o->z = 0, o->w = 1;
                o->color = a->color;
                o->tex[0][0] = pu[k], o->tex[0][1] = pv[k], o->tex[0][3] = 1;
            }
            static const uint32_t quad[6] = {0, 1, 2, 0, 2, 3};
            for (int k = 0; k < 6; k++) ix[ni + k] = nv + quad[k];
            nv += 4;
            ni += 6;
        }
        const uint32_t h2 = t == MM_TEX_FONT ? tex[1] : t == MM_TEX_COVER ? tex[2] : tex[0];
        if (!h2) continue;
        for (uint32_t s2 = 0; s2 < RENDERER_STAGES; s2++) renderer_set_stage_texture(s2, s2 ? 0 : h2);
        renderer_draw(&vp, 0, v, nv, ix, ni);
    }
}

static int render_frame_ops(const FrameList *l, const uint32_t *px, uint32_t w, uint32_t *out);
/* --render-process loopback (render_rec.h render_rec_prehash): the content cache's hashes of the frame's
 * geometry (as the draws below pass it) on a worker thread; the list does not change until the end. */
static void render_prehash(const FrameList *l, const uint32_t *px, uint32_t w) {
    render_rec_prehash_base(l->draws && l->base ? l->base : px, (int)w * 4);   /* first: renderer_begin needs it first */
    const int skin_gpu = renderer_skin_on() && !skin_force_cpu;   /* GPU-skinned draws pass their streams, not l->v */
    for (uint32_t i = 0; i < l->nops; i++) {
        const FrameOp *op = &l->ops[i];
        if (op->kind != OP_DRAW) continue;
        if (op->skin && op->skin <= l->nskin) {
            const SkinRec *r = &l->skin[op->skin - 1];
            /* its constants' register delta from the previous skinned draw's (skin_draw passes this block
             * unless --anim-interp blends it: the encoder then compares as before); the record carries the
             * constants before its payloads */
            if (skin_gpu && (uint64_t)r->consts + VSH_NCONST * 16 <= l->skin_nbytes) render_rec_prehash_consts(l->skin_bytes + r->consts);
            if ((uint64_t)r->data + r->data_bytes <= l->skin_nbytes) render_rec_prehash(l->skin_bytes + r->data, r->data_bytes);
        }
        if ((!op->skin || !skin_gpu) && (uint64_t)op->first_vertex + op->nv <= l->nv) render_rec_prehash(l->v + op->first_vertex, (size_t)op->nv * sizeof(RVertex));
        if ((uint64_t)op->first_index + op->ni <= l->ni) render_rec_prehash(l->idx + op->first_index, (size_t)op->ni * 4);
        if (op->wl.first && ((uint64_t)op->wl.first - 1 + op->nv) * 3 <= l->wl.nn)
            render_rec_prehash(l->wl.n + (size_t)(op->wl.first - 1) * 3, (size_t)op->nv * 12);
    }
    render_rec_prehash_start();
}
static int render_frame(const FrameList *l, const uint32_t *px, uint32_t w, uint32_t *out) {
    const int prehash = render_rec_prehash_on();
    if (prehash) render_prehash(l, px, w);
    const int r = render_frame_ops(l, px, w, out);
    if (prehash) render_rec_prehash_end();
    return r;
}
static int render_frame_ops(const FrameList *l, const uint32_t *px, uint32_t w, uint32_t *out) {
    static int warned;
    const int mat_map = mat_wanted() ? mat_take() : 0;   /* before the frame is recorded: it may wait for the GPU */
    if (walls_wanted()) wall_take(l);   /* likewise */
    else if (hle_d3d_volfog.on) mat_poll();   /* --volfog: the ground mist's map */
    renderer_begin(l->draws && l->base ? l->base : px, (int)w * 4);
    tp_frame(l->draws);
    tp_budget_frame();
    const int skin_gpu = renderer_skin_on() && !skin_force_cpu;   /* else skin_cpu_fill filled their vertices */
    skin_check_n = 0;
    if (hle_d3d_tri_check) tri_check_frame_begin();
    anim_stats_frame(l);
    /* --bloom: the scene ends with its last depth-tested back-buffer draw; the HUD, text and menu
     * overlays come after it without depth test (none in a frame of menus or movies: no bloom). The
     * lamp halo draws move to right after the split (bloom_order): they are not bloomed. Without
     * bloom the ops replay in their recorded order. */
    static uint32_t *order;
    static uint8_t *kind;
    static uint32_t order_cap, kind_cap;
    uint32_t split = l->nops;   /* replay position after which renderer_bloom runs (none: nops) */
    const uint32_t *ord = NULL;
    /* --cinema-bars (cinema_bars.h): the title's cutscene letterbox bars are depth-tested draws
     * near the end of the frame. They are not part of the 3D scene for the rules below (bloom
     * split, scene end, filmic look): the bars are replayed right after the scene's last draw,
     * over every post pass anchored there, and drawn without the depth test. */
    static uint8_t *bar, *scene_op;
    static uint32_t bar_cap, scene_cap;
    const uint8_t *is_bar = NULL;
    int bars_seen = -1;   /* -1: not scanned */
    if (cinema_bars_on && l->draws && grow((void **)&bar, &bar_cap, l->nops, 1) && grow((void **)&scene_op, &scene_cap, l->nops, 1)) {
        uint32_t nbars = 0;
        for (uint32_t i = 0; i < l->nops; i++) {
            const FrameOp *op = &l->ops[i];
            const int back = op->kind == OP_DRAW && !op->target_c && !op->target_z;
            bar[i] = back && !op->skin && cinema_bar_draw(l->v + op->first_vertex, op->nv, op->tex_key[0] || op->tex_surf[0], op->vsh != 0, op->rs.blend_enable != 0);
            scene_op[i] = back && !bar[i] && (op->rain || (op->draw_flags & RENDERER_DEPTH_TEST));
            nbars += bar[i];
        }
        if (nbars) is_bar = bar;
        bars_seen = nbars != 0;
    }
    /* hle_gore: the character blood is the retail one while the cutscene bars show */
    if (cinema_bars_listener && l->draws) cinema_bars_listener(bars_seen >= 0 ? bars_seen : frame_shows_bars(l));
    if (hle_d3d_bloom.on && renderer_bloom_on() && l->draws && grow((void **)&order, &order_cap, l->nops, sizeof *order) &&
        grow((void **)&kind, &kind_cap, l->nops, sizeof *kind)) {
        for (uint32_t i = 0; i < l->nops; i++) {
            const FrameOp *op = &l->ops[i];
            const int off = op->target_c || op->target_z;
            kind[i] = op->kind == OP_CLEAR ? (off ? BLOOM_OP_OTHER : BLOOM_OP_CLEAR)
                      : op->kind != OP_DRAW || (is_bar && is_bar[i]) || !bloom_scene_draw(off, op->draw_flags) ? BLOOM_OP_OTHER
                      : op->halo ? BLOOM_OP_HALO : BLOOM_OP_SCENE;
        }
        const uint32_t at = bloom_order(kind, l->nops, order);
        if (at) split = at - 1;
        ord = order;
    }
    if (is_bar && grow((void **)&order, &order_cap, l->nops, sizeof *order)) {
        static uint32_t *bar_tmp;
        static uint32_t bar_tmp_cap;
        if (!ord)
            for (uint32_t i = 0; i < l->nops; i++) order[i] = i;
        ord = order;
        if (grow((void **)&bar_tmp, &bar_tmp_cap, l->nops, sizeof *bar_tmp)) {
            const uint32_t moved = cinema_bars_reorder(order, l->nops, is_bar, scene_op, bar_tmp, &split);
            static int logged;
            if (!logged++) printf("[cinema-bars] cutscene letterbox bars kept over the post effects (%u moved after the scene's last draw)\n", moved);
        }
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
    DepthFit depth = {0};   /* the frame's depth projection, fitted on first use */
    RRain rr;
    int rain = level > 0 && frame_rain(l, &depth, fade.layer, (const float(*)[2])fade.scroll, fade.n, &rr);
    /* --damp-streets: outdoors at night without rain (neither rain HD's nor the title's own layers)
     * and with the level's material map, the damp ground fades in by the title's clock, else out;
     * while it rains the rain's wet ground is the only one. */
    static DampFade damp_fade;
    float damp_level = 0;
    if (hle_d3d_damp.on && renderer_rain_wet_on() && l->draws) {
        const int want = !rain && !l->sky_rain && mat_map && l->has_cam && frame_damp_sky(l);
        damp_level = damp_fade_step(&damp_fade, want, l->has_frame_ms, l->frame_ms);
    }
    const int damp = damp_level > 0 && !rain && mat_map && l->has_cam;
    /* The replay position of the frame's last depth-tested back-buffer draw (not a rain layer):
     * the 3D scene ends there (with bloom: the halos moved after the split, else the split's own
     * draw); the HUD, text and other overlays come after it without depth test. */
    uint32_t scene_k = l->nops;
    for (uint32_t k = 0; k < l->nops && (rain || damp); k++) {
        const FrameOp *op = &l->ops[ord ? ord[k] : k];
        if (op->kind == OP_DRAW && !op->rain && !op->target_c && !op->target_z && (op->draw_flags & RENDERER_DEPTH_TEST) &&
            !(is_bar && is_bar[ord ? ord[k] : k]))
            scene_k = k;
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
    const uint32_t ssao_at = hle_d3d_ssao.on && renderer_ssao_on() && l->draws ? frame_ssao(l, &depth, &sp) : l->nops;
    /* --lamp-lights: at the same place, right before it (they share the split). --char-lighting
     * uses the same light list (built for it alone too) in the character draws: with the lamp pass
     * it needs the character mask, which lets the lamp pass leave character pixels to it (without
     * the mask the lamp pass lights them as before and char lighting stays off this frame). */
    static RLampLights lp, clp;
    const int lamp_pass = hle_d3d_lamp.on && renderer_lamp_lights_on();
    const int char_lit_on = hle_d3d_char_light.on && renderer_char_lights_on() && (!lamp_pass || renderer_rain_chars_on());
    /* --world-lighting: the same light list in the world and object draws (with their recorded
     * normals); with the lamp pass it needs the character mask too, so the lamp pass lights only
     * the characters (no_world), or nothing when char lighting lights them: then it is skipped. */
    const int world_lit_on = hle_d3d_world_light.on && renderer_char_lights_on() && (!lamp_pass || renderer_rain_chars_on());
    /* --lamp-shadows: the same light list again, with the frame's receiver projection. */
    const int lamp_shadow_on = hle_d3d_lamp_shadow.on && renderer_lamp_shadows_on() && l->has_lsh_prj;
    /* --light-shafts: the same light list (built for it alone too) as cones in the air, in a split of
     * its own right after the lamp, ssao and splash ones (frame_shafts). */
    const int shafts_on = hle_d3d_shafts.on && !hle_d3d_volfog.on && renderer_light_shafts_on();
    /* --volfog: the same list again (built for it alone too), plus the neon signs, in the air
     * through the froxel volume (frame_volfog), in the shafts' place (it replaces them). */
    const int volfog_on = hle_d3d_volfog.on && renderer_volfog_on();
    /* --wet-light-streaks: the same light list again, mirrored in the wet ground (built for it alone too). */
    const int streaks_on = rain && wet_streaks_used() && renderer_rain_wet_on();
    const uint32_t lights_at =
        (lamp_pass || char_lit_on || world_lit_on || lamp_shadow_on || shafts_on || volfog_on || streaks_on || damp) && l->draws ? frame_lamp(l, &depth, &lp) : l->nops;
    if (lamp_shadow_on && lights_at < l->nops) {
        static RLampShadows ls;
        const int k = lamp_shadow_setup((const float (*)[4])l->lsh_prj, frame_lamp_world, lp.n, frame_lamp_eye, hle_d3d_lamp_shadow.strength, &ls);
        lamp_shadow_angles(&ls, hle_d3d_lamp_shadow.min_angle);
        ls.title_keep = hle_d3d_title_shadow_full != 0;   /* --title-shadow-full: the title's shadow stays under lamps */
        ls.no_walls = !hle_d3d_lamp_shadow.walls;         /* the map may be loaded for --light-walls alone */
        if (k > 0) renderer_lamp_shadows(&ls);
        static int logged;
        if (!logged++)
            printf("[lamp-shadows] %d of %u lights cast character shadows%s (light projection A %.5g B %.5g)\n", k < 0 ? 0 : k, lp.n,
                   k < 0 ? ": not a perspective light, none" : "", ls.depth_a, ls.depth_b);
    }
    static RLightShafts shp;
    const uint32_t shaft_at = shafts_on && lights_at < l->nops && frame_shafts(l, &lp, &shp) ? lights_at : l->nops;
    uint32_t lamp_at = lamp_pass ? lights_at : l->nops;
    const int char_lit = char_lit_on && lights_at < l->nops;
    const int world_lit = world_lit_on && lights_at < l->nops;
    lp.no_world = 0;
    uint32_t lit_from = l->nops, lit_to = 0;
    if (char_lit || world_lit) {
        clp = lp;
        clp.world_gain = hle_d3d_world_light.strength;
        clp.title_shadow_full = hle_d3d_title_shadow_full != 0;
        const uint8_t *kind;
        const uint32_t end = frame_world_end(l, &kind);
        if (end < l->nops) {
            uint32_t from = 0;
            for (uint32_t i = 0; i <= end; i++)
                if (kind[i] == SSAO_OP_CLEAR) from = i + 1;
            lit_from = from;
            lit_to = end;
            renderer_char_lights(&clp);
            if (char_lit) lp.char_gain = 0;   /* the lamp pass leaves character pixels to the lit draws */
            if (world_lit) lp.no_world = 1;   /* and the rest to the world-lit draws */
            if (char_lit && world_lit) lamp_at = l->nops;
        }
    }
    /* --contact-shadows: at the same place, before both (they share the split). */
    RContact cs;
    const uint32_t contact_at = hle_d3d_contact.on && renderer_contact_on() && l->draws ? frame_contact(l, &depth, &cs) : l->nops;
    /* --rain-hd splashes: right after it (the same split when both run). */
    RRainSplash rs;
    const uint32_t splash_at = rain && hle_d3d_rain.splash && renderer_rain_splash_on() ? frame_splash(l, &depth, fade.layer, fade.n, &rs) : l->nops;
    if (splash_at < l->nops) rain_fade_apply(level, NULL, &rs, NULL);
    /* --soft-particles: from the same place (after the occlusion and splashes), the particle draws
     * read the depth of the opaque world. */
    RSoft so;
    const uint32_t soft_at = hle_d3d_soft.on && renderer_soft_on() && l->draws ? frame_soft(l, &depth, &so) : l->nops;
    /* --rain-hd wet ground: where the 3D scene ends (scene_k: the effects, halos and bloom are
     * drawn by then, so they are reflected too), before any HUD, text or overlay, which the colour
     * it reflects must not hold (the title may draw them before its rain layers). */
    RRainWet rw;
    /* --neon-reflections: the frame's glowing signs, for the streaks of the wet or damp ground (and
     * the lights --reflections sssr's glass mirrors). */
    static LampLight signs[NEON_REFL_CAP];
    const uint32_t nsigns =
        hle_d3d_neon_refl.on && ng.active && (((streaks_on || damp || renderer_wet_glass_on()) && scene_k < l->nops) || (volfog_on && lights_at < l->nops))
            ? frame_neon(l, signs)
            : 0;
    static RVolfog vf;
    const uint32_t volfog_at = volfog_on && lights_at < l->nops && frame_volfog(l, &lp, signs, nsigns, damp_level, &vf) ? lights_at : l->nops;
    int wet = rain && scene_k < l->nops && hle_d3d_rain.wet && renderer_rain_wet_on() && frame_wet(l, &depth, fade.layer, fade.n, &rw);
    if (wet && streaks_on) {   /* --wet-light-streaks: frame_lamp's lights (none when it found none) */
        const uint32_t nl = lights_at < l->nops ? lp.n : 0, nlamp = frame_lamp_n < nl ? frame_lamp_n : nl;
        const uint32_t k = wet_streaks_build(&hle_d3d_wet_streaks, frame_wet_light, nl, nlamp, hle_d3d_lamp.strength, l->cam_view, l->cam_proj,
                                             l->has_fog, l->fog_color, &rw);
        static int logged;
        if (rw.streaks > 0 && !logged++)
            printf("[wet-streaks] %u of %u lights mirrored in the wet ground (cap %d, strength %.2f); miss fallback %s %06x\n", k, nl,
                   hle_d3d_wet_streaks.cap, hle_d3d_wet_streaks.strength, l->has_fog ? "fog" : "default", l->has_fog ? l->fog_color & 0xFFFFFFu : WET_STREAKS_FOG_DEFAULT);
        if (hle_d3d_stats && !(hle_d3d_frame_count() % 600)) printf("[wet-streaks] frame %u: %u of %u lights\n", hle_d3d_frame_count(), k, nl);
        frame_neon_apply(l, signs, nsigns, &rw);   /* --neon-reflections */
    }
    if (wet && materials_wet_build(&hle_d3d_materials, mat_map, l->cam_view, l->cam_proj, &rw)) {   /* --materials */
        static int logged;
        if (!logged++) printf("[materials] the wet ground takes the surface materials (strength %.2f)\n", rw.materials);
    }
    if (wet) rain_fade_apply(level, NULL, NULL, &rw);
    if (!wet && damp && scene_k < l->nops) wet = frame_damp(l, &depth, lights_at < l->nops ? lp.n : 0, signs, nsigns, damp_level, &rw);   /* --damp-streets */
    if (wet) sssr_glass_apply(&hle_d3d_sssr, &rw);   /* --reflections sssr: glass on the wet or damp frame's pass */
    else if (scene_k < l->nops) wet = frame_glass(l, &depth, lights_at < l->nops ? lp.n : 0, signs, nsigns, &rw);   /* and alone on a dry one */
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
    /* --world-shadows: the same opaque world (the SSAO set, characters excepted: the title's own
     * map has them) is drawn again along the shadow light into the world shadow map; the frame's
     * world-program receivers test it (off under a roof: the renderer's indoor gate), weaker near the
     * tracked lamps of the light list when one of the lamp effects built it. */
    uint32_t ws_from = l->nops, ws_to = 0;
    if (hle_d3d_world_shadow.on && renderer_world_shadow_on() && l->has_ws && l->draws) {
        static double last_ms;
        const double t = now_ms();
        const float dt = last_ms > 0 ? (float)((t - last_ms) / 1000.0) : 0;
        last_ms = t;
        float lamps[RENDERER_WORLD_SHADOW_LAMPS][4];
        uint32_t nl = 0;
        for (uint32_t k = 0; lights_at < l->nops && k < lp.n && nl < RENDERER_WORLD_SHADOW_LAMPS; k++) {
            if (!(frame_lamp_world[k].intensity > 0.01f)) continue;
            memcpy(lamps[nl], frame_lamp_world[k].pos, sizeof frame_lamp_world[k].pos);
            lamps[nl++][3] = frame_lamp_world[k].radius;
        }
        RWorldShadow ws;
        if (world_shadow_build(&hle_d3d_world_shadow, (const float (*)[4])l->ws_wvp, l->ws_sun, (const float (*)[4])lamps, nl, &ws)) {
            ws.dt = dt;
            const uint8_t *kind;
            const uint32_t end = frame_world_end(l, &kind);
            if (end < l->nops) {
                uint32_t from = 0, n = 0;
                for (uint32_t i = 0; i <= end; i++)
                    if (kind[i] == SSAO_OP_CLEAR) from = i + 1;
                for (uint32_t i = from; i <= end; i++) n += kind[i] == SSAO_OP_OPAQUE && !l->ops[i].character;
                ws_from = from;
                ws_to = end;
                renderer_world_shadow(&ws);
                static int logged;
                if (!logged++)
                    printf("[world-shadows] %u opaque world draws (ops %u-%u) cast along (%.3f %.3f %.3f), %u lamps\n", n, from, end, l->ws_sun[0],
                           l->ws_sun[1], l->ws_sun[2], nl);
            }
        }
        static uint32_t frames;
        if (hle_d3d_stats && ++frames % 300 == 0) {
            float share;
            const float g = renderer_world_shadow_gate(&share);
            printf("[world-shadows] indoor gate %.2f (%.0f%% of the points ahead of the camera under cover)\n", g, 100 * share);
        }
    }
    /* --cutscene-dof: at the filmic anchor, while the cutscene letterbox bars show (found here also
     * with --no-cinema-bars), fading in and out with them; the focus snaps at a new cutscene and at
     * a cut of the title's camera. Decided here: its frames need the character mask below. */
    RDofFrame dofp = {0};
    if (hle_d3d_dof.on && renderer_dof_on() && l->draws) dof_frame(l, &depth, &dofp);
    if (hle_d3d_auto_exposure.on && renderer_auto_exposure_on() && l->draws) ae_cut_frame(l);
    /* The character mask (--rain-hd, lamp lights, contact shadows, --cutscene-dof): with splashes,
     * wet ground, lamps, contact shadows or the depth of field this frame, the characters' draws of
     * the 3D scene (depth-tested back-buffer draws after the frame's last depth clear, up to the end
     * of the opaque world) are flagged RENDERER_CHARACTER: they get no splashes or wet ground, the
     * flatness tests around them ignore them, they are left out of the rain cover map, and the
     * depth of field keeps them sharp. */
    uint32_t chars_from = l->nops, chars_to = 0;
    if ((splash_at < l->nops || wet || lamp_at < l->nops || contact_at < l->nops || dofp.weight > 0) && renderer_rain_chars_on()) {
        const uint8_t *kind;
        const uint32_t end = frame_world_end(l, &kind);
        if (end < l->nops) {
            uint32_t from = 0, n = 0;
            for (uint32_t i = 0; i <= end; i++)
                if (kind[i] == SSAO_OP_CLEAR) from = i + 1;
            for (uint32_t i = from; i <= end; i++) n += l->ops[i].kind == OP_DRAW && l->ops[i].character;
            chars_from = from;
            chars_to = end;
            static int logged, dof_logged;
            if (n && dofp.weight > 0 && !dof_logged++) printf("[dof] character mask: %u character draws kept sharp\n", n);
            else if (n && !(dofp.weight > 0) && !logged++) printf("[rain] character mask: %u character draws kept dry (no splashes, wet ground or cover)\n", n);
        }
    }
    /* --filmic and --auto-exposure: after the 3D scene's last draw (scene_k's rule, the title's rain
     * layers included: the bloom split, halos, wet ground and rain are drawn by then), before the
     * HUD, text and menu overlays; none in a frame without a 3D scene (menus, movies: the
     * adaptation holds). */
    uint32_t filmic_k = l->nops;
    for (uint32_t k = 0; k < l->nops && l->draws && renderer_filmic_on(); k++) {
        const FrameOp *op = &l->ops[ord ? ord[k] : k];
        if (op->kind == OP_DRAW && !op->target_c && !op->target_z && (op->rain || (op->draw_flags & RENDERER_DEPTH_TEST)) &&
            !(is_bar && is_bar[ord ? ord[k] : k]))
            filmic_k = k;
    }
    /* --taa: the end of the jittered 3D scene, after its last depth-tested
     * back-buffer draw (not a cutscene bar or a rain layer) and before anything that should see the
     * stable image or must not be accumulated: the bloom split (its glow; the halos come after it),
     * the wet ground, the procedural rain (its first layer op or its fade-out position) and the
     * filmic anchor. None in a frame without a 3D scene (menus, movies). */
    uint32_t taa_k = l->nops;
    RTaa taa;
    if (taa_options.on && renderer_taa_on() && l->draws) {
        uint32_t end = l->nops, first_rain = l->nops;
        for (uint32_t k = 0; k < l->nops; k++) {
            const uint32_t i = ord ? ord[k] : k;
            const FrameOp *op = &l->ops[i];
            if (op->kind != OP_DRAW || op->target_c || op->target_z || (is_bar && is_bar[i])) continue;
            if (op->rain) {
                if (rain && first_rain == l->nops) first_rain = k;
            } else if (op->draw_flags & RENDERER_DEPTH_TEST) {
                end = k;
            }
        }
        const uint32_t before_rain = first_rain == 0 ? l->nops : first_rain - 1;
        const uint32_t lim[5] = {split, wet_k, rain_k, filmic_k, before_rain};
        taa_k = end;
        for (int n = 0; n < 5; n++)
            if (lim[n] < taa_k) taa_k = lim[n];
        if (taa_k < l->nops) frame_taa(l, &depth, is_bar != NULL, &taa);
    }
    /* --frame-gen: the HUD-less frame and its motion at the filmic anchor's
     * place (after the 3D scene's last draw, its bloom, halos, wet ground, rain, filmic look and depth of
     * field; before the HUD, text and menus); none in a frame without a 3D scene (menus, movies: the
     * real frame is shown twice). */
    uint32_t fg_k = l->nops, fg_end_k = l->nops;
    RFrameGen fgp;
    /* --hdr (src/hdr.h): the same two places tell the HDR output the scene from the HUD (what changed
     * in between is the HUD, at paper white): renderer_hdr_scene and renderer_hdr_hud_end. */
    const int fg_want = framegen_options.on && renderer_frame_gen_on() && l->draws && (out || fg_show);
    const int hdr_want = hle_d3d_hdr.mode != HDR_OFF && renderer_hdr_on() && l->draws;
    if (fg_want || hdr_want) {
        for (uint32_t k = 0; k < l->nops; k++) {
            const FrameOp *op = &l->ops[ord ? ord[k] : k];
            if (op->kind == OP_DRAW && !op->target_c && !op->target_z && (op->rain || (op->draw_flags & RENDERER_DEPTH_TEST)) &&
                !(is_bar && is_bar[ord ? ord[k] : k]))
                fg_k = k;
        }
        /* The full-screen draws right after the scene (the title's colour tint, the frame-feedback
         * blur, a fade) belong to it: as HUD they would make every pixel the real frame's. Offscreen
         * draws and the bars are passed over; the first other back-buffer operation ends it. */
        uint32_t full = 0;
        for (uint32_t k = fg_k + 1; fg_k < l->nops && k < l->nops; k++) {
            const uint32_t i = ord ? ord[k] : k;
            const FrameOp *op = &l->ops[i];
            if ((op->kind == OP_DRAW && (op->target_c || op->target_z)) || (is_bar && is_bar[i])) continue;
            if (op->kind != OP_DRAW || !framegen_full_screen(l->v + op->first_vertex, sizeof(RVertex), op->nv)) break;
            fg_k = k;
            full++;
        }
        static int full_logged;
        if (fg_want && full && !full_logged++) printf("[frame-gen] the HUD-less frame includes %u full-screen draw%s after the scene (tint, blur)\n", full, full > 1 ? "s" : "");
        /* The HUD ends before the full-screen draws at the end of the frame (the title draws its tint
         * and frame-feedback blur over its HUD): those cover the scene and the HUD alike, so the HUD
         * is what the frame gained between the anchor and them (renderer_frame_gen_hud_end). */
        for (uint32_t k = l->nops; fg_k < l->nops && k-- > fg_k + 1;) {
            const uint32_t i = ord ? ord[k] : k;
            const FrameOp *op = &l->ops[i];
            if ((op->kind == OP_DRAW && (op->target_c || op->target_z)) || (is_bar && is_bar[i])) continue;
            if (op->kind != OP_DRAW || !framegen_full_screen(l->v + op->first_vertex, sizeof(RVertex), op->nv)) break;
            fg_end_k = k - 1;
        }
        if (fg_want && fg_k < l->nops) frame_fg(l, &depth, is_bar != NULL, &fgp);
    }
    const uint32_t hdr_k = hdr_want ? fg_k : l->nops, hdr_end_k = hdr_want ? fg_end_k : l->nops;
    if (!fg_want) fg_k = fg_end_k = l->nops;
    /* --sky-hd: the dome draw is replaced by the procedural sky, the cap is dropped, and the skyline
     * draw is followed by its windows and haze. */
    SkyStage sst;
    RSky sky;
    RSkyline sline;
    int sky_line = 0;
    const int sky_on = hle_d3d_sky.on && renderer_sky_on() && l->draws && frame_sky(l, &sst, &sky, &sline, &sky_line);
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
        } else if (sky_on && (int32_t)i == sst.dome) {
            renderer_sky(&sky);
        } else if (sky_on && (int32_t)i == sst.cap) {
            /* the procedural sky has its own clouds */
        } else {
            tex_race_skip = 0;
            if (op->prev_mark) renderer_prev_frame_mark();
            int no_prev = 0;   /* a stage samples the previous frame, which the renderer did not keep: no draw */
            for (int s = 0; s < RENDERER_STAGES; s++) {
                const uint32_t h = op->tex_surf[s] ? rt_handle(op->tex_surf[s]) : op->tex_key[s] ? resolve_texture(op, s) : 0;
                no_prev |= RT_IS_PREV(op->tex_surf[s]) && !h;
                renderer_set_stage_texture(s, h);
            }
            renderer_set_pixel_shader(op->has_ps ? &op->ps : NULL);
            neon_bind(op, off);
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
            const int person = op->character || op->hat;   /* --hat-fix: a worn hat is part of its character */
            const int cover = i >= cover_from && i <= cover_to && !off && !person && ssao_opaque_draw(off, op->draw_flags, op->rs.depth_write);
            const int chr = i >= chars_from && i <= chars_to && !off && person && (op->draw_flags & RENDERER_DEPTH_TEST);
            const int wsc = i >= ws_from && i <= ws_to && !off && !person && ssao_opaque_draw(off, op->draw_flags, op->rs.depth_write);
            const uint32_t fl = (op->draw_flags & ~(is_bar && is_bar[i] ? RENDERER_DEPTH_TEST : 0u)) | (cover ? RENDERER_RAIN_COVER : 0) |
                                (chr ? RENDERER_CHARACTER : 0) | (wsc ? RENDERER_WORLD_SHADOW : 0) | (op->world_particle ? RENDERER_PARTICLE : 0);
            const int lit = char_lit && i >= lit_from && i <= lit_to && !off && op->character && (op->draw_flags & RENDERER_DEPTH_TEST);
            const int hat_lit = char_lit && i >= lit_from && i <= lit_to && !off && op->hat && !op->skin && op->wl.first && (op->draw_flags & RENDERER_DEPTH_TEST);
            const int wlit = world_lit && lit_from < l->nops && i >= lit_from && !off && op->wl.first && !hat_lit;
            relief_bind(op, off, lit || wlit || hat_lit, wlit && !lit);
            if (hle_d3d_hair.on) hair_bind(op, lit);
            if (hle_d3d_eyes.on || hle_d3d_cloth.on) mat_bind(op, lit);
            else if (hle_d3d_skin.on) skin_bind(op, lit);
            if (hle_d3d_tri_check) tri_check_op(l, op, i, skin_gpu);
            if (tex_race_skip)   /* a stage texture changed under the replay (tex_race.h): drawn again next frame */
                tex_race_skipped++;
            else if (no_prev)
                ;
            else if (soft_at < l->nops && i > soft_at && op->particle)
                soft_draw(l, op, fl);
            else if (op->skin && skin_gpu)
                skin_draw(l, op, fl, lit, clp.px, clp.py, wlit ? &clp : NULL);
            else if (lit)
                char_lit_draw(l, op, fl, clp.px, clp.py);
            else if (hat_lit)
                hat_lit_draw(l, op, fl);
            else if (wlit)
                world_lit_draw(l, op, fl, &clp);
            else
                renderer_draw(&op->vp, fl, l->v + op->first_vertex, op->nv, l->idx + op->first_index, op->ni);
            if (sky_on && sky_line && !tex_race_skip && (int32_t)i == sst.skyline)
                renderer_skyline(&op->vp, l->v + op->first_vertex, op->nv, l->idx + op->first_index, op->ni, &sline);
        }
        if (i == contact_at) renderer_contact(&cs);
        if (i == lamp_at) renderer_lamp_lights(&lp);
        if (i == ssao_at) renderer_ssao(&sp);
        if (i == splash_at) renderer_rain_splash(&rs);
        if (i == shaft_at) renderer_light_shafts(&shp);
        if (i == volfog_at) renderer_volfog(&vf);
        if (i == soft_at) renderer_soft(&so);
        if (k == taa_k) renderer_taa(&taa);
        if (k == split) renderer_bloom();
        if (k == wet_k) renderer_rain_wet(&rw);
        if (k == rain_k && !rain_done++) renderer_rain(&rr);
        if (k == filmic_k) {
            if (dofp.weight > 0) renderer_dof(&dofp);
            renderer_filmic();
        }
        if (k == fg_k) renderer_frame_gen(&fgp);
        if (k == hdr_k) renderer_hdr_scene();
        if (k == fg_end_k && fg_k < l->nops) {
            renderer_set_target(0, 0);   /* on the frame target (the next operation sets its own) */
            renderer_frame_gen_hud_end();
        }
        if (k == hdr_end_k && hdr_k < l->nops) {
            renderer_set_target(0, 0);
            renderer_hdr_hud_end();
        }
    }
    if (hle_d3d_stats && renderer_dof_on()) {   /* --stats: the focus once per 60 frames that ran it */
        static uint32_t dof_frames;
        float fs[4];
        if (renderer_dof_count() && !(++dof_frames % 60) && renderer_dof_state(fs))
            printf("[dof] weight %.2f, sharp %.2f-%.2f view units (%s)\n", fs[3], exp2f(fs[0]), exp2f(fs[1]),
                   fs[2] > 2.5f ? "speaker" : fs[2] > 1.5f ? "characters" : fs[2] > 0.5f ? "centre meter" : "nothing metered");
    }
    if (hle_d3d_stats && renderer_auto_exposure_on()) {   /* --stats: the adaptation once per 60 frames */
        static uint32_t ae_frames;
        float ae[4];
        if (!(++ae_frames % 60) && renderer_auto_exposure_state(ae))
            printf("[exposure] meter %.2f, adapted %.2f (log2 linear luma), gain %+.2f stop\n", ae[3], ae[0], ae[1]);
    }
    if (ng.active) renderer_set_emission(0, 0, 0, 0);   /* nothing drawn after the frame's operations glows */
    if (rm.bound) renderer_set_relief(0, 0, 0, 0, 0, 0);
    rm.bound = 0;
    if (l->cd_slot && l->cd_n) combat_draw(l);   /* --combat-debug: the volumes over the frame */
    if (!photo_no_overlay) modmenu_draw(w, modmenu_h);   /* a mod's menu over the frame (hle_modmenu.h); not in a photo capture */
    renderer_gamma_ramp(l->gamma ? l->gamma_lut : NULL);   /* --gamma-ramp: the last pass on the frame */
    if (l->gamma) {
        static int gamma_logged;
        if (!gamma_logged++) {
            if (renderer_gamma_ramp_on())
                printf("[renderer] game gamma ramp on (mid-grey 128 -> %u %u %u)\n", l->gamma_lut[128] & 0xff, l->gamma_lut[128] >> 8 & 0xff,
                       l->gamma_lut[128] >> 16 & 0xff);
            else printf("[renderer] game gamma ramp unavailable on this device; frames are shown as rendered\n");
        }
    }
    int failed = out ? renderer_end(out, out_w * 4) : renderer_present();
    if (hle_d3d_tri_check) tri_check_frame_end();
    if (!failed && out) skin_check(l);
    renderer_last_timing(&hstat_end);   /* --stats and the [hitch] lines */
    hstat_valid = 1;
    if (failed) {
        if (!warned++) printf("[renderer] frame failed: %s\n", renderer_error());
        return -1;
    }
    if (l->cpu_after_draw && warned++ < 3) printf("[renderer] %u movie quads after 3D draws were not composited\n", l->cpu_after_draw);
    return 0;
}

/* --fps-overlay: one presented host frame; hands the renderer a new text when the value shown
 * changes (about twice a second). --fps-overlay-items cpu / gpu: this process's CPU time over the
 * same window (100% = every logical core), and the GPU time of the frames completed in it per
 * frame over the mean frame period (the game's own GPU work, timestamps read without waiting). */
static void fps_overlay_frame(void) {
    static FpsMeter meter;
    static char shown[FPS_MAX_CHARS + 8];
    static double cpu0, wall0;
    static float gpu = -1;
    char text[FPS_MAX_CHARS + 8];
    const int items = hle_d3d_fps_overlay_items ? hle_d3d_fps_overlay_items : FPS_ITEMS_DEFAULT;
    const double now = now_ms();
    const int started = meter.t0 <= 0;
    if (!fps_meter_frame(&meter, now)) {
        if (started && (items & FPS_ITEM_CPU)) cpu0 = plat_process_cpu_ms(), wall0 = now;
        return;
    }
    float cpu = -1;
    if (items & FPS_ITEM_CPU) {
        const double c = plat_process_cpu_ms();
        cpu = fps_cpu_percent(c - cpu0, now - wall0, plat_logical_cores());
        cpu0 = c;
        wall0 = now;
    }
    if (items & FPS_ITEM_GPU) {
        double busy;
        int frames;
        if (!renderer_fps_overlay_gpu(&busy, &frames)) gpu = -1;
        else if (frames > 0) gpu = fps_gpu_percent(busy / frames, meter.ms);   /* else: the last value */
    }
    fps_overlay_format_items(text, sizeof text, items, meter.fps, meter.ms, cpu, gpu);
    if (!strcmp(text, shown)) return;
    memcpy(shown, text, sizeof shown);
    renderer_fps_overlay_text(text);
}

/* --frame-gen, once per presented host frame: whether generated frames are shown (framegen_decide on
 * the window's display rate, then the slow-source guard on the interval between frames); the
 * [frame-gen] line whenever the decision or the guard changes. */
static void fg_host_frame(int presenting_now) {
    static FrameGenPace pace;
    static FrameGenClock clock;
    static double last_t;
    static int last_decision = -1, pacing_logged;
    char line[200];
    const FrameGenDecision d = framegen_decide(&framegen_options, renderer_frame_gen_on(), presenting_now, hle_sim60_active,
                                               presenting_now ? video_display_hz() : 0, line, sizeof line);
    if ((int)d != last_decision) {
        printf("%s\n", line);
        last_decision = (int)d;
    }
    const double t = now_ms();
    int changed = 0;
    const int fast = framegen_pace(&pace, last_t > 0 ? t - last_t : 0, &changed);
    last_t = t;
    if (changed && d == FRAMEGEN_SHOW)
        printf("[frame-gen] %s\n", fast ? "the game holds 60 fps again: generated frames resume" : "the game runs under 60 fps: generated frames paused");
    fg_show = d == FRAMEGEN_SHOW && fast;
    /* Paused: the real frame twice, so the swapchain queue and the pacing keep their cadence. */
    renderer_frame_gen_present(fg_show ? 1 : d == FRAMEGEN_SHOW ? 2 : 0);
    if (d != FRAMEGEN_SHOW) return;
    double pt;
    int waited;
    if (renderer_frame_gen_pacing(&pt, &waited)) framegen_clock_add(&clock, pt, waited);
    if (!pacing_logged && !renderer_frame_gen_present_wait()) {
        pacing_logged = 1;
        printf("[frame-gen] pacing: no present waits on this driver (VK_KHR_present_wait): the two presents a frame queue up "
               "in the swapchain as they come\n");
    } else if (pacing_logged < 2 && framegen_clock_period(&clock) > 0) {
        pacing_logged = 2;
        printf("[frame-gen] pacing by present completion: a real frame on screen every %.3f ms (the display at %.2f Hz)\n",
               framegen_clock_period(&clock), 2000.0 / framegen_clock_period(&clock));
    }
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
static void host_stats(double render, double present, uint32_t draws, const FrameTimes *ft) {
    static double t_start, r_sum, e_sum[4], p_sum, lat[4], cpu_start;
    static uint32_t frames, draws_sum, lat_n;
    double t = now_ms();
    if (!t_start) {
        t_start = t;
        cpu_start = process_cpu_ms();
    }
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
    /* Real CPU per presented frame: the whole process (every thread), and the vertex-program
     * pool's busy and spinning time (--stats measures it on the workers). */
    double cpu = process_cpu_ms(), busy = __atomic_exchange_n(&vp_stat.busy_ns, 0, __ATOMIC_RELAXED) / 1e6,
           spin = __atomic_exchange_n(&vp_stat.spin_ns, 0, __ATOMIC_RELAXED) / 1e6;
    uint32_t queued = __atomic_exchange_n(&vp_stat.queued, 0, __ATOMIC_RELAXED),
             inl = __atomic_exchange_n(&vp_stat.inline_draws, 0, __ATOMIC_RELAXED);
    printf("[stats] swap %u: CPU %.1f ms/frame (process, %.2f cores) | vertex-program pool, %d worker%s: busy %.1f ms/frame, "
           "spinning %.1f ms/frame (%.0f%% busy); program draws %.0f/frame queued, %.0f/frame on the recording thread\n",
           sw, (cpu - cpu_start) / frames, (cpu - cpu_start) / (t - t_start > 0 ? t - t_start : 1), vp_workers > 0 ? vp_workers : 0,
           vp_workers == 1 ? "" : "s", busy / frames, spin / frames, busy + spin > 0 ? 100 * busy / (busy + spin) : 0, (double)queued / frames,
           (double)inl / frames);
    {
        double cn = __atomic_exchange_n(&vp_prog_stat.ns[1], 0, __ATOMIC_RELAXED) / 1e6,
               on = __atomic_exchange_n(&vp_prog_stat.ns[0], 0, __ATOMIC_RELAXED) / 1e6;
        double cv = (double)__atomic_exchange_n(&vp_prog_stat.verts[1], 0, __ATOMIC_RELAXED),
               ov = (double)__atomic_exchange_n(&vp_prog_stat.verts[0], 0, __ATOMIC_RELAXED);
        printf("[stats] swap %u: vertex programs CPU %.2f ms/frame: characters %.2f ms (%.0f vertices), others %.2f ms (%.0f vertices)\n",
               sw, (cn + on) / frames, cn / frames, cv / frames, on / frames, ov / frames);
        double w[VP_CPU_WHY];
        for (int k = 0; k < VP_CPU_WHY; k++) w[k] = (double)__atomic_exchange_n(&vp_cpu_why[k], 0, __ATOMIC_RELAXED) / frames;
        printf("[stats] swap %u: world/object program vertices on the CPU per frame: unsupported %.0f, --no-gpu-vertex-programs %.0f, "
               "debug colour %.0f, hat %.0f, depth stage before the --shadow-range fit %.0f\n",
               sw, w[VP_CPU_UNSUPPORTED], w[VP_CPU_OFF], w[VP_CPU_DEBUG_COLOR], w[VP_CPU_HAT], w[VP_CPU_SHADOW_FIT]);
    }
    {
        static uint32_t since;
        if ((since += frames) >= GPU_PASSES_FRAMES) {
            char what[16];
            snprintf(what, sizeof what, "%u", sw);
            gpu_passes_line(0, what);
            since = 0;
        }
    }
    if (hle_d3d_gpu_skin.on && renderer_skin_on()) {
        float gms;
        uint32_t sd, sv;
        renderer_skin_stats(&gms, &sd, &sv);
        printf("[stats] swap %u: gpu skinning: %u draws, %u vertices in the last frame, device %.3f ms (pre-pass), host %.3f ms/frame (jobs, weld tables)\n",
               sw, sd, sv, gms, skin_host_ms / frames);
    }
    skin_host_ms = 0;
    cpu_start = cpu;
    t_start = t;
    r_sum = p_sum = e_sum[0] = e_sum[1] = e_sum[2] = e_sum[3] = 0;
    lat[0] = lat[1] = lat[2] = lat[3] = 0;
    frames = draws_sum = lat_n = 0;
}

/* ---- photo mode: the frozen frame's replay (hle_d3d.h hle_d3d_photo_*) ---- */
int (*hle_d3d_photo_view)(HleD3dPhotoView *v);
const int *hle_d3d_photo_prepare_hook;
void (*hle_d3d_photo_pixels)(const uint32_t *px, uint32_t w, uint32_t h);
static const FrameList *photo_src;   /* host thread: the last frame taken (the frozen one while frozen) */
int hle_d3d_photo_frozen(void) {
    if (!__atomic_load_n(&photo_blocked, __ATOMIC_ACQUIRE)) return 0;
    pthread_mutex_lock(&d3d_lock);
    const int idle = st.q_count == 0;   /* the host has taken the guest's last frame */
    pthread_mutex_unlock(&d3d_lock);
    return idle;
}
void hle_d3d_photo_output(int *w, int *h) { *w = out_w, *h = out_h; }
int hle_d3d_photo_camera(float wvp[4][4]) {
    if (!photo_src || !photo_src->has_photo_wvp) return 0;
    memcpy(wvp, photo_src->photo_wvp, sizeof photo_src->photo_wvp);
    return 1;
}
/* r = m r (column form; r's rows are clip components) */
static void photo_rows(const float m[4][4], float r[4][4]) {
    float t[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) t[i][j] = m[i][0] * r[0][j] + m[i][1] * r[1][j] + m[i][2] * r[2][j] + m[i][3] * r[3][j];
    memcpy(r, t, sizeof t);
}
/* A D3D row-vector view matrix (p_view = [p 1] V, v[4 i + j]) followed by the view change d. */
static void photo_view_rows(float v[16], const float d[4][4]) {
    float t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) t[4 * i + j] = v[4 * i] * d[j][0] + v[4 * i + 1] * d[j][1] + v[4 * i + 2] * d[j][2] + v[4 * i + 3] * d[j][3];
    memcpy(v, t, sizeof t);
}
/* A D3D row-vector projection followed by the clip-space zoom / tile / jitter. */
static void photo_proj_rows(float p[16], const HleD3dPhotoView *pv) {
    for (int i = 0; i < 4; i++) {
        p[4 * i] = pv->sx * p[4 * i] + pv->ox * p[4 * i + 3];
        p[4 * i + 1] = pv->sy * p[4 * i + 1] + pv->oy * p[4 * i + 3];
    }
}
/* A point / direction of a right-handed view space (z towards the viewer: world_light.h) through d. */
static void photo_rh_point(const float d[4][4], const float in[3], float w, float out[3]) {
    const float p[3] = {in[0], in[1], -in[2]};
    for (int i = 0; i < 3; i++) out[i] = d[i][0] * p[0] + d[i][1] * p[1] + d[i][2] * p[2] + d[i][3] * w;
    out[2] = -out[2];
}
/* The frozen frame src as the photo view pv sees it: a copy whose vertices, GPU-skinned constants,
 * view-dependent inputs and hidden draws are changed (the index, skin and base arrays are shared). */
static const FrameList *photo_list(const FrameList *src, const HleD3dPhotoView *pv) {
    static FrameList l;
    static FrameOp *ops;
    static RVertex *v;
    static uint8_t *sb, *done, *cdone;
    static float *wn;
    static uint32_t ops_cap, v_cap, sb_cap, done_cap, wn_cap, cdone_cap;
    if (!grow((void **)&ops, &ops_cap, src->nops + 1, sizeof *ops) || !grow((void **)&v, &v_cap, src->nv + 1, sizeof *v) ||
        !grow((void **)&sb, &sb_cap, src->skin_nbytes + 1, 1) || !grow((void **)&done, &done_cap, src->nv + 1, 1) ||
        !grow((void **)&wn, &wn_cap, src->wl.nn * 3 + 1, sizeof *wn) || !grow((void **)&cdone, &cdone_cap, src->skin_nbytes / 16 + 1, 1))
        return NULL;
    l = *src;
    memcpy(ops, src->ops, (size_t)src->nops * sizeof *ops);
    memcpy(v, src->v, (size_t)src->nv * sizeof *v);
    if (src->skin_nbytes) memcpy(sb, src->skin_bytes, src->skin_nbytes);
    if (src->wl.nn) memcpy(wn, src->wl.n, (size_t)src->wl.nn * 3 * sizeof *wn);   /* nn normals, 3 floats each */
    memset(done, 0, src->nv);
    memset(cdone, 0, src->skin_nbytes / 16 + 1);   /* consecutive skinned draws share one constant block: transform it once */
    l.ops = ops, l.v = v, l.skin_bytes = sb, l.wl.n = wn;
    for (uint32_t i = 0; i < l.nops; i++) {
        FrameOp *op = &ops[i];
        if (op->kind == OP_GLASS && pv->hide_hud) { op->kind = OP_CLEAR, op->flags = 0; continue; }
        if (op->kind != OP_DRAW || op->target_c || op->target_z) continue;
        if ((pv->hide_hud && !op->cam3d) || (pv->hide_chars && (op->character || op->hat))) {
            op->nv = op->ni = 0;
            continue;
        }
        if (!op->cam3d) continue;
        const float (*m)[4] = op->cam3d == 2 ? pv->m_sky : pv->m;
        for (uint32_t k = op->first_vertex; k < op->first_vertex + op->nv && k < l.nv; k++) {
            if (done[k]) continue;
            done[k] = 1;
            RVertex *p = &v[k];
            const float c[4] = {p->x, p->y, p->z, p->w};
            p->x = m[0][0] * c[0] + m[0][1] * c[1] + m[0][2] * c[2] + m[0][3] * c[3];
            p->y = m[1][0] * c[0] + m[1][1] * c[1] + m[1][2] * c[2] + m[1][3] * c[3];
            p->z = m[2][0] * c[0] + m[2][1] * c[1] + m[2][2] * c[2] + m[2][3] * c[3];
            p->w = m[3][0] * c[0] + m[3][1] * c[1] + m[3][2] * c[2] + m[3][3] * c[3];
        }
        if (op->skin && op->skin <= l.nskin) {   /* --gpu-skinning: its c69-c72 */
            const SkinRec *r = &l.skin[op->skin - 1];
            if ((uint64_t)r->consts + VSH_NCONST * 16 <= l.skin_nbytes && !cdone[r->consts / 16]) {
                cdone[r->consts / 16] = 1;
                photo_rows(m, (float (*)[4])(sb + r->consts + C_WVP * 16));
            }
        }
        if (op->wl.first) {   /* --world-lighting: its view normals and box (right-handed view space) */
            for (uint32_t k = 0; k < op->nv && (uint64_t)op->wl.first - 1 + k < l.wl.nn; k++) {
                float *n = wn + ((size_t)op->wl.first - 1 + k) * 3, o[3];
                photo_rh_point(pv->d, n, 0, o);
                memcpy(n, o, sizeof o);
            }
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (int c = 0; c < 8; c++) {
                const float q[3] = {op->wl.box[c & 1 ? 3 : 0], op->wl.box[c & 2 ? 4 : 1], op->wl.box[c & 4 ? 5 : 2]};
                float o[3];
                photo_rh_point(pv->d, q, 1, o);
                for (int a = 0; a < 3; a++) lo[a] = o[a] < lo[a] ? o[a] : lo[a], hi[a] = o[a] > hi[a] ? o[a] : hi[a];
            }
            for (int a = 0; a < 3; a++) op->wl.box[a] = lo[a], op->wl.box[3 + a] = hi[a];
        }
        op->ssao_scale[0] *= pv->sx;
        op->ssao_scale[1] *= pv->sy;
        if (op->soft_view.px != 0) {
            op->soft_view.px *= pv->sx;
            op->soft_view.py *= pv->sy;
            if (op->soft_view.has_up) {
                float o[3];
                if (op->soft_view.zsign < 0) photo_rh_point(pv->d, op->soft_view.up, 0, o);
                else
                    for (int a = 0; a < 3; a++) o[a] = pv->d[a][0] * op->soft_view.up[0] + pv->d[a][1] * op->soft_view.up[1] + pv->d[a][2] * op->soft_view.up[2];
                memcpy(op->soft_view.up, o, sizeof o);
            }
        }
    }
    float rot[4][4];   /* the sky's view change: rotation only */
    memcpy(rot, pv->d, sizeof rot);
    rot[0][3] = rot[1][3] = rot[2][3] = 0;
    photo_view_rows(l.cam_view, pv->d);
    photo_proj_rows(l.cam_proj, pv);
    photo_view_rows(l.sky_view, rot);
    photo_proj_rows(l.sky_proj, pv);
    photo_view_rows(l.lamp_view, pv->d);
    photo_view_rows(l.fire_view, pv->d);
    photo_view_rows(l.level_view, pv->d);
    photo_view_rows(l.game_cam, pv->d);
    photo_rows(pv->m, l.taa_wvp);
    photo_rows(pv->m, l.ws_wvp);
    photo_rows(pv->m, l.cd_wvp);
    photo_rows(pv->m, l.photo_wvp);
    return &l;
}
/* One replay of the frozen frame (host thread), when the photo view asks for one. */
static void photo_replay(const FrameList *src, const uint32_t *px, uint32_t w) {
    static uint32_t *buf;
    static size_t buf_bytes;
    HleD3dPhotoView pv;
    if (!hle_d3d_photo_view || !hle_d3d_photo_view(&pv)) return;
    const FrameList *l = photo_list(src, &pv);
    if (!l) return;
    const size_t bytes = (size_t)out_w * out_h * 4;
    if (buf_bytes != bytes) {
        free(buf);
        buf = malloc(bytes);
        buf_bytes = buf ? bytes : 0;
    }
    photo_no_overlay = pv.readback;
    if (pv.readback) {
        if (buf && !render_frame(l, px, w, buf) && hle_d3d_photo_pixels) hle_d3d_photo_pixels(buf, (uint32_t)out_w, (uint32_t)out_h);
    } else if (presenting) {
        if (!render_frame(l, px, w, NULL)) video_frame_presented();
    } else if (buf && !render_frame(l, px, w, buf)) {
        video_present(buf, out_w, out_h, out_w * 4);
        if (hle_d3d_host_frame_hook) hle_d3d_host_frame_hook(buf, (uint32_t)out_w, (uint32_t)out_h, 0, l->draws, 0);
    }
    photo_no_overlay = 0;
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
                          hle_d3d_stats ? now_ms() : 0, st.queue[st.q_head].g_ms, st.queue[st.q_head].hle_ms,
                          st.queue[st.q_head].pace_ms};
        memset(&st.queue[st.q_head], 0, sizeof st.queue[0]);
        st.q_head = (st.q_head + 1) % FRAME_QUEUE;
        st.q_count--;
        fresh = 1;
        pthread_cond_signal(&queue_space);
    }
    pending = st.q_count;
    pthread_mutex_unlock(&d3d_lock);
    photo_src = &ops;
    static uint32_t photo_w;   /* the last frame's back-buffer width (its replays) */
    if (fresh) photo_w = w;
    else if (opened && renderer_on && !pending && hle_d3d_photo_view && hle_d3d_photo_frozen()) photo_replay(&ops, px, photo_w);
    if (fresh) {
        if (!opened) {
            /* Window: --resolution, else the back buffer (16:9 wide with --widescreen). A swapchain
             * needs the Vulkan renderer, the SDL window and no --dump-frame (which reads back). */
            int ww = hle_d3d_res_w > 0 ? hle_d3d_res_w : hle_d3d_widescreen ? (int)(h * 16 + 4) / 9 : (int)w;
            int wh = hle_d3d_res_h > 0 ? hle_d3d_res_h : (int)h;
            video_want_vulkan = !strcmp(hle_d3d_renderer, "vulkan") && !hle_d3d_dump_path && strcmp(hle_d3d_present, "readback");
            video_fullscreen = hle_d3d_fullscreen;
            video_open(hle_d3d_video, ww, wh, "WarriorsReborn");
            printf("[video] renderer_open: starting\n");
            renderer_open(w, h);
            printf("[video] renderer_open: done\n");
            opened = 1;
            if (hle_d3d_gpu_skin.on) {   /* without it, later frames run the character programs on the CPU again */
                __atomic_store_n(&gsk_live, renderer_skin_on(), __ATOMIC_RELAXED);
                printf("[gpu-skin] %s\n", renderer_skin_on() ? "character vertex programs run on the GPU"
                                                             : "unavailable on this device: character vertex programs stay on the CPU");
            }
        }
        clear_parity_check(&ops, px, w, h);
        tex_race_frame = seq;
        if (ops.nskin && !renderer_skin_on()) skin_cpu_fill(&ops);   /* recorded before the renderer said it cannot */
        hstat_end = (RendererTiming){0};
        hstat_end.gpu_busy_ms = hstat_end.upload_ms = hstat_end.pipeline_ms = -1;   /* unless the renderer runs */
        hstat_valid = 0;
        double t0 = now_ms();
        /* Frames without draws go through the renderer too when its output is scaled. */
        const uint32_t *shown = px;
        uint32_t sw = w, sh = h;
        int scaled = out_w != (int)w || out_h != (int)h;
        if (framegen_options.on && renderer_on) fg_host_frame(presenting);
        modmenu_h = h;
        if (presenting && render_frame(&ops, px, w, NULL)) {   /* the swapchain failed: readback from now on */
            device_lost_check();
            presenting = 0;
            printf("[video] present: readback through the %s backend (swapchain lost: %s)\n", hle_d3d_video, renderer_present_info());
            if (video_vulkan_fallback()) finish("no window");
        } else if (presenting) {
            shown = NULL;
            video_frame_presented();
        }
        if (shown && renderer_on && (ops.draws || scaled)) {
            if (!out) out = malloc((size_t)out_w * out_h * 4);
            if (out && !render_frame(&ops, px, w, out)) {
                shown = out;
                sw = (uint32_t)out_w;
                sh = (uint32_t)out_h;
            } else if (out) {
                device_lost_check();
                if (!__atomic_fetch_add(&readback_failures, 1, __ATOMIC_RELAXED))   /* counted for the exit summary */
                    printf("[renderer] ERROR: readback frame %u failed to render (%s); the CPU back buffer is shown instead\n", seq,
                           renderer_error());
            }
        }
        double t1 = now_ms();
        if (shown) video_present(shown, (int)sw, (int)sh, (int)sw * 4);
        if (hle_d3d_host_frame_hook) hle_d3d_host_frame_hook(shown, sw, sh, seq, ops.draws, ops.movie_draws + ops.cpu_draws);
        if (hle_d3d_settings_hook) hle_d3d_settings_hook();   /* in-game settings applied live (src/settings_apply.h) */
        if (shown && hle_d3d_dump_path) keep_shown(shown, sw, sh, seq);
        /* --frame-gen: also the frame before each dumped one and the generated frame between them, as
         * <path>.<frame>-g.bmp (it sorts between the two). */
        const uint32_t every = hle_d3d_dump_every > 0 ? (uint32_t)hle_d3d_dump_every : 0;
        if (shown && hle_d3d_dump_path && every && seq >= (uint32_t)(hle_d3d_dump_from > 0 ? hle_d3d_dump_from : 0) && (seq % every == 0 || (framegen_options.on && (seq + 1) % every == 0))) {
            char name[1024];
            snprintf(name, sizeof name, "%s.%05u.bmp", hle_d3d_dump_path, seq);
            video_write_bmp(name, shown, (int)sw, (int)sh, (int)sw * 4);
            static uint32_t *gen;
            if (framegen_options.on && seq % every == 0 && shown == out && (gen || (gen = malloc((size_t)out_w * out_h * 4))) &&
                renderer_frame_gen_readback(gen, out_w * 4)) {
                snprintf(name, sizeof name, "%s.%05u-g.bmp", hle_d3d_dump_path, seq);
                video_write_bmp(name, gen, (int)sw, (int)sh, (int)sw * 4);
            }
        }
        if (hle_d3d_stats) host_stats(t1 - t0, now_ms() - t1, ops.draws, &ft);
        perf_frame(t1 - t0, &ft, ops.draws, ops.movie_draws + ops.cpu_draws, seq);
        if (hle_d3d_fps_overlay && renderer_on) fps_overlay_frame();
        if (hle_spawn_report) hle_spawn_frame(seq);
        if (hle_fx_report) hle_fx_frame(seq);
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
    if (__atomic_load_n(&quit_code, __ATOMIC_ACQUIRE) >= 0)   /* hle_d3d_request_quit */
        finish(device_lost_seen ? "the GPU stopped responding (Vulkan device lost)" : "quit requested");
}
