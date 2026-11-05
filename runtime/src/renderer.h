/* Host renderer behind a C ABI (implementation: src/renderer/vk_renderer.cpp, Vulkan, C++).
 * Offscreen: one color target (B8G8R8A8, the XRGB8888 byte order Swap presents) and one depth
 * target at the back-buffer size; each frame is recorded between renderer_begin and renderer_end
 * and read back into a caller buffer, so the existing present path (video.h, --dump-frame, the
 * null backend) is unchanged. Not thread-safe: all calls must come from one host thread (the
 * runtime's main thread, see hle_d3d_service). */
#ifndef RENDERER_H
#define RENDERER_H
#include <stdint.h>
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

/* Texture formats: B8G8R8A8 texels (uint32_t 0xAARRGGBB), BC1/2/3 blocks (DXT1/3/5, row-major
 * blocks) when renderer_has_bc() says the device samples them, or 8-bit luminance (one byte per
 * texel, sampled as (L, L, L, 1) like the CPU decode of L8; the --movie-hd planes). */
enum { RENDERER_TEX_BGRA8, RENDERER_TEX_BC1, RENDERER_TEX_BC2, RENDERER_TEX_BC3, RENDERER_TEX_L8 };

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
    /* Screen-space ambient occlusion of the opaque 3D world (renderer_ssao): ssao_strength (> 0)
     * scales how much an occluded pixel darkens, ssao_radius (> 0, view units) is how far around a
     * point the occluders are looked for. 0 = off: the depth target, the frame and every pipeline
     * are exactly as without it. */
    int ssao;
    float ssao_strength, ssao_radius;
    /* Lamp lights (renderer_lamp_lights). 0 = off: no resource or pipeline is created, the
     * targets are as without it and renderer_lamp_lights is ignored. */
    int lamp;
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
    /* Procedural sky and skyline (renderer_sky, renderer_skyline). 0 = off: no pipeline is created
     * and both are ignored, so every frame is exactly as without it. */
    int sky;
} RendererConfig;
enum { RENDERER_PRESENT_AUTO, RENDERER_PRESENT_FIFO, RENDERER_PRESENT_MAILBOX, RENDERER_PRESENT_IMMEDIATE };
void renderer_configure(const RendererConfig *c);
/* Target size (what renderer_end writes; the supersampled internal size divided by the factor),
 * samples per pixel, frames in flight and anisotropy actually in use. */
void renderer_output_size(int *w, int *h);
int renderer_samples(void);
int renderer_frames_in_flight(void);
/* Supersampling factor in use (1 when off; see RendererConfig.supersample). */
int renderer_supersample(void);
/* Sharpening in use (0-100, 0 when off or unavailable; see RendererConfig.sharpen). */
int renderer_sharpen(void);
float renderer_anisotropy(void);

/* Creates the instance, device and targets (w x h: the back-buffer size the caller records in)
 * on the first suitable GPU. Returns 0 on success; otherwise -1 and renderer_error() says why
 * (no loader, no device, ...). */
int renderer_init(int w, int h);
const char *renderer_error(void);
const char *renderer_device_name(void);
void renderer_shutdown(void);

/* Starts a frame. base (XRGB8888 rows of pitch_bytes) is the initial color, NULL = black; depth
 * starts at 1.0. */
void renderer_begin(const uint32_t *base, int pitch_bytes);
/* Whole-target clear of color (XRGB8888) and/or depth, in order with the draws. */
void renderer_clear(uint32_t flags, uint32_t color, float z);
/* Indexed triangle list (indices into v). With RENDERER_DEPTH_TEST: depth test and write as the
 * current RState says (default less-or-equal with writes). Depth is clamped to the viewport range
 * instead of clipped at the near/far planes (when the device supports it); the color comes from
 * the current combiner program (renderer_set_pixel_shader) over the stage textures and the vertex
 * colors, then the alpha test, blended per the current RState. */
void renderer_draw(const RViewport *vp, uint32_t flags, const RVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni);
/* Texture cache. Textures are immutable and keyed by the caller (see xtex_key); a handle stays
 * valid until the next renderer_begin, which may drop the whole cache when it grows too large, so
 * callers look handles up again every frame. 0 = no texture. */
