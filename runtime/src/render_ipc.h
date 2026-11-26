/* render_ipc: the shared-memory channel between the 32-bit game (the parent) and the 64-bit render
 * helper.
 *
 * One mapping, created by the parent (Linux: memfd + mmap MAP_SHARED; Windows: a pagefile-backed
 * section), holds a 4 KB control block and the regions of plan 3.1: command ring, content arena,
 * back ring, stats block, input ring. The command ring (parent -> helper) and the back ring
 * (helper -> parent) are byte streams: the writer publishes its position every `chunk` bytes and on
 * r64_flush, the reader consumes in place (r64_peek / r64_consume) or by copy (r64_read). A side
 * spins `spin_us` and then sleeps on a futex word of the control block (Linux) or an inherited
 * auto-reset event (Windows), and is only woken when it said it sleeps.
 *
 * Lifecycle: r64_spawn (parent) maps the channel, starts the helper with the mapping inherited
 * (`--ipc-fd N` / `--ipc-handles ...`), and runs the handshake; r64_attach (helper) maps it from
 * its command line, checks it and answers. Both sides compare magic, version, control-block layout
 * and the wire schema hash; any mismatch is refused, with a clear log line, on both sides.
 * Death: the parent watches a pidfd (Linux) or the process handle (Windows) while it waits; the
 * helper dies with its parent (PR_SET_PDEATHSIG / a kill-on-close Job object) and also watches
 * getppid() / the parent's process handle. Every blocking call returns R64_ERR_PEER_DEAD once the
 * other side is gone; the caller prints the FAULT line (r64_peer_exit describes the exit).
 *
 * Single producer, single consumer per ring; the calls on one R64Ipc are not thread-safe. */
#ifndef RENDER_IPC_H
#define RENDER_IPC_H

#include <stddef.h>
#include <stdint.h>

#define R64_MAGIC 0x49343652u   /* "R64I" */
#define R64_VERSION 1u          /* bump on any change of the control block or of the handshake */
/* The wire schema the skeleton speaks until the wire format (W1, RW_SCHEMA) is wired in. */
#define R64_SKELETON_SCHEMA 0x52363453u

enum { R64_PARENT, R64_HELPER };

/* Return codes of every call (0 = ok). */
enum {
    R64_OK = 0,
    R64_ERR_SYS = -1,        /* an OS call failed (spawn, map, ...) */
    R64_ERR_VERSION = -2,    /* handshake refused: magic / version / layout / schema mismatch */
    R64_ERR_TIMEOUT = -3,
    R64_ERR_PEER_DEAD = -4,  /* the other process exited or died */
    R64_ERR_QUIT = -5,       /* helper: the parent asked it to quit (and the ring is drained) */
    R64_ERR_ARGS = -6,
};

/* Regions of the mapping (plan 3.1), in this order after the control block. */
enum { R64_REG_CONTROL, R64_REG_CMD, R64_REG_CONTENT, R64_REG_BACK, R64_REG_STATS, R64_REG_INPUT, R64_REG_COUNT };
#define R64_CONTROL_BYTES 4096u
#define R64_DEFAULT_CMD_BYTES (32u << 20)
#define R64_DEFAULT_CONTENT_BYTES (128u << 20)
#define R64_DEFAULT_BACK_BYTES (1u << 20)
#define R64_DEFAULT_STATS_BYTES (64u << 10)
#define R64_DEFAULT_INPUT_BYTES (64u << 10)
#define R64_DEFAULT_CHUNK (256u << 10)

/* Byte-stream rings: CMD is written by the parent, BACK by the helper. */
enum { R64_RING_CMD, R64_RING_BACK, R64_RING_COUNT };

/* Wake-up events: one per (ring, data|space), and one per side for control changes. */
enum { R64_EV_CMD_DATA, R64_EV_CMD_SPACE, R64_EV_BACK_DATA, R64_EV_BACK_SPACE, R64_EV_PARENT, R64_EV_HELPER, R64_EV_COUNT };

