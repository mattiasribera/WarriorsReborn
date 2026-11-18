/* --hair, --skin (below): character hair with strand-aligned highlights instead of a flat,
 * evenly lit painted shell.
 *
 * The title paints hair into each character's one opaque DXT1 skin (no alpha cards: every skin is
 * opaque), on the head joint's triangles, and draws it like the rest of the body: the per-vertex
 * light and --char-lighting's diffuse term, no highlight. --hair finds the hair texels of a skin
 * from the draw's own bind-pose mesh (the rig's head joint, minus
 * the face) and the skin's colours (texels that look like the face's skin, or like a
 * brightly coloured cap, are left out), once per skin, and the lit character draw then shades
 * those texels as hair (geom.frag HAIR): Kajiya-Kay highlights along the strands (a primary one in
 * the light's colour and a secondary one tinted by the hair), broken into strands by the skin's
 * own detail across the strand direction, a weak sheen of the baked light from above, and darker
 * under-layers towards the nape. Everything is computed at run time from the player's files.
 *
 * The mask (hair_mask_finish) is a small BGRA8 texture in the skin's texture space:
 *   R  hair weight (0-255),
 *   G  height along the hair (0 the lowest hair, 255 the crown),
 *   B, A  the strand direction (down the head) in texture space, 128 + 127 (du, dv), v down the image.
 * Game-independent: tested by tests/hair_test.c. */
#ifndef HAIR_H
#define HAIR_H
#include <stdint.h>

typedef struct {
    int on;          /* --hair / --no-hair (default off) */
    float strength;  /* --hair-strength F (HAIR_STRENGTH_MIN..MAX, default 1) */
} HairConfig;
#define HAIR_DEFAULTS {0, 1.0f}
#define HAIR_STRENGTH_MIN 0.1f
#define HAIR_STRENGTH_MAX 3.0f

/* Parses one option (val: the next argument or NULL). Returns 1 (a flag) or 2 (an option and its
 * value) when consumed, -1 for a bad value, 0 when opt is not a hair option. */
int hair_arg(HairConfig *c, const char *opt, const char *val);

/* The title's rig: palette slot k is joint k + 2; a vertex's SHORT3
 * bone indices hold slot x 3. */
#define HAIR_SLOT_NECK 3
#define HAIR_SLOT_HEAD 4        /* bip_sw_head */
#define HAIR_SLOT_FACE_FIRST 5  /* bip_brow_left .. bip_lip_upper: the face's joints */
#define HAIR_SLOT_FACE_LAST 13
#define HAIR_SLOTS 32
/* A head triangle facing forward by more than this (normal . forward) is face, as regions.py. */
#define HAIR_FACE_FACING 0.25f
/* ... unless it faces up by more than this (the top of the head, a fringe). */
#define HAIR_TOP_FACING 0.6f
/* The mask's size: at most this many texels a side (the hair's weight, height and direction are
 * smooth; the strand detail comes from the skin itself). */
#define HAIR_MASK_MAX 128

/* One draw's bind-pose mesh (model space, z up). */
typedef struct {
    uint32_t nv;
    const float (*pos)[3];
    const float (*nrm)[3];    /* unit vertex normals, or NULL (the triangles' own) */
    const float (*uv)[2];     /* the skin's texture coordinates */
    const int (*slot)[3];     /* palette slots of the three influences (< 0 or >= HAIR_SLOTS: unused) */
    const float (*weight)[3];
    const uint32_t *idx;      /* triangle list into the vertices */
    uint32_t ni;
} HairMesh;

/* The mask's size for a w x h skin. */
void hair_mask_size(uint32_t w, uint32_t h, uint32_t *mw, uint32_t *mh);

/* Adds one mesh's hair to mask (mw x mh words, 0xAARRGGBB as above; zero = no hair yet) with the
 * skin's colours at the mask's size (albedo, mw x mh BGRA8 words, or NULL: geometry only). The
 * face's skin colour is the median of the texels under the face joints' triangles; ref_rgb keeps
 * it between calls (in: the previous one when *have_ref, out: this mesh's when it has a face).
 * Returns the number of hair texels written by this mesh. */
