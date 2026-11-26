/* Live game control: --control, --control-idle, --fast-boot (see gamectl.h). */
#include "gamectl.h"
#include "gamectl_proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
int gamectl_parse_arg(int argc, char **argv, int *i, char *err, size_t cap) {
    const char *a = argv[*i];
    if (strcmp(a, "--control") && strcmp(a, "--control-idle") && strcmp(a, "--fast-boot")) return 0;
    snprintf(err, cap, "%s: live game control is for Linux developer builds only", a);
    return -1;
}
int gamectl_wanted(void) { return 0; }
void gamectl_boot_plan(uint32_t input_end, int need_lua, int need_level) {}
int gamectl_install(char *err, size_t cap) { return 0; }
void gamectl_level_loaded(const char *level) {}
#else

#include "hle_d3d.h"
#include "hle_devlua.h"
#include "hle_goto.h"
#include "hle_photo.h"
#include "hle_input.h"
#include "hle_warp.h"
#include "hle_xapi.h"
#include "platform.h"
#include "renderer.h"
#include "render_rec.h"   /* its renderer_* calls go through the recorder / loopback wrappers */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define FAST_BOOT_CAP 30000u          /* frames: --fast-boot ends here whatever happened */
#define LUA_WAIT_FRAMES 3600u         /* a chunk waits this long for a lua_call (cutscenes run none for 10+ s) */
#define CAPTURE_WAIT_FRAMES 300u
#define STALL_SECONDS 120             /* watchdog: no Swap for this long ends the process */
#define FIRST_FRAME_SECONDS 300       /* ... and before the first Swap */
#define QUIT_GRACE_SECONDS 30         /* a quit that did not end the process by then: _exit */
#define CLIENT_TIMEOUT_SECONDS 60     /* a client that sends nothing for this long is dropped */

static const char *opt_path;
static int opt_fast, opt_idle = 600;
static struct { uint32_t input_end; int need_lua, need_level; } boot = {0, 0, 1};

static double t_start;
static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static int64_t now_ms(void) { return (int64_t)(now_s() * 1000); }

/* ---- state shared between threads (atomics or under state_lock) ---- */
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static char level_name[64];             /* the last level load request */
static uint32_t level_gen;              /* level load requests seen ("level..." names) */
static uint32_t ready_gen;              /* level_gen whose first frame was presented */
static uint32_t ready_frame;            /* ... at this Swap */
static int fast_active, turbo;
static uint32_t cur_frame;
static int64_t last_swap;               /* now_ms of the last Swap, 0 before the first (watchdog) */
static int64_t last_activity;           /* the last client connect / reply */
static int busy;                        /* the socket thread serves a client */
static int quit_requested;
static int64_t quit_at;
static int lua_seen;                    /* devlua_host_tick ran at least once */

static GcQueue queue;

/* ---- options ---- */

int gamectl_parse_arg(int argc, char **argv, int *i, char *err, size_t cap) {
    const char *a = argv[*i];
    if (!strcmp(a, "--fast-boot")) { opt_fast = 1; return 1; }
    if (strcmp(a, "--control") && strcmp(a, "--control-idle")) return 0;
    if (*i + 1 >= argc) { snprintf(err, cap, "%s: missing value", a); return -1; }
    const char *v = argv[++*i];
    if (!strcmp(a, "--control")) {
        if (!*v || strlen(v) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
            snprintf(err, cap, "--control %s: the socket path must be 1-%zu characters", v, sizeof(((struct sockaddr_un *)0)->sun_path) - 1);
            return -1;
        }
        opt_path = v;
        return 1;
    }
    char *end;
    long s = strtol(v, &end, 10);
    if (!*v || *end || s < 10 || s > 86400) { snprintf(err, cap, "--control-idle %s: seconds, 10-86400", v); return -1; }
    opt_idle = (int)s;
    return 1;
}

int gamectl_wanted(void) { return opt_path || opt_fast; }

void gamectl_boot_plan(uint32_t input_end, int need_lua, int need_level) {
    boot.input_end = input_end;
    boot.need_lua = need_lua;
    boot.need_level = need_level;
}

void gamectl_level_loaded(const char *level) {
    if (!level || strncmp(level, "level", 5)) return;
    pthread_mutex_lock(&state_lock);
    snprintf(level_name, sizeof level_name, "%s", level);
    __atomic_add_fetch(&level_gen, 1, __ATOMIC_ACQ_REL);
    pthread_mutex_unlock(&state_lock);
}

static void get_level(char *out, size_t cap) {
    pthread_mutex_lock(&state_lock);
    snprintf(out, cap, "%s", level_name);
    pthread_mutex_unlock(&state_lock);
}

