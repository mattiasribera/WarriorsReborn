/* Wire format of the renderer.h call stream: what --record-renderer writes
 * and what a renderer process would receive. Shared by the runtime's recorder (render_rec.c, i386)
 * and the replay tool (tools/render64/render64_replay.c, built for i386 and x86_64).
 *
 * File: RwFileHeader, then records. A record is an 8-byte header (id, flags, payload length; the
 * payload padded to 8 bytes) and its payload: the call's arguments in parameter order, then its
 * result (non-void calls); renderer_begin's ends with the frame time (renderer_frame_time, a double).
 * Scalars are little-endian, 4 bytes (uint64_t, double: 8), unaligned. Pointer
 * arguments are payloads: a u32 byte count (RW_NULL for NULL), padding to 8 bytes from the record
 * start, the bytes, padding to 8: the reader hands out pointers into the record, aligned for any
 * renderer.h type. Plain structs travel as their bytes (only 4-byte fields: the same layout in
 * both ABIs; the header carries their sizes); RendererConfig, the material and wall maps and the
 * GPU-skinning normals tables have codecs (render_wire.c, render_wire_gen.h). The per-function
 * part is generated: tools/render64/gen_wire.py writes render_wire_gen.h from renderer.h. */
#ifndef RENDER_WIRE_H
#define RENDER_WIRE_H
#include <stddef.h>
#include <stdint.h>
#include "renderer.h"
#include "gamma_ramp.h"

#define RW_MAGIC "R64WIRE4"   /* 2: renderer_begin carries the frame time, RW_REC_FRAME the generated frame;
                                 3: references, constant deltas, the base image when changed, draw-state records;
                                 4: the content cache (RW_CACHED, RW_CACHE_NEW, RW_CACHE_MOVE) */
#define RW_NULL 0xFFFFFFFFu
#define RW_REF 0xFFFFFFFEu    /* a payload's u32 byte count: a reference to an earlier payload follows (u32 index) */
#define RW_REF_MIN 64u        /* payloads under this many bytes are always sent, never referenced or indexed */
/* Content cache (W4): a payload's u32 byte count may also be one of these. */
#define RW_CACHED 0xFFFFFFFDu      /* content the reader holds: u32 arena offset, u32 bytes */
#define RW_CACHE_NEW 0xFFFFFFFCu   /* new content: u32 arena offset, u32 bytes, padding to 8, the bytes, padding */
#define RW_CACHE_MOVE 0xFFFFFFFBu  /* content the reader holds, moved first: u32 from, u32 to, u32 bytes */
#define RW_CACHE_MIN 64u           /* payloads under this many bytes never use the cache */
#define RW_CACHE_ARGS_MIN 256u     /* other arguments (RW_CAT_ARGS, e.g. RSkinJob's 176 bytes per skinned draw) under this go whole */
#define RW_CACHE_BYTES (128u << 20)   /* the content arena (render_ipc's content region, R64_DEFAULT_CONTENT_BYTES) */
#define RW_ALIGN 8u

/* Records the recorder adds (above every generated RW_ID_*). */
enum {
    RW_REC_FRAME = 0xF000,   /* after renderer_end: frame number, result, w, h, hash, generated-frame hash, optional pixels */
    RW_REC_PREAMBLE_BEGIN,   /* the state replayed before the first recorded frame (see render_rec.c) */
    RW_REC_PREAMBLE_END,
    RW_REC_DRAW_STATE,       /* the draw-state setters called since the last flush whose value changed (rw_draw_state_*) */
    RW_REC_PIPE_PLAN,        /* before renderer_end/present: the frame's pipeline decisions (renderer_pipeline_plan) */
    RW_REC_QUERY,            /* --render-process helper (W6): u32 query index + its arguments; the helper replies on its BACK ring */
};
#define RW_FLAG_PREAMBLE 1u
#define RW_FLAG_REPLY 2u   /* --render-process helper: the game waits for this record's result (render_rec.c) */
#define RW_FLAG_LATER 4u   /* the record's result was not waited for (fire-and-forget): a replay does not compare it */

/* Payload categories (the traffic report): ARGS = everything that is not a counted payload
 * (scalars, plain structs, headers, padding). */
enum {
    RW_CAT_ARGS, RW_CAT_VERTS, RW_CAT_INDICES, RW_CAT_NORMALS, RW_CAT_SKIN_PROG, RW_CAT_SKIN_CONSTS, RW_CAT_SKIN_DATA,
    RW_CAT_BASE, RW_CAT_TEXTURES, RW_CAT_MOVIE, RW_CAT_MAPS, RW_CATS
};
extern const char *const rw_cat_name[RW_CATS];

