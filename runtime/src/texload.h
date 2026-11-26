/* Texture pack load queue: which replacements (and emission masks) the loader threads read and
 * decode, and which loaded images the render thread uploads this frame. Game-, thread- and
 * clock-independent (the caller holds its own mutex around every call and passes frame numbers
 * and upload times), tested by tests/texload_test.c; used by src/hle_d3d.c.
 *
 * A request is keyed by (content hash, kind): the same texture asked for again (another draw,
 * another cache key with the same texels, a prefetch) shares one slot. Demand requests (a draw
 * needs the image now) load before prefetch requests (a level load registered the texture), each
 * kind first in, first out; a prefetch asked for by a draw becomes a demand request.
 *
 * Loaded images wait in their slots ("staged") until the render thread takes them. Staged bytes
 * are capped: prefetch loads do not start while the cap is reached, and the oldest staged
 * prefetch images are dropped (they are read again when drawn) to make room. A staged demand
 * image no draw asked for during keep_frames frames (its texture was destroyed, or left the
 * screen) is dropped too, and a staged prefetch image after 4 x keep_frames, so slots and the cap
 * never fill with images nobody takes.
 *
 * Upload budget per frame: the first image of a frame always goes (an image larger than the
 * budget must still reach the screen); later ones while the frame's uploaded bytes stay within
 * budget_bytes and its measured upload time below budget_ms. The rest stay staged for the next
 * frames, drawn with the guest texture meanwhile.
 *
 * Slots being loaded are never dropped (their loader writes the result back). Payloads are
 * malloc'd and freed here when dropped. */
#ifndef TEXLOAD_H
#define TEXLOAD_H
#include <stddef.h>
#include <stdint.h>

enum { TL_BASE, TL_MASK };                                    /* kinds: replacement, emission mask */
enum { TL_DEMAND, TL_PREFETCH };                              /* priorities */
enum { TL_FREE, TL_QUEUED, TL_LOADING, TL_READY, TL_FAILED }; /* slot states */

/* A loaded image: format (the caller's), size, levels and malloc'd data of `bytes` bytes, or
 * a failure reason; ms = the loader's read + decode time. */
typedef struct {
    uint32_t format, w, h, levels;
    void *data;
    size_t bytes;
    const char *why;
    double ms;
} TlImage;

typedef struct {
    uint64_t hash;
    int kind, prio, state;
    char *path;          /* queued: the file to read (malloc'd, taken by tl_next) */
    TlImage img;         /* ready / failed */
    uint32_t touched;    /* frame of the last request (or of the load's end) */
    uint64_t seq;        /* request order */
    int waited;          /* ready, held back by the upload budget at least once */
} TlSlot;

#define TL_MAX_SLOTS 2048
typedef struct {
    TlSlot *slot;
    int n;                           /* at most TL_MAX_SLOTS */
    uint64_t seq;
    size_t staged, staged_cap;       /* bytes of ready images; their cap */
    uint32_t keep_frames;            /* a ready demand image nobody asks for is dropped after this */
    size_t budget_bytes;             /* per-frame upload budget */
    double budget_ms;
    uint32_t frame;                  /* the frame the counters below are for */
    size_t up_bytes;
    double up_ms;
    int ups;
    uint32_t dropped_stale, dropped_cap, deferred;   /* statistics (deferred: images that waited for a later frame) */
    uint32_t promoted, prefetch_hits;                /* prefetches drawn; of them, already loaded */
    int prefetch_par;                /* prefetch loads at once (0 or 1: one, the disk stays free for the game) */
    uint16_t index[2 * TL_MAX_SLOTS];   /* (hash, kind) -> slot + 1, open addressing (tl_find) */
} TexLoadQueue;

void tl_init(TexLoadQueue *q, TlSlot *slots, int n, size_t staged_cap, uint32_t keep_frames, size_t budget_bytes, double budget_ms);
/* The slot of (hash, kind), or -1. */
int tl_find(const TexLoadQueue *q, uint64_t hash, int kind);
/* Asks for (hash, kind): its slot (touched; a prefetch asked for on demand is promoted), or a
 * new queued slot with *created = 1 whose path the caller sets (a slot left without a path fails
 * in tl_next's caller). A demand request finding no free slot drops the oldest queued or staged
 * prefetch; -1 when nothing can make room (retry on a later frame). */