static int level_ready(void) {
    uint32_t g = __atomic_load_n(&level_gen, __ATOMIC_ACQUIRE);
    return g && __atomic_load_n(&ready_gen, __ATOMIC_ACQUIRE) == g;
}

/* ---- Lua (run at the lua_call hook) ---- */
enum { LUA_IDLE, LUA_WANTED, LUA_RUNNING, LUA_DONE };
static pthread_mutex_t lua_lock = PTHREAD_MUTEX_INITIALIZER;
static int lua_state;
static char lua_code[GC_LINE_MAX + 512], lua_res[2048], lua_err[512];
static int lua_status;
static const char *lua_job;             /* a --goto Lua step to queue (warp coney / gym) */
static char lua_job_name[32];
static uint32_t lua_seq;

static void host_tick(const DevLuaApi *api, uint32_t L, uint32_t frame) {
    __atomic_store_n(&lua_seen, 1, __ATOMIC_RELEASE);
    pthread_mutex_lock(&lua_lock);
    if (lua_job) {   /* inside devlua_tick after its job loop: devlua_add is safe here */
        devlua_add(lua_job, strlen(lua_job), lua_job_name, 0, 1);
        lua_job = NULL;
    }
    if (lua_state != LUA_WANTED) { pthread_mutex_unlock(&lua_lock); return; }
    lua_state = LUA_RUNNING;
    char name[32];
    snprintf(name, sizeof name, "ctl%u", ++lua_seq);
    pthread_mutex_unlock(&lua_lock);
    char res[sizeof lua_res], err[sizeof lua_err];
    err[0] = 0;
    int st = devlua_run_result(api, L, lua_code, strlen(lua_code), name, res, sizeof res, err, sizeof err);
    pthread_mutex_lock(&lua_lock);
    memcpy(lua_res, res, sizeof res);
    memcpy(lua_err, err, sizeof err);
    lua_status = st;
    lua_state = LUA_DONE;
    pthread_mutex_unlock(&lua_lock);
}

static void lua_request(const char *code) {
    pthread_mutex_lock(&lua_lock);
    snprintf(lua_code, sizeof lua_code, "%s", code);
    lua_state = LUA_WANTED;
    pthread_mutex_unlock(&lua_lock);
}

/* LUA_DONE: copies the result and goes idle. */
static int lua_poll(char *res, size_t rcap, char *err, size_t ecap, int *st) {
    pthread_mutex_lock(&lua_lock);
    int done = lua_state == LUA_DONE;
    if (done) {
        snprintf(res, rcap, "%s", lua_res);
        snprintf(err, ecap, "%s", lua_err);
        *st = lua_status;
        lua_state = LUA_IDLE;
    }
    pthread_mutex_unlock(&lua_lock);
    return done;
}

static void lua_cancel(void) {
    pthread_mutex_lock(&lua_lock);
    if (lua_state == LUA_WANTED) lua_state = LUA_IDLE;
    pthread_mutex_unlock(&lua_lock);
}

/* ---- host thread: level-ready detection, captures, renderer options ---- */
enum { CAP_IDLE, CAP_WANTED, CAP_DONE, CAP_FAILED };
static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
static int cap_state, cap_w, cap_h;
static uint32_t cap_seq;
static uint64_t cap_hash;
static char cap_path[GC_LINE_MAX], cap_err[512];
static int hopt_state;                  /* 1: hopt_name / hopt_v to apply, 2: applied */
static char hopt_name[32];
static float hopt_v;

static void host_frame(const uint32_t *px, uint32_t w, uint32_t h, uint32_t seq, uint32_t draws, uint32_t movie) {
    static uint32_t seen_gen, skip;
    const uint32_t g = __atomic_load_n(&level_gen, __ATOMIC_ACQUIRE);
    /* A level's first frame: drawn, no movie, no load pending, a few frames after the request
     * (frames queued before it may still be in flight). */
    if (g != seen_gen) seen_gen = g, skip = 3;
    else if (skip) skip--;
    else if (g && __atomic_load_n(&ready_gen, __ATOMIC_ACQUIRE) != g && draws && !movie && !warp_level_pending()) {
        __atomic_store_n(&ready_frame, hle_d3d_frame_count(), __ATOMIC_RELEASE);
        __atomic_store_n(&ready_gen, g, __ATOMIC_RELEASE);
    }
    pthread_mutex_lock(&host_lock);
    if (cap_state == CAP_WANTED) {
        if (!px) {
            snprintf(cap_err, sizeof cap_err, "this frame went to a window: capture needs the readback present (--video null or --present readback)");
            cap_state = CAP_FAILED;
        } else if (gc_write_image(cap_path, px, (int)w, (int)h, (int)w, cap_err, sizeof cap_err)) {
            cap_state = CAP_FAILED;
        } else {
            cap_w = (int)w, cap_h = (int)h, cap_seq = seq;
            cap_hash = gc_frame_hash(px, (int)w, (int)h, (int)w);
            cap_state = CAP_DONE;
        }
    }
    if (hopt_state == 1) {
        if (!strcmp(hopt_name, "taa-sharpness")) renderer_taa_sharpness(hopt_v);
        else if (!strcmp(hopt_name, "parallax")) renderer_set_parallax(hopt_v);
        hopt_state = 2;
    }
    pthread_mutex_unlock(&host_lock);
}