/* The state a stream keeps between its records (W2); NULL in a writer or
 * reader: every payload travels whole (the plain format, the preamble's records).
 *   - References: a payload of RW_REF_MIN bytes or more passed again in the same frame chunk by the
 *     same pointer and size, with the same bytes, travels as RW_REF + the index of the earlier one (both
 *     ends number every whole payload of RW_REF_MIN bytes or more of the chunk). A frame chunk starts at
 *     renderer_begin and again after RW_REC_FRAME: both ends call rw_enc_frame / rw_dec_frame there.
 *   - GPU-skinning programs (rw_put_skin_prog): the same pointer as an earlier program of the chunk is a
 *     reference whatever its bytes are now; the renderer keys its copies by that pointer (prog_at), so the
 *     reader hands it the same pointer again. Every whole program is indexed, whatever its size.
 *   - GPU-skinning constants (rw_put_skin_consts): the registers that changed since the last skinned
 *     draw of the stream, and whether the pointer is the last draw's (the renderer's last_consts: the
 *     reader then rewrites that block in place and passes the same pointer).
 *   - The base image (rw_put_base): "the same bytes as the last one sent" or the image.
 *   - The content cache (rw_enc_cache, W4): a payload of RW_CACHE_MIN bytes or more of the cached categories
 *     (vertices, indices, normals, skinning streams, maps, other arguments of RW_CACHE_ARGS_MIN bytes or more;
 *     not textures, movie planes or the base image) is keyed by a 64-bit hash of its bytes and its size (rw_hash64). The writer owns the
 *     allocator of the reader's content arena (RW_CACHE_BYTES): new content travels as RW_CACHE_NEW with
 *     the offset the reader copies it to, content the reader already holds as RW_CACHED with its offset. The
 *     arena is filled circularly; content is evicted in allocation order (the oldest first), and a hit on
 *     content in the older half (the next to be overwritten) moves it to the head instead (RW_CACHE_MOVE:
 *     the reader moves the bytes it has, nothing travels; at most once per chunk): an LRU approximation.
 *     New and moved bytes are capped per frame chunk (an eighth of the arena), and content used in this
 *     chunk or the one before is never evicted (the payload goes whole instead). The reader keeps no table (the
 *     stream says where everything is), so a replay's arena is the same as the receiving process's.
 *     With the cache on, in-frame references (RW_REF) are not used.
 * The reader's pointers stay valid until its next rw_dec_frame; an unchanged base image's until the next
 * base image is read; cached content's until the next rw_dec_frame too. */
typedef struct RwEnc RwEnc;
typedef struct RwDec RwDec;

typedef struct {
    uint8_t *buf;
    size_t n, cap;
    size_t rec;             /* start of the record being written */
    uint64_t cat[RW_CATS];  /* payload bytes per category (ARGS: the rest), since the last rw_reset */
    int bad;                /* out of memory: the records since are lost */
    RwEnc *enc;             /* NULL: no references or deltas */
    int no_refs;            /* with enc: this record's payloads are indexed but never sent as references */
    int timed;              /* with enc: this record's content hashing is timed (RwEncStats.cache_hash_ticks) */
} RwWriter;

void rw_reset(RwWriter *w);   /* empties the buffer and the counters (keeps the memory) */
void rw_free(RwWriter *w);
/* n bytes of p through the buffer's growth path (the inline writers below when they do not fit). */
void rw_put_slow(RwWriter *w, const void *p, size_t n);
/* The scalar writers and the record frame: inline (the stream has ~3000 records and ~11 k setter calls
 * per frame in the busy street). A writer that ran out of memory (bad) has
 * cap 0, so every write takes the slow path, which drops it. */
