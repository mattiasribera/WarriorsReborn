/* Host renderer behind a C ABI (implementation: src/renderer/vk_renderer.cpp, Vulkan, C++).
 * Offscreen: one color target (B8G8R8A8, the XRGB8888 byte order Swap presents) and one depth
 * target at the back-buffer size; each frame is recorded between renderer_begin and renderer_end
 * and read back into a caller buffer, so the existing present path (video.h, --dump-frame, the
 * null backend) is unchanged. Not thread-safe: all calls must come from one host thread (the
 * runtime's main thread, see hle_d3d_service). */
#ifndef RENDERER_H
#define RENDERER_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Vertex in D3D clip space (y up, 0 <= z <= w), the two vertex colors of the pixel shader (v0 =
 * diffuse / oD0, v1 = specular / oD1, ARGB8888), the texture coordinates of stages 0-3 (s, t,
 * r, q; D3D: 0,0 = top left of the texture, wrapped) and the fog amount (1 - fog factor; 0 = no
 * fog), interpolated and applied to the fog register's alpha (fog.a = RPixelShader.fog alpha x
 * (1 - fog)). */
#define RENDERER_STAGES 4
typedef struct {
    float x, y, z, w;
    uint32_t color, specular;
    float tex[RENDERER_STAGES][4];
    float fog;
} RVertex;

/* NV2A register-combiner program of the following draws: the fields of a D3DPIXELSHADERDEF
 * (Xbox d3d8types.h) with the per-stage constants already
 * resolved (UNIQUE_C0/C1 and the PSC0/PSC1 mappings to SetPixelShaderConstant values, see
 * psh.h). Evaluated per fragment by the renderer's combiner shader: inputs (register, channel,
 * mapping) per byte A B C D of rgb_in/alpha_in, outputs (AB, CD, SUM registers, dot products, mux
 * on r0.a, output mapping, blue to alpha) per rgb_out/alpha_out, then the final combiner
 * (A*B + (1-A)*C + D, alpha G) when final_abcd or final_efg is nonzero, else r0. */
typedef struct {
    uint32_t combiners;   /* PSCombinerCount: stage count in bits 0-7, MUX_MSB bit 8 */
    uint32_t rgb_in[8], alpha_in[8], rgb_out[8], alpha_out[8];
    uint32_t c0[8], c1[8];             /* stage constants, ARGB8888 */
    uint32_t final_abcd, final_efg;    /* PSFinalCombinerInputsABCD / EFG (EFG bits 0-7: flags) */
    uint32_t final_c0, final_c1;       /* ARGB8888 */
    uint32_t tex_modes;   /* PSTextureModes: 5 bits per stage (0 none, 1 project2d, 2 project3d, 3 cube,
                             4 pass-through, 5 clip plane; others sample 0) */
    uint32_t fog;         /* fog register: ARGB8888 fog color, alpha = fog factor (255 = fog off), times the
                             interpolated 1 - RVertex.fog */
    uint32_t shadow_func; /* NV097_SET_SHADOW_DEPTH_FUNC (0 NEVER, 1 LESS, 2 EQUAL, 3 LEQUAL, 4 GREATER,
                             5 NOTEQUAL, 6 GEQUAL, 7 ALWAYS) for stages whose texture is a depth surface
                             (renderer_surface): PROJECT3D gives texel <func> clamp(r/q, 0, max) ? 1 : 0
                             in every channel (max 0xFFFF for D16, 0xFFFFFF for D24; texel scaled to it),
                             PROJECT2D texel <func> 0; outside the surface the texel is the maximum
                             (border); bilinear stages filter the four compare results */
    float shadow_depth[2]; /* light projection of the depth stages' coordinates, r = A q - B (A, B > 0;
                              stored depth d is at view depth B / (A - d)); used by
                              RendererConfig.shadow_range; A = 0: unknown (the compare as is) */
} RPixelShader;

/* D3DVIEWPORT8 in render-target pixels. */
typedef struct {
    float x, y, w, h, minz, maxz;
} RViewport;

#define RENDERER_DEPTH_TEST 1u   /* renderer_draw flags: D3DRS_ZENABLE (writes per RState.depth_write) */
#define RENDERER_RAIN_COVER 2u   /* renderer_draw flag: also drawn into the rain cover map (renderer_rain_cover) */
#define RENDERER_CHARACTER 4u    /* renderer_draw flag: a skinned character (the rain's character mask, renderer_rain_chars_on) */
#define RENDERER_SOFT 8u         /* renderer_draw flag: a soft particle (renderer_soft) */
#define RENDERER_WORLD_SHADOW 16u /* renderer_draw flag: also drawn into the world shadow map (renderer_world_shadow) */
#define RENDERER_PARTICLE 32u    /* renderer_draw flag: a world particle draw (smoke, fire, sparks, halos): --upscaler fsr3's reactive mask */

/* Output state of the following draws, in the values the title sends to the NV2A (methods and GL
 * enums of xemu hw/xbox/nv2a/nv2a_regs.h). The target has
 * no alpha channel (like the X8R8G8B8 back buffer), so DST_ALPHA reads as 1. */
typedef struct {
    uint32_t blend_enable;          /* NV097_SET_BLEND_ENABLE (0x304): 0/1 */
    uint32_t blend_src, blend_dst;  /* 0x344/0x348: 0 ZERO, 1 ONE, 0x300-0x308 SRC_COLOR..SRC_ALPHA_SATURATE,
                                       0x8001-0x8004 CONSTANT_COLOR..ONE_MINUS_CONSTANT_ALPHA */
    uint32_t blend_eq;              /* 0x350: 0x8006 ADD, 0x800A SUBTRACT, 0x800B REVERSE_SUBTRACT, 0x8007 MIN,
                                       0x8008 MAX; the signed variants 0xF005/0xF006 as their unsigned ones */
    uint32_t blend_color;           /* 0x34C: constant color, ARGB8888 */
    uint32_t color_mask;            /* 0x358: B 1<<0, G 1<<8, R 1<<16, A 1<<24 */
    uint32_t depth_write;           /* 0x35C (D3DRS_ZWRITEENABLE): 0/1, with RENDERER_DEPTH_TEST only */
    uint32_t depth_func;            /* 0x354: 0x200 NEVER .. 0x207 ALWAYS */
    uint32_t cull;                  /* D3DRS_CULLMODE: 0 none, 0x900 cull clockwise, 0x901 cull counterclockwise
                                       (winding on screen, y down) */
} RState;

/* Texture formats: B8G8R8A8 texels (uint32_t 0xAARRGGBB), BC1/2/3/7 blocks (DXT1/3/5 and BC7,
 * UNORM, row-major blocks; BC7 from .dds texture-pack files) when renderer_has_bc() says the
 * device samples them, or 8-bit luminance (one byte per texel, sampled as (L, L, L, 1) like the
 * CPU decode of L8; the --movie-hd planes). */
enum { RENDERER_TEX_BGRA8, RENDERER_TEX_BC1, RENDERER_TEX_BC2, RENDERER_TEX_BC3, RENDERER_TEX_L8, RENDERER_TEX_BC7 };

#define RENDERER_CLEAR_COLOR 1u
#define RENDERER_CLEAR_DEPTH 2u

/* Filmic look of the 3D scene (RendererConfig.filmic, renderer_filmic; src/filmic.h builds it):
 * the shader parameters of shaders/filmic.frag and the grade's 3D LUT. Per pixel, in linear
 * light (display values ^ 2.2): pseudo-HDR (the light-masked highlights scaled up to 1 + hdr),
 * the tonemap blend x ACES(exposure x) x norm + (1 - blend) x min(x, 1) (norm = 1 / ACES(exposure
 * x white): white maps to 1, mid-grey 0.18 to itself), back to display values; then the halation
 * (the bloom glow, red-orange, screened, x halation), the LUT (lut_mix of it), the vignette and
 * the grain. lut: lut_size^3 RGBA8 texels, red fastest, then green, then blue (the .cube order),
 * copied at renderer_init. */
typedef struct {
    float exposure, white, norm, blend, hdr;
    float lut_mix, halation, grain, vignette;
    const uint8_t *lut;
    int lut_size;   /* 2-64 */
} RFilmic;

/* Automatic exposure, eye adaptation (RendererConfig.auto_exposure; src/auto_exposure.h builds it):
 * at the filmic anchor (renderer_filmic) a meter pass reads the scene copy, the centre-weighted
 * log2 average of the linear luma of the pixels outside the light mask, and an adaptation pass
 * updates a 1x1 state image on the GPU (no readback in the frame path): the adapted level moves
 * toward the meter with tau_up (brighter) or tau_down (darker) seconds, at most rate_up stops per
 * second up, kept where the gain is not at a limit; a frame after a gap or one after
 * renderer_auto_exposure_snap (a camera cut) snaps it, a meter change alone never does. The gain in stops is clamp(adapt x (key - adapted), ev_min, ev_max),
 * applied to the linear scene before the tonemap; brightened highlights roll off from `knee`
 * toward 1 instead of clipping, and a TPDF dither (+-1 LSB, dither != 0) hides the 8-bit steps. */
typedef struct {
    float key;              /* log2 linear luma of the title's typical lit street: gain 0 there */
    float adapt;            /* fraction of the deviation from key that is corrected (0-1) */
    float ev_min, ev_max;   /* gain limits in stops (ev_min <= 0 <= ev_max) */
    float tau_up, tau_down; /* adaptation time constants in seconds */
    float rate_up;          /* the most the adapted level rises per second, in stops */
    float knee;             /* linear output where the roll-off of a brightened frame starts (< 1) */
    int dither;
} RAutoExposure;

/* Cinematic depth of field (RendererConfig.dof; src/dof.h builds it), at the filmic anchor in the
 * frames renderer_dof asks for (the cutscenes). Needs the character mask (renderer_rain_chars_on:
 * created for it, drawn in its frames from the draws flagged RENDERER_CHARACTER). Per frame on the
 * GPU: a focus pass finds the sharp range [N, F]: with a speaker (RDofFrame.speaker > 0, his view
 * distance from the caller: --cutscene-dof-speaker), his distance widened by char_margin; else, with
 * characters visible over the central area
 * (the mask's depth is the visible one there), from the nearest to the farthest of them widened by
 * char_margin; without, a grid over the centre of the frame (centre-weighted, a low percentile of
 * the view distances) at both ends. A 1x1 state image smooths both ends in log space with time
 * constant `tau` (RDofFrame.snap sets them). A character pixel has no circle of confusion (with a
 * speaker: its own times char_blur, at most char_cap K, times RDofFrame.speaker: the speaker sharp,
 * the others mildly soft); another
 * one of view distance d has none inside [N, F] and, in target pixels, K (1 - G / d) outside it
 * with G the nearer end (N) or the farther one (F) and K = coc x height x clamp(focus_ref / G,
 * focus_min_scale, 1), between -near_max K (near) and K (far): a far focus blurs less, like a wide
 * shot. A half-size prefilter (colour and signed CoC), a half-size gather
 * over a disc (taps behind the pixel only spread as far as the pixel's own CoC: no background
 * halo around a sharp character; taps in front spread their own: a near shoulder blurs over it)
 * and a full-size composite (depth-aware upsample, blend by the pixel's CoC or the near field's
 * coverage, times RDofFrame.weight). The alpha (the bloom light mask) is kept. */
typedef struct {
    float coc;              /* the far field's largest CoC, a fraction of the target height */
    float near_max;         /* the near field's largest CoC, in units of the far one */
    float focus_ref;        /* view distance at and below which the full CoC applies */
    float focus_min_scale;  /* the CoC scale's floor for a far focus (0-1) */
    float focus_min;        /* the nearest focus distance (view units) */
    float pct;              /* the focus percentile of the centre's view distances (0-1) */
    float tau;              /* focus smoothing, seconds */
    float char_margin;      /* the sharp range around the characters: nearest / (1 + m) to farthest x (1 + m) */
    float char_blur;        /* with a speaker: the other characters' CoC scale (dof_coc_speaker) */
    float char_cap;         /* and its limit, in units of the far field's largest CoC */
} RDof;

/* Output options, set before renderer_init (kept across renderer_shutdown). The caller keeps
 * recording in back-buffer pixels (viewports of renderer_draw, the base image of renderer_begin);
 * the renderer draws into an out_w x out_h target (viewports and scissors scaled by out_w / w,
 * out_h / h; the base image stretched with linear filtering) with `samples` samples per pixel
 * (resolved before the readback; clamped to what the device supports) and anisotropic filtering
 * up to `aniso` (0 or 1 = off; clamped to the device limit). Zero sizes = the back-buffer size. */