/* ---- the executor (game thread, at Swap) ---- */
static struct {
    GcRequest *r;
    GcCommand c;
    uint32_t start, until, gen0;
    int stage;
    long stats_off;
    GotoPlan plan;
    char level[64];
    int checkpoint;
} ex;

static char reply[GC_REPLY_MAX];

static void finish(size_t n) {
    gcq_finish(&queue, ex.r, reply, n);
    ex.r = NULL;
}
static void finish_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void finish_error(const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    finish(gc_json_error(reply, sizeof reply, gc_kind_name(ex.c.kind), msg));
}

static void status_fields(GcJson *j, uint32_t n) {
    char lv[64];
    get_level(lv, sizeof lv);
    gc_json_uint(j, "frame", n);
    gc_json_str(j, "level", lv[0] ? lv : NULL);
    gc_json_bool(j, "level_ready", level_ready());
}

static const char HELP[] =
    "ping | status | help | opts | quit\n"
    "lua CHUNK                  run CHUNK in the game's Lua state (=EXPR returns EXPR)\n"
    "input FRAGMENT             --input-script steps, frames from the next frame (0:a:6,30:start)\n"
    "warp TARGET                mN[:C], levelNN[:C], hideout[:C], coney, gym, title; replies at the level's first frame\n"
    "wait frames N              reply after N presented frames\n"
    "wait level [MAXFRAMES]     reply once a level's first frame was presented (at once when it was)\n"
    "capture PATH.bmp|PATH.png  write the next presented frame; the reply has its hash\n"
    "photo enter|exit|status|capture [PATH.png]|set KEY VALUE|move R U F [YAW PITCH ROLL ZOOM]  photo mode (src/hle_photo.h)\n"
    "stats N                    the --stats lines of the next N frames\n"
    "opt NAME VALUE             change a live option (opts lists them)\n";

/* Warp target -> level, checkpoint (0 = keep) and a --goto Lua step. 0, or -1 with err. */
static int warp_target(const char *t, char *err, size_t cap) {
    memset(&ex.plan, 0, sizeof ex.plan);
    ex.checkpoint = 0;
    ex.level[0] = 0;
    if (!strcmp(t, "title")) { snprintf(ex.level, sizeof ex.level, "menu"); return 0; }
    if (!strcmp(t, "rumble")) { snprintf(err, cap, "warp rumble: the Rumble menu needs the menu input; start a session with --goto rumble"); return -1; }
    char gerr[192];
    GotoPlan p;
    if (!goto_parse(t, &p, gerr, sizeof gerr) && strcmp(p.place, "title")) {
        ex.plan = p;
        if (p.warp_on) {
            snprintf(ex.level, sizeof ex.level, "%s", p.warp.level->level);
            ex.checkpoint = p.warp.checkpoint;
        } else snprintf(ex.level, sizeof ex.level, "level95");
        return 0;
    }
    WarpTarget w;
    if (!warp_parse(t, &w, gerr, sizeof gerr)) {
        snprintf(ex.level, sizeof ex.level, "%s", w.level->level);
        ex.checkpoint = strchr(t, ':') ? w.checkpoint : 0;
        return 0;
    }
    snprintf(err, cap, "warp %s: %s", t, gerr);
    return -1;
}

static int stdout_size(long *off) {
    struct stat st;
    fflush(stdout);
    if (fstat(1, &st) || !S_ISREG(st.st_mode)) return -1;
    *off = (long)st.st_size;
    return 0;
}

