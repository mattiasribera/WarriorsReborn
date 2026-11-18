/* Texture layers of the Lua mods (docs/MODDING.md, "Texture layers"): a mod's meshes folder may
 * hold, beside its mesh overrides, files "<tex>@<label>.png" (<tex>: the texture-pack content hash
 * of a texture, 16 lower-case hex digits; <label>: 1-40 of a-z 0-9 - _). All files of one label
 * form the layer "<mod id>/<label>", off until the mod's Lua turns it on (ModTexLayer). While a
 * layer is on, a character draw whose stage-0 texture is <tex> is drawn with that texture (or the
 * PNG of a mesh override that replaces the draw) with the layer's PNG blended over it by its alpha;
 * several layers of one texture stack in load order (mods in their load order, a mod's files by name). So a mod can change
 * part of a skin (clothes, a face) per character and stage without replacing the whole texture,
 * and two mods that change different parts of the same skin both show.
 *
 * Threads: texlayer_select runs where draws are recorded, texlayer_compose where textures are
 * resolved; the combination table is append-only (an id stays valid until texlayer_close).
 * Nothing here is game-specific: the files are made by a mod's local tool. */
#ifndef TEXLAYER_H
#define TEXLAYER_H
#include <stddef.h>
#include <stdint.h>

#define TEXLAYER_MAX_LAYERS 256
#define TEXLAYER_MAX_FILES 4096
#define TEXLAYER_MAX_STACK 8      /* layers blended over one texture at most */
#define TEXLAYER_MAX_COMBOS 1024  /* distinct (texture, active layers) combinations per run */

/* Parses "<tex>@<label>.png": 0 when it is a layer file (tex and label set; label has room for 41). */
int texlayer_parse_name(const char *name, uint64_t *tex, char *label);
/* Loads every layer file of dir (not its subfolders, in file-name order) as layers of the mod id;
 * the number of files loaded, -1 when dir cannot be read. A file of a label and texture already loaded is skipped. */
int texlayer_add_dir(const char *dir, const char *id);
/* Turns the layer "<mod id>/<label>" on or off: the number of textures it covers, -1 when there is
 * no such layer. texlayer_on: 1 on, 0 off, -1 none. */
int texlayer_set(const char *name, int on);
int texlayer_on(const char *name);
/* Number of layers on (0: draws need no texture hash for layers). */
int texlayer_active(void);
/* The combination for a draw whose stage-0 texture hash is tex: 0 when no layer of tex is on, else
 * an id for texlayer_key / texlayer_compose (the layers on now, in load order). */
uint16_t texlayer_select(uint64_t tex);
/* A key unique to the combination (its texture and layers), for caching the composed texture. */
uint64_t texlayer_key(uint16_t id);
/* Reads a layer PNG as B8G8R8A8 texels (alpha in the top byte); malloc'd, NULL on failure. */
typedef uint32_t *(*TexLayerLoad)(const char *path, uint32_t *w, uint32_t *h);
/* Blends the combination's layers over bgra (w x h, alpha in the top byte) in place: each texel
 * becomes layer * a + base * (1 - a) in colour, its alpha kept; a layer of another size is sampled
 * at the nearest texel. Layer files are read once (load) and kept. 0 when every layer applied,
 * else the number that could not be read (reported once each). */
int texlayer_compose(uint16_t id, uint32_t *bgra, uint32_t w, uint32_t h, TexLayerLoad load);
/* Pure blend of one layer (lw x lh) over bgra (w x h); exposed for tests. */
void texlayer_blend(uint32_t *bgra, uint32_t w, uint32_t h, const uint32_t *layer, uint32_t lw, uint32_t lh);
int texlayer_count(void);   /* layer files loaded */
void texlayer_close(void);
#endif