typedef struct {
    int out_w, out_h;
    int samples;
    int aniso;
    /* Direct present (renderer_present): the window system's instance extensions, a callback that
     * creates the window's VkSurfaceKHR for the instance (returns 0 on success) and one that
     * returns the window's drawable size; present_mode is a RENDERER_PRESENT_* preference. Without
     * create_surface (or when any step fails) the renderer stays offscreen (renderer_end). */
    const char *const *instance_ext;
    uint32_t instance_ext_count;
    int (*create_surface)(void *instance, uint64_t *surface);
    void (*drawable_size)(int *w, int *h);
    int present_mode;
    /* The frame may hold glass operations (renderer_glass): keeps the multisampled color stored
     * across the render pass split they need. */
    int glass;
    /* Frames the GPU may still be executing when renderer_present returns (1-3, 0 = 2): the
     * frame's fence is waited for at the renderer_begin that reuses its resources, frames_in_flight
     * frames later, instead of right after presenting. 1 = submit, present, wait. renderer_end
     * (readback) always waits for its own frame. */
    int frames_in_flight;
    /* Supersampling (1, 2 or 4; 0 = 1; other values round down to one of them): the frame is
     * rendered at supersample x the out_w x out_h target (viewports, MSAA, glass operations and
     * offscreen surfaces follow that internal size; halved while it exceeds the device's image
     * limit) and box-filtered down to out_w x out_h (successive 2:1 linear blits, each an exact
     * 2x2 average) before renderer_end reads it back or renderer_present blits it. With
     * out_w x out_h equal to the back-buffer size the base image is sampled nearest, so it comes
     * back unchanged. */
    int supersample;
    /* Offscreen surfaces (the title's render targets: character shadows) at shadow_scale (1-4;
     * 0 = 1) x their output-scaled size. shadow_soft: depth stages filter the compare over a 3x3
     * grid of bilinear taps one texel apart (a 4x4-texel tent) instead of one 2x2 tap. */
    int shadow_scale;
    int shadow_soft;
    /* Shadow range (> 0, view units of the light; 0 = off: every pipeline as without it): a depth
     * stage's compare that fails (GREATER or GEQUAL: stored depth nearer than the receiver) still
     * counts as lit when the receiver lies more than shadow_range behind the stored depth, fading
     * between shadow_range and 1.5 x shadow_range (smoothstep); needs the draw's
     * RPixelShader.shadow_depth. */
    float shadow_range;
    /* Contrast-adaptive sharpening of the finished frame (0-100; 0 = off: the frame is exactly as
     * without it): a pass over the output-size image (after the supersampling downsample) that
     * renderer_end reads back and renderer_present blits. AMD FidelityFX CAS's filter, own code
     * (shaders/cas.frag). A device that cannot run it renders without (renderer_sharpen() = 0). */
    int sharpen;
    /* FSR 1 (--fsr, src/fsr.h): fsr_w x fsr_h (> 0, at most out_w x out_h on both axes and
     * smaller on one; anything else = off) is the internal render size of the scene: every scene
     * pass, effect and offscreen surface follows it as it would follow a smaller out_w x out_h.
     * After the main pass AMD FidelityFX EASU upscales the frame to out_w x out_h and RCAS
     * sharpens it (fsr_sharpness: stops, 0 = strongest, clamped to 0..2); the gamma ramp, the frame
     * counter, renderer_end and renderer_present then work on that image. With it, supersample
     * and sharpen are ignored (no supersampling, RCAS instead of CAS). A device that cannot create
     * the passes renders and reads back at fsr_w x fsr_h (renderer_fsr() = 0,
     * renderer_output_size the internal size). */
    int fsr_w, fsr_h;
    float fsr_sharpness;
    /* Bloom of the 3D scene (renderer_bloom): bloom_strength (> 0) scales the glow added to the
     * scene, bloom_threshold (0-1) is the brightness where it starts (soft knee below it). 0 = off:
     * the frame and every pipeline are exactly as without it. */
    int bloom;
    float bloom_strength, bloom_threshold;
    /* Procedural rain (renderer_rain). 0 = off: no pipeline is created and renderer_rain is
     * ignored, so every frame is exactly as without it. */
    int rain;
    /* Wet ground (renderer_rain_wet), with rain. 0 = off: nothing is created and renderer_rain_wet is ignored. */
    int rain_wet;
    /* Rain cover map (renderer_rain_cover), with rain. 0 = off: nothing is created, the rain,
     * splash and wet pipelines are exactly as without it and renderer_rain_cover is ignored. */
    int rain_cover;
    /* Damp streets (RRainWet.damp): the wet ground's pass (and the character mask it reads) is
     * created without rain too. 0 = as without it (the wet ground only with rain and rain_wet). */
    int damp;
    /* The wet ground's reflections (--reflections, src/sssr.h): 0 = classic (the short screen-space
     * march; every pass exactly as without the option). Above 0 (the march budget: the most
     * hierarchy intersections, at most 96) = stochastic screen-space reflections (FidelityFX SSSR port): a min-depth hierarchy of the depth
     * built in the wet pass, the reflected ray GGX-sampled by the surface's roughness and marched
     * through the hierarchy, then accumulated over frames (camera reprojection, neighbourhood clamp)
     * before the blur. Needs the wet pass (rain_wet or damp); without the resources it falls back to
     * classic. */
    int sssr;
    /* With sssr: rays per reduced texel (1 or 2; 0 = 1), the glass reflections' surface pass
     * (RRainWet.glass; 0 = none: no pass, and the wet pass is created only with rain_wet or damp as
     * before; 1 = also for dry frames), and the trace at the internal size instead of half of it (1). */
    int sssr_rays, sssr_glass, sssr_full;
    /* Screen-space ambient occlusion of the opaque 3D world (renderer_ssao): ssao_strength (> 0)
     * scales how much an occluded pixel darkens, ssao_radius (> 0, view units) is how far around a
     * point the occluders are looked for. 0 = off: the depth target, the frame and every pipeline
     * are exactly as without it. */
    int ssao;
    float ssao_strength, ssao_radius;
    /* ssao_mode 1 (ssao.h SSAO_MODE_GTAO): XeGTAO's compute passes (depth MIPs,
     * horizon search, edge-aware denoise; gtao_quality: ssao.h GTAO_QUALITY_*) make the occlusion
     * instead of the classic ones; the split, composite and light mask are the same. 0 = classic. A
     * device without compute on the queue or the storage formats falls back to classic
     * (renderer_ssao_gtao_on). */
    int ssao_mode, gtao_quality;
    /* ssgi (ssao.h SSGI_LOW-SSGI_HIGH): one diffuse bounce of the frame's lit
     * colour at the split, gathered by visibility-bitmask slices on XeGTAO's depth MIPs (needs ssao
     * with ssao_mode 1 and XeGTAO in use), accumulated over frames with TAA, denoised along the depth
     * and composited before the occlusion as dst x (1 + ssgi_strength x light x albedo / the local lit
     * brightness): the pixel's own colour tints what it receives; light-masked pixels (neon, fire)
     * receive none. 0 = off: no resource or pass is created and the frame is as without it. */
    int ssgi;
    float ssgi_strength;
    /* Lamp lights (renderer_lamp_lights). 0 = off: no resource or pipeline is created, the
     * targets are as without it and renderer_lamp_lights is ignored. */
    int lamp;
    /* Lamp character shadows (renderer_lamp_shadows). 0 = off: no resource is created, the shadow
     * word and every pipeline are as without it and renderer_lamp_shadows is ignored. */
    int lamp_shadow;
    /* Light walls (RLampLights.walls): the wall map (renderer_lamp_wall_map) is kept for the lights
     * too, without lamp shadows. 0 = off: the map exists only with lamp_shadow, and every pipeline is
     * as without it. */
    int light_walls;
    /* World shadow map (renderer_world_shadow): its texels per side (512-4096), 0 = off: no resource
     * is created, the shadow word and every pipeline are as without it and renderer_world_shadow
     * is ignored. */
    int world_shadow;
    /* Light shafts (renderer_light_shafts). 0 = off: no resource or pipeline is created, the
     * targets are as without it and renderer_light_shafts is ignored. shafts_cheap: a quarter-size
     * pass (half the working size of the others) with fewer steps per ray. */
    int shafts, shafts_cheap;
    /* Volumetric fog (renderer_volfog). 0 = off: no resource or pipeline is created, the targets
     * are as without it and renderer_volfog is ignored. volfog_quality: VOLFOG_LOW..VOLFOG_HIGH
     * (volfog.h volfog_grid: the froxel grid). With it on the wall map is kept too (as light_walls). */
    int volfog, volfog_quality;
    /* Character lighting (renderer_char_lights, renderer_draw_lit). 0 = off: no resource or
     * pipeline is created, renderer_char_lights is ignored and renderer_draw_lit draws as
     * renderer_draw. */
    int char_light;
    /* World lighting (renderer_draw_world_lit; uses the character lighting's resources, created for
     * either). 0 = off: renderer_draw_world_lit draws as renderer_draw (unless char_light is set). */
    int world_light;
    /* --hair (src/hair.h): the hair strength (0 = off: no hair pipeline is made, renderer_set_hair
     * is ignored and every draw is recorded exactly as without it). */
    float hair;
    /* --skin (src/hair.h): the skin strength (0 = off: no skin pipeline is made, renderer_set_skin
     * is ignored and every draw is recorded exactly as without it). */
    float skin;
    /* --eyes, --cloth (src/hair.h): their strengths (0 = off). With either, every skin draw
     * (renderer_set_skin) takes a material variant instead, whose mask is the three masks stacked
     * (skin, eyes, cloth; renderer_set_skin), and no material pipeline is made without them: every
     * draw is recorded exactly as without them. */
    float eyes, cloth;
    /* Soft particles (renderer_soft). 0 = off: no pipeline or render pass is created, the depth
     * target is as without it and renderer_soft and RENDERER_SOFT are ignored. */
    int soft;
    /* Contact shadows (renderer_contact). 0 = off: no resource or pipeline is created and
     * renderer_contact is ignored. */
    int contact;
    /* Filmic look (renderer_filmic). NULL = off: nothing is created, renderer_filmic is ignored and
     * bloom keeps its 4 levels. With it the bloom chain has 6 levels and its bright pass lets the
     * light-masked pixels feed up to 1 + filmic->hdr. Read at renderer_init (the LUT is copied). */
    const RFilmic *filmic;
    /* Automatic exposure (RAutoExposure, at renderer_filmic). NULL = off: nothing is created and the
     * filmic pass (if any) is exactly as without it. Without the filmic look it runs a minimal
     * exposure-only pass at the same anchor. Read at renderer_init (copied). */
    const RAutoExposure *auto_exposure;
    /* Cinematic depth of field (RDof, at renderer_filmic in the frames renderer_dof asks for). NULL
     * = off: nothing is created and every frame is exactly as without it. Without the filmic look
     * and auto exposure the anchor runs only in those frames. Read at renderer_init (copied). */
    const RDof *dof;
    /* Procedural sky and skyline (renderer_sky, renderer_skyline). 0 = off: no pipeline is created
     * and both are ignored, so every frame is exactly as without it. */
    int sky;
    /* GPU vertex programs (renderer_draw_skinned). 0 = off: no pipeline is created and
     * renderer_skin_on() is 0. */
    int gpu_skin;
    /* Vertices written straight into the frame's mapped geometry buffer instead of a host copy
     * uploaded at the end of the frame (the same bytes at the same offsets; one copy of every
     * vertex less). 0 = off: the host copy, as before. */
    int direct_geometry;
    /* Frame counter overlay (renderer_fps_overlay_text): a small box with
     * the text drawn last, after the sharpening, on the read-back frame (renderer_end) or on the
     * swapchain image after the blit (renderer_present), at that target's own pixels; corner is a
     * FPS_CORNER_* (fps_overlay.h). 0 = off: no resource is created, the swapchain usage is as
     * without it and every frame is exactly as without it. */
    int fps_overlay;
    int fps_overlay_corner;
    /* With fps_overlay: GPU % (--fps-overlay-items gpu): a timestamp
     * query pool (two per frame slot) around each frame's commands, read frames_in_flight frames
     * later without waiting (renderer_fps_overlay_gpu). 0: no query pool. */
    int fps_overlay_gpu;
    /* The same timestamp query pool without the overlay: the GPU time of the latest completed frame
     * in RendererTiming.gpu_busy_ms (the always-on [hitch] and [level-summary] lines). */
    int gpu_timestamps;
    /* Persistent pipeline cache: pipeline_cache_dir holds
     * pipeline-cache.bin (the driver's compiled pipelines, kept only for the same GPU and driver)
     * and pipeline-keys.bin (the geometry pipelines used, built again on a low-priority thread at
     * the next renderer_init so the frames that need them find them ready). NULL: the cache stays in
     * memory for the run. no_pipeline_cache: no cache at all (every pipeline compiled as it is
     * needed, as before). pipeline_stats: renderer_pipeline_report also gives a line per 60 s. */
    const char *pipeline_cache_dir;
    int no_pipeline_cache;
    int pipeline_stats;
    /* 1: no VK_EXT_extended_dynamic_state even when the device offers it: one geometry pipeline per
     * cull mode and depth state, as before. 0: with it, those are set per draw and draws differing
     * only there share a pipeline. */
    int no_dynamic_state;
    /* Geometry pipelines a frame needs that are not built yet: 1: built
     * on compile worker threads (half the CPUs, 1 to 4) while the frame waits at most 4 ms for
     * them; until one is ready its draws use the base variant when that is built (no emissive mask,
     * relief, hair or skin shading) or are left out, so a first encounter never freezes the frame
     * for the driver's compile. 0: built on the render thread when first drawn (complete frames
     * from the first one, as before). */
    int pipeline_async;
    /* --stats: GPU time per pass: one timestamp query pool per frame
     * slot, a timestamp after each major pass, read when the slot is next recorded (never a wait);
     * renderer_gpu_passes reports them. 0: no query pool, nothing written. */
    int pass_stats;
    /* Temporal anti-aliasing (renderer_taa; src/taa.h): the frames that call renderer_taa have their
     * 3D draws jittered and are blended with a reprojected history there; taa_sharpness (0-1)
     * sharpens the resolved frame (never the history). 0 = off: nothing is created, renderer_taa is
     * ignored and every frame is exactly as without it. */
    int taa;
    float taa_sharpness;
    /* Temporal upscaling (--taa-upscale), with taa: taa_up_w x taa_up_h (> 0, at most
     * out_w x out_h on both axes; equal to it: --taa-upscale native, the same resolve at ratio 1 for
     * anti-aliasing only) is the 3D scene's render size. The frame
     * target stays at the output size: in a frame that calls renderer_taa the operations before it
     * render into its top-left taa_up_w x taa_up_h rectangle (every pass of the scene at that size),
     * the resolve rebuilds the output size from the jittered frames into an output-size history and
     * the operations after it (bloom, wet ground, rain, filmic, HUD) run at the output size. Textures
     * get the LOD bias taa_lod_bias. 0 or an unusable size: plain TAA at the output size. */
    int taa_up_w, taa_up_h;
    /* --upscaler, with taa_up_w: 0 our TAA resolve, 1 the AMD FidelityFX FSR 3.1
     * upscaler port (compute passes; falls back to 0 where the device lacks what it needs) with its
     * reactive and transparency & composition masks, 2 the same without
     * the masks (--fsr3-masks off). */
    int taa_upscaler;
    /* Frame generation (renderer_frame_gen; src/framegen.h): 1 builds a frame
     * half way between the previous and the new one in every frame that calls renderer_frame_gen after
     * one that did; with a window it is presented before the new frame (renderer_frame_gen_present),
     * the swapchain gets two more images and FIFO. 0 = off: nothing is created, renderer_frame_gen is
     * ignored and every frame and present is exactly as without it. 2: also optical flow (block
     * matching of the two HUD-less frames) for pixels the motion field cannot place (rain, flicker). */
    int frame_gen;
    /* Texture streaming (--level-prefetch): 1 = the batched texture
     * uploads are submitted without waiting for them (the next batch waits, counted as upload time).
     * With it or without, renderer_begin frees the least recently used textures (looked up or
     * created longest ago, never in the last two frames) when the texture cache is over its budget
     * or entry limit, or after a failed create, and drops the whole cache only when that cannot get
     * under them. 0 = off: uploads wait for their batch. */
    int texture_streaming;
    /* --taa-lite (src/taa.h TaaLite: 0 auto, 1 on, 2 off): whether the upscale
     * resolve takes the cheaper LITE shader (taa_lite_wanted). 0 (auto): on an integrated GPU only. */
    int taa_lite;
    /* --vrs (src/renderer/vrs.h RENDERER_VRS_*): variable rate shading at the next
     * renderer_init. 0 = off. renderer_vrs_configure (tests) overrides it until the next renderer_configure. */
    int vrs;
    /* --hdr (src/hdr.h HDR_OFF / HDR_AUTO / HDR_ON): auto and on take an
     * HDR10 (PQ, BT.2020) swapchain format when the surface offers one (VK_EXT_swapchain_colorspace) and
     * the HDR output pass can be made; the swapchain blit then goes through it (renderer_hdr_on 1), with
     * the scene's highlights expanded towards hdr_peak nits and everything else at hdr_paper nits
     * (renderer_hdr_scene / renderer_hdr_hud_end tell them apart). Otherwise the swapchain is SDR as
     * without it and the [hdr] log line says why. 0 = off: SDR, nothing created. renderer_end (readback)
     * is always SDR. RENDERER_HDR_TEST (tests): no swapchain needed; every renderer_end also runs the HDR
     * pass, read with renderer_hdr_readback. */
    int hdr;
    float hdr_peak, hdr_paper;   /* nits; 0 = the defaults (1000, 200) */
    /* --saturation (0.5-2; 0 or 1 = unchanged): the final grade's saturation (renderer_saturation). */
    float saturation;
} RendererConfig;
#define RENDERER_HDR_TEST 3
enum { RENDERER_PRESENT_AUTO, RENDERER_PRESENT_FIFO, RENDERER_PRESENT_MAILBOX, RENDERER_PRESENT_IMMEDIATE };
void renderer_configure(const RendererConfig *c);
/* Target size (what renderer_end writes; the supersampled internal size divided by the factor,
 * FSR's upscaled size),
 * samples per pixel, frames in flight and anisotropy actually in use. */