/* The --stats lines written to standard output since off. */
static size_t stats_reply(void) {
    static char text[GC_REPLY_MAX - 1024];
    size_t n = 0;
    fflush(stdout);
    FILE *f = fopen("/proc/self/fd/1", "rb");
    if (f && !fseek(f, ex.stats_off, SEEK_SET)) {
        char line[2048];
        while (fgets(line, sizeof line, f)) {
            static const char *const pre[] = {"[stats]", "[pacing]", "[latency]", "[gpu-passes]", "[fx]", "[pipeline]", "[hitch]"};
            int keep = 0;
            for (size_t i = 0; i < sizeof pre / sizeof pre[0]; i++) keep |= !strncmp(line, pre[i], strlen(pre[i]));
            size_t l = strlen(line);
            if (keep && n + l + 1 < sizeof text) memcpy(text + n, line, l), n += l;
        }
    }
    if (f) fclose(f);
    text[n] = 0;
    GcJson j;
    gc_json_begin(&j, reply, sizeof reply, 1, "stats");
    gc_json_uint(&j, "from", ex.start);
    gc_json_uint(&j, "frames", ex.c.n);
    gc_json_lines(&j, "lines", text);
    return gc_json_end(&j);
}

/* Starts ex.c; returns 1 when it finished at once. */
static int start(uint32_t n) {
    char err[512];
    GcJson j;
    ex.start = n;
    ex.stage = 0;
    if (gc_parse(ex.r->line, &ex.c, err, sizeof err)) { finish_error("%s", err); return 1; }
    switch (ex.c.kind) {
    case GC_PING:
        gc_json_begin(&j, reply, sizeof reply, 1, "ping");
        status_fields(&j, n);
        finish(gc_json_end(&j));
        return 1;
    case GC_STATUS:
        gc_json_begin(&j, reply, sizeof reply, 1, "status");
        status_fields(&j, n);
        gc_json_uint(&j, "level_frame", __atomic_load_n(&ready_frame, __ATOMIC_ACQUIRE));
        gc_json_bool(&j, "fast_boot", fast_active);
        gc_json_bool(&j, "turbo", turbo);
        gc_json_bool(&j, "lua_hook", __atomic_load_n(&lua_seen, __ATOMIC_ACQUIRE));
        gc_json_bool(&j, "stats", hle_d3d_stats);
        gc_json_uint(&j, "queued", (uint64_t)gcq_pending(&queue));
        gc_json_uint(&j, "idle_timeout", (uint64_t)__atomic_load_n(&opt_idle, __ATOMIC_ACQUIRE));
        gc_json_num(&j, "uptime", now_s() - t_start);
        gc_json_uint(&j, "pid", (uint64_t)getpid());
        finish(gc_json_end(&j));
        return 1;
    case GC_HELP:
        gc_json_begin(&j, reply, sizeof reply, 1, "help");
        gc_json_lines(&j, "lines", HELP);
        finish(gc_json_end(&j));
        return 1;
    case GC_OPTS: {
        char text[4096];
        size_t k = 0;
        for (int i = 0; i < gc_option_count && k < sizeof text - 200; i++)
            k += (size_t)snprintf(text + k, sizeof text - k, "%s %s: %s%s\n", gc_options[i].name,
                                  gc_options[i].cls == GC_OPT_LIVE ? "(live)" : "(start option)",
                                  gc_options[i].cls == GC_OPT_LIVE ? "" : "restart the session with ", gc_options[i].what);
        gc_json_begin(&j, reply, sizeof reply, 1, "opts");
        gc_json_lines(&j, "lines", text);
        finish(gc_json_end(&j));
        return 1;
    }
    case GC_QUIT:   /* the socket thread asks for the exit once this reply is sent */
        gc_json_begin(&j, reply, sizeof reply, 1, "quit");
        gc_json_uint(&j, "frame", n);
        finish(gc_json_end(&j));
        return 1;
    case GC_SAVE_COPY:
        finish_error("not feasible: a running game cannot swap its save (the profile is read through the menus at boot); "
                     "start a new session with that save (xbctl start --save %s)", ex.c.text);
        return 1;
    case GC_INPUT: {
        uint32_t end;
        if (input_script_append(ex.c.text, n + 1, &end, err, sizeof err)) { finish_error("input: %s", err); return 1; }
        gc_json_begin(&j, reply, sizeof reply, 1, "input");
        gc_json_uint(&j, "from", n + 1);
        gc_json_uint(&j, "until", end);
        finish(gc_json_end(&j));
        return 1;
    }
    case GC_OPT: {
        const GcOptInfo *o = gc_opt_find(ex.c.name);
        float v[2];
        if (!o) { finish_error("unknown option '%s' (opts lists them)", ex.c.name); return 1; }
        if (gc_opt_value(o, ex.c.text, v, err, sizeof err)) { finish_error("%s", err); return 1; }
        if (!strcmp(o->name, "taa-sharpness") || !strcmp(o->name, "parallax")) {
            pthread_mutex_lock(&host_lock);
            snprintf(hopt_name, sizeof hopt_name, "%s", o->name);
            hopt_v = v[0];
            hopt_state = 1;
            pthread_mutex_unlock(&host_lock);
            ex.until = n + CAPTURE_WAIT_FRAMES;
            return 0;   /* replies once the host thread applied it */
        }
        if (!strcmp(o->name, "turbo")) turbo = v[0] != 0;
        else if (!strcmp(o->name, "tri-check")) hle_d3d_tri_check = v[0] != 0;
        else if (!strcmp(o->name, "spin")) input_set_spin(v[0] != 0);
        else if (!strcmp(o->name, "mash")) input_set_mash(v[0] != 0);
        else if (!strcmp(o->name, "right-stick")) input_set_auto_stick((int16_t)lrintf(v[0] * 32767), (int16_t)lrintf(v[1] * 32767));
        else if (!strcmp(o->name, "idle-timeout")) __atomic_store_n(&opt_idle, (int)v[0], __ATOMIC_RELEASE);
        gc_json_begin(&j, reply, sizeof reply, 1, "opt");
        gc_json_str(&j, "name", o->name);
        gc_json_str(&j, "value", ex.c.text);
        finish(gc_json_end(&j));
        return 1;
    }
    case GC_LUA:
        lua_request(ex.c.text);
        ex.until = n + LUA_WAIT_FRAMES;
        return 0;
    case GC_WARP: {
        if (warp_target(ex.c.text, err, sizeof err)) { finish_error("%s", err); return 1; }
        char code[512];
        int k = snprintf(code, sizeof code, "if SoundPauseSound then SoundPauseSound(false) end if SoundStopMusicTrack then SoundStopMusicTrack() end ");
        if (ex.checkpoint) k += snprintf(code + k, sizeof code - (size_t)k, "SetCheckPoint(%d) ", ex.checkpoint);
        snprintf(code + k, sizeof code - (size_t)k, "MenuLoadLevel(\"%s\")", ex.level);
        ex.gen0 = __atomic_load_n(&level_gen, __ATOMIC_ACQUIRE);
        lua_request(code);
        ex.until = n + LUA_WAIT_FRAMES;
        return 0;
    }
    case GC_WAIT_FRAMES:
        ex.until = n + ex.c.n;
        return 0;
    case GC_WAIT_LEVEL:
        ex.until = n + ex.c.n;
        return 0;
    case GC_CAPTURE:
        pthread_mutex_lock(&host_lock);
        snprintf(cap_path, sizeof cap_path, "%s", ex.c.text);
        cap_state = CAP_WANTED;
        pthread_mutex_unlock(&host_lock);
        ex.until = n + CAPTURE_WAIT_FRAMES;
        return 0;
    case GC_STATS:
        if (!hle_d3d_stats) { finish_error("start the session with --stats (xbctl start adds it)"); return 1; }
        if (stdout_size(&ex.stats_off)) { finish_error("standard output is not a file: stats reads the --stats lines from the log"); return 1; }
        ex.until = n + ex.c.n;
        return 0;
    default:
        finish_error("unknown command");
        return 1;
    }
}

