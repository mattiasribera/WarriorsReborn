/* --filmic: the options, the tonemap and grade math of the filmic look
 * of the 3D scene, the procedural 70s-film 3D LUT and the .cube parser. The renderer runs the
 * same tonemap in shaders/filmic.frag from the parameters filmic_params computes (RFilmic). */
#ifndef FILMIC_H
#define FILMIC_H
#include "renderer.h"
#include <stddef.h>
#include <stdint.h>

#define FILMIC_STRENGTH_DEFAULT 1.0f
#define FILMIC_GRAIN_DEFAULT 0.0f      /* off: --filmic-grain turns it on */
#define FILMIC_VIGNETTE_DEFAULT 0.25f  /* corners about 6% darker */
#define FILMIC_LUT_SIZE 32             /* the procedural LUT */
typedef struct {
    int on;               /* --filmic / --no-filmic */
    float strength;       /* --filmic-strength 0-2: 0 = the scene as without it */
    float grain;          /* --filmic-grain 0-1 */
    float vignette;       /* --filmic-vignette 0-1 */
    const char *lut_path; /* --filmic-lut FILE.cube: replaces the procedural grade */
} FilmicConfig;
#define FILMIC_DEFAULTS {0, FILMIC_STRENGTH_DEFAULT, FILMIC_GRAIN_DEFAULT, FILMIC_VIGNETTE_DEFAULT, NULL}

/* Parses one option (val: the next argument or NULL). Returns the arguments consumed (1 or 2), 0
 * when opt is not a filmic option, -1 for a missing or out-of-range value (c unchanged). */
int filmic_arg(FilmicConfig *c, const char *opt, const char *val);

/* The shader parameters for strength (0-2), grain and vignette (0-1); lut / lut_size are left
 * NULL / 0. Strength scales the pseudo-HDR range (hdr = strength, white = 1 + hdr), the tonemap
 * blend (0.6 x strength, at most 1), the LUT mix (at most 1) and the halation; exposure is solved
 * so linear mid-grey 0.18 maps to itself. */
void filmic_params(float strength, float grain, float vignette, RFilmic *p);

/* Krzysztof Narkowicz's ACES filmic fit, x >= 0. */
float filmic_aces(float x);
/* The tonemap of a linear value (0..white) to linear 0..1 (shaders/filmic.frag: tonemap). */
float filmic_tonemap(const RFilmic *p, float lin);
/* The tonemap of a display-encoded value (0..1, ^2.2 to linear) with light mask 0..1: the
 * pseudo-HDR expansion, the tonemap, back to display values. */
float filmic_display(const RFilmic *p, float v, float mask);

/* The procedural grade of a display-encoded colour (0..1 per channel): a crushed-black toe, teal
 * shadows, warm highlights, slightly less saturation. Mid-grey stays close to itself. */
void filmic_grade(const float in[3], float out[3]);
/* The n^3 grade LUT (RGBA8, red fastest; n 2-64) of filmic_grade. */
void filmic_lut_build(int n, uint8_t *rgba);

/* Parses Adobe/Resolve .cube text (LUT_3D_SIZE 2-64, default domain only, TITLE and # comments
 * ignored). Returns a malloc'd n^3 RGBA8 LUT and sets *n, or NULL with a reason in err. */
uint8_t *filmic_cube_parse(const char *text, int *n, char *err, size_t errn);
/* Reads and parses a .cube file (at most 16 MB). */
uint8_t *filmic_cube_load(const char *path, int *n, char *err, size_t errn);
#endif
