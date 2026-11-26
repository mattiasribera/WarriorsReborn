/* The player's disc folder as files and asset chunks: one reader for XBoxWad.idx and the WAD files
 * it names (used by the materials / collision / acoustics level reads, the sound pack's name table,
 * the keyboard glyph table and the mod asset engine, src/modassets.h), and the game's asset
 * container (packs of items of class chunks). Plain C, no game data, no runtime state: the launcher
 * builds it too.
 *
 * XBoxWad.idx: {u32 wads, files, ?, wad table, file table}; the wad table holds `wads` entries of
 * {char name[32]; u32 handle} (36 bytes), the file table `files` entries of {u32 loc, size, 0,
 * name hash} (16 bytes): loc >> 24 is the wad, (loc & 0xffffff) << 10 the byte offset in it. The
 * name hash is CRC-32 of "ee_files\" + the lowercased name with backslashes.
 *
 * Container:
 *   pack:  {count, total, 0, 0xDE686795} followed by `count` items
 *   item:  {nchunks, S, 0, name hash} followed by class chunks {class, size, 0, hash} + payload
 *          until the payload sizes sum to S
 *   bare:  a single item at offset 0 (no pack header)
 * Character chunks (class 0x47 with head {1, 0x76}) and the skin binding table (class 0x44 of the
 * bare warriors.glr) are parsed here too; texture chunks (0x2a) give their D3D resource. */
#ifndef DISC_H
#define DISC_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISC_PACK_MAGIC 0xDE686795u
#define DISC_IDX_MAX (8u << 20)      /* a larger XBoxWad.idx is not the game's */
#define DISC_CLASS_MESH 0x47u
#define DISC_CLASS_TEXTURE 0x2Au
#define DISC_CLASS_SKINS 0x44u

/* CRC-32 (zlib's) of n bytes, continuing crc (0 to start). */
uint32_t disc_crc32(uint32_t crc, const void *p, size_t n);
/* The XBoxWad.idx name hash of a file name ("level51.lev", "paks\global.pak"; '/' counts as '\'). */
uint32_t disc_name_hash(const char *name);
/* The 0x47 chunk hash of a character model named in the game's scripts (CfgChar's model key, e.g.
 * "warr_cl"): CRC-32 of the name as written. */
uint32_t disc_model_hash(const char *name);

/* The file whose name hash is `hash` (the first entry of the file table with that hash) from the
 * disc folder dir: malloc'd bytes (at least 1 byte allocated, so an empty file is not NULL) and
 * *n, or NULL (no XBoxWad.idx, no such file, a file larger than max, a read error). */
uint8_t *disc_read_hash(const char *dir, uint32_t hash, uint32_t max, size_t *n);

typedef struct {
    uint32_t cls, hash;    /* chunk class and name hash */
    uint32_t item;         /* name hash of the item holding it */
    uint32_t file;         /* index in the file table */
    uint32_t off, size;    /* payload offset in the file, payload bytes */
    uint8_t bare;          /* 1: from a bare item (no pack header) */
} DiscChunk;

typedef struct {
    char dir[1024];
    uint32_t nwads, nfiles;
    char (*wads)[33];      /* wad file names */
    uint32_t *loc, *size, *hash;   /* the file table */
    uint64_t fingerprint;  /* FNV-1a 64 of the XBoxWad.idx bytes (it lists every file's place and size) */
    DiscChunk *chunks;     /* disc_scan: every chunk, in file table order */
    uint32_t nchunks;
    DiscChunk *tables;     /* disc_scan: the 0x44 chunks of bare items (the skin binding tables) */
    uint32_t ntables;
    FILE *fp[8];           /* open wad files (the first 8 wads) */
    int fpw[8];
} Disc;

/* Parses XBoxWad.idx bytes into d (dir left as is): 0, or -1 when they are not an index. */
int disc_index_parse(const uint8_t *idx, size_t n, Disc *d);
/* Opens the disc folder dir (reads its XBoxWad.idx): 0, or -1 (d zeroed). */
int disc_open(Disc *d, const char *dir);
void disc_close(Disc *d);
/* Reads `size` bytes at offset `off` of file i of the table into buf: 0, or -1. */
int disc_pread(Disc *d, uint32_t i, uint32_t off, void *buf, uint32_t size);
/* File i of the table, malloc'd (size bytes in *n), or NULL. */
uint8_t *disc_file(Disc *d, uint32_t i, size_t *n);
/* Walks every file's container headers (only the 16-byte headers are read, never a whole file):
 * fills d->chunks (packs, else a bare item, as container.walk) and d->tables (0x44 chunks of the
 * bare item of each file that passes the skin table loader's quick check, as
 * bindings.load_skin_bindings). Returns the number of chunks, or -1. */