/* One frame of the running command; returns 1 when it finished. */
static int step(uint32_t n) {
    GcJson j;
    char res[2048], err[512];
    int st;
    switch (ex.c.kind) {
    case GC_LUA:
        if (lua_poll(res, sizeof res, err, sizeof err, &st)) {
            if (st) { finish_error("%s", err[0] ? err : "Lua error"); return 1; }
            gc_json_begin(&j, reply, sizeof reply, 1, "lua");
            gc_json_str(&j, "result", res);
            gc_json_uint(&j, "frame", n);
            finish(gc_json_end(&j));
            return 1;
        }
        if (n >= ex.until) {
            lua_cancel();
            finish_error("no lua_call in %u frames (%s)", LUA_WAIT_FRAMES,
                         __atomic_load_n(&lua_seen, __ATOMIC_ACQUIRE) ? "the game is not running Lua now" : "the Lua hook is not installed");
            return 1;
        }
        return 0;
    case GC_WARP:
        if (ex.stage == 0) {
            if (lua_poll(res, sizeof res, err, sizeof err, &st)) {
                if (st) { finish_error("warp: %s", err[0] ? err : "Lua error"); return 1; }
                ex.stage = 1;
                ex.until = n + GC_LEVEL_WAIT_DEFAULT;
                return 0;
            }
            if (n >= ex.until) { lua_cancel(); finish_error("warp: no lua_call in %u frames", LUA_WAIT_FRAMES); return 1; }
            return 0;
        }
        if (ex.stage == 1) {
            uint32_t g = __atomic_load_n(&level_gen, __ATOMIC_ACQUIRE);
            if (g != ex.gen0 && level_ready()) {
                if (!ex.plan.lua) ex.stage = 3;
                else {
                    pthread_mutex_lock(&lua_lock);
                    lua_job = ex.plan.lua;
                    snprintf(lua_job_name, sizeof lua_job_name, "warp:%s", ex.plan.place);
                    pthread_mutex_unlock(&lua_lock);
                    ex.stage = 2;
                    return 0;
                }
            } else if (n >= ex.until) { finish_error("warp: %s not ready after %u frames", ex.level, GC_LEVEL_WAIT_DEFAULT); return 1; }
        }
        if (ex.stage == 2) {
            pthread_mutex_lock(&lua_lock);
            int queued = lua_job != NULL;
            pthread_mutex_unlock(&lua_lock);
            if (queued || devlua_pending()) {
                if (n >= ex.until) { finish_error("warp: the %s Lua step did not finish", ex.plan.place); return 1; }
                return 0;
            }
            ex.stage = 3;
        }
        if (ex.stage == 3) {
            char lv[64];
            get_level(lv, sizeof lv);
            gc_json_begin(&j, reply, sizeof reply, 1, "warp");
            gc_json_str(&j, "level", lv);
            if (ex.checkpoint) gc_json_uint(&j, "checkpoint", (uint64_t)ex.checkpoint);
            gc_json_uint(&j, "frame", n);
            gc_json_uint(&j, "frames", n - ex.start);
            finish(gc_json_end(&j));
            return 1;
        }
        return 0;
    case GC_WAIT_FRAMES:
        if (n < ex.until) return 0;
        gc_json_begin(&j, reply, sizeof reply, 1, "wait");
        gc_json_uint(&j, "frame", n);
        finish(gc_json_end(&j));
        return 1;
    case GC_WAIT_LEVEL:
        if (level_ready()) {
            gc_json_begin(&j, reply, sizeof reply, 1, "wait");
            status_fields(&j, n);
            gc_json_uint(&j, "level_frame", __atomic_load_n(&ready_frame, __ATOMIC_ACQUIRE));
            finish(gc_json_end(&j));
            return 1;
        }
        if (n >= ex.until) { finish_error("no level ready after %u frames", ex.c.n); return 1; }
        return 0;
    case GC_CAPTURE:
    case GC_OPT: {
        pthread_mutex_lock(&host_lock);
        int cs = cap_state, hs = hopt_state;
        if (ex.c.kind == GC_CAPTURE && (cs == CAP_DONE || cs == CAP_FAILED)) cap_state = CAP_IDLE;
        if (ex.c.kind == GC_OPT && hs == 2) hopt_state = 0;
        pthread_mutex_unlock(&host_lock);
        if (ex.c.kind == GC_OPT) {
            if (hs == 2) {
                gc_json_begin(&j, reply, sizeof reply, 1, "opt");
                gc_json_str(&j, "name", ex.c.name);
                gc_json_str(&j, "value", ex.c.text);
                finish(gc_json_end(&j));
                return 1;
            }
            if (n >= ex.until) {
                pthread_mutex_lock(&host_lock);
                hopt_state = 0;
                pthread_mutex_unlock(&host_lock);
                finish_error("no frame was presented to apply %s", ex.c.name);
                return 1;
            }
            return 0;
        }
        if (cs == CAP_FAILED) { finish_error("capture: %s", cap_err); return 1; }
        if (cs == CAP_DONE) {
            char hash[24];
            snprintf(hash, sizeof hash, "%016llx", (unsigned long long)cap_hash);
            gc_json_begin(&j, reply, sizeof reply, 1, "capture");
            gc_json_str(&j, "path", cap_path);
            gc_json_uint(&j, "width", (uint64_t)cap_w);
            gc_json_uint(&j, "height", (uint64_t)cap_h);
            gc_json_uint(&j, "frame", cap_seq);
            gc_json_str(&j, "hash", hash);
            finish(gc_json_end(&j));
            return 1;
        }
        if (n >= ex.until) {
            pthread_mutex_lock(&host_lock);
            cap_state = CAP_IDLE;
            pthread_mutex_unlock(&host_lock);
            finish_error("capture: no frame presented in %u frames", CAPTURE_WAIT_FRAMES);
            return 1;
        }
        return 0;
    }
    case GC_STATS:
        if (n < ex.until) return 0;
        finish(stats_reply());
        return 1;
    default:
        finish_error("unknown command");
        return 1;
    }
}

