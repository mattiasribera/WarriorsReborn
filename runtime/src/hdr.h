/* HDR10 output and the saturation of the final grade.
 *
 * --hdr off|auto|on (default off): with auto or on the swapchain takes an HDR10 surface format (PQ,
 * VK_COLOR_SPACE_HDR10_ST2084_EXT, BT.2020 primaries) when the display and driver offer one; else it
 * stays SDR and the log says why. The frame itself stays the 8-bit, display-encoded (gamma 2.2) SDR
 * picture the post chain makes; the HDR output pass (shaders/output.frag, HDR variant) maps it to
 * nits at the swapchain blit:
 *   - the HUD, text, menus, cutscene bars, fades over everything and frames without a 3D scene (Bink
 *     movies, menus) at paper white: linear value x --hdr-paper-white nits;
 *   - the 3D scene (what did not change between the scene's end and the HUD's end, the frame
 *     generation's HUD rule) the same, with its highlights expanded towards --hdr-peak nits by
 *     hdr_expand: values above the knee (the part filmic tonemapping and the 8-bit target compress)
 *     rise smoothly to the peak, light sources (the bloom light mask: neon, lamps, fire, glows)
 *     fully, the rest of the scene partly;
 *   - BT.709 to BT.2020, then the SMPTE ST 2084 (PQ) encoding.
 * --saturation F (0.5-2, default 1 = unchanged): chroma around the BT.709 luminance in linear light,
 * in the final grade: in the HDR pass, or in SDR a pass after the gamma ramp (only when F != 1, so the
 * default frame is exactly as without it).
 *
 * Everything here is the CPU reference of the shader (the tests check one against the other) plus the
 * surface format choice, without Vulkan headers (the values are Vulkan's). */
#ifndef HDR_H
#define HDR_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { HDR_OFF, HDR_AUTO, HDR_ON };
#define HDR_PEAK_DEFAULT 1000.0f   /* nits */
#define HDR_PEAK_MIN 400.0f
#define HDR_PEAK_MAX 10000.0f
#define HDR_PAPER_DEFAULT 200.0f   /* nits: Windows' SDR content brightness default is about 200 */
#define HDR_PAPER_MIN 80.0f
#define HDR_PAPER_MAX 500.0f
#define HDR_SAT_DEFAULT 1.0f
#define HDR_SAT_MIN 0.5f
#define HDR_SAT_MAX 2.0f
/* hdr_expand: the knee (linear) and the weight of scene pixels outside the light mask. */
#define HDR_KNEE 0.6f
#define HDR_SCENE_WEIGHT 0.4f

typedef struct {
    int mode;          /* --hdr: HDR_OFF / HDR_AUTO / HDR_ON */
    float peak;        /* --hdr-peak NITS (400-10000) */
    float paper;       /* --hdr-paper-white NITS (80-500) */
    float saturation;  /* --saturation F (0.5-2) */
} HdrConfig;
#define HDR_DEFAULTS {HDR_OFF, HDR_PEAK_DEFAULT, HDR_PAPER_DEFAULT, HDR_SAT_DEFAULT}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (2), 0 when opt
 * is not one of these, -1 for a missing or invalid value (c unchanged). */
int hdr_arg(HdrConfig *c, const char *opt, const char *val);

/* SMPTE ST 2084: nits (0-10000) to the PQ signal 0-1, and back. */
float hdr_pq_encode(float nits);
float hdr_pq_decode(float v);
/* Linear BT.709 to linear BT.2020 (ITU-R BT.2087). */
void hdr_709_to_2020(const float in[3], float out[3]);
float hdr_luma(const float lin[3]);   /* BT.709 luminance of linear RGB */
/* Saturation in linear light: luma + sat x (c - luma), channels kept >= 0. sat 1 returns c exactly. */
void hdr_saturate(const float lin[3], float sat, float out[3]);
/* The highlight expansion of a linear luminance y (0-1, 1 = paper white): y below HDR_KNEE as is,
 * above it y + w x (ratio - 1) x t^3 with t = (y - knee) / (1 - knee): continuous with a continuous
 * slope at the knee, 1 + w x (ratio - 1) at y = 1 (ratio = peak / paper white; w 0-1). */
float hdr_expand(float y, float ratio, float w);

typedef struct {
    float paper, peak, saturation;
} HdrLevels;
/* The HDR output of one display-encoded pixel (0-1, gamma 2.2): scene 1 for the 3D scene (0: HUD,
 * paper white), light 0-1 its bloom light mask. Writes the PQ signal (BT.2020) and returns the
 * output's largest channel in nits (before PQ). */
float hdr_pixel(const HdrLevels *l, const float display[3], float scene, float light, float pq[3]);
/* The SDR grade of one display-encoded pixel: saturation in linear light, back to gamma 2.2. */
void hdr_sdr_grade(float saturation, const float display[3], float out[3]);

