/* --hat-fix, --hat-subdiv: the characters' hats lit smoothly and rounder.
 *
 * The title draws hats (top hats, caps, helmets, berets; also the hairpieces it attaches the same
 * way) as separate rigid objects after the characters, with its object vertex program
 * (vsObjectShadow*): every object is a receiver of the title's coarse character shadow map and of a
 * per-vertex sun switch (oD1.w = n . sun >= 0), so a hat on its wearer's head samples the wearer's
 * own shadow and shows lit and shadowed blocks ("squares"), and a low-poly hat shows its facets.
 *
 * A hat draw is an object-program draw of a small mesh (hat_is_small) whose world origin lies near
 * the head of a character drawn earlier in the same frame (hat_near_head). With --hat-fix it is no
 * longer a receiver of the title's character shadow (as the head under it, which never is), its
 * normals are smoothed within an angle (hat_smooth_normals: facets merge, brims and rims stay
 * sharp) and, with --char-lighting, it is lit per pixel like the characters. On by default in
 * every configuration (the false shadow is a bug with or without --char-lighting). With --hat-subdiv
 * its mesh is also subdivided into curved PN triangles (hat_subdivide: each triangle into
 * HAT_SUBDIV_LEVEL^2), so crowns and brims are round. Both are computed once per mesh.
 * Game-independent: tested by tests/hats_test.c. */
#ifndef HATS_H
#define HATS_H
#include <stdint.h>

typedef struct {
    int fix;      /* --hat-fix / --no-hat-fix; -1 (default): on */
    int subdiv;   /* --hat-subdiv / --no-hat-subdiv; -1 (default): on with the fix */
} HatConfig;
#define HAT_DEFAULTS {-1, -1}

/* Parses one option. Returns 1 when consumed, 0 when opt is not a hat option. */
int hat_arg(HatConfig *c, const char *opt);
/* The effective switches: the fix is on unless --no-hat-fix (every preset, with or without
 * --char-lighting), the subdivision follows it unless set. */
int hat_fix_on(const HatConfig *c);
int hat_subdiv_on(const HatConfig *c);

/* Detection. A hat mesh's object-space box diagonal is below HAT_MAX_DIAG (a top hat: 0.34 m), its
 * origin within HAT_HEAD_REACH of a head (the centre of the head joint's vertices; a
 * Hi-Hat's top hat: 0.12 m). */
#define HAT_MAX_DIAG 0.7f
#define HAT_HEAD_REACH 0.25f
#define HAT_MAX_VERTS 2048
int hat_is_small(const float box_min[3], const float box_max[3]);
/* 1 when p lies within reach of one of the n points of heads. */
int hat_near_head(const float p[3], const float (*heads)[3], int n, float reach);

/* A hat mesh: nv interleaved vertices of stride bytes, an indexed triangle list. Position and
 * normal are 3 floats at pos_off and nrm_off; the attributes (texture coordinates, colours) are
 * interpolated by hat_subdivide; bytes outside every element are copied from the nearest corner. */
enum { HAT_ATTR_FLOAT = 0, HAT_ATTR_BYTES = 1 };   /* n floats; n unsigned bytes (a D3DCOLOR: 4) */
#define HAT_MAX_ATTRS 12
typedef struct {
    uint32_t off;
    uint8_t kind, n;
} HatAttr;
typedef struct {
    uint32_t nv, stride;
    uint8_t *v;
    uint32_t pos_off, nrm_off;
    int nattr;
    HatAttr attr[HAT_MAX_ATTRS];
    uint16_t *idx;
    uint32_t ni;
} HatMesh;

/* Faces whose normals are within this angle of a vertex's own normal share its smoothed normal. */
#define HAT_SMOOTH_DEG 50.0f
/* Replaces every normal of m by the angle-weighted sum of the normals of the triangles touching its
 * position (exactly equal positions are one) that lie within max_deg of its own (each triangle
 * turned to the side of its corners' normals; a vertex without a usable normal takes its first
 * triangle's), normalized: facets at less than max_deg merge, sharper edges (a brim's rim, a crown's
 * top edge) stay sharp. Returns 0, or -1 (m unchanged) for a degenerate mesh or no memory. */
int hat_smooth_normals(HatMesh *m, float max_deg);

/* Edge segments per triangle edge in hat_subdivide (a triangle becomes level^2). */
#define HAT_SUBDIV_LEVEL 3
/* Subdivides m into out (allocated; hat_mesh_free) as curved PN triangles (Vlachos et al. 2001)
 * from its positions and normals, each triangle into level^2 with its own vertices: corners keep
 * their positions, each edge's points are computed once from the edge's two ends (curved when every
 * triangle on it has the same normals at both ends, straight otherwise: creases and seams stay
 * closed), normals and attributes are interpolated across each triangle (normals renormalized).
 * The same layout as m. Returns 0, or -1 (out empty) for level < 1, too many vertices for 16-bit
 * indices or no memory. */
int hat_subdivide(const HatMesh *m, int level, HatMesh *out);
/* Frees what hat_subdivide (or the caller with malloc) put in m's v and idx. */
void hat_mesh_free(HatMesh *m);

#endif