int renderer_has_bc(void);
/* Device memory the cache may hold before renderer_begin drops it (0 = the default, 512 MiB). */
void renderer_set_texture_budget(uint64_t bytes);
uint32_t renderer_texture_find(uint64_t key);
/* levels mip levels, each w>>l x h>>l (at least 1), stored one after the other without padding.
 * Returns the handle, or 0 on failure (unsupported format, out of memory). */
uint32_t renderer_texture_create(uint64_t key, uint32_t format, uint32_t w, uint32_t h, uint32_t levels, const void *data);
/* Movie planes (--movie-hd): plane 0-2 of the frame being recorded (after renderer_begin), w x h
 * 8-bit luminance, tightly packed. Kept outside the texture cache: one persistent image per plane,
 * made again only when its size changes, rewritten by a copy at the start of the frame (no
 * synchronous upload). Sampled as (L, L, L, 1), bilinear, clamped at the edges. Returns the
 * stage-texture handle, or 0 on failure. */
uint32_t renderer_movie_plane(int plane, uint32_t w, uint32_t h, const void *data);
/* Planes copied by renderer_movie_plane so far. */
uint32_t renderer_movie_upload_count(void);
/* Times renderer_begin dropped the whole texture cache (budget, handle limit or a failed create). */
uint32_t renderer_texture_drop_count(void);
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
 * it is redrawn in the same frame sees the new content, not the old). The handle is a texture
 * handle too (renderer_set_stage_texture); depth surfaces sample with the shadow compare of
 * RPixelShader.shadow_func. Returns 0 on failure (renderer_error). */
enum { RENDERER_SURFACE_COLOR, RENDERER_SURFACE_DEPTH16, RENDERER_SURFACE_DEPTH24 };
uint32_t renderer_surface(uint64_t key, uint32_t kind, int w, int h);
/* Actual size of a surface (0 x 0 for an unknown handle). */
void renderer_surface_size(uint32_t surface, int *w, int *h);
uint32_t renderer_surface_count(void);
/* Target of the following clears and draws: a color and/or a depth surface (either may be 0;
 * both 0 = the frame target). Reset to the frame target by renderer_begin. */
void renderer_set_target(uint32_t color, uint32_t depth);

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

/* Filmic look (RendererConfig.filmic), in order with the draws on the frame target: marks the end
 * of the 3D scene (after the bloom split, the halos, rain and wet ground; before the HUD). At this
 * point the render pass is split (MSAA resolved first), the scene is copied and every pixel of the
 * target is replaced by its filmic version (RFilmic, shaders/filmic.frag): pseudo-HDR from the
 * light mask in the target's alpha, tonemap, halation from the bloom glow (frames with a bloom
 * split), the LUT grade, vignette and grain. The alpha (light mask) is kept. At most one per
 * frame; ignored when the look is off or unavailable, and while an offscreen target is set. */
void renderer_filmic(void);
/* The look in use (configured and the device runs it), and the passes executed by the last frame. */
int renderer_filmic_on(void);
uint32_t renderer_filmic_count(void);

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
} RRainWet;
void renderer_rain_wet(const RRainWet *p);
/* Wet ground in use (rain and rain_wet configured, the device samples the depth), and that of the last frame. */
int renderer_rain_wet_on(void);
uint32_t renderer_rain_wet_count(void);

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

/* Character mask (with rain; the splashes and the wet ground need it). In a frame with
 * renderer_rain_splash or renderer_rain_wet, the frame-target draws flagged RENDERER_CHARACTER
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
    float char_gain;          /* characters' share (>= 1: they gain the most) */
    uint32_t n;               /* lights in l */
    RLamp l[RENDERER_LAMP_MAX];
} RLampLights;
void renderer_lamp_lights(const RLampLights *p);
/* Lamp lights in use (configured and the device runs them), and the passes executed by the last frame. */
int renderer_lamp_lights_on(void);
uint32_t renderer_lamp_lights_count(void);

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

#ifdef __cplusplus
}
#endif
#endif