/* ---- the swapchain surface format ---- */
#define HDR_VK_FORMAT_R8G8B8A8_UNORM 37
#define HDR_VK_FORMAT_R8G8B8A8_SRGB 43
#define HDR_VK_FORMAT_B8G8R8A8_UNORM 44
#define HDR_VK_FORMAT_B8G8R8A8_SRGB 50
#define HDR_VK_FORMAT_A2R10G10B10_UNORM 58
#define HDR_VK_FORMAT_A2B10G10R10_UNORM 64
#define HDR_VK_FORMAT_R16G16B16A16_SFLOAT 97
#define HDR_VK_COLOR_SPACE_SRGB_NONLINEAR 0
#define HDR_VK_COLOR_SPACE_HDR10_ST2084 1000104008u
typedef struct {
    uint32_t format, color_space;
    int blit_dst;   /* the format can be a blit destination (VK_FORMAT_FEATURE_BLIT_DST_BIT) */
} HdrSurfaceFormat;
/* off / auto / on (header-only, like the choice below: the renderer links neither hdr.c nor libm for them). */
static inline const char *hdr_mode_name(int mode) { return mode == HDR_ON ? "on" : mode == HDR_AUTO ? "auto" : "off"; }
static inline int hdr_is_hdr_format(uint32_t f) {
    return f == HDR_VK_FORMAT_A2B10G10R10_UNORM || f == HDR_VK_FORMAT_A2R10G10B10_UNORM || f == HDR_VK_FORMAT_R16G16B16A16_SFLOAT;
}

/* Picks the surface format: with mode auto / on, colorspace_ext (VK_EXT_swapchain_colorspace enabled)
 * and an HDR10 ST 2084 entry of A2B10G10R10, A2R10G10B10 or R16G16B16A16_SFLOAT that can be blitted
 * to, that one (*hdr = 1); else the SDR choice as before: UNORM first (B8G8R8A8, R8G8B8A8,
 * A2B10G10R10, then the sRGB ones), the sRGB non-linear colour space preferred over other colour
 * spaces of the same format (*hdr = 0). why gets the reason for an SDR choice under auto / on ("" for
 * HDR or off). Returns the index, -1 when nothing can be blitted to. */
static inline int hdr_pick_format(const HdrSurfaceFormat *f, int n, int mode, int colorspace_ext, int *hdr, char *why, size_t whyn) {
    *hdr = 0;
    if (why && whyn) why[0] = 0;
    if (mode != HDR_OFF) {
        int offered = 0, unblittable = 0;
        const uint32_t hdr_prefs[] = {HDR_VK_FORMAT_A2B10G10R10_UNORM, HDR_VK_FORMAT_A2R10G10B10_UNORM, HDR_VK_FORMAT_R16G16B16A16_SFLOAT};
        for (int i = 0; i < n; i++)
            if (f[i].color_space == HDR_VK_COLOR_SPACE_HDR10_ST2084) {
                offered++;
                unblittable += hdr_is_hdr_format(f[i].format) && !f[i].blit_dst;
            }
        for (int p = 0; colorspace_ext && p < (int)(sizeof hdr_prefs / sizeof *hdr_prefs); p++)
            for (int i = 0; i < n; i++)
                if (f[i].format == hdr_prefs[p] && f[i].color_space == HDR_VK_COLOR_SPACE_HDR10_ST2084 && f[i].blit_dst) {
                    *hdr = 1;
                    return i;
                }
        if (why && whyn) {
            if (!colorspace_ext)
                snprintf(why, whyn, "the Vulkan loader or driver lacks VK_EXT_swapchain_colorspace");
            else if (unblittable)
                snprintf(why, whyn, "the display's HDR10 formats cannot be blit targets");
            else if (offered)
                snprintf(why, whyn, "the display's HDR10 colour space comes with no 10-bit or FP16 format");
            else
                snprintf(why, whyn, "the display offers no HDR10 (ST 2084) format: HDR off in the system settings, or an SDR display");
        }
    }
    const uint32_t prefs[] = {HDR_VK_FORMAT_B8G8R8A8_UNORM, HDR_VK_FORMAT_R8G8B8A8_UNORM, HDR_VK_FORMAT_A2B10G10R10_UNORM,
                                     HDR_VK_FORMAT_B8G8R8A8_SRGB, HDR_VK_FORMAT_R8G8B8A8_SRGB};
    for (int p = 0; p < (int)(sizeof prefs / sizeof *prefs); p++) {
        int any = -1;
        for (int i = 0; i < n; i++)
            if (f[i].format == prefs[p] && f[i].blit_dst) {
                if (f[i].color_space == HDR_VK_COLOR_SPACE_SRGB_NONLINEAR) return i;
                if (any < 0) any = i;
            }
        if (any >= 0) return any;
    }
    return -1;
}

#ifdef __cplusplus
}
#endif
#endif