/* Per-side state words of the control block. */
enum { R64_STATE_NONE, R64_STATE_ALIVE, R64_STATE_QUIT, R64_STATE_DEAD };
/* The handshake word. */
enum { R64_HELLO_NONE, R64_HELLO_SENT, R64_HELLO_ACCEPTED, R64_HELLO_REFUSED };

/* Helper capabilities reported in the handshake (plan 2.1). */
#define R64_CAP_RT 1u
#define R64_CAP_PRESENT_WAIT 2u
#define R64_CAP_DLSS 4u

typedef struct { uint32_t offset, size; } R64Region;

/* The control block: the same bytes in i386 and x86_64 (only 4-byte fields, explicit 64-byte
 * alignment); the first 512 bytes (header and hello) keep their offsets in every version so that a
 * mismatched peer can still read why it was refused. */
typedef struct {
    /* 0: written by the parent before the spawn, read-only afterwards */
    uint32_t magic, version, layout, ctl_bytes;
    uint32_t total, chunk, spin_us, parent_pid;
    uint32_t schema, ptr_bits, flags, reserved0;
    R64Region region[R64_REG_COUNT];
    /* 128: the helper's answer */
    _Alignas(64) volatile uint32_t hello;
    uint32_t h_magic, h_version, h_layout, h_ctl_bytes, h_schema, h_ptr_bits, h_pid, h_caps;
    _Alignas(64) char h_device[64];
    char refuse[256];   /* 256: why the handshake was refused (by either side), NUL-terminated */
    /* 512: per-side state (R64_STATE_*), indexed by R64_PARENT / R64_HELPER */
    _Alignas(64) volatile uint32_t state[2];
    _Alignas(64) volatile uint32_t seq[R64_EV_COUNT];       /* futex words, one per event */
    _Alignas(64) volatile uint32_t waiting[R64_EV_COUNT];   /* the side sleeping on that event */
    /* ring positions (bytes, mod 2^32), each on its own cache line */
    struct {
        _Alignas(64) volatile uint32_t head;   /* writer: published bytes */
        _Alignas(64) volatile uint32_t tail;   /* reader: consumed bytes */
    } ring[R64_RING_COUNT];
    /* frame sequence numbers (the skeleton's echo protocol; W4 uses them for frames in flight) */
    _Alignas(64) volatile uint32_t frames_sent, frames_done;
} R64ControlFields;
typedef union {
    R64ControlFields f;
    uint8_t bytes[R64_CONTROL_BYTES];
} R64Control;

typedef struct R64Ipc R64Ipc;

typedef struct {
    const char *helper_path;     /* required */
    uint32_t schema;             /* the wire schema hash both sides must agree on */
    uint32_t cmd_bytes, content_bytes, back_bytes, stats_bytes, input_bytes;   /* 0 = defaults; rings: powers of two */
    uint32_t chunk_bytes;        /* 0 = R64_DEFAULT_CHUNK */
    uint32_t spin_us;            /* 0 = 50; spinning before a side sleeps */
    uint32_t handshake_ms;       /* 0 = 2000 */
    int log_fd;                  /* Linux: the helper's stdout and stderr (-1: inherited); Windows: always the pipe above */
    const char *const *extra_args;   /* NULL-terminated, appended to the helper's command line (tests) */
    uint32_t test_version;       /* tests: advertise this version instead of R64_VERSION */
    const struct R64InConfig *input;   /* the input region's window config (NULL: zeros); see r64_in_init */
} R64SpawnOpts;

typedef struct {
    uint32_t schema;             /* the schema this helper was built for */
    uint32_t caps;               /* R64_CAP_* */
    const char *device;          /* GPU name for the log ("" before the renderer exists) */
    uint32_t test_version;       /* tests: advertise this version instead of R64_VERSION */
} R64AttachOpts;