static void fast_boot_step(uint32_t n) {
    if (!fast_active) return;
    const int lvl = !boot.need_level || level_ready();
    const int in = n > boot.input_end;
    const int lua = !boot.need_lua || !devlua_pending();
    if ((lvl && in && lua) || n >= FAST_BOOT_CAP) {
        fast_active = 0;
        char lv[64];
        get_level(lv, sizeof lv);
        printf("[fast-boot] %s at frame %u, %.1f s after start: normal pacing from here\n",
               n >= FAST_BOOT_CAP && !(lvl && in && lua) ? "gave up (no level ready)" : lv[0] ? lv : "front end", n, now_s() - t_start);
        fflush(stdout);
    }
}

static int pace(uint32_t n) {
    static uint32_t reported_gen;
    __atomic_store_n(&cur_frame, n, __ATOMIC_RELEASE);
    __atomic_store_n(&last_swap, now_ms(), __ATOMIC_RELEASE);
    if (__atomic_load_n(&quit_requested, __ATOMIC_ACQUIRE) && (!hle_d3d_frame_limit || hle_d3d_frame_limit > (int)n + 1))
        hle_d3d_frame_limit = (int)n + 1;   /* the normal exit path at the next Swap */
    const uint32_t rg = __atomic_load_n(&ready_gen, __ATOMIC_ACQUIRE);
    if (rg != reported_gen) {
        reported_gen = rg;
        char lv[64];
        get_level(lv, sizeof lv);
        printf("[control] %s ready at frame %u, %.1f s after start\n", lv, __atomic_load_n(&ready_frame, __ATOMIC_ACQUIRE), now_s() - t_start);
        fflush(stdout);
    }
    fast_boot_step(n);
    if (opt_path)
        for (int k = 0; k < 16; k++) {   /* commands that finish at once: several per frame */
            if (!ex.r) {
                ex.r = gcq_peek(&queue);
                if (!ex.r) break;
                if (!start(n)) break;
            } else if (!step(n)) break;
        }
    hle_xapi_unthrottled = fast_active || turbo;
    return fast_active || turbo;
}