void renderer_output_size(int *w, int *h);
int renderer_samples(void);
int renderer_frames_in_flight(void);
/* Supersampling factor in use (1 when off; see RendererConfig.supersample). */
int renderer_supersample(void);
/* The frame counter text (RendererConfig.fps_overlay): characters of FPS_FONT_CHARS, at most
 * FPS_MAX_CHARS; "" or NULL hides the box. Kept until changed (the caller sets it when its value
 * changes). Ignored when the overlay is off. renderer_fps_overlay_on: in use (created). */
void renderer_fps_overlay_text(const char *text);
int renderer_fps_overlay_on(void);
/* RendererConfig.fps_overlay_gpu: the GPU time of the frames completed since the last call (ms, in
 * all) and their count, then starts again from 0. Returns 1 when timestamps are in use, 0 when not
 * (off, or the queue has no timestamps: timestampValidBits 0) and both stay 0. */
int renderer_fps_overlay_gpu(double *busy_ms, int *frames);
/* Sharpening in use (0-100, 0 when off or unavailable; see RendererConfig.sharpen). */
int renderer_sharpen(void);
/* FSR in use (1) or not (0; see RendererConfig.fsr_w); *render_w x *render_h (either may be NULL)
 * receives the internal render size of the scene (the output size without FSR). */
int renderer_fsr(int *render_w, int *render_h);
float renderer_anisotropy(void);
/* The mip LOD bias of the game textures' sampler: log2(render / output width) with
 * RendererConfig.taa_up_w (taa_lod_bias; the scene's textures keep the output size's detail), else 0.
 * Anisotropic filtering (renderer_anisotropy) applies with it. */
float renderer_texture_lod_bias(void);

/* Creates the instance, device and targets (w x h: the back-buffer size the caller records in)
 * on the first suitable GPU. Returns 0 on success; otherwise -1 and renderer_error() says why
 * (no loader, no device, ...). */
int renderer_init(int w, int h);
const char *renderer_error(void);
/* 1 once a Vulkan call returned VK_ERROR_DEVICE_LOST (the GPU hung or the driver reset it, a TDR on
 * Windows) since renderer_init: every later frame fails and the picture cannot come back with this
 * device (hle_d3d then closes the game with a message). */
int renderer_device_lost(void);
const char *renderer_device_name(void);
void renderer_shutdown(void);

/* Starts a frame. base (XRGB8888 rows of pitch_bytes) is the initial color, NULL = black; depth
 * starts at 1.0. */
void renderer_begin(const uint32_t *base, int pitch_bytes);
/* The frame time the last renderer_begin took (milliseconds of the host's monotonic clock): the
 * clock of the renderer's time-driven state (--auto-exposure's adaptation). The renderer call
 * stream carries it in renderer_begin's record (render_wire.h). */
double renderer_frame_time(void);
/* The next renderer_begin takes t_ms as its frame time instead of reading the clock (once; < 0
 * cancels): a replay of a recorded stream (tools/render64/render64_replay.c), or a renderer process,
 * runs on the recording's clock. */
void renderer_set_frame_time(double t_ms);
/* Whole-target clear of color (XRGB8888) and/or depth, in order with the draws. */
void renderer_clear(uint32_t flags, uint32_t color, float z);
/* Indexed triangle list (indices into v). With RENDERER_DEPTH_TEST: depth test and write as the
 * current RState says (default less-or-equal with writes). Depth is clamped to the viewport range
 * instead of clipped at the near/far planes (when the device supports it); the color comes from
 * the current combiner program (renderer_set_pixel_shader) over the stage textures and the vertex
 * colors, then the alpha test, blended per the current RState. */
void renderer_draw(const RViewport *vp, uint32_t flags, const RVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni);
/* Texture cache. Textures are immutable and keyed by the caller (see xtex_key); a handle stays
 * valid until the next renderer_begin, which frees the least recently used textures (or, when none
 * of them can go, drops the whole cache) when it grows too large, so callers look handles up again
 * every frame. 0 = no texture. */
int renderer_has_bc(void);
/* Device memory the cache may hold before renderer_begin frees textures (0 = the default, 512 MiB). */
void renderer_set_texture_budget(uint64_t bytes);
uint32_t renderer_texture_find(uint64_t key);
/* levels mip levels, each w>>l x h>>l (at least 1), stored one after the other without padding.
 * Returns the handle, or 0 on failure (unsupported format, the cache's entry limit, out of memory;
 * renderer_error names it): renderer_begin then frees the least recently used textures, so the
 * caller draws something else this frame and tries again on a later one. */
uint32_t renderer_texture_create(uint64_t key, uint32_t format, uint32_t w, uint32_t h, uint32_t levels, const void *data);
/* Movie planes (--movie-hd): plane 0-2 of the frame being recorded (after renderer_begin), w x h
 * 8-bit luminance, tightly packed. Kept outside the texture cache: one persistent image per plane,
 * made again only when its size changes, rewritten by a copy at the start of the frame (no
 * synchronous upload). Sampled as (L, L, L, 1), bilinear, clamped at the edges. Returns the
 * stage-texture handle, or 0 on failure. */
uint32_t renderer_movie_plane(int plane, uint32_t w, uint32_t h, const void *data);
/* Planes copied by renderer_movie_plane so far. */
uint32_t renderer_movie_upload_count(void);
/* Times renderer_begin dropped the whole texture cache: over the budget or entry limit, or after a
 * failed create, with no least recently used texture to free (all used by the last two frames). */
uint32_t renderer_texture_drop_count(void);
/* VK_EXT_memory_budget: the budget the driver gives this process on the largest device-local heap
 * and the process's usage there (bytes, all of its allocations), and that heap's index. Returns 0
 * (and leaves them) when the instance or device lacks the extension, offscreen without a device,
 * or the driver reports no budget. Cheap; call it on the render thread. */
int renderer_memory_budget(uint64_t *budget, uint64_t *usage, int *heap);
/* Device memory the texture cache holds now (what renderer_set_texture_budget caps). */
uint64_t renderer_texture_bytes(void);
/* Device-local memory the renderer allocated, per category (the --stats [vram] line; the launcher's
 * video-memory meter, launcher/src/vram.h, is calibrated against it): SCENE = the frame's color,
 * depth, MSAA and supersampling targets; SURFACES = the title's offscreen surfaces (shadow maps,
 * scaled by the render scale and --shadow-scale); TEXTURES = the texture cache (game textures and
 * packs); EFFECTS = the post-processing targets (bloom, ssao, lamp lights, rain, glass HUD, ...);
 * SKIN = the GPU skinning buffers; OTHER = the rest (movie planes, filmic LUT). The swapchain is
 * the driver's and not counted. */
enum { RENDERER_VRAM_SCENE, RENDERER_VRAM_SURFACES, RENDERER_VRAM_TEXTURES, RENDERER_VRAM_EFFECTS, RENDERER_VRAM_SKIN,
       RENDERER_VRAM_OTHER, RENDERER_VRAM_CATS };
/* Current and peak bytes per category (either may be NULL) and the peak of their sum. */
uint64_t renderer_vram(uint64_t now[RENDERER_VRAM_CATS], uint64_t peak[RENDERER_VRAM_CATS]);
/* The swapchain images' size (width x height x 4 x image count), 0 offscreen. */
uint64_t renderer_swapchain_bytes(void);
/* Texture of the following draws on stage 0 (renderer_set_stage_texture(0, handle)). */
void renderer_set_texture(uint32_t handle);
/* Texture of a stage (0-3) for the following draws; 0 = white (1, 1, 1, 1). Reset by renderer_begin. */
void renderer_set_stage_texture(uint32_t stage, uint32_t handle);
/* Combiner program of the following draws (copied); NULL = the fixed "stage-0 texture x vertex
 * color" program (renderer_default_pixel_shader). Reset to NULL by renderer_begin. */
void renderer_set_pixel_shader(const RPixelShader *ps);
/* r0 = t0 x v0 (rgb and alpha), stage 0 projected 2D, no final combiner. */
void renderer_default_pixel_shader(RPixelShader *ps);
/* Distinct combiner programs of the current frame so far (the default one included). */
uint32_t renderer_pixel_shader_count(void);
/* Alpha test of the following draws: fragments whose alpha (combiner output, 0..255) fails
 * "alpha func ref" are discarded. func uses the D3DCMPFUNC numbering (1 NEVER, 2 LESS, 3 EQUAL,
 * 4 LESSEQUAL, 5 GREATER, 6 NOTEQUAL, 7 GREATEREQUAL, 8 ALWAYS); 0 = off. Reset by renderer_begin. */
void renderer_set_alpha_test(uint32_t func, uint32_t ref);
uint32_t renderer_texture_count(void);
/* The NV2A power-on defaults: no blending, ONE/ZERO, ADD, all channels, depth write LEQUAL, no
 * culling. */
void renderer_state_default(RState *s);
/* Output state of the following draws (copied). Reset to the default by renderer_begin. */
void renderer_set_state(const RState *s);
/* Pipeline variants created so far (cached by state, kept until shutdown). */
uint32_t renderer_pipeline_count(void);

/* Offscreen surfaces: the title's own render targets and depth surfaces, kept across frames
 * (created on first use; color cleared to black, depth to 1.0) and never dropped with the texture
 * cache. A surface is created at the guest size w x h times the render scale (internal render
 * height, supersampling included / back-buffer height) times RendererConfig.shadow_scale, at most
 * the device's image limit; viewports of draws into it stay in guest pixels. Their passes have one
 * sample and run before the frame's main pass, in the order they were recorded, so a surface
 * drawn and then sampled in the same frame is read after it was drawn (a surface sampled before
 * it is redrawn in the same frame sees the new content, not the old; a draw that samples the colour
 * surface it draws into reads what the draws before it left there). The handle is a texture
 * handle too (renderer_set_stage_texture); depth surfaces sample with the shadow compare of
 * RPixelShader.shadow_func (a white border outside), color surfaces bilinear, clamped to the edge.
 * Draws into a color surface write its alpha when RState.color_mask has A (the frame target never
 * does). Returns 0 on failure (renderer_error). */
enum { RENDERER_SURFACE_COLOR, RENDERER_SURFACE_DEPTH16, RENDERER_SURFACE_DEPTH24 };
/* Or'ed into kind: the surface is created at the guest size w x h, without the render scale and
 * shadow_scale (a separate surface from the same key without it). For the title's blur chain
 * (0xbc1c0: the back buffer drawn into a 512x256 target, then passes that shift it by half a
 * texel): its blur is measured in guest texels, so a scaled target would blur a fraction as wide. */
#define RENDERER_SURFACE_GUEST_SIZE 0x100u
uint32_t renderer_surface(uint64_t key, uint32_t kind, int w, int h);
/* Actual size of a surface (0 x 0 for an unknown handle). */
void renderer_surface_size(uint32_t surface, int *w, int *h);
uint32_t renderer_surface_count(void);
/* Target of the following clears and draws: a color and/or a depth surface (either may be 0;
 * both 0 = the frame target). Reset to the frame target by renderer_begin. */
void renderer_set_target(uint32_t color, uint32_t depth);

/* Previous frame (the title's frame-feedback blur: a quad that samples the front buffer): a stage-texture handle of the last frame's colour, the frame
 * target as it ended (HUD included, before sharpening, FSR and the gamma ramp) at the internal size,
 * sampled opaque (alpha 1, bilinear, clamped). Call it between renderer_begin and renderer_end; the
 * call asks for the history, which costs one copy of the frame target at the end of this frame and
 * of the next one. Returns 0 when the last frame was not kept (the first frame, or two frames
 * without a request): draw nothing then. */
uint32_t renderer_prev_frame(void);
/* In order with the draws on the frame target (ignored while an offscreen target is set): the
 * history of this frame is its target as it is here, not at its end (the first mark of a frame
 * counts). For the title's blur that samples the back buffer from an offscreen pass: the next frame
 * blurs this frame's scene, not the blur composited over it. Splits the main render pass. */
void renderer_prev_frame_mark(void);

/* Frosted-glass panel (the HUD glass of src/hud_glass.h), in order with the draws on the frame
 * target (ignored while an offscreen target is set): the rect (back-buffer pixels, rounded corners
 * of radius `corner`) is replaced by a gaussian blur of what is drawn under it (radius `blur`,
 * sigma = blur / 2; MSAA is resolved first), mixed with the tint color (alpha = strength) and
 * edged with a 1 back-buffer pixel rim (alpha = strength), lighter at the top. Splits the main
 * render pass; without glass operations the frame is recorded as before. */
typedef struct {
    float x0, y0, x1, y1;
    float blur, corner;
    uint32_t tint, rim;   /* ARGB8888 */
} RGlass;
void renderer_glass(const RGlass *g);
/* Glass operations executed by the last renderer_end / renderer_present. */
uint32_t renderer_glass_count(void);

/* Neon glow (--neon-glow, src/neon_glow.h): the emission mask of the following draws (an L8 texture
 * handle, 0 = none), the stage whose colour it lights, the strength and the bloom light mask weight.
 * A draw on the frame target with a mask (not a soft particle) uses the emissive variant, lit or not
 * (renderer_draw_lit, renderer_draw_world_lit and their skinned forms keep their lighting): after
 * the final combiner (the fog mix) and the lighting, with e = stage colour x mask x strength (at
 * most 1), its colour becomes b + e/2 x (1 - b) with b = max(colour, e): the lit texels keep at
 * least their own colour through fog and darkness and read hotter than paint, and when it writes
 * the bloom light mask as opaque (alpha 0 otherwise, renderer_bloom) it writes mask x bloom instead.
 * The mask is sampled with the stage's texture coordinates. Without a mask nothing changes. */
void renderer_set_emission(uint32_t mask, uint32_t stage, float strength, float bloom);
/* Emissive draws executed by the last frame. */
uint32_t renderer_emission_count(void);

/* Relief (--normal-maps, src/normal_maps.h): the normal map and specular map of the following draws
 * (texture handles, 0 = none; both 0 = off), the stage whose texture coordinates they use, the
 * normal strength (tilt scale), the specular strength and the baked-light weight (0-1). Only lit
 * draws (renderer_draw_lit, renderer_draw_world_lit and their skinned forms, on the frame target,
 * not soft particles and not emissive ones: an emission mask wins) use them, in the lit relief
 * variant (shaders/geom.frag RELIEF): the shading normal is the mesh normal tilted by the
 * tangent-space map (OpenGL convention, +Y up the image) in a per-pixel tangent frame from the
 * screen-space derivatives of the view position and the stage's coordinates; the frame's lights
 * shade that normal and add a Blinn-Phong highlight scaled by the specular map (R intensity, G
 * gloss), after the final combiner and faded by the fog; the vertex (baked) colour is scaled by
 * mix(1, (1 + cos) / 2, baked), cos between the shading and the mesh normal. Every other draw, and
 * every draw while it is off, is recorded exactly as without it. */
