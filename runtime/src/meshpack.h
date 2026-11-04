/* --mesh-pack DIR: replacement meshes for skinned characters, keyed by a content hash of the
 * guest's draw. A replacement keeps the guest's vertex format (stream 0:
 * 0x20 bytes, float3 position, NORMPACKED3 normal, float2 uv0, float2 uv1; stream 1: 18 bytes,
 * SHORT3 palette offsets and float3 weights) and the same 32-slot bone palette, so the title's own
 * vertex program skins it with the matrices of the current animation frame.
 *
 * Content hash: FNV-1a 64 over a 16-byte header {index count, stride 0, stride 1, 0} (uint32 LE),
 * then the u16 indices of the draw, then the stream 0 and stream 1 bytes of the vertices the
 * indices reach (lowest to highest index). Guest addresses never enter it.
 *
 * File DIR/<hash>.wmesh (16 lower-case hex digits): "WMP1", uint32 vertex count, uint32 index
 * count, uint32 stride 0, uint32 stride 1, then stream 0, stream 1 and the u16 triangle-list
 * indices. Game-independent; tools/mesh_pack writes the files. */
#ifndef MESHPACK_H
#define MESHPACK_H
#include <stdint.h>
#include <stddef.h>

#define MESHPACK_STRIDE0 0x20
#define MESHPACK_STRIDE1 18
#define MESHPACK_MAX_VERTS 65535

typedef struct {
    uint64_t hash;
    uint32_t nv, ni;
    const uint8_t *s0, *s1;   /* nv * MESHPACK_STRIDE0, nv * MESHPACK_STRIDE1 bytes */
    const uint16_t *idx;      /* ni triangle-list indices, each < nv */
} MeshPackMesh;

uint64_t meshpack_hash(const uint16_t *idx, uint32_t ni, uint32_t lo, uint32_t hi, const uint8_t *s0, uint32_t stride0,
                       const uint8_t *s1, uint32_t stride1);
/* Parses one .wmesh image (copied); 0 on success. */
int meshpack_parse(const uint8_t *data, size_t len, uint64_t hash, MeshPackMesh *out);
/* Loads every DIR/<hash>.wmesh (invalid files are reported and skipped); the number loaded, -1 if
 * DIR cannot be read. */
int meshpack_open(const char *dir);
int meshpack_count(void);
const MeshPackMesh *meshpack_find(uint64_t hash);
/* Memo of a draw's guest identity (buffer addresses, count, index range, fingerprint) -> hash, so
 * each mesh is hashed once; key 0 is never used. */
int meshpack_memo_get(uint64_t key, uint64_t *hash);
void meshpack_memo_set(uint64_t key, uint64_t hash);
void meshpack_close(void);
#endif