/* ---- socket thread ---- */
static int listen_fd = -1;

static void request_quit(const char *why) {
    if (__atomic_exchange_n(&quit_requested, 1, __ATOMIC_ACQ_REL)) return;
    __atomic_store_n(&quit_at, now_ms(), __ATOMIC_RELEASE);
    if (opt_path) unlink(opt_path);
    printf("[control] %s: quitting\n", why);
    fflush(stdout);
}

static void send_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return;
        }
        p += w, n -= (size_t)w;
    }
}

static void serve(int fd) {
    static GcRequest req;
    static char buf[GC_LINE_MAX * 2];
    size_t have = 0;
    struct timeval tv = {CLIENT_TIMEOUT_SECONDS, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    for (;;) {
        char *nl = memchr(buf, '\n', have);
        if (!nl) {
            if (have == sizeof buf) {   /* a line longer than the buffer: refuse it and drop the client */
                char r[256];
                size_t n = gc_json_error(r, sizeof r, "?", "line too long");
                send_all(fd, r, n);
                return;
            }
            ssize_t got = recv(fd, buf + have, sizeof buf - have, 0);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) return;
            have += (size_t)got;
            continue;
        }
        *nl = 0;
        size_t len = (size_t)(nl - buf);
        if (len >= sizeof req.line) len = sizeof req.line - 1;
        memcpy(req.line, buf, len);
        req.line[len] = 0;
        have -= (size_t)(nl + 1 - buf);
        memmove(buf, nl + 1, have);
        if (!req.line[0] || (req.line[0] == '\r' && !req.line[1])) continue;
        if (!strncmp(req.line, "photo", 5) && (!req.line[5] || req.line[5] == ' ')) {
            /* photo mode (hle_photo.h): answered here, not at a Swap (the game is frozen in photo mode) */
            req.line[strcspn(req.line, "\r")] = 0;
            req.reply_len = hle_photo_command(req.line + 5, req.reply, sizeof req.reply);
        } else {
            if (!strncmp(req.line, "quit", 4) && hle_photo_active()) {   /* the quit runs at a Swap: let the game go on first */
                char r[256];
                hle_photo_command(" exit", r, sizeof r);
            }
            gcq_submit(&queue, &req);
        }
        send_all(fd, req.reply, req.reply_len);
        __atomic_store_n(&last_activity, now_ms(), __ATOMIC_RELEASE);
        GcCommand c;
        char err[8];
        if (!gc_parse(req.line, &c, err, sizeof err) && c.kind == GC_QUIT) {
            request_quit("quit command");
            return;
        }
    }
}