void renderer_set_relief(uint32_t normal, uint32_t specular, uint32_t stage, float strength, float spec_strength, float baked);
/* Relief draws executed by the last frame. */
uint32_t renderer_relief_count(void);
/* --dry-highlights (src/normal_maps.h): while on, the specular maps of the following
 * renderer_set_relief calls are read with their material channels: a texel with B > 0 shades with
 * intensity 2 B and gloss A (exponent 2^(3 + 7 A), 8-1024) instead of R / G, its highlights reach
 * RENDERER_DRY_REACH times each light's radius (a world draw's light mask must reach as far:
 * world_light_mask_reach) and a glossy texel adds a soft, Fresnel-weighted glint of the lights;
 * B = 0 shades exactly as without. Off (the default): the maps' B / A are ignored. */
#define RENDERER_DRY_REACH 2.0f
void renderer_set_relief_dry(int on);
/* --reflections sssr's glass (RendererConfig.sssr_glass): the specular map of the following
 * renderer_set_relief calls is glossy (normal_maps_image_glossy: glass, chrome, car paint); with
 * renderer_set_relief_dry on, only such draws are drawn again into the glass's surface pass. Off by
 * default (no draw goes into it); the frame's own drawing is the same either way. */
void renderer_set_relief_glossy(int on);
/* --parallax (src/normal_maps.h): parallax occlusion mapping with the height in the normal map's
 * alpha (255 = the surface, 0 = `depth` below it, depth a fraction of the texture's size; 0 = off,
 * the default). It applies to world-lit relief draws (not characters) whose renderer_set_relief
 * call followed renderer_set_relief_height(1) (the map holds a height): the view ray is marched
 * through the height field in the texture's tangent frame (from the screen-space derivatives), 4 to
 * 12 steps by the length of its shift in pixels (one mip level), refined linearly between the last two, and the
 * draw's stage texture, normal map and specular map are read where it meets the height (with the
 * unshifted derivatives). The shift fades out under a pixel (far surfaces cost one test) and is
 * limited at grazing angles (at most about 3 x depth). Off, or without the height flag, every draw
 * is recorded exactly as without it. */
void renderer_set_parallax(float depth);
void renderer_set_relief_height(int on);
/* --hair (src/hair.h, RendererConfig.hair > 0): the hair mask (a texture handle, 0 = none) of the
 * following draws, sticky as renderer_set_relief. A character draw lit by renderer_draw_lit or
 * renderer_draw_skinned (not a world-lit draw, nor one with an emission mask) whose mask is set
 * takes the hair variant (geom.frag HAIR; with relief maps, relief and hair): the mask (hair.h:
 * R weight, G height along the hair, B / A the strand direction in texel space), sampled at the
 * stage-0 coordinates, shades its hair texels with Kajiya-Kay highlights of the frame's
 * renderer_char_lights (a primary one in the light's colour, a secondary one tinted by the texel,
 * both along the strands: perpendicular to RLampLights.up_view projected on the surface), broken
 * into strands by the stage-0 texture's detail across the strand direction, plus a weak sheen of
 * the draw's own colour from above, and darkens the lower layers (by the height) and the gaps
 * between strands, all scaled by the weight and RendererConfig.hair. Texels of weight 0 are drawn
 * exactly as without the mask. */
void renderer_set_hair(uint32_t mask);
/* Hair draws executed by the last frame. */
uint32_t renderer_hair_count(void);
/* --skin (src/hair.h, RendererConfig.skin > 0): the skin mask (a texture handle, 0 = none) of the
 * following draws, sticky as renderer_set_hair, for the same lit character draws. A draw whose mask
 * is set takes a skin variant (geom.frag SKIN, with relief and / or hair as the draw has them): the
 * mask (hair.h: R weight, G thinness, B sweat sheen weight), sampled at the stage-0 coordinates,
 * shades its skin texels with each of the frame's renderer_char_lights as skin: a wrapped diffuse
 * term whose terminator turns red (light scattered under the skin), light passing through thin
 * parts and rims with the light behind them (red, by the thinness, the light's falloff and its
 * walls' share), a broad, low sheen (by the sheen weight), and darkens the skin texture's creases
 * relative to its surroundings and the surfaces facing down a little; all by the weight and
 * RendererConfig.skin. Texels of weight 0 are drawn exactly as without the mask. */
void renderer_set_skin(uint32_t mask);
/* Skin draws executed by the last frame. */
uint32_t renderer_skin_count(void);
/* --eyes: how open the eyes of the next lit character draws are (hair.h eyes_lid_open: 1 open, the
 * default; 0 shut, mid-blink): it scales their eye shading (a shut lid is drawn over the eye's
 * texels, as skin). Read by the material variants only. */
void renderer_set_eye_open(float open);
/* --eyes, --cloth (RendererConfig.eyes / .cloth > 0): renderer_set_skin then takes the skin's three
 * masks stacked in one texture three masks tall (hair.h hair_cache_mat_with: the skin mask, zero
 * without --skin, then the eyes, then the cloth), for the same lit character draws; such a draw
 * takes a material variant (geom.frag SKIN and MAT) that shades its skin as --skin (when
 * RendererConfig.skin > 0) and, with each of the frame's renderer_char_lights:
 * - eye texels (by the eye weight x RendererConfig.eyes): over an eyeball (the normal bent by the
 *   texel's offset from its eye's centre), a small, sharp wet glint of each light, a faint
 *   catchlight from the camera's side scaled by the draw's own (baked) light, both dimmed on an eye
 *   too small for a point and kept under half the light a white texel gets there, and slightly
 *   darker lid edges; all x renderer_set_eye_open;
 * - cloth texels (x RendererConfig.cloth): leather a medium-gloss highlight broken by the texture's
 *   creases (a bump from its detail), a broad sheen of the draw's own light from above on the same
 *   bump, and darker creases; denim a very low, broad sheen with a fine
 *   twill; cotton a faint sheen at grazing angles (velvet). Only the lights add light.
 * Texels of weight 0 draw as the skin variant. Material draws executed by the last frame: */
uint32_t renderer_mat_count(void);

/* Bloom (RendererConfig.bloom), in order with the draws on the frame target: marks the end of the
 * scene. Everything drawn on the frame target before it glows, everything after it (the HUD, text)
 * is drawn over the glow unchanged. At this point the render pass is split (as for glass, MSAA
 * resolved first) and the scene is run through the bloom chain from half the internal size down to
 * 1/16: a bright pass with a soft knee at bloom_threshold (max channel; rescaled so white glows the
 * same at any threshold; at most white per channel), 13-tap downsamples (the first one
 * Karis-weighted against single bright pixels) and 3x3 tent upsamples added level by level, then
 * screened onto the scene (scene + glow x (1 - scene)) times bloom_strength, so bright areas do
 * not clip into flat white. Light-aware: while bloom is on, the target's alpha channel (otherwise
 * unused: DST_ALPHA reads as 1) holds a light mask for the scene draws before the split: additive
 * draws (blend ONE or SRC_ALPHA onto ONE: fire, neon, flares) add their alpha to it; opaque draws
 * clear it, alpha-blended draws (the lamps' halo sprites among them: they already are a glow) fade
 * it by their alpha, other blends keep it. The bright pass lowers the threshold by up to a fifth
 * and raises the glow up to 1.3x where the mask is set. At most one per
 * frame (later ones are ignored); a frame without one is recorded exactly as without bloom.
 * Ignored when bloom is off or unavailable, and while an offscreen target is set. */
void renderer_bloom(void);
/* Bloom in use (configured and the device runs it), and the splits executed by the last frame. */
int renderer_bloom_on(void);
uint32_t renderer_bloom_count(void);

/* Temporal anti-aliasing (RendererConfig.taa), in order with the draws on the frame target: marks
 * the end of the jittered 3D scene. In a frame that calls it, every draw recorded before it on the
 * frame target with RENDERER_DEPTH_TEST is offset by the frame's sub-pixel jitter (taa_jitter of the
 * renderer's TAA frame count, added to the clip position in geom.vert, so geometry past the screen
 * edge still covers it); later draws, offscreen passes and frames without it are not. At this point the render pass is split (MSAA resolved
 * first) and a resolve pass blends the frame with the history: each pixel's previous position is
 * its NDC (x, y) in the viewport `vp` (back-buffer pixels) at the view distance of the nearest
 * depth around it (NDC depth = depth_a - depth_b / w; at the far end a direction), through the
 * reprojection map (taa_reprojection; valid = 0: no motion), or, on a skinned character drawn with
 * renderer_set_draw_identity (its surface in the velocity pass at the same distance), through its
 * own previous pose and camera; the history there (Catmull-Rom) is
 * clipped to the variance box of the 3 x 3 neighbourhood in YCoCg and blended with about a tenth of
 * the frame (more for faster motion), the alpha (the bloom light mask) likewise. reset (a cut, a
 * level load), a frame without it before, or a reprojection off screen take the frame as is. The
 * result becomes the history and is drawn back into the target, sharpened by taa_sharpness. At most
 * one per frame (later ones are ignored); ignored while an offscreen target is set and when TAA is
 * off or unavailable (the depth target cannot be sampled). */
typedef struct {
    float reproject[3][4];   /* taa_reprojection (current to previous clip x, y, w) */
    float depth_a, depth_b;  /* the scene's depth projection: NDC depth = depth_a - depth_b / w */
    float vp[4];             /* the 3D viewport: x, y, width, height in back-buffer pixels */
    int valid;               /* reproject is usable (else no motion is assumed) */
    int reset;               /* drop the history this frame */
} RTaa;
void renderer_taa(const RTaa *p);
/* TAA in use (configured and the device runs it), the resolves executed by the last frame and the
 * jitter of the next one (target pixels; 0 0 when off). */
int renderer_taa_on(void);
uint32_t renderer_taa_count(void);
/* The skinned draws whose own motion (renderer_set_draw_identity) the last frame's resolve used. */
uint32_t renderer_taa_object_draws(void);
void renderer_taa_jitter(float out[2]);
/* Temporal upscaling in use (1; RendererConfig.taa_up_w) or not (0); *render_w x *render_h (either may
 * be NULL) the 3D scene's render size when in use. */
int renderer_taa_upscale(int *render_w, int *render_h);
/* The upscaler in use under temporal upscaling: 1 FSR 3.1 (RendererConfig.taa_upscaler), else 0. */
int renderer_taa_fsr3(void);
/* --upscaler fsr3's masks of the last frame (tests): the top-left w x h
 * of the mask target, RGBA8 per pixel (r the reactive mask, g the transparency & composition mask,
 * before FSR's dilation), and the draws that wrote each (draws may be NULL). Returns 0, or -1 when
 * FSR 3.1 or its masks are not in use or w x h exceeds the render size. Waits for the device. */
int renderer_fsr3_masks(uint8_t *rgba, int w, int h, uint32_t draws[2]);
/* --dynres (src/dynres.h): the 3D scene's render size from the next frame on, clamped to 1..the size
 * temporal upscaling started with (RendererConfig.taa_up_w x taa_up_h: the rectangle every pass is
 * sized for). Nothing is recreated: the history stays at the output size, the jitter length follows
 * the new size (taa_jitter_phases), the texture LOD bias stays the starting size's (a sampler
 * setting), and the ambient occlusion's own temporal history is dropped for the first frame at the
 * new size. Ignored without temporal upscaling. */
void renderer_taa_render_size(int w, int h);
/* The TAA sharpening (RendererConfig.taa_sharpness, clamped to 0..TAA_SHARPNESS_MAX) from the next
 * frame on; --dynres follows the render size with it when --taa-sharpness is left out. */
void renderer_taa_sharpness(float s);

/* Frame generation (RendererConfig.frame_gen), in order with the draws on the frame target: marks the
 * end of the 3D scene and its effects, before the HUD (the filmic anchor's place). There the frame
 * target is copied (the MSAA resolved) and a motion field is written at the display size: each pixel's
 * previous position through the reprojection map from its depth (as renderer_taa; valid = 0: nothing
 * moves), or a skinned character's own motion when the TAA velocity pass drew it. The HUD is what the
 * target gains from here to renderer_frame_gen_hud_end (or the end of the frame): those pixels (and,
 * when the post chain resamples or sharpens, the ones next to them) are still and in front of
 * everything, like the area outside the 3D viewport. The generated frame is built at the display size
 * from this and the previous frame's outputs as shown: each pixel moves half way along its motion
 * (nearest surface first, holes from the farthest one around), both frames are read along it (a side
 * where a nearer surface, the HUD or the bars cover it is dropped) and blended; the HUD keeps the new
 * frame's pixels. reset (a cut), or no frame_gen frame right before, take the output as is. At most one
 * per frame; ignored while an offscreen target is set and when off or unavailable. */
typedef struct {
    float reproject[3][4];   /* taa_reprojection (current to previous clip x, y, w) */
    float depth_a, depth_b;  /* the scene's depth projection: NDC depth = depth_a - depth_b / w */
    float vp[4];             /* the 3D viewport: x, y, width, height in back-buffer pixels */
    int valid;               /* reproject is usable (else no motion is assumed) */
    int reset;               /* no interpolation this frame (a cut, a level load) */
} RFrameGen;
void renderer_frame_gen(const RFrameGen *p);
/* Frame generation: the end of the HUD, before full-screen draws that cover the scene and the HUD alike
 * (the title's colour tint and frame-feedback blur after its HUD). The HUD is what the frame target
 * gained between renderer_frame_gen and here (or the end of the frame without it). Ignored without a
 * renderer_frame_gen before it in the frame, and while an offscreen target is set. */
void renderer_frame_gen_hud_end(void);
/* Frame generation in use (configured and the device runs it); the generated frames built by the last
 * frame (0 or 1). */
int renderer_frame_gen_on(void);
uint32_t renderer_frame_gen_count(void);
/* The generated frame of the last renderer_end (XRGB8888 like it, the output size), when it built
 * one: returns 1 and fills out, else 0. */
int renderer_frame_gen_readback(uint32_t *out, int pitch_bytes);
/* renderer_present shows the generated frame (when the frame built one) one refresh before the real
 * one (1), or builds it without showing it (0, the default: one present a frame, as without it), or
 * presents the real frame twice (2: generated frames paused, the queue keeps its cadence). */
void renderer_frame_gen_present(int on);
/* Pacing by present completion (VK_KHR_present_wait) in use: before a frame takes its swapchain images
 * the renderer waits until the real frame before the last one is on screen. */
int renderer_frame_gen_present_wait(void);
/* The last present wait since the previous call: 1 with its return time (now_ms clock) and whether it
 * waited (else the frame was on screen already); 0 when none. */
int renderer_frame_gen_pacing(double *t_ms, int *waited);
/* Presents of generated frames since renderer_init. */
uint32_t renderer_frame_gen_presented(void);

