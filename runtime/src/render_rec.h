/* Renderer call recorder: hle_d3d.c includes this after renderer.h and
 * gamma_ramp.h, which routes its renderer_* calls through the rr_* wrappers of render_rec.c. With
 * none of the options below the wrappers only test rr_active and call the renderer: nothing else
 * changes.
 *   --record-renderer FILE         writes the call stream (render_wire.h) of the window's frames
 *   --record-renderer-frames S:N   the window: renderer frames S to S + N - 1 (default 1:60; a
 *                                  frame is one renderer_begin, counted from 1)
 *   --record-renderer-pixels K     also stores every K-th recorded frame's pixels (0 = none)
 *   --renderer-traffic S:N         serializes the window's frames without writing them and prints
 *                                  the per-frame traffic ([render64] lines)
 *   --mem-map-every N              an address-space map by owner every N renderer frames and at
 *                                  exit ([mem-map] lines)
 *   --render-process loopback      every call goes through the wire into this process's renderer
 *                                  (render_rec.c "the loopback"); with
 *                                  --record-renderer the file is a tee of that stream from frame 1
 *   --render-process helper        the same stream into the x86_64 helper's renderer (r64_input.c starts
 *                                  the helper); the recording gets the
 *                                  helper's frame hashes; --render64-frames K: frames in flight (2)
 *   --renderer-cache on|off|check  the stream's content cache (render_wire.h; check: collision check)
 *   --renderer-hashes FILE         "<frame> <hash> <generated-frame hash>" per read-back frame
 *   --renderer-check-queries       snapshot answers compared with the live value (stale count) */
#ifndef RENDER_REC_H
#define RENDER_REC_H
#include "renderer.h"
#include "gamma_ramp.h"

extern int rr_active;
/* Parses argv[*i] (and its value) when it is one of the options above: 1 (consumed, *i on its last
 * word), -1 (bad value: message printed), 0 (not ours). */
int render_rec_option(int argc, char **argv, int *i);
/* At the runtime's exit: closes an open recording and prints its reports. */
void render_rec_exit(void);
/* With the loopback: 1 when calls of this RW_ID_* do not wait for their result (the caller gets a
 * placeholder: GPU-skinned draws); the replay does not compare them. */
int render_rec_result_later(uint32_t id);
/* Hash-ahead for the content cache (with the loopback): when
 * render_rec_prehash_on(), the renderer thread queues ranges it will pass as payloads in this frame
 * (render_rec_prehash; renderer_begin's image first, render_rec_prehash_base, which starts the worker thread
 * on the queue), closes the queue (render_rec_prehash_start) and, before any of those bytes may change or be
 * freed, waits for the worker (render_rec_prehash_end). The encoder then takes the
 * worker's hash of a payload instead of hashing it on the renderer thread. The bytes must not change
 * between render_rec_prehash and render_rec_prehash_end. */
int render_rec_prehash_on(void);
void render_rec_prehash(const void *p, size_t n);
void render_rec_prehash_base(const uint32_t *base, int pitch_bytes);   /* renderer_begin's image (its size from renderer_init) */
/* A GPU-skinned draw's 192 x 16-byte constants block, in draw order: the worker also computes its register
 * delta from the block queued before it (render_wire.h rw_enc_consts_hook). */
void render_rec_prehash_consts(const void *consts);
void render_rec_prehash_start(void);
void render_rec_prehash_end(void);

/* The helper side of --render-process helper (W6; the game side is the loopback's encoder,
 * render_rec.c): decodes the channel's CMD ring into this process's renderer on the calling thread (the helper's
 * render thread), answers on the BACK ring, until the game quits (returns 0) or is gone (4); 5 on a fatal error. */
struct R64Ipc;
typedef struct {
    /* the helper's Vulkan window (NULL create_surface: none, the renderer stays offscreen; a game that presents then
     * gets its frames read back and dropped here: the headless present) */
    const char *const *instance_ext;
    uint32_t instance_ext_count;
    int (*create_surface)(void *instance, uint64_t *surface);
    void (*drawable_size)(int *w, int *h);
    int hash;                       /* hash every frame presented headless (into the frame's ack) */
    volatile uint32_t *presented;   /* incremented after every frame shown in the window (the main thread then
                                       calls video_frame_presented: --refresh-match) */
} RenderServe;
int render_rec_serve(struct R64Ipc *x, const RenderServe *o);

#ifndef RENDER_REC_NO_ROUTE
#define RW_GEN_ROUTE
#define RW_GEN_PROTOS
#include "render_wire_gen.h"
#undef RW_GEN_ROUTE
#undef RW_GEN_PROTOS
#endif
#endif
