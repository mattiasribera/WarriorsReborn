/* Game-independent geometry helpers for the host renderer: Xbox vertex declarations and FVF codes
 * to element layouts, vertex element decoding (incl. the Xbox NORMPACKED3 and SHORT types),
 * D3D row-vector matrix math, vertex-shader-constant transforms, primitive-to-triangle-list
 * expansion and the debug color used while textures and shaders are not rendered. */
#ifndef GEOM_H
#define GEOM_H
#include <stdint.h>

/* Xbox D3DVSDT_* element types (d3d8types.h of the XDK). */
enum {
    GEOM_FLOAT1 = 0x12, GEOM_FLOAT2 = 0x22, GEOM_FLOAT3 = 0x32, GEOM_FLOAT4 = 0x42,
    GEOM_D3DCOLOR = 0x40,
    GEOM_SHORT1 = 0x15, GEOM_SHORT2 = 0x25, GEOM_SHORT3 = 0x35, GEOM_SHORT4 = 0x45,
    GEOM_NORMSHORT1 = 0x11, GEOM_NORMSHORT2 = 0x21, GEOM_NORMSHORT3 = 0x31, GEOM_NORMSHORT4 = 0x41,
    GEOM_NORMPACKED3 = 0x16,
    GEOM_PBYTE1 = 0x14, GEOM_PBYTE2 = 0x24, GEOM_PBYTE3 = 0x34, GEOM_PBYTE4 = 0x44,
    GEOM_FLOAT2H = 0x72, GEOM_NONE = 0x02,
};

/* Xbox fixed-function input registers of FVF vertices. */
enum { GEOM_REG_POSITION = 0, GEOM_REG_WEIGHT = 1, GEOM_REG_NORMAL = 2, GEOM_REG_DIFFUSE = 3, GEOM_REG_SPECULAR = 4,
       GEOM_REG_TEX0 = 9 };

#define GEOM_MAX_ELEMENTS 16
typedef struct {
    uint8_t reg, stream, type;
    uint16_t offset;   /* bytes from the start of the vertex in its stream */
} GeomElement;
typedef struct {
    GeomElement e[GEOM_MAX_ELEMENTS];
    int n;
    uint16_t size[4];  /* bytes the declaration covers per stream (stride is set separately) */
} GeomLayout;

int geom_type_size(uint32_t type);   /* bytes, -1 for an unknown type */
/* Parses an Xbox vertex shader declaration (STREAM / REG / SKIP / SKIPBYTES / NOP, CONSTMEM and
 * TESSELLATOR skipped, END = 0xFFFFFFFF). Returns 0, or -1 for an unknown type or no END within
 * max_tokens. */
int geom_layout_from_decl(const uint32_t *tokens, int max_tokens, GeomLayout *l);
/* FVF -> layout in stream 0 with the fixed-function registers above; returns the vertex size in
 * bytes, or -1 for a position type it does not know. *rhw is set for D3DFVF_XYZRHW. */
int geom_layout_from_fvf(uint32_t fvf, GeomLayout *l, int *rhw);
const GeomElement *geom_find(const GeomLayout *l, int reg);
/* Decodes one element; missing components default to (0, 0, 0, 1). */
void geom_fetch(uint32_t type, const uint8_t *p, float out[4]);
/* geom_fetch for n vertices stride bytes apart, into rows (component q of vertex j at
 * rows[q * row_stride + j]); the same values bit for bit, with the type decoded once. */
void geom_fetch_rows(uint32_t type, const uint8_t *p, uint32_t stride, int n, float *rows, int row_stride);

/* Row-major 4x4 matrices (D3DMATRIX _11.._44), row vectors: out = v * m. */
void geom_mat_mul(const float a[16], const float b[16], float out[16]);
void geom_row_xform(const float v[4], const float m[16], float out[4]);
/* out[i] = dot(v, rows[i]) for four constant registers (vertex shader dp4 with c[n..n+3]). */
void geom_dp4_rows(const float v[4], const float rows[16], float out[4]);
/* Three-bone skinning as in the title's vsCharacter program: bone k contributes weight w[k] times
 * the 3x4 rows c[idx[k] + base .. + 2] (c = constant registers, index 0 = c-96); out.w = 1. */