/* HDR10 output (RendererConfig.hdr, src/hdr.h), in order with the draws on the frame target:
 * renderer_hdr_scene marks the end of the 3D scene (the frame generation's anchor: after the scene's
 * last draw and the full-screen tint right after it, before the HUD); the target is copied there.
 * renderer_hdr_hud_end marks the HUD's end (before the full-screen draws over everything at the end of
 * the frame); without it the end of the frame. Pixels that changed in between are the HUD (paper
 * white), the others the scene (highlights expanded). A frame without renderer_hdr_scene (menus, Bink
 * movies) is all at paper white. Both ignored while HDR is not in use and while an offscreen target is
 * set. renderer_hdr_on: 1 while the swapchain is HDR10 (or with RENDERER_HDR_TEST). */
void renderer_hdr_scene(void);
void renderer_hdr_hud_end(void);
int renderer_hdr_on(void);
/* Peak and paper white in nits (clamped to the hdr.h ranges), from the next frame. */
void renderer_hdr_levels(float peak, float paper);
/* RENDERER_HDR_TEST: the HDR pass's output of the last renderer_end (A2B10G10R10: red in bits 0-9,
 * green 10-19, blue 20-29, PQ code values), at the output size: 1, else 0. */
int renderer_hdr_readback(uint32_t *out, int pitch_bytes);
/* The final grade's saturation (0.5-2, 1 = unchanged: no pass in SDR), from the next frame. */
void renderer_saturation(float s);

/* Filmic look (RendererConfig.filmic), in order with the draws on the frame target: marks the end
 * of the 3D scene (after the bloom split, the halos, rain and wet ground; before the HUD). At this
 * point the render pass is split (MSAA resolved first), the scene is copied and every pixel of the
 * target is replaced by its filmic version (RFilmic, shaders/filmic.frag): pseudo-HDR from the
 * light mask in the target's alpha, tonemap, halation from the bloom glow (frames with a bloom
 * split), the LUT grade, vignette and grain. The alpha (light mask) is kept. At most one per
 * frame; ignored when the look is off or unavailable, and while an offscreen target is set. */
void renderer_filmic(void);
/* The pass in use (the look or the exposure-only pass configured and the device runs it), and the
 * passes executed by the last frame. */
int renderer_filmic_on(void);
uint32_t renderer_filmic_count(void);
/* Automatic exposure in use (RendererConfig.auto_exposure and the device runs it), and the
 * adaptation state of the last completed frame that ran it (adapted level and meter in log2
 * linear luma, the gain in stops, the cut timer: RAutoExposure). Returns 0 before one completed. */
int renderer_auto_exposure_on(void);
int renderer_auto_exposure_state(float out[4]);
/* A camera cut in the frame being recorded: its adaptation snaps to the meter (at its
 * renderer_filmic; the request waits for the next frame that runs the pass). */
void renderer_auto_exposure_snap(void);
/* Depth of field (RendererConfig.dof) in this frame, before its renderer_filmic: `weight` 0-1 (0:
 * none: the frame is exactly as without it), the frame's depth projection (NDC depth = depth_a -
 * depth_b / distance), the seconds since the last DOF frame (at most 0.1 is used) and snap != 0
 * (a camera cut, the first frame of a cutscene): the focus jumps to the metered one. Ignored when
 * the depth of field is off or unavailable and while an offscreen target is set. */
typedef struct {
    float weight;
    float depth_a, depth_b;
    float dt;
    int snap;
    float speaker;        /* 0-1: the speaker's weight (dof_speaker_step; 0: no speaker, as without it) */
    float speaker_dist;   /* his view distance (> 0 while speaker > 0): the sharp range is around it */
} RDofFrame;
void renderer_dof(const RDofFrame *p);
/* Depth of field in use (configured and the device runs it: it samples the depth and has the
 * character mask), the frames of the last renderer_end / renderer_present that ran it, and the
 * focus state of the last completed frame that ran it (log2 near and log2 far end of the sharp
 * range, 3 when a speaker set it, 2 when characters did, 1 the centre meter, 0 when nothing was metered: held,
 * the frame's weight). Returns 0 before one completed. */
int renderer_dof_on(void);
uint32_t renderer_dof_count(void);
int renderer_dof_state(float out[4]);

/* Procedural rain slices (renderer_rain): streaks in depth slices, drawn back to front over the
 * frame target. Each slice is a full-target layer at NDC depth `depth`, depth tested (less or
 * equal) against what is drawn so far and not written, so the scene hides the slice where it is
 * nearer. Its pattern is a grid of cells (cell_w x cell_h) in a frame tilted by RRain.tilt and
 * scrolled by (scroll_x, scroll_y); a cell holds a streak with probability `fill`, at a random
 * place across it, `length` x cell_h long (bright head, fading tail) and `width` wide, of
 * RRain.color at opacity `alpha` times its coverage (streaks thinner than a pixel fade instead of
 * breaking up), over a uniform veil of RRain.color at opacity `haze` (the rain between slices: a
 * pixel the scene leaves open to more slices gets more veil, so far surfaces haze more than near
 * ones). `lit` > 0 also brightens what lies behind a streak by that factor times RRain.color and
 * the streak's coverage (dst x (1 + lit x ...)): the streaks catch the scene's light, bright near
 * lamps and faint in the dark, instead of a flat colour. With RRain.depth_a > 0 every streak has
 * its own view distance, dist x spread^h (h random per streak in [0, 1)), drawn at its own NDC
 * depth depth_a - depth_b / distance and sized 1 / spread^h of the slice's streaks, so the scene
 * hides streaks one by one rather than in a few flat layers. Sizes are in screen heights (the target's height = 1), so the look does not depend on the
 * resolution. The grid repeats every RENDERER_RAIN_PERIOD cells in both directions, so a
 * scroll can be wrapped by that many cells without a jump. */
#define RENDERER_RAIN_SLICES 6
#define RENDERER_RAIN_PERIOD 4096
typedef struct {
    float depth;               /* NDC depth, 0-1 */
    float cell_w, cell_h;      /* screen heights */
    float length, width;       /* fraction of cell_h; screen heights */
    float scroll_x, scroll_y;  /* screen heights, in the tilted frame (y: the fall direction) */
    float alpha, fill;         /* 0-1 */
    float haze;                /* 0-1, 0 = streaks only */
    float dist, spread;        /* view distance of the slice; streak distances dist..dist x spread (>= 1) */
    float lit;                 /* 0-2, 0 = flat colour only */
} RRainSlice;
typedef struct {
    RRainSlice slice[RENDERER_RAIN_SLICES];
    uint32_t slices;           /* used, in drawing order (far first) */
    float tilt;                /* radians: fall direction from straight down, positive = towards the right */
    uint32_t color;            /* ARGB8888 (alpha ignored: per slice) */
    float depth_a, depth_b;    /* the scene's depth projection (NDC depth = a - b / distance); a = 0: slice depth only */
} RRain;
/* In order with the draws on the frame target (ignored while an offscreen target is set, when rain
 * is off or unavailable, and without slices). One pipeline in the main render pass: no pass split,
 * and with MSAA every sample is depth tested. */
void renderer_rain(const RRain *r);
/* Rain in use (configured and the device runs it), and the rain operations of the last frame. */
int renderer_rain_on(void);
uint32_t renderer_rain_count(void);

/* Procedural sky (RendererConfig.sky), in order with the draws on the frame target: one
 * full-target triangle, no depth test or write, that replaces the title's sky dome and cloud cap.
 * Per pixel the world-space view direction d = fwd + x right + y up (x, y: D3D NDC of the pixel,
 * y up; world z up) gives:
 *   - the gradient: ramp[k] at elevation k x 90 / (RENDERER_SKY_RAMP - 1) degrees (XRGB,
 *     interpolated), below the horizon the silhouette colour;
 *   - a distant city band just above the horizon, its building heights hashed per azimuth cell
 *     (unique over 360 degrees: no repeat), in the silhouette colour hazed toward the horizon, with
 *     a glow over it that varies slowly with azimuth;
 *   - stars: hashed cells on the sphere (sky.h SKY_STAR_CELLS), each holding a nearly white star
 *     with chance `stars` (sky.h SKY_STAR_CHANCE), one pixel wide at any resolution, fading toward
 *     the horizon and under clouds, twinkling slightly with `seconds`;
 *   - clouds: value noise on a plane above the camera drifting with `seconds`, `cover` 0-1 of the
 *     sky, lit from below by the horizon colour (overcast toward cover 1);
 *   - the moon: a disc of angular radius moon_size (radians; 0 = none) toward `moon`, limb darkened,
 *     with a halo, at strength moon_light 0-1, behind the clouds.
 * The target's alpha (the bloom light mask, renderer_bloom) gets the moon disc. Ignored when sky
 * is off or unavailable, and while an offscreen target is set. */
#define RENDERER_SKY_RAMP 8
typedef struct {
    float right[3], up[3], fwd[3];
    uint32_t ramp[RENDERER_SKY_RAMP];
    uint32_t silhouette;
    float stars, cover;
    float moon[3], moon_size, moon_light;
    float seconds;
} RSky;
void renderer_sky(const RSky *p);
/* Sky in use (configured and the device runs it), and the sky operations of the last frame. */
int renderer_sky_on(void);
uint32_t renderer_sky_count(void);

/* Skyline windows and haze (RendererConfig.sky), in order with the draws on the frame target,
 * right after the title's skyline draw with the same vertices (v, nv, idx, ni, viewport vp in
 * back-buffer pixels as for renderer_draw): drawn again, depth tested (less or equal) and not
 * written, over it. Each vertex holds its world position in tex[2].xyz and the skyline atlas's
 * coordinates in tex[0].xy (the atlas's left half, u fraction under 0.5, is the title's painted
 * window). Each painted window, identified by its centre, stays as painted (lit) or is dimmed to
 * the facade (unlit), one decision per window: a `windows` 0-1 share of half of them lit, a rare one
 * flickering for a few seconds (`seconds`). Then the pixel fades toward `haze` (XRGB) by haze_max x
 * (1 - exp(-distance / haze_dist)) (view distance from the draw's clip w). No light is added and the
 * bloom light mask is left alone: the world blends some of its surfaces over the sky stage, and
 * anything bright added here would show through them. Ignored when sky is off or unavailable, and while an offscreen target is
 * set. */
typedef struct {
    uint32_t haze;
    float haze_max, haze_dist;
    float windows;
    float seconds;
} RSkyline;
void renderer_skyline(const RViewport *vp, const RVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni, const RSkyline *p);
uint32_t renderer_skyline_count(void);

/* Rain splashes and ripples (with RendererConfig.rain), in order with the draws on the frame target:
 * after the opaque world (where renderer_ssao runs; both share one pass split when this follows it
 * directly), the depth drawn so far is read and every pixel of an upward-facing surface (normal
 * from the neighbouring depths within 45 degrees of `up`, flat over a few centimetres: not walls,
 * not characters' curved tops) gets the drops landing there: its world position (from the depth,
 * px/py and the camera) falls in a grid of `cell` x `cell` world cells on the ground plane, each
 * holding at most one impact per `period` seconds (with probability `chance`, at a random place and
 * phase), so the pattern is fixed in the world (no swimming when the camera moves) and changes only
 * with `time`. An impact is a crown of 2-3 droplets thrown up and out for ~0.15 s (seen in front of
 * the ground behind them: parallax from the camera) and, on flat ground, a ring that widens and
 * fades over ~0.5 s. They brighten what lies under them (dst x (1 + splash or ripple x color x
 * coverage)): clear near lamps, and add `flat` times that much colour as is (a second pass,
 * additive), so they do not vanish in the dark; they fade out towards `fade` view units and are
 * never thinner than a pixel. They are worked out at the output size (the internal size divided by
 * the supersampling factor) into a mask that is then laid over the target bilinearly. Draws after it (effects, rain streaks, HUD) are drawn over them. At
 * most one per frame (later ones are ignored); a frame without one is recorded exactly as without
 * it. Ignored when rain is off, the device cannot sample the depth, and while an offscreen target
 * is set. */
typedef struct {
    float depth_a, depth_b;            /* the scene's depth projection: NDC depth = depth_a - depth_b / distance */
    float px, py;                      /* clip x = px * view x, clip y = py * view y (x right, y up, at distance 1) */
    float eye[3];                      /* camera position (world) */
    float right[3], up_v[3], back[3];  /* camera axes in the world: view x, view y, towards the viewer */
    float up[3];                       /* world up (unit) */
    float time;                        /* seconds */
    float cell, period, chance;        /* world units, seconds, 0-1 */
    float splash, ripple;              /* brightening of droplets and rings, 0-4 */
    float flat;                        /* 0-1: their flat colour, a share of that brightening added as is */
    float fade;                        /* view distance where they are gone */
    uint32_t color;                    /* ARGB8888 (alpha ignored) */
} RRainSplash;
void renderer_rain_splash(const RRainSplash *p);
/* Splashes in use (rain configured and the device samples the depth), and those of the last frame. */
int renderer_rain_splash_on(void);
uint32_t renderer_rain_splash_count(void);

/* Wet ground (with RendererConfig.rain and rain_wet), in order with the draws on the frame target:
 * meant right before the procedural rain, so the colour drawn so far (the opaque world, effects,
 * lamp halos, bloom) is what the ground reflects, and the depth is the opaque world's. Ends the
 * render pass there; the resolved colour is reduced to half the output size, and at that size
 * every pixel of an upward-facing surface (normal from the neighbouring depths within ~35 degrees
 * of `up_view`) traces the view ray mirrored about `up_view` through the depth (screen space, a
 * short march with a final refinement) out to `reach` view units; what it hits is weighted by its
 * brightness (the bright parts: lamps, windows, signs, lights), by a Schlick fresnel term (water)
 * and by fades towards the screen edges, the end of the ray and rays towards the camera, and blurred
 * mostly vertically (the streaks of wet asphalt). The composite at the internal size: dst x (1 -
 * darken x wet) + reflect x that reflection. Draws after it (rain, HUD) are drawn over it. At most
 * one per frame (later ones are ignored); a frame without one is recorded exactly as without it.
 * Ignored when rain or rain_wet is off, the device cannot sample the depth, and while an offscreen
 * target is set. */