uint32_t hair_mask_add(uint32_t *mask, uint32_t mw, uint32_t mh, const HairMesh *m, const uint32_t *albedo, float ref_rgb[3], int *have_ref);

/* Headgear modeled into a skin (a few characters wear a cap or helmet that is part of their head
 * shell, not a separate hat object): never hair, whatever its colour. Such a head is recognised by
 * the print of its geometry: the bind-pose positions (rounded to 0.1 mm) of the vertices weighted
 * most to the head joint, each distinct position once, combined in any order (vertex order and
 * duplicates do not change it; 0 for a mesh without head vertices). hair_mask_add finds no hair on
 * a head whose print is known (the title's built-in list, plus hair_headgear_register's). */
uint64_t hair_head_print(const HairMesh *m);
int hair_is_headgear(const HairMesh *m);
void hair_headgear_register(uint64_t print);
/* The prints known (built-in and registered). */
int hair_headgear_known(void);

/* Box-filters a w x h BGRA8 image down to mw x mh (mw, mh divide w, h). */
void hair_downsample(const uint32_t *src, uint32_t w, uint32_t h, uint32_t *dst, uint32_t mw, uint32_t mh);

/* The per-skin cache (thread safe): the guest thread adds meshes as their draws are recorded, the
 * render thread reads the masks. key: the skin's texture key; sig: the mesh's identity. */
#define HAIR_CACHE_SKINS 1024
#define HAIR_CACHE_MESHES 4   /* distinct meshes per skin (a skin drawn by several draws) */
/* 1 when (key, sig) has not been added yet and the cache has room for it. */
int hair_cache_wants(uint64_t key, uint64_t sig);
/* Adds the mesh (albedo: the skin's level 0, w x h BGRA8, or NULL). Returns the skin's hair texels. */
uint32_t hair_cache_add(uint64_t key, uint64_t sig, const HairMesh *m, const uint32_t *albedo, uint32_t w, uint32_t h);
/* Under the cache's lock: fn(ctx, version, mw, mh, mask) for key's mask when it has hair; returns
 * fn's result, or 0 (no mask, or no hair in it). version grows with each mesh that adds hair. */
uint32_t hair_cache_with(uint64_t key, uint32_t (*fn)(void *ctx, uint32_t version, uint32_t mw, uint32_t mh, const uint32_t *mask), void *ctx);
/* Skins in the cache, and those with hair. */
void hair_cache_stats(uint32_t *skins, uint32_t *with_hair);
/* Empties the cache (tests). */
void hair_cache_clear(void);

/* ---- --skin: the skin's skin texels ----
 *
 * The same skins carry the characters' skin (face, neck, arms, the bare torso under the open vests).
 * --skin finds those texels with the hair's meshes and colours: every triangle of the skin's draws
 * is rasterized in UV with its region (the dominant joint: body, neck, head, face; a head triangle
 * facing forward is face, as for the hair) and how far out to the side of the head it lies (the
 * ears); a texel is skin when its colour (3 x 3 texels) has the face's median hue and about its
 * brightness, and is not hair; below the head the reference is the hands' median colour (always
 * bare) and the hue must match more closely (tan and brown clothes are near skin hues), and the
 * pelvis and legs are left out. Tattoos, clothes and hair have other hues or are much darker.
 *
 * The skin mask is a second small BGRA8 texture in the skin's texture space, as the hair's:
 *   R  skin weight (0-255),
 *   G  thinness: how much light passes through (the ears 255, the rest of the head ~115, the
 *      neck ~100, the body ~75),
 *   B  the sweat sheen's weight (the face 255, the head ~180, the neck ~130, the body ~100),
 *   A  255. */
typedef struct {
    int on;          /* --skin / --no-skin (default off) */
    float strength;  /* --skin-strength F (SKIN_STRENGTH_MIN..MAX, default 1) */
} SkinConfig;
#define SKIN_DEFAULTS {0, 1.0f}
#define SKIN_STRENGTH_MIN 0.1f
#define SKIN_STRENGTH_MAX 3.0f
/* As hair_arg, for --skin, --no-skin and --skin-strength. */
int skin_arg(SkinConfig *c, const char *opt, const char *val);