/* Parent: creates the mapping, starts the helper and runs the handshake (<= handshake_ms).
 * On failure *out is NULL, err holds one line and the helper is gone (reaped). */
int r64_spawn(const R64SpawnOpts *o, R64Ipc **out, char *err, size_t errlen);
/* Loopback (W4): the same mapping and regions, without a helper: *parent writes
 * the command ring and *helper reads it, both in this process (helper_path, extra_args and the handshake
 * are not used; the schema is recorded). Death checks never fire. Both views are freed by r64_shutdown of
 * either one (the other becomes invalid). A single thread may use both views as long as it only writes
 * what fits (r64_writable) and only reads what is there (r64_readable): nothing then waits. */
int r64_loopback(const R64SpawnOpts *o, R64Ipc **parent, R64Ipc **helper, char *err, size_t errlen);
/* Helper: maps the channel named on its command line (--ipc-fd / --ipc-handles), checks the parent's
 * header, answers the handshake and waits for its acceptance. Logs every refusal with [r64]. */
int r64_attach(int argc, char **argv, const R64AttachOpts *o, R64Ipc **out);

/* Streams. Blocking calls wait without a time limit but return R64_ERR_PEER_DEAD when the other
 * side is gone (and R64_ERR_QUIT on the helper once the parent quit and the ring is empty). */
int r64_write(R64Ipc *x, int ring, const void *p, uint32_t n);   /* copies, publishes every chunk */
int r64_flush(R64Ipc *x, int ring);                              /* publishes the rest, wakes the reader */
int r64_read(R64Ipc *x, int ring, void *dst, uint32_t n);        /* copies exactly n bytes */
/* Zero-copy read: the readable contiguous span (>= 1 byte; it ends at the ring's end). */
int r64_peek(R64Ipc *x, int ring, const void **p, uint32_t *n);
void r64_consume(R64Ipc *x, int ring, uint32_t n);
uint32_t r64_readable(R64Ipc *x, int ring);                      /* non-blocking */
uint32_t r64_writable(R64Ipc *x, int ring);                      /* non-blocking: free bytes for the writer */

/* Lifecycle. */
int r64_poll(R64Ipc *x);                       /* non-blocking: R64_OK or R64_ERR_PEER_DEAD (host loop, once per frame) */
int r64_quit_requested(R64Ipc *x);             /* helper: the parent asked to quit */
const char *r64_peer_exit(R64Ipc *x, char *buf, size_t n);   /* "signal 9" / "code 3" / "running" */
long r64_peer_pid(R64Ipc *x);
void r64_mark_dead(R64Ipc *x);                 /* this side hit a fatal error: the peer stops waiting at once */
/* Parent: asks the helper to quit, waits <= timeout_ms for its exit, kills it after that; returns
 * R64_OK when it exited by itself (*exit_code: its code, or 128 + signal). Helper: marks this side
 * as quit. Both: unmap and free x. */
int r64_shutdown(R64Ipc *x, uint32_t timeout_ms, int *exit_code);

void *r64_region(R64Ipc *x, int region, uint32_t *size);
R64Control *r64_control(R64Ipc *x);
uint64_t r64_now_ns(void);   /* monotonic, the same clock in both processes */

/* --render-process helper (W6): the game's channel to its helper, opened with the window by
 * src/r64_input.c (r64_input_open) and streamed into by src/render_rec.c; render_rec.c's options tell r64_input.c
 * what the helper does (r64_helper_render: it renders, with r64_helper_schema in the handshake; r64_helper_hash: it
 * hashes the frames it presents headless; r64_helper_frames: frames in flight). r64_fault_printed: the one
 * "=== FAULT (render64 helper exited ...)" line of a helper that died, whoever saw it first. */
extern R64Ipc *r64_shared_channel;
extern int r64_helper_render, r64_helper_hash;
extern uint32_t r64_helper_schema, r64_helper_frames;
extern int r64_fault_printed;

