/* High-level emulation (HLE) of the Direct3D 8 API boundary: every D3D8 library entry point the
 * title calls is replaced with a host function, so the statically linked D3D8 code never
 * programs the NV2A (MMIO at 0xFD000000). Device, surfaces, textures and buffers are guest-memory
 * objects with the Xbox D3DResource layout; Clear fills the render target, Swap posts the back
 * buffer to the host video backend (src/video.h). Title code that writes D3D globals inline
 * (D3D_g_RenderState, deferred state arrays, dirty flags) keeps writing them at their original
 * addresses; nothing consumes them yet (the title's blend and depth states arrive through
 * SetRenderState_Simple). */
#ifndef HLE_D3D_H
#define HLE_D3D_H
#include "hle_audio.h"   /* HleLookup, HLE_SIG_LEN */
#include "renderer.h"    /* RState */
#include "hud_glass.h"   /* HudGlassConfig */
#include "bloom.h"       /* BloomConfig */
#include "filmic.h"      /* FilmicConfig */
#include "shadow_range.h" /* ShadowRangeConfig */
#include "lamp_shadow.h" /* LampShadowConfig */
#include "rain.h"        /* RainConfig */
#include "ssao.h"        /* SsaoConfig */
#include "lamp_lights.h" /* LampLightsConfig */
#include "light_shafts.h" /* LightShaftsConfig */
#include "char_light.h"  /* CharLightConfig */
#include "world_light.h" /* WorldLightConfig */
#include "fire_light.h"  /* FireLightConfig */
#include "gpu_skin.h"    /* GpuSkinConfig */
#include "soft_particles.h"   /* SoftConfig */
#include "vsh.h"         /* VshProgram */
#include "sky.h"         /* SkyConfig */
#include "sharp_faces.h" /* SharpFacesConfig */
#include "neon_glow.h"   /* NeonGlowConfig */
#include <stdint.h>

/* Patches the D3D entry points (same rules as hle_audio_install: an entry is patched only when
 * its first bytes match and its symbol, if known, resolves to the same address). */
int hle_d3d_install(HleLookup lookup);
void hle_d3d_summary(void);              /* per-function call counts, frames, last clear color */
extern int hle_d3d_log_limit;            /* log lines per function after the first Swap (default 1) */
extern int hle_d3d_frame_limit;          /* exit after this many presented frames (0 = no limit) */
extern const char *hle_d3d_dump_path;    /* BMP of the last presented frame, written on exit */
extern const char *hle_d3d_video;        /* video backend preference ("sdl" or "null") */
extern const char *hle_d3d_renderer;     /* host renderer: "vulkan" (default, CPU path if it fails) or "none" */
extern int hle_d3d_tri_check;            /* --tri-check / XB_TRI_CHECK=1: log non-finite and exploded post-transform vertices (tri_check.h) */
/* Always-on [hitch] / [level-summary] lines (perf_log.h): the name of each level the title loads
 * (any thread; the summary of the previous level is printed at the next presented frame), and
 * whether a level load is pending (the frames are loading frames; NULL: unknown). */
void hle_d3d_level_loaded(const char *name);
extern int (*hle_d3d_level_pending)(void);
extern int hle_d3d_stats;                /* --stats: a timing line per 60 presented frames */
/* HD output, host side only (the title keeps its 640x480 device):
 * --render-scale N, --resolution WxH, --msaa N, --aniso N, --widescreen. */
extern int hle_d3d_render_scale, hle_d3d_res_w, hle_d3d_res_h, hle_d3d_msaa, hle_d3d_aniso, hle_d3d_widescreen;
/* Graphics options: --supersample N (internal render size N x the output, box-filtered down;
 * RendererConfig.supersample), --shadow-scale N (the title's render targets at N x their scaled
 * size) and --shadow-soft (3x3 PCF on the character shadows). 0 = off / the default. */
extern int hle_d3d_supersample, hle_d3d_shadow_scale, hle_d3d_shadow_soft;
extern int hle_d3d_sharpen; /* --sharpen 0-100 (RendererConfig.sharpen) */
extern int hle_d3d_fps_overlay;        /* --fps-overlay (RendererConfig.fps_overlay) */
extern int hle_d3d_fps_overlay_corner; /* --fps-overlay-corner: FPS_CORNER_* (fps_overlay.h) */
extern int hle_d3d_fps_overlay_items;  /* --fps-overlay-items: FPS_ITEM_* mask, 0 = FPS_ITEMS_DEFAULT (fps, ms) */
extern int hle_d3d_pipeline_cache;     /* --no-pipeline-cache = 0 (RendererConfig.no_pipeline_cache) */
extern const char *hle_d3d_pipeline_cache_dir; /* the --hdd folder (RendererConfig.pipeline_cache_dir); NULL: memory only */
/* --present auto|mailbox|fifo|immediate|readback: auto = a Vulkan swapchain in the SDL window
 * (mailbox when available, else fifo); readback = the renderer's image read back and shown by
 * the video backend (always used with --video x11|null, --dump-frame or when the swapchain
 * cannot be made). --fullscreen starts in desktop fullscreen. */
