/* Effect pools of the title.
 *
 * The TaskManager at [0x5aceb8] (constructor 0x2b2b10, called from GameModes_Initialize) owns
 * FreeListContainers of fixed size, carved from the main heap at start-up:
 *   +0x838 particle tasks, 1400 of 0xdc bytes (blood, debris, smoke, glass shards, sparks)
 *   +0x83c particle tasks, 100 (second pool)
 *   +0x84c glass tasks, 100 of 0xf0 bytes (the GlassTaskManager is a FreeListContainer)
 * A FreeListContainer keeps its capacity at +0xc and the used count at +0x10 (vtable +0x1c
 * returns capacity - used). The sound tasks ([0x4a6f64], 256 of 0xe0 bytes) and the script
 * objects ([0x5a752c], 100 of 0x80 bytes) are plain FreeLists: capacity +0x8, used +0xc.
 *
 * Particle systems (255 slots in the ResourceMgr) draw their sprites through a per-system buffer
 * of `max` quads (+0x24); each sprite added in a frame increments +0x28, and one added at or
 * above max is dropped (0xa0ad0 / 0xa08a0 still count it). The per-system flush 0xc0480 draws and
 * then clears +0x28 with the store at 0xc0499, so count - max there is the frame's drop count.
 *
 * --stats (D0): a [fx] line per 60 presented frames with the used counts and peaks of the pools
 * above, and per sprite-pool size the peak sprites asked for in one flush and the sprites dropped
 * (the flush store becomes a call to a counter that then runs the store).
 *
 * Big heap (D1; --big-heap, implied by --fx-scale above 1): the main heap ("System Memory Pool",
 * 0x6bd70) asks MmAllocateContiguousMemoryEx for 0x2d80000 bytes below 64 MB, after an 18.5 MB
 * probe block that our no-op MmFreeContiguousMemory never returns, and steps down 4 KB until it
 * fits (45.4 MB here). Every pool above is carved from it before the Sector Pool (streamed world)
 * takes what is left, so raising a pool on the Xbox heap shrinks the world budget. The patch
 * raises HighestAcceptable of both requests to 128 MB (the runtime's contiguous window) and the
 * heap size to FX_HEAP_SIZE, leaving the rest of the window for the D3D data the runtime hands
 * out later from the same window (hle_d3d.c data_alloc).
 *
 * --fx-scale N (D2, D3; 1.0-4.0, default 1 = no patch): multiplies the particle-task pools, glass
 * and sound tasks, and the blood fade threshold (blo_splat and blood_splat_ground fade or die
 * when 512 or fewer particle tasks are free, 0x2b4b40), so blood survives fights; and the sprite
 * pools part_page1 640 / 400 / 200 and part_fire 256, clamped to the 2048 quads of the shared
 * index buffer (0xc1620). Each system's 3 VBs (max * 0x60 bytes each, 0xc14e0) come from the graphics
 * device's 2 MB "Dynamic vertex buffers" heap (0xbb12f, itself carved from the main heap; a failed
 * allocation crashes on purpose at 0x24574b), so that heap grows by the raised pools' extra VB
 * bytes plus 64 KB; when its site differs no sprite pool is raised. Particle tasks use no
 * handle-table entries. Not raised: the 60 characters, the 255
 * particle-system slots and the audio channels. */
#ifndef HLE_FX_H
#define HLE_FX_H
#include <stdint.h>

#define FX_TASK_MANAGER 0x005aceb8u   /* TaskManager pointer */
#define FX_SOUND_LIST 0x004a6f64u     /* sound-task FreeList pointer (0x28080) */
#define FX_SCRIPT_LIST 0x005a752cu    /* script-object FreeList pointer (0x2945d0) */
#define FX_SPRITE_FLUSH 0x000c0499u   /* mov dword [esi+0x28], 0 in the particle-system flush 0xc0480 */