/* A texel's region (the highest of the triangles over it). */
enum { SKIN_REGION_NONE, SKIN_REGION_BODY, SKIN_REGION_NECK, SKIN_REGION_HEAD, SKIN_REGION_FACE, SKIN_REGION_HAND };
/* Palette slots of the hands and fingers (bip_sw_l_hand .. finger2, bip_sw_r_hand .. finger2) and
 * of the pelvis and legs (never bare in this game: trousers; left out). */
#define SKIN_SLOT_PELVIS 0
#define SKIN_SLOT_LEGS_FIRST 26
#define SKIN_IS_HAND(s) (((s) >= 17 && (s) <= 19) || ((s) >= 23 && (s) <= 25))
/* Adds one mesh's triangles to the region map and the thinness map (both mw x mh bytes; thinness:
 * 255 the head's most lateral texels, the ears; 0 elsewhere). */
void skin_geom_add(uint8_t *region, uint8_t *thin, uint32_t mw, uint32_t mh, const HairMesh *m);
/* Writes the whole skin mask (mw x mh words, as above) from the maps, the skin's colours at the
 * mask's size, the face's skin colour (hair_mask_add's ref_rgb; NULL: no skin) and the hair mask
 * (NULL: none; its hair is not skin). Returns the skin texels. */
uint32_t skin_mask_build(uint32_t *skin, uint32_t mw, uint32_t mh, const uint8_t *region, const uint8_t *thin, const uint32_t *albedo,
                         const float ref_rgb[3], const uint32_t *hair);
/* Skin masks in the cache: from now on hair_cache_add also keeps each skin's maps and rebuilds its
 * skin mask after each mesh (when its colours and the face's colour are known). */
void hair_cache_set_skin(int on);
/* As hair_cache_with, for the skin mask (version grows with each rebuild that finds skin). */
uint32_t hair_cache_skin_with(uint64_t key, uint32_t (*fn)(void *ctx, uint32_t version, uint32_t mw, uint32_t mh, const uint32_t *mask), void *ctx);
/* The skin texels of key's skin mask (0: none). */
uint32_t hair_cache_skin_texels(uint64_t key);

/* ---- --eyes, --cloth: the eyes and the clothes' materials ----
 *
 * The rig's one eye-lid joint (bip_eye_lid, palette slot 12) carries the triangles around both
 * painted eyes: only the upper lid's line is weighted to it, and a blink turns it to pull that line
 * down, so the eye's opening under it shrinks to nothing and the thin strip of upper lid above it
 * stretches over the eye. --eyes rasterizes the openings (the triangles a pull down shrinks) with
 * their strongest eye-lid weight, never the strip, keeps the texels whose colour is not the face's
 * skin (the white, the iris), and finds each eye's centre and
 * extent (its connected texels), so the shader can bend the normal over an eyeball and put a small,
 * sharp glint on it. --cloth classifies the body's clothes (not skin, not hair; the head, hands and
 * feet are left out) by colour and place: leather (dark red to brown on the torso and arms: the
 * Warriors' vests, jackets), denim (blue) and cotton (the rest: shirts, trousers).
 *
 * Both are small BGRA8 masks of the skin mask's size; with either option the skin mask's texture
 * is uploaded three masks tall (hair_cache_mat_with): the skin mask (zero without --skin), the eyes,
 * the clothes.
 * Eyes:  R eye weight, G / B the texel's offset from its eye's centre in the eye's half extents
 *        (128 + 127 x (du, dv), clamped to -1..1; v down the image), A 255.
 * Cloth: R leather, G denim, B cotton weight (their sum at most 255), A 255. */