static inline void rw_put_fast(RwWriter *w, const void *p, size_t n) {
    if (w->n + n <= w->cap) {
        __builtin_memcpy(w->buf + w->n, p, n);
        w->n += n;
    } else rw_put_slow(w, p, n);
}
static inline void rw_u32(RwWriter *w, uint32_t v) { rw_put_fast(w, &v, 4); }
static inline void rw_i32(RwWriter *w, int32_t v) { rw_put_fast(w, &v, 4); }
static inline void rw_f32(RwWriter *w, float v) { rw_put_fast(w, &v, 4); }
static inline void rw_u64(RwWriter *w, uint64_t v) { rw_put_fast(w, &v, 8); }
static inline void rw_f64(RwWriter *w, double v) { rw_put_fast(w, &v, 8); }   /* IEEE 754 binary64, little-endian (both ABIs) */
void rw_begin_record(RwWriter *w, uint32_t id, uint32_t flags);
void rw_end_record(RwWriter *w);
/* n bytes of p (present = 0: a NULL pointer, n ignored). */
void rw_blob(RwWriter *w, const void *p, size_t n, int present, int cat);
void rw_put_filmic(RwWriter *w, const RFilmic *f);
void rw_put_map(RwWriter *w, const RMaterialMap *m);   /* RLampWallMap has the same layout */
void rw_put_skin_normals(RwWriter *w, const RSkinNormals *n, uint32_t nv);
void rw_put_config(RwWriter *w, const RendererConfig *c);
/* n raw bytes (a whole record copied in). */
void rw_bytes(RwWriter *w, const void *p, size_t n);
/* n bytes, never a reference and never indexed (the draw-state record's setter payloads). */
void rw_blob_plain(RwWriter *w, const void *p, size_t n);
void rw_put_skin_prog(RwWriter *w, const void *ins, size_t n);
void rw_put_skin_consts(RwWriter *w, const float (*consts)[4]);
void rw_put_base(RwWriter *w, const void *base, size_t n);

/* Encoder state (RwWriter.enc). */
typedef struct {
    uint64_t ref_bytes[RW_CATS];   /* payload bytes sent as references, per category */
    uint64_t refs, stale;          /* references; same pointer and size again with other bytes (sent whole) */
    uint64_t prog_refs, prog_stale;   /* skinning programs by reference; of those, with other bytes now */
    uint64_t consts_draws, consts_same_ptr, consts_regs, consts_saved;   /* skinned draws, last pointer again, registers sent, bytes saved */
    uint64_t base_sent, base_same, base_saved;   /* base images sent, unchanged, bytes saved */
    /* content cache */
    uint64_t cache_hit_bytes[RW_CATS];   /* payload bytes the reader already held (hits and moves), per category */
    uint64_t cache_hits, cache_moves, cache_move_bytes, cache_new, cache_new_bytes;
    uint64_t cache_evicted, cache_refused, cache_hashed;   /* entries evicted; payloads sent whole (budget, size); bytes hashed */
    uint64_t cache_collisions;   /* rw_enc_cache_verify: a hit whose bytes differ (sent again as new) */
    uint64_t cache_hash_ticks;   /* time in rw_hash64 and the hash hook, of the records written with RwWriter.timed (TSC ticks; x86 builds) */
    uint64_t cache_prehashed;    /* bytes whose hash came from rw_enc_hash_hook (not hashed here) */
    uint64_t consts_delta_known; /* skinned draws whose register delta came from rw_enc_consts_hook */
    uint64_t consts_delta_wrong; /* of those, a delta the bytes contradict (--renderer-cache check; the bytes win) */
} RwEncStats;
RwEnc *rw_enc_new(void);
void rw_enc_free(RwEnc *e);
void rw_enc_frame(RwEnc *e);    /* a frame chunk starts: forget its references and the last constants pointer */
void rw_enc_stream(RwEnc *e);   /* a stream starts: also forget the constants and the base image */
RwEncStats *rw_enc_stats(RwEnc *e);
/* Called for every payload of RW_REF_MIN bytes or more sent whole (not for references). */
void rw_enc_on_whole(RwEnc *e, void (*fn)(const void *p, size_t n, int cat));
/* The content cache: on with the reader's arena size (a multiple of 16, at least 1 MB; 0: off). Off by
 * default; rw_enc_stream empties it. Returns 0, or -1 (out of memory: off). */
int rw_enc_cache(RwEnc *e, uint32_t arena_bytes);
/* Hashes computed elsewhere (render_rec.c's hash-ahead worker): fn returns 1 and *h = rw_hash64(p, n) when
 * it has the hash of exactly these bytes; the encoder then does not read them (stats: cache_prehashed). It is
 * asked for the base image and every cached payload except other arguments (RW_CAT_ARGS: never queued). */
void rw_enc_hash_hook(RwEnc *e, int (*fn)(const void *p, size_t n, uint64_t *h));
/* Skinning-constant deltas computed elsewhere (the hash-ahead worker): fn returns 1 and mask[6] = the
 * registers (bit i of mask[i / 32]) where the 192 x 16-byte block cur differs from prev when it knows
 * them for exactly these two blocks, both unchanged since. rw_put_skin_consts asks with prev = the last
 * block it sent (the reader's registers are that block's bytes); the stream does not change. */
void rw_enc_consts_hook(RwEnc *e, int (*fn)(const void *cur, const void *prev, uint32_t mask[6]));
/* Payloads to come whose hash is known (the hash-ahead worker's): stage 0 prefetches the cache's bucket of h,
 * stage 1 its first entry (after an earlier stage 0), so the lookup does not wait for memory. No effect on the stream. */