extern const char *hle_d3d_present;
/* --frames-in-flight N (1-3, default 2): frames the GPU may still be executing when the swapchain
 * path moves on to the next one (RendererConfig.frames_in_flight); 1 = wait after each present. */
extern int hle_d3d_frames_in_flight;
/* --direct-geometry / --no-direct-geometry (on by default): the renderer writes each frame's vertices
 * straight into its mapped geometry buffer (RendererConfig.direct_geometry) instead of a host copy. */
extern int hle_d3d_direct_geometry;
/* --hud-hd / --hud-glass and their tunables (src/hud_glass.h; off by default): the target health
 * bars are restyled (thicker, outline, gradient fill) or drawn as low-opacity bars on a
 * frosted-glass panel (renderer_glass); the rage meter (player panel) is never changed. */
extern HudGlassConfig hle_d3d_glass;
/* --bloom, --bloom-strength, --bloom-threshold (src/bloom.h; off by default): glow of the 3D scene,
 * split from the HUD after the frame's last depth-tested back-buffer draw (renderer_bloom). */
extern BloomConfig hle_d3d_bloom;
/* --filmic, --filmic-strength, --filmic-grain, --filmic-vignette, --filmic-lut (src/filmic.h; off
 * by default): the filmic look of the 3D scene, applied where it ends, before the HUD
 * (renderer_filmic). */
extern FilmicConfig hle_d3d_filmic;
/* --rain-hd, --rain-hd-density (src/rain.h; off by default): while the title draws its rain layers,
 * they are replaced by the renderer's procedural rain (renderer_rain) in their place. */
extern RainConfig hle_d3d_rain;
/* --sky-hd (src/sky.h; off by default): the title's sky dome and cloud cap are replaced by the
 * renderer's procedural sky (renderer_sky), and its skyline backdrop gets windows and haze
 * (renderer_skyline). */
extern SkyConfig hle_d3d_sky;
/* --sharp-faces (src/sharp_faces.h; off by default): character skins are upscaled 2x from the game's
 * own texels on a worker thread; a texture pack or built-in replacement for the same texture wins. */
extern SharpFacesConfig hle_d3d_sharp_faces;
/* --neon-glow (src/neon_glow.h; off by default): textures with an emission mask (<hash>.emit.png /
 * .emit.dds in a texture pack) keep their lit texels' own colour through fog and darkness and mark
 * them in the bloom light mask (renderer_set_emission). */
extern NeonGlowConfig hle_d3d_neon_glow;
/* --ssao, --ssao-strength, --ssao-radius (src/ssao.h; off by default): ambient occlusion of the
 * opaque 3D world, applied after its last opaque draw (renderer_ssao), before effects and HUD. */
extern SsaoConfig hle_d3d_ssao;
/* --lamp-lights, --lamp-lights-strength (src/lamp_lights.h; off by default): the title's lamp halos
 * become point lights that light the opaque 3D world around them (renderer_lamp_lights). */
extern LampLightsConfig hle_d3d_lamp;
/* --light-shafts, --light-shafts-strength, --light-shafts-cheap (src/light_shafts.h; off by default):
 * the lamp halos' lights (as for --lamp-lights, which it does not need) and the fires of --fire-light
 * hang a visible cone or glow of light in the air, thicker while the title's rain falls
 * (renderer_light_shafts). */
extern LightShaftsConfig hle_d3d_shafts;
/* --char-lighting (src/char_light.h; off by default): the lamp halos' lights (as for --lamp-lights,
 * which it does not need) light the characters per pixel with their own smooth normals
 * (renderer_draw_lit); with --lamp-lights the lamp pass then leaves character pixels alone. */
extern CharLightConfig hle_d3d_char_light;
/* --world-lighting, --world-lighting-strength (src/world_light.h; off by default): the lamp halos'
 * lights (as for --lamp-lights, which it does not need) light the world and prop draws per pixel
 * with their own normals (renderer_draw_world_lit); with --lamp-lights the lamp pass then lights
 * only the characters (or is skipped when --char-lighting lights them). */