#define FX_SCALE_MAX 4.0
#define FX_SPRITE_CAP 2048            /* quads in the shared particle index buffer (0x6000 bytes) */
#define FX_HEAP_HIGHEST 0x08000000u   /* HighestAcceptable for the probe and the heap: the 128 MB window */
#define FX_HEAP_SIZE 0x04d80000u      /* main heap with --big-heap (Xbox: 0x2d80000 asked, 0x2d70000 got) */

/* Patch sites (guest addresses; the bytes are checked before each patch). */
#define FX_PROBE_HIGHEST 0x0006bd97u  /* push 0x4000000: probe block's HighestAcceptable */
#define FX_HEAP_SIZE_SITE 0x0006bdbeu /* mov dword [esi], 0x2d80000: heap size */
#define FX_HEAP_HIGHEST_SITE 0x0006bdcdu /* push 0x4000000: heap's HighestAcceptable */
#define FX_PTASK_SITE 0x002b2bbau     /* push 0x578: 1400 particle tasks */
#define FX_PTASK2_SITE 0x002b2bf7u    /* push 0x64; mov ecx, eax; call 0x2ab7c0: 100 particle tasks */
#define FX_PTASK_CTOR 0x002ab7c0u
#define FX_GLASS_SITE 0x0029f6d1u     /* push 0x64; mov esi, ecx; call 0x29f860: 100 glass tasks */
#define FX_GLASS_CTOR 0x0029f860u
#define FX_SOUND_SITE 0x000280a3u     /* push 0x100: 256 sound tasks */
#define FX_FADE_SITE 0x002b4b56u      /* cmp eax, 0x200: blood fades at 512 or fewer free particle tasks */
#define FX_SPRITE640_SITE 0x000a6d2cu /* push 0x280: part_page1 */
#define FX_SPRITE400_SITE 0x000a6d83u /* push 0x190: part_page1 */
#define FX_SPRITE200_SITE 0x000a6e25u /* push 0xc8: part_page1 */
#define FX_FIRE256_SITE 0x000a6fbfu   /* push 0x100: part_fire */
#define FX_VB_HEAP_SITE 0x000bb12fu   /* push 0x200000: "Dynamic vertex buffers" heap of the graphics device */
#define FX_SPRITE_VB_BYTES (3 * 0x60) /* per sprite: 3 VBs of 0x60 bytes (0xc14e0) */

/* round(value * scale), clamped to cap when cap > 0, never below value; value <= 0 unchanged. */
int fx_scaled(int value, double scale, int cap);
/* "N" with 1 <= N <= FX_SCALE_MAX (decimals allowed) -> 0 and *out = N; anything else -> -1. */
int fx_scale_parse(const char *s, double *out);

enum { FX_POOL_PTASK, FX_POOL_PTASK2, FX_POOL_GLASS, FX_POOL_SOUND, FX_POOL_SCRIPT, FX_POOLS };

typedef struct {
    int valid[FX_POOLS];          /* the pool object exists */
    uint32_t used[FX_POOLS], cap[FX_POOLS];
} FxPools;
void fx_pools_read(FxPools *p);

typedef struct {
    uint32_t max;                 /* pool size in sprites (+0x24) */
    uint32_t peak;                /* most sprites asked for in one flush since the last read */
    uint32_t drops;               /* sprites dropped since the last read */
    uint32_t peak_run, drops_run; /* the same over the whole run */
} FxSprites;
/* Copies up to n sprite-pool records (sorted by max) and starts a new window; returns the count. */
int fx_sprites_take(FxSprites *out, int n);

/* Installs the --stats flush counter (stats != 0), the big heap (big_heap != 0) and the scaled
 * pools (scale > 1); returns the number of sites patched (0 for 1, 0, 0). */
int hle_fx_install(double scale, int big_heap, int stats);

extern int hle_fx_report;            /* print a [fx] line per 60 presented frames */
void hle_fx_frame(uint32_t swap);    /* host thread, once per presented frame */
#endif