int disc_scan(Disc *d);
/* disc_scan with an index file: when `path` holds the chunk list of a disc with this fingerprint
 * (written by an earlier call), it is read instead of walking every file (a cold walk reads the
 * headers of the whole disc: seconds); else the disc is scanned and the list written to `path`
 * (through `path`.tmp and a rename; a write failure only costs the next scan). Returns the number
 * of chunks, or -1; *loaded = 1 when it came from the file. */
int disc_scan_cached(Disc *d, const char *path, int *loaded);
/* The payload of a chunk, malloc'd, or NULL. */
uint8_t *disc_chunk_read(Disc *d, const DiscChunk *c);

/* The container walk over any byte source: read(ud, off, buf, 16) returns 0 when the 16 bytes at
 * off exist. emit(ud, chunk) gets each chunk (file field 0). Returns the number of chunks, 0 when
 * the source is neither a pack nor a bare item. bare_only: parse as a bare item even when it starts
 * with a pack header (the skin table loader's view). */
typedef int (*DiscReadFn)(void *ud, uint64_t off, uint8_t out[16]);
int disc_walk(DiscReadFn read, void *ud, uint64_t len, int bare_only, void (*emit)(void *ud, const DiscChunk *c));
/* disc_walk over a buffer into out (at most max written): the number of chunks. */
int disc_walk_buffer(const uint8_t *buf, size_t len, int bare_only, DiscChunk *out, int max);

/* ---- skin bindings (class 0x44 of warriors.glr) ---- */
typedef struct {
    uint32_t id;      /* the hash of the character's 0x47 chunk for the record that pack ships with */
    uint32_t group, model, tex;   /* model: the character's item hash; tex: a texture (0x2a) hash */
} DiscSkin;
/* Records of a 0x44 payload into out (at most max written; out may be NULL with max 0): the number
 * of records in the table, or -1 when it is not the table. */
int disc_skins_parse(const uint8_t *p, size_t n, DiscSkin *out, int max);
/* The skin records of one character: the record whose id is its 0x47 chunk hash first, then the
 * other records of the same model item, in table order. Returns their number (at most max). */
int disc_skins_for(const DiscSkin *recs, int nrec, uint32_t chunk_hash, uint32_t item_hash, DiscSkin *out, int max);

/* ---- characters (0x47 chunks with head {1, 0x76}) ---- */
#define DISC_STRIDE0 0x20
#define DISC_STRIDE1 18
#define DISC_PALETTE 32
typedef struct {
    const uint8_t *s0, *s1;   /* stream 0 (0x20 bytes per vertex), stream 1 (18 bytes) */
    const uint8_t *idx;       /* u16 triangle-list indices */
    uint32_t nv, ni;
    float inverse_bind[DISC_PALETTE][16];   /* D3D row vectors, per palette slot (slot k = joint k + 2) */
} DiscChar;
/* 1 when a 0x47 payload is a skinned character. */
int disc_is_char(const uint8_t *p, size_t n);
/* Parses a character payload (pointers into p): 0, or -1 with the reason in err. */
int disc_char_parse(const uint8_t *p, size_t n, DiscChar *c, char *err, size_t cap);

/* ---- static props (0x47 chunks with head {2, 0x6d}) ----
 * One vertex buffer of `stride` bytes per vertex (layout from the FVF, as geom.c: float3 position,
 * then float3 normal (or NORMPACKED3 when the stride is 8 bytes short), D3DCOLOR diffuse, float2
 * uvs) and u16 indices as stored (a triangle list or strip), drawn by the game exactly as stored:
 * the draw's content hash (`xbrun --mesh-log`) is meshover_draw_hash of these bytes. */
typedef struct {
    const uint8_t *vb;     /* nv * stride bytes */
    const uint8_t *idx;    /* nidx u16 indices, as stored */
    uint32_t nv, nidx, stride, fvf, prim;   /* prim: 5 triangle list, 6 strip */
    int normal, normal_packed, colour, uv0;  /* byte offsets in a vertex, -1 when absent */
} DiscProp;
/* 1 when a 0x47 payload is a static prop. */
int disc_is_prop(const uint8_t *p, size_t n);
/* Parses a static prop payload (pointers into p), with the Python reference's validations: 0, or -1
 * with the reason in err. */
int disc_prop_parse(const uint8_t *p, size_t n, DiscProp *o, char *err, size_t cap);

/* ---- textures (0x2a chunks) ---- */
/* The D3D resource of a texture payload: Format, Size and the level-0 data offset into the payload.
 * 0, or -1 (not a texture resource). */
int disc_tex_parse(const uint8_t *p, size_t n, uint32_t *format, uint32_t *size, uint32_t *data_off);

#ifdef __cplusplus
}
#endif
#endif