#define RENDERER_WET_LIGHTS 32
typedef struct {
    float depth_a, depth_b;   /* the scene's depth projection: NDC depth = depth_a - depth_b / distance */
    float px, py;             /* clip x = px * view x, clip y = py * view y (x right, y up, at distance 1) */
    float up_view[3];         /* world up in view space (x right, y up, z towards the viewer), unit */
    float reflect;            /* reflection gain, 0-8 */
    float darken;             /* 0-1: how much darker wet ground gets */
    float reach;              /* view distance a reflected ray is traced */
    /* Puddles (--rain-hd-puddles): 0 = none (the wet ground exactly as without them). Above 0, how
     * strongly they show (0-1): world-anchored patches of open, flat, wet ground (value noise on the
     * world's ground plane, more in low spots: ground lower than the rain cover map around it) are
     * darker, reflect more of the scene than its bright parts, less blurred, and their reflection
     * is tilted by rings of drops landing (a grid of world cells, one ring per cell and period, by
     * `time`). The camera in the world (world up +z) anchors them. */
    float puddles;
    float time;                         /* seconds (the title's clock) */
    float eye[3];                       /* camera position (world) */
    float right[3], up_v[3], back[3];   /* camera axes in the world: view x, view y, towards the viewer */
    /* Light streaks (--wet-light-streaks): 0 = none (the wet ground exactly as without them). Above
     * 0, their gain (0-8): every wet pixel mirrors each of the n lights (view space, colour x
     * intensity) about the plane through its own position with normal up_view and adds a highlight
     * stretched along the view's direction on the ground (an anisotropic lobe, longer with the
     * pixel's view distance and rougher in world-anchored patches, the camera in the world above),
     * times a Schlick fresnel (F0 0.15: the oily sheen of wet asphalt) and a falloff over streak_reach x the light's radius,
     * a mirrored light being several times brighter than the frame's white; lights below the
     * pixel add nothing. It is combined with the scene's reflection by max (plus a share of the
     * smaller), so a lamp the screen-space trace also finds is not counted twice, and it is not
     * clipped by the 8-bit scene. Where the trace finds nothing (off the screen, a miss, its fades)
     * the ground reflects `fallback` (0-1) x fog_rgb instead, standing water most. The blur's
     * streaks lengthen with view distance. */
    float streaks;
    float streak_rough;                  /* lobe width along the view on the ground (slope units, > 0) */
    float streak_reach;                  /* falloff distance in light radii (> 0) */
    float fallback;                      /* 0-1 */
    float fog_rgb[3];                    /* 0-1 */
    /* Materials (--materials): 0 = none (the wet ground exactly as without them). Above 0, their
     * strength (0-2): with a material map (renderer_material_map) every wet pixel takes the look of
     * the surface material under it by its world position (the camera in the world above): how
     * much it reflects, how much darker it gets, how many puddles it holds, and the gloss and
     * wetness of its light streaks (asphalt glossy, dirt matte mud, grass barely wet); where the
     * map has no surface near the pixel's height the look is as without materials. */
    float materials;
    /* Damp streets (--damp-streets): 0 = the rain's wet ground (exactly as without the option).
     * Above 0 (needs materials and their map), the damp mode of a dry frame: only the surfaces the
     * map places as asphalt (fully) or concrete (a little) get wet, never under a second, higher
     * surface of the map (a roof, a walkway) nor where the map places nothing; the reflection keeps
     * only the brightest parts and is blurred less (tight), the puddles become small damp patches
     * near kerbs (the map's raised edges) without ripples, and the streaks are tighter. */
    float damp;
    /* Neon reflections (--neon-reflections): 0 = none (the wet ground exactly as without them).
     * Above 0 (with the streaks on), the reflected scene keeps saturated colours as bright as their
     * strongest channel (red and blue neon tubes, dark by luma alone) up to this share (0-1 used),
     * and lights with a size are signs: their streak widens by that extent seen from the ground. */
    float neon;
    uint32_t nlights;                    /* lights in light (at most RENDERER_WET_LIGHTS) */
    struct {
        float pos[3];   /* right-handed view position (x right, y up, the camera looks down -z) */
        float radius;   /* view units (> 0) */
        float rgb[3];   /* colour x intensity, >= 0 */
        float size;     /* half the extent of a sign (view units; 0: a point light, a lamp) */
    } light[RENDERER_WET_LIGHTS];
    /* --reflections sssr's glass (RendererConfig.sssr_glass; 0 = none: every pass as without it).
     * Above 0, its strength (0-2): the frame's world draws whose specular map holds a material
     * (--dry-highlights' B shine / A gloss) are drawn again into a surface target (face normal,
     * shine, gloss, depth), and the glossy ones (glass, shop windows, car paint, chrome) reflect what
     * the screen shows by the same stochastic march, their roughness from the gloss, weighted by a
     * Schlick Fresnel term: the reflection replaces that share of the surface's colour (energy kept:
     * the night stays as dark), lamps and neon a little brighter than the 8-bit frame. The wet ground's
     * roughness also follows the gloss of its own texture there. */
    float glass;
    /* 1: a dry frame (no rain, no damp streets): only the glass reflects, the ground stays as it is
     * (reflect, darken, puddles, streaks unused). 0: the wet or damp ground as above. */
    int dry;
} RRainWet;
void renderer_rain_wet(const RRainWet *p);
/* Wet ground in use (rain and rain_wet configured, the device samples the depth), and that of the last frame. */
int renderer_rain_wet_on(void);
uint32_t renderer_rain_wet_count(void);

/* --reflections sssr in use (RendererConfig.sssr and the wet ground's resources), and (tests) level
 * `level` (0: the internal size, each next half of it rounded up) of its min-depth hierarchy as the last
 * frame left it: NDC depths, *w x *h of them into out (at most max_texels). Waits for the GPU. Returns
 * 0, or -1 without SSSR, a bad level or too small a buffer. */
int renderer_wet_sssr_on(void);
/* --reflections sssr's glass in use (RendererConfig.sssr_glass and its surface pass), and the material
 * draws the last frame's surface pass drew. */
int renderer_wet_glass_on(void);
uint32_t renderer_wet_glass_count(void);
int renderer_wet_hiz_read(int level, float *out, int max_texels, int *w, int *h);

/* The level's top-down material map for the wet ground's materials (RRainWet.materials; built by
 * src/materials.c): w x h texels, texel (x, y) covering world x0 + [x, x + 1) cell, y0 + [y, y + 1)
 * cell (z up); two 16-bit layers per texel (the top surface, then the second): material class << 12
 * | height, height 0 no surface, h in 1-4095 world z = z0 + (h - 1) zstep. Copied to the GPU (the
 * caller keeps its data); NULL or an empty map drops it. Call between frames (not while one is
 * being recorded): it waits for the GPU. Ignored without the wet ground (renderer_rain_wet_on). */
typedef struct {
    int w, h;
    float x0, y0, cell;
    float z0, zstep;
    const uint16_t *texels;   /* w x h x 2 */
} RMaterialMap;
void renderer_material_map(const RMaterialMap *m);
/* A material map is loaded. */
int renderer_material_map_on(void);

/* Rain cover map (RendererConfig.rain_cover, with rain): which places are under a roof. In a frame
 * with it, the frame-target draws flagged RENDERER_RAIN_COVER (the opaque world) are drawn once more,
 * depth only, into a RENDERER_RAIN_COVER_SIZE square depth map by clip_to_map (applied to each
 * vertex's clip x, y, w and 1: map NDC x, y and depth, 0 = top), before the frame's main pass; the
 * map is cleared to depth 1 (nothing there). The rain streaks (at their own distance), the splash
 * mask and the wet ground then rebuild each point's right-handed view position (x right, y up, z
 * towards the viewer; from the screen position, the distance and inv_px / inv_py), take it to the
 * map by view_to_map (u, v in 0-1, depth) and count it covered where the map's surface lies more
 * than `above` world units over it (fully at above + soft; `height` world units per map depth
 * unit), bilinear over the four nearest map texels (soft edges); covered points get no streaks,
 * splashes or wet ground. Outside the map and over cleared texels nothing is covered. At most one
 * per frame (the first); a frame without it draws no map and covers nothing. Ignored when cover or
 * rain is off and while an offscreen target is set. */
#define RENDERER_RAIN_COVER_SIZE 512
typedef struct {
    float clip_to_map[3][4];   /* rows: (clip x, y, w, 1) -> map NDC x, y (-1..1) and depth (0-1) */
    float view_to_map[3][4];   /* rows: (view x, y, z, 1) -> map u, v (0-1) and depth (0-1) */
    float height;              /* world units per map depth unit */
    float above, soft;         /* world units */
    float inv_px, inv_py;      /* view x = NDC x x distance x inv_px, likewise y */
} RRainCover;
void renderer_rain_cover(const RRainCover *c);
/* Cover in use (rain and rain_cover configured, the device runs it), and the maps drawn in the last frame. */
int renderer_rain_cover_on(void);
uint32_t renderer_rain_cover_count(void);

/* Character mask (with rain, lamp lights, contact shadows or the depth of field: they need it). In
 * a frame with renderer_rain_splash, renderer_rain_wet or renderer_dof (and the lamp and contact
 * passes), the frame-target draws flagged RENDERER_CHARACTER
 * (skinned characters) are drawn once more, depth only, into a depth image of the internal size
 * before the frame's main pass (cleared to 1 first, also without such draws). A pixel whose
 * visible depth is that image's (within a small tolerance) shows a character: it gets no splashes
 * and no wet ground (darkening or reflection), and the splashes' and wet ground's flatness tests
 * on the ground around it ignore the samples that land on characters. Draws flagged
 * RENDERER_CHARACTER are never drawn into the rain cover map (the ground under someone is not
 * under a roof). Ignored when rain is off and on offscreen targets. */
int renderer_rain_chars_on(void);
/* Character draws in the mask of the last frame. */
uint32_t renderer_rain_chars_count(void);

/* Ambient occlusion (RendererConfig.ssao), in order with the draws on the frame target: marks the
 * end of the opaque world. At this point the render pass is split (as for bloom) and the depth
 * drawn so far is read: at half the internal size, a view position is rebuilt from the depth
 * (view depth d = depth_b / (depth_a - NDC depth), x = NDC x * d / px, y = NDC y * d / py), its
 * normal from the neighbouring depths, and RENDERER_SSAO_SAMPLES points on a disc of ssao_radius
 * (view units) around it tell how much of the hemisphere above the surface is occluded (nearer
 * surfaces within the radius, fading with distance); two depth-aware blur passes remove the
 * per-pixel sample rotation without blurring across depth edges, and the result is upsampled
 * with the full-size depth (taps at another depth are left out) and multiplied onto the target's
 * color (1 - ssao_strength x occlusion, at least 0; alpha unchanged). An unoccluded pixel and
 * the far plane (depth 1, nothing drawn) keep their exact color. Draws after it (effects, HUD) are
 * not darkened. At most one per frame (later ones are ignored); a frame without one is recorded
 * exactly as without ssao. Ignored when ssao is off or unavailable (the device cannot sample the
 * depth format at the sample count), and while an offscreen target is set. */
#define RENDERER_SSAO_SAMPLES 12
typedef struct {
    float depth_a, depth_b;   /* the scene's depth projection: clip z = depth_a w - depth_b */
    float px, py;             /* clip x = px * view x, clip y = py * view y */
} RSsao;
void renderer_ssao(const RSsao *p);
/* Ambient occlusion in use (configured and the device runs it), and the splits executed by the
 * last frame. */
int renderer_ssao_on(void);
uint32_t renderer_ssao_count(void);
/* The occlusion is XeGTAO's (RendererConfig.ssao_mode honoured): 1, or 0 (classic or off). */
int renderer_ssao_gtao_on(void);
/* 1 when RendererConfig.ssgi is in use (XeGTAO on, its passes created); 0 otherwise. */
int renderer_ssgi_on(void);

/* Contact shadows (RendererConfig.contact), in order with the draws on the frame target: marks the
 * end of the opaque world, right before renderer_lamp_lights and renderer_ssao (one pass split with
 * them when they follow it directly; else a split of its own). The depth drawn so far is read: at
 * half the output size, every pixel of an upward-facing surface (normal from the neighbouring
 * depths within ~45 degrees of `up_view`) that is not a character (the character mask) sends a
 * cone of short rays up to `radius` view units above it through the depth; surfaces in front of a
 * ray point (within a thickness) occlude it, nearer ones more. The result is multiplied onto the
 * target's colour (1 - strength x occlusion, rgb only), bilinear, character pixels left out at
 * full size: the ground right under and around people and props gets darker, the characters
 * themselves never. Effects, halos, rain and the HUD drawn later are not darkened. At most one per
 * frame (later ones are ignored); a frame without one is recorded exactly as without it. Ignored
 * when contact is off, the device cannot sample the depth, and while an offscreen target is set. */
typedef struct {
    float depth_a, depth_b;   /* the scene's depth projection: NDC depth = depth_a - depth_b / distance */
    float px, py;             /* clip x = px * view x, clip y = py * view y */
    float up_view[3];         /* world up in view space (x right, y up, z towards the viewer), unit */
    float radius;             /* view units the rays reach */
    float strength;           /* 0-2: darkening at full occlusion (at most 0.9) */
} RContact;
void renderer_contact(const RContact *p);
/* Contact shadows in use (configured, the device samples the depth), and those of the last frame. */
int renderer_contact_on(void);
uint32_t renderer_contact_count(void);

/* Lamp lights (RendererConfig.lamp), in order with the draws on the frame target: marks the end of
 * the opaque world (where renderer_ssao runs; they share one pass split when renderer_ssao follows
 * it directly, and the splashes when they follow either). The depth drawn so far is read: at the
 * occlusion's working size (ssao_work_size) each pixel's view position and normal are rebuilt from
 * the depth (as for renderer_ssao) and the n lights (at most RENDERER_LAMP_MAX) add colour x
 * intensity x strength x falloff (lamp_lights.h lamp_falloff, 0 from the radius on) x a wrapped
 * Lambert term; the first RENDERER_LAMP_SHADOWED lights are tested with a short screen-space ray
 * towards the light; the sum goes through a soft knee (L / (1 + L / 2)). Pixels showing a
 * character (the rain's character mask, when the device runs it) get char_gain times as much,
 * pixels of the bloom light mask (the scene's alpha: additive glow, neon, fire) less, down to none
 * from mask 0.5 up. The result is upsampled with the full-size depth and applied as dst x (1 +
 * light) (rgb; alpha, the light mask, unchanged); the far plane and unlit pixels keep their exact
 * colour. Draws after it (effects, halos, HUD) are not lit. At most one per frame (later ones are
 * ignored); a frame without one, or with n = 0, is recorded exactly as without lamp lights.
 * Ignored when lamp is off or unavailable (the device cannot sample the depth format at the sample
 * count) and while an offscreen target is set. */