static void *server(void *arg) {
    plat_thread_init();
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            printf("[control] accept failed: %s; the socket is closed\n", strerror(errno));
            fflush(stdout);
            return NULL;
        }
        __atomic_store_n(&busy, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&last_activity, now_ms(), __ATOMIC_RELEASE);
        if (!__atomic_load_n(&quit_requested, __ATOMIC_ACQUIRE)) serve(fd);
        else send_all(fd, "{\"ok\":false,\"cmd\":\"?\",\"error\":\"the game is exiting\"}\n", 52);
        close(fd);
        __atomic_store_n(&last_activity, now_ms(), __ATOMIC_RELEASE);
        __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
    }
}

/* Idle quit, stalled game, unfinished quit. Blocks only in its 1 s sleep. */
static void *watchdog(void *arg) {
    plat_thread_init();
    for (;;) {
        struct timespec d = {1, 0};
        nanosleep(&d, NULL);
        const int64_t t = now_ms(), swap = __atomic_load_n(&last_swap, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&quit_requested, __ATOMIC_ACQUIRE)) {
            if (t - __atomic_load_n(&quit_at, __ATOMIC_ACQUIRE) > QUIT_GRACE_SECONDS * 1000) {
                printf("[control] the game did not exit %d s after the quit: ending the process\n", QUIT_GRACE_SECONDS);
                fflush(stdout);
                _exit(0);
            }
            continue;
        }
        if (hle_photo_active()) {   /* photo mode holds the game at a Swap on purpose */
            __atomic_store_n(&last_swap, t, __ATOMIC_RELEASE);
            continue;
        }
        if ((swap && t - swap > STALL_SECONDS * 1000) || (!swap && t - (int64_t)(t_start * 1000) > FIRST_FRAME_SECONDS * 1000)) {
            if (opt_path) unlink(opt_path);
            printf("[control] no frame for %d s: ending the process\n", swap ? STALL_SECONDS : FIRST_FRAME_SECONDS);
            fflush(stdout);
            _exit(3);
        }
        if (opt_path && !__atomic_load_n(&busy, __ATOMIC_ACQUIRE) &&
            t - __atomic_load_n(&last_activity, __ATOMIC_ACQUIRE) > (int64_t)__atomic_load_n(&opt_idle, __ATOMIC_ACQUIRE) * 1000) {
            char why[64];
            snprintf(why, sizeof why, "no command for %d s", __atomic_load_n(&opt_idle, __ATOMIC_ACQUIRE));
            request_quit(why);
        }
    }
}

int gamectl_install(char *err, size_t cap) {
    if (!gamectl_wanted()) return 0;
    t_start = now_s();
    last_activity = now_ms();
    fast_active = opt_fast;
    hle_xapi_unthrottled = fast_active;
    hle_d3d_pace_hook = pace;
    hle_d3d_host_frame_hook = host_frame;
    if (opt_path) devlua_host_tick = host_tick;
    if (opt_fast)
        printf("[fast-boot] on: unpaced until %s (menu input to frame %u%s)\n", boot.need_level ? "the first level frame" : "the menu input ends",
               boot.input_end, boot.need_lua ? ", then the --goto Lua step" : "");
    if (opt_path) {
        gcq_init(&queue);
        struct sockaddr_un a = {0};
        a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof a.sun_path, "%s", opt_path);
        listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd < 0) { snprintf(err, cap, "--control: socket: %s", strerror(errno)); return -1; }
        struct stat st;
        if (!lstat(opt_path, &st) && S_ISSOCK(st.st_mode)) unlink(opt_path);   /* a stale socket of an ended session */
        mode_t old = umask(077);
        int b = bind(listen_fd, (struct sockaddr *)&a, sizeof a);
        umask(old);
        if (b || listen(listen_fd, 8)) {
            snprintf(err, cap, "--control %s: %s", opt_path, strerror(errno));
            close(listen_fd);
            return -1;
        }
        pthread_t t;
        if (pthread_create(&t, NULL, server, NULL)) { snprintf(err, cap, "--control: cannot start the socket thread"); return -1; }
        pthread_detach(t);
        printf("[control] listening on %s (idle quit after %d s)\n", opt_path, opt_idle);
    }
    pthread_t w;   /* sessions only: a plain --fast-boot run keeps the usual behaviour */
    if (opt_path && pthread_create(&w, NULL, watchdog, NULL) == 0) pthread_detach(w);
    fflush(stdout);
    return 0;
}
#endif