extern WorldLightConfig hle_d3d_world_light;
/* --fire-light, --fire-light-strength (src/fire_light.h; off by default): the flame draws of the
 * burning barrels become flickering warm point lights (the nearest FIRE_MAX_LIGHTS) added to the
 * light list of --world-lighting, --char-lighting and --lamp-lights; without one of them it does
 * nothing. */
extern FireLightConfig hle_d3d_fire;
/* --lamp-shadows, --lamp-shadows-strength (src/lamp_shadow.h; off by default): characters near a lit
 * lamp or fire (the lights of --lamp-lights / --fire-light, tracked for it alone too) cast a ground
 * shadow pointing away from it (renderer_lamp_shadows). */
extern LampShadowConfig hle_d3d_lamp_shadow;
/* --gpu-skinning / --no-gpu-skinning (src/gpu_skin.h; on by default): the character vertex programs
 * the matcher knows run on the GPU (renderer_draw_skinned) instead of the CPU interpreter, with the
 * same output; other programs, and devices without it, stay on the CPU. With it,
 * --gpu-vertex-programs / --no-gpu-vertex-programs (on by default): the world and object programs
 * of gsk_world_program_allowed too; the CPU still computes what its own passes read of them (the
 * depth fit's positions through the program's oPos slice, the --shadow-range fit's draw). */
extern GpuSkinConfig hle_d3d_gpu_skin;
/* --contact-shadows, --contact-shadows-strength (src/ssao.h; off by default): the ground right under
 * and around characters and props darkens a little (renderer_contact). */
extern ContactConfig hle_d3d_contact;
/* --soft-particles, --soft-particles-distance (src/soft_particles.h; off by default): the title's
 * view-plane particle billboards after the opaque world fade where they meet the scene behind them
 * (renderer_soft, RENDERER_SOFT). */
extern SoftConfig hle_d3d_soft;
/* --shadow-range, --shadow-range-dist (src/shadow_range.h; off by default): character shadows fade
 * out on receivers more than the distance below the caster (RendererConfig.shadow_range). */
extern ShadowRangeConfig hle_d3d_shadow_range;
extern int hle_d3d_fullscreen;
/* --dump-textures DIR: every unique texture sampled by a draw as DIR/<hash>.png + manifest.csv;
 * --texture-pack DIR (repeatable, up to HLE_D3D_MAX_PACKS): replaces textures whose content hash
 * has a DIR/<hash>.png (src/texpack.h); with several packs the first one that has a hash wins. */
#define HLE_D3D_MAX_PACKS 8
extern const char *hle_d3d_dump_textures, *hle_d3d_texture_packs[HLE_D3D_MAX_PACKS];
extern int hle_d3d_texture_pack_count;
/* --texpack-sync: replacements and --neon-glow masks are read and decoded on the render thread
 * when first drawn (the old behaviour, for debugging) instead of on the loader threads with a
 * per-frame upload budget. --no-texpack-prefetch: the loaders read only what is drawn, not the
 * textures a level load registers. */
extern int hle_d3d_texpack_sync, hle_d3d_texpack_prefetch;
/* --texpack-budget MB: the texture-pack cache's maximum (0 = 1 GiB); lowered further when the
 * driver reports a smaller memory budget (VK_EXT_memory_budget, src/vram_budget.h). */
extern unsigned hle_d3d_texpack_budget_mb;
/* --mesh-pack DIR: replaces skinned character meshes whose content hash has a DIR/<hash>.wmesh
 * (src/meshpack.h); NULL (the default) leaves every draw as the title issues it. */
extern const char *hle_d3d_mesh_pack;
/* --button-icons xbox|kbm|auto: the in-game button prompts as the game's own (or the first
 * texture pack's) glyphs, as key caps and mouse drawings for the current key map
 * (src/kbm_glyphs.h), or following the last used device (keyboard/mouse or controller; the two
 * atlases are cached under separate keys, so a switch costs nothing after the first use). The
 * cell rects come from the game folder (hle_d3d_glyphs_load); without them icons stay Xbox. */
enum { HLE_ICONS_XBOX, HLE_ICONS_KBM, HLE_ICONS_AUTO };
extern int hle_d3d_button_icons;
int hle_d3d_glyphs_load(const char *disc_dir);   /* rect count, or -1 (icons stay Xbox) */
/* --sharp-fonts (src/sharp_font.h; on by default, --no-sharp-fonts turns it off): the main
 * font/HUD atlas and the big font atlas upscaled 4x with rebuilt letter edges on a worker thread
 * the first time they are drawn (when no texture pack replaces them); the button icons and other
 * art keep their look. */