#define RENDERER_LAMP_MAX 32
#define RENDERER_LAMP_SHADOWED 4
typedef struct {
    float pos[3];      /* right-handed view position (x right, y up, the camera looks down -z) */
    float radius;      /* view units (> 0) */
    float rgb[3];      /* 0-1 */
    float intensity;   /* >= 0 */
} RLamp;
typedef struct {
    float depth_a, depth_b;   /* the scene's depth projection: clip z = depth_a w - depth_b */
    float px, py;             /* clip x = px * view x, clip y = py * view y */
    float strength;           /* scales every light (> 0) */
    float char_gain;          /* characters' share (>= 1: they gain the most; 0: none, they are lit by renderer_draw_lit) */
    uint32_t n;               /* lights in l */
    RLamp l[RENDERER_LAMP_MAX];
    /* renderer_lamp_lights: 1 = pixels that are not characters get no light (renderer_draw_world_lit
     * lights them); 0 = as described above. */
    uint32_t no_world;
    /* renderer_char_lights: the world draws' share (renderer_draw_world_lit; > 0). */
    float world_gain;
    /* renderer_char_lights: where the title's own character shadow (a depth stage's compare) darkens
     * a lit draw, 0 = it darkens only the title's light: the added light keeps its full share (a
     * shadow removes only the light its caster blocks); 1 = it darkens the whole lit colour, the
     * added light too (the look before 2026-11-13, --title-shadow-full). */
    uint32_t title_shadow_full;
    /* The level's walls stop the lights: 1 = with a wall map loaded
     * (renderer_lamp_wall_map), a light reaches a lit pixel only when the segment from the pixel
     * (moved 1.5 map cells off its surface along its normal, then from 1.5 cells away) to the light
     * (up to its own cell) crosses no wall cell within its height span; near the light (within
     * 1 world unit, horizontally) only walls reaching above it block (a lamp's own post or bracket
     * does not). Only the lights adding at least a quarter of the strongest one's light at the pixel are
     * tested, at most 4 per pixel (weaker ones pass walls). Each pixel offsets the segment's steps (at
     * most 16, about one per map cell) within their interval, and the world-lit draws average the result over the pixel's 2 x 2 quad, so
     * the wall's shadow edge is soft. The lamp pass, renderer_char_lights (character and world-lit
     * draws) alike. 0, no map, or no light_walls / lamp_shadow configured: exactly as without it. */
    uint32_t walls;
    /* With walls: the view to the world (rows: world (x, y, z, z up) = to_world x (view x, y, z, 1)). */
    float to_world[3][4];
    /* --hair (renderer_set_hair): world up in view space (unit; all 0: unknown, view y is taken). */
    float up_view[3];
} RLampLights;
void renderer_lamp_lights(const RLampLights *p);
/* Lamp lights in use (configured and the device runs them), and the passes executed by the last frame. */
int renderer_lamp_lights_on(void);
uint32_t renderer_lamp_lights_count(void);

/* Light shafts (RendererConfig.shafts), in order with the draws on the frame target: marks the end
 * of the opaque world (put it after renderer_lamp_lights, renderer_ssao and renderer_rain_splash:
 * it takes a pass split of its own). The depth drawn so far is read: at the occlusion's working
 * size (ssao_work_size; twice as coarse with shafts_cheap) each texel's view ray, from the camera to
 * the visible surface (at most max_dist view units), is sampled where it crosses a light's volume:
 * a lamp's cone below it (light_shafts.h light_shafts_cone, axis `axis`, length `length`) with a
 * small glow round its head, or a fire's round glow (kind 1, rising along `axis`). Each sample adds
 * colour x intensity x a forward-scattering phase; the sum x density goes through a soft knee
 * (L / (1 + L)). The result is upsampled with the full-size depth and added to the target's colour
 * (rgb; alpha, the light mask, unchanged), dithered; pixels with no light keep their exact colour.
 * Draws after it (effects, halos, rain, HUD) are drawn over it. At most one per frame (later ones
 * are ignored); a frame without one, or with n = 0, is recorded exactly as without shafts. Ignored
 * when shafts is off or unavailable (the device cannot sample the depth) and while an offscreen
 * target is set. */
#define RENDERER_SHAFT_MAX 16
typedef struct {
    float pos[3];      /* right-handed view position (x right, y up, the camera looks down -z) */
    float length;      /* view units (> 0) */
    float rgb[3];      /* colour x intensity, 0-1 */
    float axis[3];     /* unit view-space direction of the volume: world down (lamp), up (fire) */
    uint32_t kind;     /* 0 lamp cone, 1 fire glow */
} RShaft;
typedef struct {
    float depth_a, depth_b;   /* the scene's depth projection: clip z = depth_a w - depth_b */
    float px, py;             /* clip x = px * view x, clip y = py * view y */
    float density;            /* scattering per view unit (> 0) */
    float max_dist;           /* longest ray (view units, > 0) */
    uint32_t n;               /* lights in l */
    RShaft l[RENDERER_SHAFT_MAX];
} RLightShafts;
void renderer_light_shafts(const RLightShafts *p);
/* Light shafts in use (configured and created), and the passes executed by the last frame. */
int renderer_light_shafts_on(void);
uint32_t renderer_light_shafts_count(void);

/* Volumetric fog (RendererConfig.volfog), in order with the draws on the frame
 * target: marks the end of the opaque world (where renderer_light_shafts goes; a pass split of its
 * own). Compute passes fill a camera-aligned froxel grid (volfog.h volfog_grid of the configured
 * quality: screen tiles x slices at volfog_slice_depth from near to far): (1) with walls, each
 * light's polar wall map (VOLFOG_POLAR_BINS directions round it on the wall map of
 * renderer_lamp_wall_map: the distance to the first wall and its height span, and to the first wall
 * standing across the light's height; a ray passes a wall above or below it); (2) per froxel at this frame's jittered sample (jitter) the in-scatter of every
 * light in reach (colour x volfog_falloff x a Henyey-Greenstein phase of volfog_phase_g; a lamp's
 * light in a defined downward cone along -up, volfog_lamp_shape, little above it; blocked behind a
 * wall of its polar map; x volfog_rain_scatter of rain) x density (thinner with height over ground
 * along up, a slow 3D noise scrolled by time; plus the ground mist, mist x volfog_mist) x the title's
 * fog keep (volfog_fog_keep of fog), blended (VOLFOG_HISTORY) with the previous frame's froxel at
 * prev x (view position) when history is 1; (3) per column the front-to-back integral (Frostbite's
 * energy-conserving step; extinction = density / VOLFOG_ALBEDO). The composite reads, per pixel,
 * the integral at the pixel's view depth (far plane and sky: far) and sets the target's colour
 * (rgb; alpha, the light mask, unchanged) to colour x lerp(1, T, VOLFOG_EXTINCTION_MIX) + S,
 * dithered; pixels with no light keep their exact colour. Draws after it (effects, halos, rain,
 * HUD) are drawn over it. At most one per frame (later ones are ignored); a frame without one, or
 * with n = 0, is recorded exactly as without volumetric fog (the history is dropped). Ignored when
 * volfog is off or unavailable (the device cannot sample the depth or store to RGBA16F 3D images)
 * and while an offscreen target is set. */
#define RENDERER_VOLFOG_MAX 48
#define VOLFOG_POLAR_BINS 128
typedef struct {
    float pos[3];      /* right-handed view position */
    float radius;      /* reach (view units, > 0) */
    float rgb[3];      /* colour x intensity (>= 0) */
    uint32_t kind;     /* VOLFOG_LAMP, VOLFOG_FIRE, VOLFOG_NEON */
} RVolfogLight;
typedef struct {
    float depth_a, depth_b;   /* the scene's depth projection: clip z = depth_a w - depth_b */
    float px, py;             /* clip x = px * view x, clip y = py * view y */
    float near_z, far_z;      /* the volume's depth range (view units, 0 < near < far) */
    float density;            /* scattering per view unit (>= 0) */
    float up[3];              /* world up in view space (unit) */
    float time;               /* seconds (the noise's drift) */
    float jitter[3];          /* this frame's sample offsets in a froxel (0-1) */
    uint32_t fog_on, fog_mode;          /* the title's fog: geom_fog_factor's mode, start, end, density */
    float fog_start, fog_end, fog_density;
    uint32_t history;         /* 1 = blend with the previous frame's froxels through prev; 0 = reset */
    float prev[3][4];         /* rows: previous view position = prev x (view position, 1) */
    float prev_px, prev_py;   /* the previous frame's projection scale */
    uint32_t walls;           /* 1 = the wall map stops the lights (with one loaded) */
    float to_world[3][4];     /* with walls: rows, world (x, y, z up) = to_world x (view, 1) */
    float ground;             /* the ground's height along up from the camera (view units; 0: the air thins from the camera's height) */
    float mist;               /* the ground mist's density at the ground, share of density (0: none; volfog_mist_density) */
    float rain;               /* the rain's level (0-1): more in-scatter near the lights, lamps scatter forward (volfog_rain_scatter) */
    uint32_t n;               /* lights in l */
    RVolfogLight l[RENDERER_VOLFOG_MAX];
} RVolfog;
void renderer_volfog(const RVolfog *p);
/* Volumetric fog in use (configured and created), and the passes executed by the last frame. */
int renderer_volfog_on(void);
uint32_t renderer_volfog_count(void);

/* Character lighting (RendererConfig.char_light): the lights of the frame being recorded for its
 * renderer_draw_lit draws (px, py, strength, char_gain, n and l of p as for renderer_lamp_lights;
 * depth_a and depth_b are not used). Call before those draws; once per frame (later calls are
 * ignored). A frame without it, or with n = 0, draws them as renderer_draw. */
void renderer_char_lights(const RLampLights *p);
/* renderer_draw, lit per fragment by the frame's renderer_char_lights when they are set, char_light
 * runs and the draw is on the frame target (else exactly renderer_draw): normals holds a unit
 * right-handed view-space normal per vertex (3 floats; char_light.h char_light_normals). Each
 * fragment's view position is (x / px, y / py, -w) of the interpolated clip position, its normal the
 * interpolated one (renormalised); every light within its radius adds colour x intensity x strength
 * x falloff (lamp_lights.h lamp_falloff) x a wrapped Lambert term, the sum goes through the lamp
 * pass's soft knee (L / (1 + L / 2)) and x char_gain, and the draw's colour becomes
 * colour x (1 + L) (rgb, before blending; alpha as computed). */
void renderer_draw_lit(const RViewport *vp, uint32_t flags, const RVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni, const float *normals);
/* Character lighting in use (configured and created), and the lit draws of the last frame. */
int renderer_char_lights_on(void);
uint32_t renderer_char_lights_count(void);
/* World lighting (RendererConfig.world_light or char_light): renderer_draw_lit in its world mode,
 * lit by the lights of the frame's renderer_char_lights whose bits are set in lights (bit i = light
 * i; 0, or no lights set, draws exactly as renderer_draw). normals: a unit right-handed view-space
 * normal per vertex (world_light.h world_light_record). Per fragment each light of the mask within
 * its radius adds colour x intensity x strength x falloff x a wrapped Lambert term (world wrap),
 * the sum goes through the soft knee (L / (1 + L / 2)) and x world_gain, and the vertex diffuse
 * colour v0 of the pixel shader becomes v0 + L x (1 - v0) (rgb) before its combiners: the
 * texture, the combiners and the final combiner's fog apply to the lit colour. */
void renderer_draw_world_lit(const RViewport *vp, uint32_t flags, const RVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni,
                             const float *normals, uint32_t lights);
/* The world-lit draws of the last frame. */
uint32_t renderer_world_lights_count(void);

/* Lamp character shadows (RendererConfig.lamp_shadow): the frame's lights for the shadow receivers
 * (draws with a stage bound to a depth surface in PROJECT3D mode: the title's character shadow map,
 * seen from a light above). Per fragment of such a draw the depth stage's interpolated coordinates
 * (s, t, r, q) give its world position (to_world); for each light within its radius a ray from the
 * fragment towards the light is marched through the shadow map as a height field: a ray point that
 * lies behind (below) a stored caster depth by less than `height` view units of the light is
 * inside a character, so the light is blocked there. The occlusion fades out from 0.6 x `length` to
 * `length` world units between the fragment and the occluder, rays start `start` units away from
 * the fragment and end `height` units above it (or at the light); each pixel offsets the march's
 * points within their steps differently (interleaved gradient noise), so the shadow's edges dither
 * (averaged by supersampling) instead of showing one copy of the caster's outline per step. One
 * lamp per character, chosen at
 * the character (a character casts one lamp shadow, not one per light in reach): where a ray meets a
 * caster, the caster's middle (its stored top in the map, 0.9 units lower) rates every light above it
 * by its brightness there (weight x the lamp falloff of renderer_lamp_lights, k 4); lights below 0.95
 * of the brightest get no share, those above it share 1 (smoothstep from 0.95 to 1, normalised), and
 * the ray's occlusion is scaled by its light's share (rated once per ray, at the first caster it
 * meets). Lights below 0.15-0.3 of the brightest at the fragment are not marched (a cost gate). So all of a character's shadow comes from the
 * lamp nearest (brightest) at the character, whichever lamp is nearer each part of the ground, and
 * exactly between two equal lamps each casts at half strength (the crossfade). The fragment's vertex
 * diffuse colour v0 (before the combiners, after any world lighting) is multiplied by
 * 1 - strength x max(shadow weight x share x occlusion) over the lights, the darkening scaled
 * by dir_share where the title's own compare at the fragment already shadows it (blends with the
 * directional shadow). Shadow weight: the light's weight x (1 - smoothstep(radius / 2, radius,
 * distance)) x smoothstep(0.5, 1, height of the light above the fragment), so a light walking out
 * of range or fading out crossfades into the next one and a light on the ground (a light pool
 * sprite) casts nothing. Call once per frame before the
 * receivers (later calls are ignored); a frame without it, or with n = 0, draws them unshadowed by
 * lamps (exactly as without lamp shadows). */
#define RENDERER_LAMP_SHADOW_MAX 8
typedef struct {
    float stq[4];    /* the light in the depth stage's homogeneous coordinates (s, t, r, q) */
    float pos[3];    /* world position (world up is +z) */
    float radius;    /* world units (> 0) */
    float weight;    /* 0-1: the light's share (intensity x fade) */
} RLampShadowLight;
typedef struct {
    float to_world[4][4];      /* rows: homogeneous world (x, y, z, w) = to_world x (s, t, r, q) */
    float depth_a, depth_b;    /* the depth stage's light projection r = A q - B (A, B > 0) */
    float strength;            /* 0-1 */
    float height, length, start;   /* world units (see above) */
    float dir_share;           /* 0-1 */
    /* The low-light fade: the light's elevation above the horizon, in degrees, seen from the caster's
     * base (its position in the map at the receiver's height) scales its share by
     * smoothstep(min_angle, full_angle, elevation) and the shadow's length by
     * mix(low_length, 1, that); full_angle <= min_angle: no fade (every light at full share). */
    float min_angle, full_angle, low_length;
    /* The camera (world). A receiver face turned away from a light (seen from this side, its plane
     * puts the light behind it: a door or wall lit from the far side) gets no shadow from that
     * light, which does not light it; receivers whose face cannot be told (a clip-plane draw) keep
     * every light. */
    float eye[3];
    /* The title's own character shadow (the receivers' depth-stage compare): 0 = it fades out where
     * a lamp in reach lights the receiver (that lamp's shadow weight there, the low-light fade
     * included), so each character keeps one shadow, from the real light, and dark places keep the
     * title's; 1 = it stays everywhere (--title-shadow-full, the look before 2026-11-13). */
    uint32_t title_keep;
    /* 1 = the wall map does not stop these shadows (--lamp-shadows-walls 0 while the map is loaded
     * for the lights); 0 = it does when loaded. */
    uint32_t no_walls;
    uint32_t n;                /* lights in l */
    RLampShadowLight l[RENDERER_LAMP_SHADOW_MAX];
} RLampShadows;
void renderer_lamp_shadows(const RLampShadows *p);
/* Lamp shadows in use (configured and created), and the frames that had lights for them. */
int renderer_lamp_shadows_on(void);
/* The level's walls for the lamp shadows (built by src/materials.c mat_wall_map_build from the
 * level's collision): w x h texels, texel (x, y) covering world x0 +
 * [x, x + 1) cell, y0 + [y, y + 1) cell (z up); two 16-bit layers per texel, the lowest and the
 * highest point of the walls over it, 0 no wall, h in 1-65535 world z = z0 + (h - 1) zstep. With a
 * map, a receiver's march towards a light stops at the first step (from 1.5 cells away from the
 * receiver, past the receiver's own wall) inside a wall cell's height span: a character beyond a
 * wall or closed door casts no shadow through it, while the part of the ray already marched keeps
 * its occlusion (a wall behind the character changes nothing). Copied to the GPU (an R16G16_UINT
 * image at set 0, binding 8; the caller keeps its data); NULL or an empty map drops it (the shadows
 * exactly as without a map). The lights use it too (RLampLights.walls). Call between frames: it
 * waits for the GPU. Ignored without lamp shadows or light walls (RendererConfig.lamp_shadow,
 * light_walls). */