void rw_enc_cache_prefetch(RwEnc *e, uint64_t h, int stage);
/* Collision check (a diagnostic): every cached payload also gets a second, independent 32-bit hash; a
 * hit whose second hash differs is a collision of rw_hash64, sent as new content (stats: cache_collisions). */
void rw_enc_cache_verify(RwEnc *e, int on);
/* The writer's buffer was emptied inside a frame chunk (loopback flushes): forget the offsets of
 * the payloads in it (the skinning programs' statistics compare against them). */
void rw_enc_writer_reset(RwEnc *e);
/* The 64-bit content hash (an XXH3-style accumulator: SSE2 when the build has it, else the scalar
 * version, which gives the same values). */
uint64_t rw_hash64(const void *p, size_t n);
uint64_t rw_hash64_scalar(const void *p, size_t n);

typedef struct {
    const uint8_t *p, *end, *rec;   /* cursor, end of the payload, record start (alignment base) */
    int bad;                         /* read past the end, or a reference without (or outside) the stream state */
    RwDec *dec;                      /* NULL: a reference is an error */
} RwReader;
void rw_reader(RwReader *r, const void *payload, size_t n);
uint32_t rw_ru32(RwReader *r);
int32_t rw_ri32(RwReader *r);
float rw_rf32(RwReader *r);
uint64_t rw_ru64(RwReader *r);
double rw_rf64(RwReader *r);
/* The payload's bytes (NULL for a NULL pointer); *n its size when n is not NULL. */
const void *rw_rblob(RwReader *r, size_t *n);
const RFilmic *rw_get_filmic(RwReader *r);   /* points into a static copy: valid until the next call */
int rw_get_map(RwReader *r, RMaterialMap *m);
int rw_get_skin_normals(RwReader *r, RSkinNormals *n);
int rw_get_config(RwReader *r, RendererConfig *c, int *had_surface);
const void *rw_rblob_plain(RwReader *r, size_t *n);
const void *rw_get_skin_prog(RwReader *r);
const void *rw_get_skin_consts(RwReader *r);
const void *rw_get_base(RwReader *r);
RwDec *rw_dec_new(void);
void rw_dec_free(RwDec *d);
/* The content arena (n bytes, 16-aligned; owned by the caller, e.g. render_ipc's content region).
 * Without one the reader allocates RW_CACHE_BYTES at the first RW_CACHE_NEW. */
void rw_dec_arena(RwDec *d, void *arena, uint32_t n);
const void *rw_dec_arena_base(RwDec *d, uint32_t *n);
/* Copy every whole skinning program into the reader's own memory (a reader whose records do not keep
 * their addresses for the frame chunk, e.g. a ring: the renderer keys programs by pointer). */
void rw_dec_copy_programs(RwDec *d, int on);
void rw_dec_frame(RwDec *d);
void rw_dec_stream(RwDec *d);

/* Draw-state records (RW_REC_DRAW_STATE): u32 mask of slots, then each slot's setter payload (the
 * arguments as that setter's own record carries them) as a plain blob. A slot is one setter
 * (renderer_set_stage_texture: one per stage); the setters of different slots write separate renderer
 * state and read none another one writes, so the reader applies them in slot order. */
#define RW_DS_SLOTS 13
/* The slot of a setter call (id, its first argument), or -1 when it is not a draw-state setter. */
int rw_draw_state_slot(uint32_t id, uint32_t first_arg);
uint32_t rw_draw_state_id(int slot);

/* File header. */
typedef struct {
    char magic[8];
    uint32_t schema;          /* RW_SCHEMA of the writer */
    uint32_t ptr_bytes;       /* the writer's pointer size (informative: nothing depends on it) */
    uint32_t nstructs;        /* RW_STRUCT_COUNT */
    uint32_t sizes[64];       /* RW_STRUCT_SIZES */
} RwFileHeader;
void rw_file_header(RwFileHeader *h);
/* 0 when h was written by a compatible writer; else -1 and why. */
int rw_check_header(const RwFileHeader *h, char *why, size_t n);

/* Payload sizes. */
size_t rw_texture_bytes(uint32_t format, uint32_t w, uint32_t h, uint32_t levels);
size_t rw_str_bytes(const char *s);   /* with the terminating 0 */
/* 64-bit hash of a frame's w x h pixels (rows of pitch bytes). */
uint64_t rw_frame_hash(const uint32_t *px, int w, int h, int pitch_bytes);

/* Record header as stored. */
typedef struct {
    uint16_t id, flags;
    uint32_t len;   /* payload bytes, a multiple of RW_ALIGN */
} RwRecordHeader;
#endif