extern int hle_d3d_sharp_fonts;
/* Title render targets (default on; --no-render-targets turns it off): draws and clears into the
 * title's own render targets and depth surfaces (the shadow map) are replayed into offscreen
 * renderer surfaces, which stage textures over the same memory then sample.
 * Off: those draws are skipped and such textures sample white, as before. */
extern int hle_d3d_render_targets;
/* --vp-threads N: vertex program worker threads (-1 auto: hle_d3d_vp_default_workers of the
 * physical cores, 0 off: every draw's vertex programs run on the recording thread). */
extern int hle_d3d_vp_threads;
extern int hle_d3d_dump_every;
/* The automatic pool size for a machine with that many physical cores. */
int hle_d3d_vp_default_workers(int physical_cores);
/* Test hooks (tests/vp_pool_test.c): one program draw of nv vertices whose v0 is a FLOAT4 stream
 * (stride bytes per vertex) through the draw path's vertex-program pool, its RVertex output in out
 * (complete after hle_d3d_vp_test_wait, as at Swap); and the program draws queued to the pool and
 * run on the recording thread since the last call (read and cleared). */
void hle_d3d_vp_test_draw(const VshProgram *prog, const uint8_t *stream, uint32_t stride, uint32_t nv, const float (*c)[4], RVertex *out);
void hle_d3d_vp_test_draw_at(const VshProgram *prog, const uint8_t *stream, uint32_t stride, uint32_t pos_offset, uint32_t nv,
                             const float (*c)[4], RVertex *out);
void hle_d3d_vp_test_wait(void);
void hle_d3d_vp_test_counts(uint32_t *queued, uint32_t *inline_draws);
/* Renderer target size for a bw x bh back buffer: N x the back buffer with --render-scale, the
 * largest rectangle of the frame's aspect inside --resolution, else the back buffer. The aspect
 * is the back buffer's, or 16:9 with --widescreen (the title then renders anamorphic frames). */
void hle_d3d_output_size(int bw, int bh, int *ow, int *oh);
extern void (*hle_d3d_on_exit)(void);  /* called once before the process exits after the frame limit or window close */

/* Host main thread: opens the window on the first frame, presents new frames, pumps window
 * events and exits the process when the window is closed or the frame limit is reached. */
void hle_d3d_service(void);

uint32_t hle_d3d_frame_count(void);
uint32_t hle_d3d_clear_color(void);      /* color of the last Clear with a target flag */
/* Frames handed to the video backend by hle_d3d_service, and Swaps dropped unpresented. */
void hle_d3d_present_stats(uint32_t *presented, uint32_t *dropped);
/* Copies the last posted frame (XRGB8888, tightly packed) into a malloc'd buffer the caller
 * frees; returns NULL before the first Swap. */
uint32_t *hle_d3d_last_frame(int *w, int *h);

/* Current output state (blend, color mask, depth write/func, cull) recorded with each draw. */
void hle_d3d_render_state(RState *out);
/* Draws recorded for the frame being drawn; *v / *nv / *ni = the last draw's vertices and its
 * index count (tests). */
uint32_t hle_d3d_recorded(const RVertex **v, uint32_t *nv, uint32_t *ni);
/* The last recorded draw of the frame being drawn (tests): its offscreen target (guest physical
 * addresses of the color and depth surfaces, 0 = none / the back buffer), the render-target
 * surface each stage samples (0 = none), its pixel shader's texture modes and shadow compare
 * function, and its viewport size. Returns 0 when the frame has no draw. */
typedef struct {
    uint32_t target_color, target_depth;
    uint32_t stage_surface[4];
    uint32_t tex_modes, shadow_func;
    uint32_t vp_w, vp_h;
} HleD3dDrawInfo;
int hle_d3d_last_draw(HleD3dDrawInfo *d);

/* Table access for tests. */
int hle_d3d_entry_count(void);
uint32_t hle_d3d_entry_addr(int i);
int hle_d3d_entry_argbytes(int i);
const char *hle_d3d_entry_name(int i);
const uint8_t *hle_d3d_entry_sig(int i);   /* HLE_SIG_LEN bytes */

/* Guest addresses in this XBE's D3D8 library (from XbSymbolDatabase and disassembly). */
#define D3D_G_PDEVICE 0x0037d978u   /* D3D8__D3D_g_pDevice */
#define D3D_DEVICE 0x0037d980u      /* the static CDevice; +0 push-buffer put, +4 end */
#endif