typedef struct {
    int w, h;
    float x0, y0, cell;
    float z0, zstep;
    const uint16_t *texels;   /* w x h x 2 */
} RLampWallMap;
void renderer_lamp_wall_map(const RLampWallMap *m);
/* A wall map is loaded. */
int renderer_lamp_wall_map_on(void);
/* Doors the title opens: copies cells [x0, x1) x [y0, y1) of m (the loaded
 * map's size, its texels changed there) to the loaded map, without a device-wide wait. Call between
 * frames. 0, or -1 without a loaded map of m's size or on a failed upload. */
int renderer_lamp_wall_map_region(const RLampWallMap *m, int x0, int y0, int x1, int y1);
uint32_t renderer_lamp_shadows_count(void);

/* World shadows (RendererConfig.world_shadow): the frame's opaque world casts shadows along a
 * directional light. In a frame with it, the frame-target draws flagged RENDERER_WORLD_SHADOW (and
 * not RENDERER_CHARACTER) are drawn once more, depth only, into a square depth map of
 * RendererConfig.world_shadow texels by clip_to_map (applied to each vertex's clip x, y, w and 1:
 * map NDC x, y and depth, 0 = nearest the light; cleared to 1, nothing there; depth clamped, so a
 * caster nearer the light than the map's top still casts; a slope-scaled depth bias), before the
 * frame's main pass. The receivers are the shadow receivers of renderer_lamp_shadows (frame-target
 * draws with a stage bound to a depth surface in PROJECT3D mode). Per fragment the clip x, y, w is
 * rebuilt from the window position (the viewport of the frame's first flagged draw) and 1 / w,
 * taken to the map (u, v = NDC / 2 + 1 / 2) and compared with its depth less `bias` (a texel's
 * neighbours too: 4 bilinear percentage-closer taps half a texel apart). Where the map blocks the
 * light, the title's own compare there says lit (>= 0.5) and the vertex diffuse alpha is below 0.5
 * (the title's rule: oD1.w = 1 on faces turned from its light, which it never shadows), the vertex
 * diffuse colour v0 (before the combiners, so the title's fog still applies) is multiplied by
 * 1 - darken x occlusion x fade, fade being: 1 up to fade_near world units from `eye`, 0 from
 * fade_far; 0 in the last tenth of the map towards its edges; smoothstep(baked_lo, baked_hi, the
 * luminance of v0); and for each lamp (world position, radius) 1 - lamp_keep x (1 - d^2 / r^2)^2
 * within its radius. Call once per frame before the receivers (later calls are ignored); a frame
 * without it draws no map and its receivers as without world shadows. Ignored while an offscreen
 * target is set. */
#define RENDERER_WORLD_SHADOW_LAMPS 8
typedef struct {
    float clip_to_map[3][4];     /* rows: (clip x, y, w, 1) -> map NDC x, y (-1..1) and depth (0-1) */
    float clip_to_world[3][4];   /* rows: (clip x, y, w, 1) -> world position */
    float eye[3];                /* camera position (world) */
    float fade_near, fade_far;   /* world units from the eye */
    float bias;                  /* map depth units */
    float darken;                /* 0-1 */
    float baked_lo, baked_hi;    /* luminance of v0 */
    float lamp_keep;             /* 0-1 */
    uint32_t lamps;              /* in lamp */
    float lamp[RENDERER_WORLD_SHADOW_LAMPS][4];   /* world x, y, z, radius (> 0) */
    float world_to_map[3][4];    /* rows: (world x, y, z, 1) -> map NDC x, y and depth (the indoor gate) */
    float ahead[2];              /* the camera's view direction along the ground (unit; the indoor gate) */
    float clearance;             /* map depth units: the indoor gate's cover test */
    float dt;                    /* seconds since the previous frame (the indoor gate's fade) */
} RWorldShadow;
void renderer_world_shadow(const RWorldShadow *p);
/* World shadows in use (configured and created), and the maps drawn in the last frame. */
int renderer_world_shadow_on(void);
uint32_t renderer_world_shadow_count(void);
/* The indoor gate (on the device, after each map): 32 points ahead of `eye` in the view (4, 7, 11
 * and 16 world units along `ahead`, 8 across within 0.6 x that distance either side, 1 unit below the
 * eye) taken to the map by world_to_map; the share of them with a surface more than `clearance`
 * nearer the light (most of the view under a roof's shadow) sets a level 1 - smoothstep(0.5, 0.75,
 * share) that darken is scaled by, reached over 0.6 s (dt) and at once on the first map after a
 * frame without one. The level and share of the latest completed map (0 before any; for --stats). */
float renderer_world_shadow_gate(float *share);

/* Soft particles (RendererConfig.soft), in order with the draws on the frame target: marks the end
 * of the opaque world (where renderer_ssao runs; they share one pass split when this follows it
 * directly). From here the frame's render pass keeps its depth attachment read-only
 * (DEPTH_STENCIL_READ_ONLY_OPTIMAL: tested, never written) and the draws flagged RENDERER_SOFT
 * after it read the depth in place (sample 0 with MSAA): each fragment's output is faded by
 * f = saturate((scene view depth - fragment view depth) / distance), view depth d = depth_b /
 * (depth_a - NDC depth); alpha only for SRC_ALPHA blends, colour and alpha for the others (ONE:
 * additive, premultiplied). Where nothing was drawn (depth 1) f = 1. Every later pass split
 * (bloom, glass, wet ground) resumes with the read-only depth. Draws flagged RENDERER_SOFT before
 * it, without depth test, with depth writes or offscreen are drawn as without the flag. A frame
 * that clears the depth or draws with depth writes on the frame target after it ignores it (and
 * every flag), so is one without it: recorded exactly as without soft particles. At most one per
 * frame (later ones are ignored). Ignored when soft is off or unavailable (the device cannot
 * sample the depth format at the sample count) and while an offscreen target is set. */
typedef struct {
    float depth_a, depth_b;   /* the scene's depth projection: NDC depth = depth_a - depth_b / view depth */
    float distance;           /* view units over which a particle fades in front of the scene (> 0) */
} RSoft;
void renderer_soft(const RSoft *p);
/* Soft particles in use (configured and the device runs them), and the soft draws executed by the
 * last frame. */
int renderer_soft_on(void);
uint32_t renderer_soft_count(void);

/* Executes the frame and reads the color target back (XRGB8888, alpha byte forced to 0; the
 * output size, see renderer_output_size). Returns 0 on success. */
int renderer_end(uint32_t *out, int pitch_bytes);
/* Where the last renderer_end spent its time (milliseconds): recording the command buffer (with
 * any pipeline variants created on the way), waiting for the device, and copying the readback
 * into the caller's frame. */
typedef struct {
    float record_ms, gpu_ms, readback_ms;
    float present_ms;   /* renderer_present: swapchain image acquire + queue present */
    /* Since the previous renderer_end / renderer_present: texture uploads (renderer_texture_create)
     * and pipelines compiled on demand; and the GPU timestamps of the latest completed frame
     * (RendererConfig.gpu_timestamps or fps_overlay_gpu; -1 when there is none). */
    float upload_ms, pipeline_ms, gpu_busy_ms;
} RendererTiming;

/* Direct present (when renderer_can_present): executes the frame and blits the target, scaled
 * with linear filtering into the largest rectangle of its aspect (black borders), to the window's
 * swapchain image and presents it; no readback. The swapchain follows the drawable size (resize,
 * fullscreen). Returns 0 when the frame was presented or skipped (minimized window); -1 when
 * presenting failed (renderer_error), after which renderer_can_present() is 0 and the window
 * surface is released, so the caller can fall back to renderer_end and its own present. */
int renderer_can_present(void);
int renderer_present(void);
/* Swapchain in use, e.g. "1920x1080 B8G8R8A8_UNORM mailbox, 3 images"; "" when offscreen. */
const char *renderer_present_info(void);
void renderer_last_timing(RendererTiming *t);
/* Pipeline cache (RendererConfig.pipeline_cache_dir): writes both files now if pipelines were
 * added since the last write (renderer_shutdown does too; also done every 32 new pipelines). */
void renderer_pipeline_cache_save(void);
/* The next pending "[pipeline] ..." log line (cache loaded or dropped, pre-warm finished, files
 * saved; with pipeline_stats per 60 s: pipelines compiled on demand, their total and longest
 * time, pre-warmed ones used, the longest renderer_end / renderer_present, and the same for the
 * whole run at renderer_pipeline_cache_save / renderer_shutdown). 0 when none. */
int renderer_pipeline_report(char *line, int size);
/* Geometry pipelines compiled on demand since renderer_init and pre-warmed ones used. */
void renderer_pipeline_counts(uint32_t *on_demand, uint32_t *prewarmed);

/* --gpu-skinning: draws whose vertices the device computes. renderer_draw_skinned is
 * renderer_draw for a draw whose nv vertices are the output of a vertex program run by the
 * frame's compute pre-pass (shaders/vsh_skin.comp) before any pass reads the frame's vertices, so
 * every pass that draws them (offscreen, rain cover, character mask, main) sees them as if they
 * had been passed in. job: the draw (src/gpu_skin.h GskJob; prog, consts, out_vertex and the
 * stream offsets are relative to the arguments and are rebased here); ins: job->nins instruction
 * words (gsk_pack_program); consts: 192 registers (gsk_pack_constants); data, data_bytes: the
 * stream bytes job->sbase points into. nrm: --char-lighting with the normals built on the device
 * from the drawn vertices (char_light_normals' rule; the weld tables of gsk_adjacency_build and the
 * camera scale), as renderer_draw_lit; NULL draws plain. Everything is copied. Ignored (nothing
 * drawn) unless renderer_skin_on(). Returns the draw's first vertex in the frame (renderer_debug_vertices;
 * normals at 3 x that in renderer_debug_normals when it is lit), or -1 when nothing was drawn. */
typedef struct {
    uint32_t prog, nins, nv, out_vertex, consts, fed, nel, pad;
    uint32_t sbase[4], stride[4];
    uint32_t el[16];      /* reg | stream << 4 | type << 8 | byte offset << 16 */
    uint32_t fog_mode;    /* 0xFFFFFFFF: no fog, else GEOM_FOG_* */
    float fog_start, fog_end, fog_density;
    float tscale[8];
} RSkinJob;
typedef struct {
    const uint32_t *group, *start, *corner;   /* nv, ng + 1 and ncorner words */
    uint32_t ng, ncorner;
    float px, py;
} RSkinNormals;
int renderer_skin_on(void);
int32_t renderer_draw_skinned(const RViewport *vp, uint32_t flags, const RSkinJob *job, const uint32_t (*ins)[4], const float (*consts)[4],
                              const uint8_t *data, uint32_t data_bytes, const uint32_t *idx, uint32_t ni, const RSkinNormals *nrm);
/* --world-lighting with --gpu-vertex-programs: renderer_draw_skinned (no device normals) drawn as
 * renderer_draw_world_lit, with normals (3 floats per vertex: the draw's unit view-space normals,
 * world_light.h world_light_record, computed on the host from the raw inputs) and the light mask
 * lights. The same conditions as renderer_draw_world_lit apply (lights 0, no frame lights, an
 * offscreen target: drawn as renderer_draw_skinned). Returns as renderer_draw_skinned. */
int32_t renderer_draw_skinned_world_lit(const RViewport *vp, uint32_t flags, const RSkinJob *job, const uint32_t (*ins)[4], const float (*consts)[4],
                                        const uint8_t *data, uint32_t data_bytes, const uint32_t *idx, uint32_t ni, const float *normals,
                                        uint32_t lights);
/* --taa: the identity (non-zero, stable across frames) of the next renderer_draw_skinned* draw. With
 * TAA in use, a draw whose identity was drawn last frame (the n-th draw of an identity matches last
 * frame's n-th) is run a second time with last frame's constants (its previous pose and camera),
 * and its motion is drawn into the TAA velocity target for the resolve (renderer_taa). Ignored
 * otherwise; cleared by the draw. */
void renderer_set_draw_identity(uint64_t id);
/* The device time of the last completed frame's pre-pass (ms; 0 without timestamps) and its draw
 * and vertex counts. */
void renderer_skin_stats(float *gpu_ms, uint32_t *draws, uint32_t *vertices);
/* RendererConfig.pass_stats: the average GPU time per completed frame of each pass since the
 * last call for the same window (0: the periodic --stats line, 1: the level summary), as text
 * ("total T ms: scene A, shadows B, ...", largest first; passes under 0.005 ms left out), and
 * that window reset. Returns the number of frames averaged (0: nothing written to buf). */
uint32_t renderer_gpu_passes(int window, char *buf, size_t len);
/* --tri-check: a GPU-skinned draw of a completed frame (renderer_draw_skinned): its nv vertices
 * from vertex first, ni indices (relative to first) from index first_index, and the tag set by
 * renderer_set_draw_tag before it was drawn. */
typedef struct {
    uint32_t first, nv, first_index, ni, tag;
} RVertexCheckDraw;
/* Called on the renderer's thread once a frame's fence has been waited (renderer_begin frames in
 * flight later, or renderer_end on the readback path), with the vertices and indices as the device
 * left them; NULL (the default) turns it off and records nothing. */
typedef void (*RVertexCheck)(const RVertex *verts, const uint32_t *indices, const RVertexCheckDraw *draws, uint32_t n);
void renderer_set_vertex_check(RVertexCheck check);
void renderer_set_draw_tag(uint32_t tag);
/* Whether the frame being recorded collects its GPU draws for the vertex check (default on): the
 * check reads mapped, often uncached device memory, so callers sample frames. */
void renderer_set_vertex_check_frame(int on);
/* render64: with pipeline_async, the last renderer_end/present's pipeline
 * decisions that missed (a draw drawn with its base variant or left out because its pipeline was still
 * compiling), as decision numbers in that frame's order: at most cap of them into out; returns their
 * count. renderer_pipeline_plan(misses, n): the next renderer_end/present takes exactly those
 * decisions as misses and builds every other pipeline before drawing (the replay of a recording made
 * with pipeline_async draws what the recording drew). */
uint32_t renderer_pipeline_misses(uint32_t *out, uint32_t cap);
void renderer_pipeline_plan(const uint32_t *misses, uint32_t n);
/* Debug readback (XB_GSK_CHECK): after renderer_end, the frame's vertices and normals as the device
 * left them (NULL when unavailable); valid until the next renderer_begin. */
const RVertex *renderer_debug_vertices(void);
const float *renderer_debug_normals(void);

#ifdef __cplusplus
}
#endif
#endif
