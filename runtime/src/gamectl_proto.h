/* Live game control (--control PATH, src/gamectl.h): the line protocol, the JSON replies, the
 * live option table and the request queue between the socket thread and the game thread. Pure
 * code (no game, no sockets): tools/gamectl_test.c covers it.
 *
 * Protocol: one command per line (at most GC_LINE_MAX bytes), one JSON object per line back:
 *   {"ok":true,"cmd":"ping","frame":4012,...}   or   {"ok":false,"cmd":"opt","error":"..."}
 * Commands:
 *   ping | status | help | opts | quit
 *   lua CHUNK                 run CHUNK in the game's Lua state ('=EXPR' is 'return EXPR')
 *   input FRAGMENT            --input-script steps with frames relative to the next frame
 *   warp TARGET               mN[:C], levelNN[:C], hideout[:C], coney, gym, title
 *   wait frames N | wait level [MAXFRAMES]
 *   capture PATH.bmp|PATH.png the next presented frame
 *   stats N                   the --stats lines of the next N presented frames
 *   opt NAME VALUE            a live option (opts lists them)
 *   save-copy load DIR        parsed, always refused (a save cannot be swapped in a running game) */
#ifndef GAMECTL_PROTO_H
#define GAMECTL_PROTO_H
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define GC_LINE_MAX 4096
#define GC_REPLY_MAX 65536
#define GC_WAIT_MAX 1000000u    /* wait frames / wait level bound */
#define GC_STATS_MAX 36000u     /* stats frames bound (10 min at 60 Hz) */
#define GC_LEVEL_WAIT_DEFAULT 7200u

typedef enum {
    GC_NONE,
    GC_PING, GC_STATUS, GC_HELP, GC_OPTS, GC_QUIT,
    GC_LUA, GC_INPUT, GC_WARP, GC_WAIT_FRAMES, GC_WAIT_LEVEL, GC_CAPTURE, GC_STATS, GC_OPT, GC_SAVE_COPY,
} GcKind;

typedef struct {
    GcKind kind;
    uint32_t n;                 /* wait frames N, wait level MAXFRAMES, stats N */
    char name[32];              /* opt NAME */
    char text[GC_LINE_MAX];     /* lua chunk, input fragment, warp target, capture path, opt value, save dir */
} GcCommand;

/* The verb as replied in "cmd" ("wait" for both waits, "?" for GC_NONE). */
const char *gc_kind_name(GcKind k);
/* Parses one line (trailing CR/LF and surrounding blanks ignored). 0, or -1 with a message in err;
 * c->kind is GC_NONE when even the verb is unknown, else the verb's kind (for the reply's "cmd"). */
int gc_parse(const char *line, GcCommand *c, char *err, size_t cap);

/* ---- JSON replies ---- */
typedef struct {
    char *buf;
    size_t cap, len;
    int overflow;
} GcJson;
/* {"ok":true|false,"cmd":"<cmd>" ... */
void gc_json_begin(GcJson *j, char *buf, size_t cap, int ok, const char *cmd);
void gc_json_str(GcJson *j, const char *key, const char *val);       /* "key":"escaped" (NULL: null) */
void gc_json_uint(GcJson *j, const char *key, uint64_t v);
void gc_json_num(GcJson *j, const char *key, double v);              /* %.6g; non-finite: null */
void gc_json_bool(GcJson *j, const char *key, int v);
void gc_json_lines(GcJson *j, const char *key, const char *text);   /* "key":["line",...] split at '\n' */
/* Closes the object with "}\n" (a reply that did not fit becomes {"ok":false,...,"error":"reply too long"}).
 * Returns the length. */
size_t gc_json_end(GcJson *j);
/* The whole error reply in one call. */
size_t gc_json_error(char *buf, size_t cap, const char *cmd, const char *error);

/* ---- live options ---- */
typedef enum { GC_OPT_UNKNOWN, GC_OPT_LIVE, GC_OPT_RESTART } GcOptClass;
typedef enum { GC_VAL_BOOL, GC_VAL_FLOAT, GC_VAL_UINT, GC_VAL_PAIR } GcValKind;
typedef struct {
    const char *name;
    GcOptClass cls;
    GcValKind val;
    float lo, hi;
    const char *what;           /* live: what it changes; restart: the start option to use */
} GcOptInfo;
extern const GcOptInfo gc_options[];
extern const int gc_option_count;
const GcOptInfo *gc_opt_find(const char *name);
/* Validates VALUE for a live option: 0 with out[0..1] filled (bool 0/1, the number, or the pair),
 * -1 with a message in err (also for restart-only and unknown names). */
int gc_opt_value(const GcOptInfo *o, const char *value, float out[2], char *err, size_t cap);

/* ---- request queue (socket thread -> game thread) ---- */
typedef struct GcRequest {
    char line[GC_LINE_MAX];
    char reply[GC_REPLY_MAX];
    size_t reply_len;
    int done;
    struct GcRequest *next;
} GcRequest;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t done_cv;
    GcRequest *head, *tail;
    int closed;
} GcQueue;

void gcq_init(GcQueue *q);
/* Socket side: queues r (r->line set) and blocks until the game thread finished it. Returns 0, or
 * -1 when the queue is closed (r->reply then holds an error reply). */
int gcq_submit(GcQueue *q, GcRequest *r);
/* Game side, non-blocking: the oldest queued request (still queued until gcq_finish), or NULL. */
GcRequest *gcq_peek(GcQueue *q);
/* Game side: r (the head) gets reply and is removed; its submitter wakes. */
void gcq_finish(GcQueue *q, GcRequest *r, const char *reply, size_t len);
/* Fails every queued request ("game exiting") and refuses later ones. */
void gcq_close(GcQueue *q);
int gcq_pending(GcQueue *q);

/* ---- capture files ---- */
/* Writes 0x00RRGGBB pixels (pitch in pixels) as a 24-bit BMP or an RGB PNG (stored deflate blocks),
 * by the path's extension (.bmp / .png, case-insensitive). 0, or -1 with a message in err. */
int gc_write_image(const char *path, const uint32_t *px, int w, int h, int pitch, char *err, size_t cap);
/* FNV-1a 64 of the pixels' low 24 bits (row by row), the frame hash in capture replies. */
uint64_t gc_frame_hash(const uint32_t *px, int w, int h, int pitch);
/* Last frame (start + duration) of an --input-script spec, 0 when it has no step or is invalid. */
uint32_t gc_script_end(const char *spec);
#endif