/* One log line (or several) with the side's prefix: "[render64] " (parent), "[r64] " (helper). */
void r64_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* The helper's output as the game prints it (Windows: r64_spawn gives the helper a pipe as
 * stdout and stderr and a thread of the game prints what comes through it with printf, which is the outlog there:
 * console and log file). Bytes in any split become complete lines without '\r' or '\n', each starting with "[r64] "
 * (added to lines the helper printed without it: the renderer's and SDL's own output); a line longer than the buffer
 * is passed in pieces. r64_lines_flush passes what is left after the last newline. */
typedef struct { char buf[1024]; uint32_t n; } R64Lines;
typedef void (*R64LineFn)(void *ctx, const char *line);
void r64_lines_feed(R64Lines *s, const char *p, size_t n, R64LineFn fn, void *ctx);
void r64_lines_flush(R64Lines *s, R64LineFn fn, void *ctx);

/* A fast streaming checksum (4 independent FNV-style lanes over 32-bit words; length a multiple of 4
 * per update, any split). The echo protocol and tests use it. */
typedef struct { uint32_t h[4]; uint32_t words; } R64Sum;
void r64_sum_init(R64Sum *s);
void r64_sum_update(R64Sum *s, const void *p, uint32_t n);
uint32_t r64_sum_final(const R64Sum *s);

/* The skeleton's echo protocol on the rings (replaced by the wire stream in W4): the parent writes
 * R64EchoFrame + `bytes` payload + uint32 checksum; the helper answers R64EchoAck on the back ring. */
#define R64_ECHO_FRAME 0x4d415246u   /* "FRAM" */
#define R64_ECHO_ACK 0x214b4341u     /* "ACK!" */
typedef struct { uint32_t tag, seq, bytes, reserved; } R64EchoFrame;
typedef struct { uint32_t tag, seq, bytes, sum, ok, t_lo, t_hi, reserved; } R64EchoAck;

/* ---- The input region ----
 * The helper owns the window, its event pump and the controllers; the game owns what input means.
 * Two lock-free single-producer single-consumer rings of fixed 64-byte records live in the input
 * region after a 1 KB header: EVENTS (helper -> game: keys, mouse, pads, window state, quit) and CMDS
 * (game -> helper: rumble, mouse capture). The parent formats the region (r64_in_init) before the
 * helper starts, with the window config the helper opens its window with; both sides check magic,
 * version and record size (r64_in_check) and refuse any other. Positions are free-running uint32
 * counters, slots counts powers of two; the producer writes the slot, then publishes the head with
 * release order; the consumer reads with acquire order and publishes the tail.
 * Overflow policy: a full ring drops the new record (counted in `dropped`) and marks the ring for a
 * resync; the next record that fits is preceded by one R64_IN_RESYNC record (v[0] = records dropped
 * so far), after which the consumer drops held keys and buttons (input_release_all) and the helper
 * sends every controller state again. A window close is also kept as the sticky `quit` word, so it
 * survives an overflow. */
#define R64_IN_MAGIC 0x4e493652u   /* "R6IN" */
#define R64_IN_VERSION 1u
#define R64_IN_HEADER_BYTES 1024u
#define R64_IN_RECORD_BYTES 64u
enum { R64_IN_EVENTS, R64_IN_CMDS, R64_IN_RING_COUNT };