int tl_request(TexLoadQueue *q, uint64_t hash, int kind, int prio, uint32_t frame, int *created);
/* Next slot to load (demand first, oldest first; a prefetch only below the staged cap and while
 * fewer than prefetch_par (at least 1) other prefetches load, so read-ahead leaves the disk to the
 * game), now loading, or -1. The
 * caller takes its path. */
int tl_next(TexLoadQueue *q);
/* A loader finished slot i: ready with img, or failed (img->why). */
void tl_done(TexLoadQueue *q, int i, const TlImage *img, int failed, uint32_t frame);
/* Takes slot i's result for upload: 1 = ready image within this frame's budget (moved to *out,
 * slot freed, its bytes counted), -1 = failed (*out holds the reason, slot freed), 0 = not yet
 * (loading, or over budget: stays staged). */
int tl_take(TexLoadQueue *q, int i, uint32_t frame, TlImage *out);
/* Adds an upload's measured time to the frame's budget. */
void tl_charge_ms(TexLoadQueue *q, uint32_t frame, double ms);
/* Once per frame: drops staged demand images and failed results untouched for keep_frames frames,
 * staged prefetch images untouched for 4 x keep_frames, and staged prefetch images (oldest first)
 * while the staged bytes exceed the cap. */
void tl_reclaim(TexLoadQueue *q, uint32_t frame);
/* Slots in a state (any kind / priority). */
int tl_count(const TexLoadQueue *q, int state);
/* Pre-upload (--level-prefetch): takes the oldest ready prefetch image of `kind` when it fits this
 * frame's budget (bytes and ms; unlike tl_take there is no first-upload exception, so a frame's
 * pre-uploads never exceed it): 1 = moved to *out with its hash, slot freed, bytes counted; 0 =
 * none ready or none fits. */
int tl_take_prefetch(TexLoadQueue *q, int kind, uint32_t frame, TlImage *out, uint64_t *hash);

/* Level texture list (--level-prefetch, <hdd>/cache/levels/<level>.txt): the pack textures a level
 * drew, in first-seen order, each tagged early (drawn within the level's first seconds; prefetched
 * at the level request) or late (drawn later, e.g. past a door; prefetched in the background once
 * the level runs). Content hashes only. The text form is one "f <16 hex digits>" (early) or
 * "l <16 hex digits>" (late) line per hash after '#' comment lines. */
#define TL_LIST_MAX 4096
typedef struct {
    uint64_t hash[TL_LIST_MAX];
    uint8_t late[TL_LIST_MAX];
    int n;
    uint16_t index[2 * TL_LIST_MAX];   /* open addressing: entry + 1, 0 = empty */
} TlList;
void tl_list_clear(TlList *l);
/* The entry of hash, or -1. */
int tl_list_find(const TlList *l, uint64_t hash);
/* 1 added, 0 already there (an early sighting makes it early), -1 full. */
int tl_list_add(TlList *l, uint64_t hash, int late);
/* Adds the lines of a NUL-terminated text (malformed lines skipped); returns the hashes added. */
int tl_list_parse(TlList *l, const char *text);
/* Writes the text form into buf (NUL-terminated when it fits); returns its length (as snprintf). */
size_t tl_list_format(const TlList *l, char *buf, size_t cap);

/* Replacements whose upload failed (renderer_texture_create: the cache full, or device memory):
 * drawn with the original texture meanwhile (the renderer frees its least recently used textures
 * at the next frame) and tried again on later draws, up to TL_UPLOAD_TRIES failures in a row per
 * hash, so a texture that can never be created does not load its file again on every draw.
 * Direct-mapped by hash (a colliding hash takes the slot over). */
#define TL_UPLOAD_TRIES 3
#define TL_UPLOAD_SLOTS 64
typedef struct {
    uint64_t hash[TL_UPLOAD_SLOTS];
    uint8_t fails[TL_UPLOAD_SLOTS];
} TlUploadFails;
/* Notes a failed upload of hash: 1 = worth another try on a later draw, 0 = it failed
 * TL_UPLOAD_TRIES times in a row (give the original for good). */
int tl_upload_failed(TlUploadFails *f, uint64_t hash);
/* A successful upload of hash forgets its failures. */
void tl_upload_ok(TlUploadFails *f, uint64_t hash);
#endif