typedef struct {
    int on;          /* --eyes / --no-eyes (default off) */
    float strength;  /* --eyes-strength F (EYES_STRENGTH_MIN..MAX, default 1) */
} EyesConfig;
#define EYES_DEFAULTS {0, 1.0f}
#define EYES_STRENGTH_MIN 0.1f
#define EYES_STRENGTH_MAX 3.0f
typedef struct {
    int on;          /* --cloth / --no-cloth (default off) */
    float strength;  /* --cloth-strength F (CLOTH_STRENGTH_MIN..MAX, default 1) */
} ClothConfig;
#define CLOTH_DEFAULTS {0, 1.0f}
#define CLOTH_STRENGTH_MIN 0.1f
#define CLOTH_STRENGTH_MAX 3.0f
/* As hair_arg, for --eyes, --no-eyes, --eyes-strength and --cloth, --no-cloth, --cloth-strength. */
int eyes_arg(EyesConfig *c, const char *opt, const char *val);
int cloth_arg(ClothConfig *c, const char *opt, const char *val);

#define HAIR_SLOT_EYE_LID 12
/* A texel's clothing region (the highest of the triangles over it): legs (the pelvis, thighs and
 * calves: trousers), arms (sleeves), torso (the spine, neck and clavicles: shirts, vests), other
 * (the head, hands and feet: never cloth). */
enum { CLOTH_REGION_NONE, CLOTH_REGION_LEGS, CLOTH_REGION_ARM, CLOTH_REGION_TORSO, CLOTH_REGION_OTHER };
/* Adds one mesh's triangles to the clothing region map and the eye map (both mw x mh bytes; eye:
 * the eye openings' strongest eye-lid weight, 255 = fully; 0 on the upper lid's strip). */
void mat_geom_add(uint8_t *region, uint8_t *eye, uint32_t mw, uint32_t mh, const HairMesh *m);
/* Writes the eyes mask (mw x mh words, as above) from the eye map, the skin's colours at the mask's
 * size and the face's skin colour (NULL: none, no eyes). Returns the eye texels. */
uint32_t eyes_mask_build(uint32_t *eyes, uint32_t mw, uint32_t mh, const uint8_t *eye, const uint32_t *albedo, const float ref_rgb[3]);
/* Writes the cloth mask from the region map, the colours, the skin mask and the hair mask (either
 * NULL: none; their texels are not cloth). Returns the cloth texels. */
uint32_t cloth_mask_build(uint32_t *cloth, uint32_t mw, uint32_t mh, const uint8_t *region, const uint32_t *albedo, const uint32_t *skin,
                          const uint32_t *hair);
/* The eyes and cloth masks in the cache (each on or off): from now on hair_cache_add also keeps each
 * skin's maps and rebuilds the masks after each mesh. */
void hair_cache_set_mat(int eyes, int cloth);
/* Under the cache's lock: fn(ctx, version, mw, 3 x mh, the three masks stacked: skin (zero unless
 * hair_cache_set_skin), eyes, cloth) for key's skin when any of the three has texels; returns fn's
 * result, or 0. version grows with each rebuild that changes them. */
uint32_t hair_cache_mat_with(uint64_t key, uint32_t (*fn)(void *ctx, uint32_t version, uint32_t mw, uint32_t mh, const uint32_t *mask), void *ctx);
/* The eye and cloth texels of key's masks. */
void hair_cache_mat_texels(uint64_t key, uint32_t *eyes, uint32_t *cloth);
/* A blink: the title turns the eye-lid joint against the head (slot HAIR_SLOT_HEAD; 0 at rest, as in
 * the bind pose) and so pulls the upper lid's line down over the eye. In level51's cutscene and play
 * the lid rests at 0-5 degrees, narrowed expressions hold up to ~8, and a blink peaks at ~15 for
 * about 7 frames. The eye's shading fades out as it turns: 1 open up to EYES_LID_OPEN_DEG, 0 shut
 * from EYES_LID_SHUT_DEG. c: a draw's vertex constants (rows rows), base: its bone palette's first
 * row (3 rows a slot). 1 when the palette is out of range or unreadable (the eye shaded as before). */
#define EYES_LID_OPEN_DEG 6.0f
#define EYES_LID_SHUT_DEG 12.0f
float eyes_lid_open(const float (*c)[4], int base, int rows);

#endif