void geom_skin3(const float pos[4], const float idx[4], const float w[4], const float (*c)[4], int base, float out[4]);
/* Screen-space (XYZRHW) vertex to D3D clip space for a viewport (y up in clip space). */
void geom_screen_to_clip(const float s[4], float vx, float vy, float vw, float vh, float out[4]);

/* Expands count vertices of a D3DPRIMITIVETYPE (Xbox numbering: 5 triangle list, 6 strip, 7 fan,
 * 8 quad list, 9 quad strip, 10 polygon) into a triangle list of positions 0..count-1 in out
 * (room for max entries). Returns the number written; points and lines give 0. */
uint32_t geom_triangulate(uint32_t prim, uint32_t count, uint32_t *out, uint32_t max);
/* Worst case number of triangle-list entries for count vertices. */
uint32_t geom_triangulated_max(uint32_t count);

/* Fixed-function vertex lighting of FVF draws (D3DRS_LIGHTING), in world space. GeomLight has the
 * layout of D3DLIGHT8 (104 bytes) and GeomMaterial of D3DMATERIAL8, so the title's structures copy
 * as-is. Material color sources (D3DRS_*MATERIALSOURCE): 0 material, 1 vertex diffuse, 2 vertex
 * specular, used only with color_vertex (D3DRS_COLORVERTEX) and when the vertex has that color. */
enum { GEOM_LIGHT_POINT = 1, GEOM_LIGHT_SPOT = 2, GEOM_LIGHT_DIRECTIONAL = 3 };
typedef struct {
    uint32_t type;
    float diffuse[4], specular[4], ambient[4];
    float pos[3], dir[3];
    float range, falloff, att[3], theta, phi;
} GeomLight;
typedef struct {
    float diffuse[4], ambient[4], specular[4], emissive[4];
    float power;
} GeomMaterial;
#define GEOM_MAX_LIGHTS 8
typedef struct {
    GeomLight light[GEOM_MAX_LIGHTS];   /* the enabled lights */
    int n;
    GeomMaterial mat;
    float ambient[4];                   /* D3DRS_AMBIENT */
    uint32_t dsrc, asrc, ssrc, esrc;    /* diffuse, ambient, specular, emissive material sources */
    int color_vertex, specular_enable, local_viewer;
    float eye[3];                       /* camera position (local viewer) */
    float view_dir[3];                  /* direction toward the viewer otherwise */
} GeomLighting;
/* pos and normal in world space (normal NULL = none: no diffuse or specular term); vd / vs the
 * vertex diffuse / specular colors (r, g, b, a in 0..1) or NULL. Writes the lit diffuse (alpha =
 * diffuse material alpha) and specular (alpha 0) colors, saturated to [0, 1]. */
void geom_light(const GeomLighting *L, const float pos[3], const float *normal, const float *vd, const float *vs, float d[4], float s[4]);

/* Fog factor (1 = no fog) for a distance d under D3DRS_FOGTABLEMODE (D3DFOG_NONE 0 = vertex fog:
 * d is the factor itself), D3DRS_FOGSTART/FOGEND (linear) and D3DRS_FOGDENSITY (exp, exp2);
 * clamped to [0, 1]. */
enum { GEOM_FOG_NONE = 0, GEOM_FOG_EXP = 1, GEOM_FOG_EXP2 = 2, GEOM_FOG_LINEAR = 3 };
float geom_fog_factor(uint32_t mode, float start, float end, float density, float d);

/* Debug color (XRGB8888): normal mapped to RGB with a fixed light, or a hash of the position
 * when there is no normal. */
uint32_t geom_debug_color(const float *normal, const float pos[4]);
#endif