/* Record types. EVENTS (helper -> game); arg and v[] per type: */
enum {
    R64_IN_NONE,
    R64_IN_RESYNC,       /* v0: records dropped so far on this ring */
    R64_IN_KEY,          /* arg: down; v0: INPUT_KEY_* key (input_map.h), v1: repeat */
    R64_IN_MOTION,       /* v0, v1: relative motion while the pointer is captured */
    R64_IN_WHEEL,        /* v0: notches */
    R64_IN_RELEASE_ALL,  /* focus lost: every key and button up */
    R64_IN_CAPTURE,      /* arg: the helper's pointer capture is now on/off (input_mouse_reset follows) */
    R64_IN_PAD,          /* arg: port; v0: 1 connected, 0 left; v1..: XPad (hle_input.h) bytes, before the remap */
    R64_IN_ESCAPE,       /* a new Escape press; arg: the pointer was captured (the game decides: menu, release, quit) */
    R64_IN_QUIT,         /* the window was closed */
    R64_IN_WINDOW,       /* v0, v1: window size, v2: R64_IN_WIN_* flags, v3: display refresh (Hz, 0 unknown) */
    R64_IN_TEST,         /* tests: arg 1 = a rumble reached a virtual controller (v0 port, v1 low, v2 high) */
    /* CMDS (game -> helper) */
    R64_IN_RUMBLE = 64,  /* arg: port; v0: low frequency motor, v1: high (0-65535) */
    R64_IN_SET_CAPTURE,  /* arg: on/off (Escape releases the pointer when no menu took it) */
};
#define R64_IN_WIN_FULLSCREEN 1u
#define R64_IN_WIN_FOCUS 2u
#define R64_IN_WIN_MINIMIZED 4u

typedef struct {
    uint32_t seq;            /* the producer's record number on this ring (0, 1, ...) */
    uint16_t type, arg;
    uint32_t t_lo, t_hi;     /* r64_now_ns() when it was written */
    uint32_t o_lo, o_hi;     /* when the input happened, if known (tests: the injection time), else 0 */
    int32_t v[10];
} R64InEvent;

/* Window config, written by the parent (R64SpawnOpts.input) and read by the helper. */
#define R64_IN_CFG_WINDOW 1u          /* run the helper's window and input pump (else: the echo skeleton) */
#define R64_IN_CFG_FULLSCREEN 2u
#define R64_IN_CFG_NO_REFRESH_MATCH 4u
#define R64_IN_CFG_VULKAN 8u          /* --render-process helper: the game presents (a Vulkan window for the helper's renderer) */
typedef struct R64InConfig {
    uint32_t flags, w, h;
    uint32_t poll_us;        /* the helper's controller poll period while one is open (0 = 1000); window input wakes it */
    char title[64];
} R64InConfig;

typedef struct {
    uint32_t magic, version, bytes, record_bytes;
    uint32_t slots[R64_IN_RING_COUNT], offset[R64_IN_RING_COUNT];
    R64InConfig cfg;
    struct {
        _Alignas(64) volatile uint32_t head;      /* producer: records published */
        _Alignas(64) volatile uint32_t tail;      /* consumer: records consumed */
        _Alignas(64) volatile uint32_t dropped;   /* producer: records dropped (full ring) */
        uint32_t resync, seq;                     /* producer only */
    } ring[R64_IN_RING_COUNT];
    _Alignas(64) volatile uint32_t quit;          /* helper: sticky window close */
    volatile uint32_t helper_state;               /* helper: 0 starting, 1 window open, 2 refused/failed */
} R64InHeader;

/* Parent: formats size bytes at region (>= 4 KB) with cfg (NULL: zeros). R64_OK or R64_ERR_ARGS. */
int r64_in_init(void *region, uint32_t size, const R64InConfig *cfg);
/* Either side: R64_OK, or R64_ERR_VERSION with the reason in why. */
int r64_in_check(const void *region, uint32_t size, char *why, size_t n);
/* Producer of `ring` (EVENTS: the helper, CMDS: the game): 1 written, 0 dropped (full, see above).
 * n <= sizeof v bytes of payload; origin_ns: when the input happened (0 unknown). */
int r64_in_put(void *region, int ring, uint16_t type, uint16_t arg, const void *payload, uint32_t n, uint64_t origin_ns);
/* Consumer: 1 and *out filled, 0 when empty. */
int r64_in_get(void *region, int ring, R64InEvent *out);
uint32_t r64_in_dropped(const void *region, int ring);

#endif
