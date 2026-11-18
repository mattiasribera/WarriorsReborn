/* Developer Lua: --lua / --lua-exec / --lua-pipe / --dev-console (see hle_devlua.h). */
#include "hle_devlua.h"
#include "hle_warp.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_JOBS 64
#define MAX_STATES 8
#define LINE_MAX_LEN 4096

typedef struct {
    char *code;
    size_t len;
    char name[64];
    uint32_t at;
    int retry, done;
    uint32_t tried_L[MAX_STATES], tried_frame[MAX_STATES];   /* retry: once per frame and state */
} Job;

static Job jobs[MAX_JOBS];
static int njobs;
static uint32_t opt_at;
static int opt_retry, opt_console, chunk_seq;
static const char *pipe_path;
static long pipe_off;
static uint32_t pipe_frame = UINT32_MAX;

/* --dev-console lines from the reader thread. */
static pthread_mutex_t con_lock = PTHREAD_MUTEX_INITIALIZER;
static char *con_lines[MAX_JOBS];
static int con_n;

void devlua_reset(void) {
    for (int i = 0; i < njobs; i++) free(jobs[i].code);
    memset(jobs, 0, sizeof jobs);
    njobs = 0;
    opt_at = 0;
    opt_retry = opt_console = chunk_seq = 0;
    pipe_path = NULL;
    pipe_off = 0;
    pipe_frame = UINT32_MAX;
}

int devlua_add(const char *code, size_t len, const char *name, uint32_t at, int retry) {
    if (njobs >= MAX_JOBS) return -1;
    char *c = malloc(len + 1);
    if (!c) return -1;
    memcpy(c, code, len);
    c[len] = 0;
    Job *j = &jobs[njobs++];
    memset(j, 0, sizeof *j);
    j->code = c;
    j->len = len;
    snprintf(j->name, sizeof j->name, "%s", name);
    j->at = at;
    j->retry = retry;
    return 0;
}

int devlua_wanted(void) { return njobs > 0 || pipe_path || opt_console; }

int devlua_pending(void) {
    int n = 0;
    for (int i = 0; i < njobs; i++) n += !jobs[i].done;
    return n;
}

static int read_file(const char *path, char **out, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t cap = 4096, n = 0;
    char *b = malloc(cap);
    size_t r;
    while (b && (r = fread(b + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) {
            char *nb = realloc(b, cap *= 2);
            if (!nb) { free(b); b = NULL; }
            else b = nb;
        }
    }
    fclose(f);
    if (!b) return -1;
    *out = b;
    *len = n;
    return 0;
}

static int parse_frame(const char *s, uint32_t *v) {
    char *end;
    unsigned long x = strtoul(s, &end, 10);
    if (!*s || *end || s[0] == '-' || x > 100000000ul) return -1;
    *v = (uint32_t)x;
    return 0;
}

int devlua_parse_arg(int argc, char **argv, int *i, char *err, size_t cap) {
    const char *a = argv[*i];
    int value = !strcmp(a, "--lua") || !strcmp(a, "--lua-exec") || !strcmp(a, "--lua-at") || !strcmp(a, "--lua-pipe");
    if (!value) {
        if (!strcmp(a, "--lua-retry")) { opt_retry = 1; return 1; }
        if (!strcmp(a, "--dev-console")) { opt_console = 1; return 1; }
        return 0;
    }
    if (*i + 1 >= argc) {
        snprintf(err, cap, "%s: missing value", a);
        return -1;
    }
    const char *v = argv[++*i];
    char name[64];
    if (!strcmp(a, "--lua-at")) {
        if (parse_frame(v, &opt_at)) {
            snprintf(err, cap, "--lua-at %s: expected a frame number (0-100000000)", v);
            return -1;
        }
        return 1;
    }
    if (!strcmp(a, "--lua-pipe")) {
        pipe_path = v;
        return 1;
    }
    if (!strcmp(a, "--lua")) {
        char *text;
        size_t len;
        if (read_file(v, &text, &len)) {
            snprintf(err, cap, "--lua %s: cannot read the file", v);
            return -1;
        }
        const char *base = strrchr(v, '/');
        snprintf(name, sizeof name, "%s", base ? base + 1 : v);
        int r = devlua_add(text, len, name, opt_at, opt_retry);
        free(text);
        if (r) { snprintf(err, cap, "--lua %s: too many chunks (at most %d)", v, MAX_JOBS); return -1; }
        return 1;
    }
    snprintf(name, sizeof name, "exec%d", ++chunk_seq);
    if (devlua_add(v, strlen(v), name, opt_at, opt_retry)) {
        snprintf(err, cap, "--lua-exec: too many chunks (at most %d)", MAX_JOBS);
        return -1;
    }
    return 1;
}

/* ---- running a chunk ---- */

static const char PRELUDE[] = "__devlua_err = nil _ERRORMESSAGE = function(m) __devlua_err = m end";

static void log_value(const DevLuaApi *api, uint32_t L, int idx, char *out, size_t cap) {
    static const char *const names[] = {"userdata", "nil", "number", "string", "table", "function"};
    int t = api->type(L, idx);
    if (t == 2 || t == 3) {
        const char *s = api->tostring(L, idx);
        snprintf(out, cap, t == 3 ? "\"%.200s\"" : "%.200s", s ? s : "?");
    } else snprintf(out, cap, "%s", t >= 0 && t <= 5 ? names[t] : "?");
}

int devlua_run(const DevLuaApi *api, uint32_t L, const char *code, size_t len, const char *name, int *truthy) {
    char chunk[72];
    snprintf(chunk, sizeof chunk, "=%s", name);
    return devlua_run_ex(api, L, code, len, chunk, name, truthy, NULL, 0);
}

int devlua_host_chunks;

int devlua_run_ex(const DevLuaApi *api, uint32_t L, const char *code, size_t len, const char *chunk, const char *name,
                  int *truthy, char *err, size_t errcap) {
    char buf[LINE_MAX_LEN + 8];
    devlua_host_chunks++;
    if (len && code[0] == '=' && len < LINE_MAX_LEN) {   /* "=expr" -> "return expr" */
        len = (size_t)snprintf(buf, sizeof buf, "return %.*s", (int)(len - 1), code + 1);
        code = buf;
    }
    *truthy = 0;
    int base = api->gettop(L);
    api->getglobal(L, "_ERRORMESSAGE");   /* base + 1: the game's handler, restored below */
    int st = api->dobuffer(L, PRELUDE, sizeof PRELUDE - 1, "=devlua");
    if (st == 0) {
        api->settop(L, base + 1);
        st = api->dobuffer(L, code, len, chunk);
    }
    int top = api->gettop(L);
    char line[1024];
    size_t n = 0;
    line[0] = 0;
    if (st == 0) {
        *truthy = top > base + 1 && api->type(L, base + 2) != 1;
        for (int i = base + 2; i <= top && n + 1 < sizeof line; i++) {
            char v[256];
            log_value(api, L, i, v, sizeof v);
            int w = snprintf(line + n, sizeof line - n, "%s%s", i > base + 2 ? ", " : "", v);
            if (w > 0) n += (size_t)w < sizeof line - n ? (size_t)w : sizeof line - n - 1;
        }
        if (name) printf("[lua] %s: ok%s%s\n", name, n ? " -> " : "", line);
    } else {
        api->getglobal(L, "__devlua_err");
        const char *m = api->type(L, -1) == 3 ? api->tostring(L, -1) : NULL;
        const char *kind = st == 1 ? "run" : st == 2 ? "file" : st == 3 ? "syntax" : st == 4 ? "memory" : "error handler";
        if (name) printf("[lua] %s: error %d (%s)%s%.300s\n", name, st, kind, m ? ": " : "", m ? m : "");
        if (err && errcap) snprintf(err, errcap, "%s error%s%.300s", kind, m ? ": " : "", m ? m : "");
    }
    if (name) fflush(stdout);
    api->pushvalue(L, base + 1);
    api->setglobal(L, "_ERRORMESSAGE");
    api->settop(L, base);
    devlua_host_chunks--;
    return st;
}

/* ---- per call ---- */

static int try_job(Job *j, uint32_t L, uint32_t frame) {
    if (j->done || frame < j->at) return 0;
    if (j->retry) {
        int slot = -1;
        for (int k = 0; k < MAX_STATES; k++) {
            if (j->tried_L[k] == L) { slot = k; break; }
            if (!j->tried_L[k] && slot < 0) slot = k;
        }
        if (slot < 0) slot = (int)(frame % MAX_STATES);
        if (j->tried_L[slot] == L && j->tried_frame[slot] == frame + 1) return 0;   /* this frame already */
        j->tried_L[slot] = L;
        j->tried_frame[slot] = frame + 1;
    }
    return 1;
}

static void poll_pipe(const DevLuaApi *api, uint32_t L) {
    FILE *f = fopen(pipe_path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < pipe_off) pipe_off = 0;   /* truncated: start over */
    if (size > pipe_off) {
        fseek(f, pipe_off, SEEK_SET);
        char line[LINE_MAX_LEN];
        long pos = pipe_off;
        while (fgets(line, sizeof line, f)) {
            size_t n = strlen(line);
            if (!n || line[n - 1] != '\n') break;   /* an unfinished line: wait for the rest */
            pos += (long)n;
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            if (!n) continue;
            int t;
            char name[32];
            snprintf(name, sizeof name, "pipe%d", ++chunk_seq);
            devlua_run(api, L, line, n, name, &t);
        }
        pipe_off = pos;
    }
    fclose(f);
}

static void drain_console(const DevLuaApi *api, uint32_t L) {
    char *lines[MAX_JOBS];
    int n;
    pthread_mutex_lock(&con_lock);
    n = con_n;
    memcpy(lines, con_lines, sizeof(char *) * (size_t)n);
    con_n = 0;
    pthread_mutex_unlock(&con_lock);
    for (int i = 0; i < n; i++) {
        int t;
        char name[32];
        snprintf(name, sizeof name, "console%d", ++chunk_seq);
        devlua_run(api, L, lines[i], strlen(lines[i]), name, &t);
        free(lines[i]);
    }
}

void devlua_tick(const DevLuaApi *api, uint32_t L, uint32_t frame) {
    static int busy;
    if (busy || !L) return;
    busy = 1;
    for (int i = 0; i < njobs; i++) {
        Job *j = &jobs[i];
        if (!try_job(j, L, frame)) continue;
        int truthy;
        int st = devlua_run(api, L, j->code, j->len, j->name, &truthy);
        if (!j->retry || (st == 0 && truthy)) {
            j->done = 1;
            if (j->retry) printf("[lua] %s: done at frame %u (Lua state %08x)\n", j->name, frame, L);
        }
    }
    if (pipe_path && frame != pipe_frame) {
        pipe_frame = frame;
        poll_pipe(api, L);
    }
    if (opt_console) drain_console(api, L);
    busy = 0;
}

/* ---- title ---- */

const DevLuaApi devlua_title_api = {
    (int (*)(uint32_t))(uintptr_t)DEVLUA_GETTOP,
    (void (*)(uint32_t, int))(uintptr_t)DEVLUA_SETTOP,
    (void (*)(uint32_t, int))(uintptr_t)DEVLUA_PUSHVALUE,
    (int (*)(uint32_t, int))(uintptr_t)DEVLUA_TYPE,
    (const char *(*)(uint32_t, int))(uintptr_t)DEVLUA_TOSTRING,
    (void (*)(uint32_t, const char *))(uintptr_t)DEVLUA_GETGLOBAL,
    (void (*)(uint32_t, const char *))(uintptr_t)DEVLUA_SETGLOBAL,
    (int (*)(uint32_t, const char *, size_t, const char *))(uintptr_t)DEVLUA_DOBUFFER,
};
static uint32_t (*frames)(void);

static void on_call(WarpFrame *f) {
    uint32_t L = *(const uint32_t *)(uintptr_t)(f->esp + 4);   /* lua_call's first argument */
    devlua_tick(&devlua_title_api, L, frames ? frames() : 0);
}

static void *console_reader(void *arg) {
    char line[LINE_MAX_LEN];
    plat_thread_init();
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        char *c = strdup(line);
        if (!c) continue;
        pthread_mutex_lock(&con_lock);
        if (con_n < MAX_JOBS) con_lines[con_n++] = c, c = NULL;
        pthread_mutex_unlock(&con_lock);
        if (c) { printf("[lua] console: queue full, line dropped\n"); free(c); }
    }
    printf("[lua] console: standard input closed\n");
    fflush(stdout);
    return NULL;
}

static const WarpSig devlua_sigs[] = {
    /* lua_call: sub esp,8; mov eax,[esp+0x10]; mov ecx,[esp+0x14]; push esi; push edi */
    {"lua_call", DEVLUA_CALL, 13, {0x83, 0xec, 0x08, 0x8b, 0x44, 0x24, 0x10, 0x8b, 0x4c, 0x24, 0x14, 0x56, 0x57}},
    /* lua_dobuffer: mov eax,[esp+0x10]; mov ecx,[esp+0xc]; sub esp,8; push edi */
    {"lua_dobuffer", DEVLUA_DOBUFFER, 12, {0x8b, 0x44, 0x24, 0x10, 0x8b, 0x4c, 0x24, 0x0c, 0x83, 0xec, 0x08, 0x57}},
    /* lua_gettop: mov ecx,[esp+4]; mov eax,[ecx]; sub eax,[ecx+0x10]; sar eax,4; ret */
    {"lua_gettop", DEVLUA_GETTOP, 13, {0x8b, 0x4c, 0x24, 0x04, 0x8b, 0x01, 0x2b, 0x41, 0x10, 0xc1, 0xf8, 0x04, 0xc3}},
    /* lua_settop: mov eax,[esp+8]; test eax,eax; jl ...; push eax */
    {"lua_settop", DEVLUA_SETTOP, 9, {0x8b, 0x44, 0x24, 0x08, 0x85, 0xc0, 0x7c, 0x13, 0x50}},
    /* lua_tostring: mov eax,[esp+8]; test eax,eax; mov ecx,[esp+4]; push esi */
    {"lua_tostring", DEVLUA_TOSTRING, 11, {0x8b, 0x44, 0x24, 0x08, 0x85, 0xc0, 0x8b, 0x4c, 0x24, 0x04, 0x56}},
    /* lua_getglobal: mov eax,[esp+8]; push esi; mov esi,[esp+8]; push edi; mov edi,[esi] */
    {"lua_getglobal", DEVLUA_GETGLOBAL, 12, {0x8b, 0x44, 0x24, 0x08, 0x56, 0x8b, 0x74, 0x24, 0x08, 0x57, 0x8b, 0x3e}},
    {"lua_setglobal", DEVLUA_SETGLOBAL, 12, {0x8b, 0x44, 0x24, 0x08, 0x56, 0x8b, 0x74, 0x24, 0x08, 0x57, 0x8b, 0x3e}},
};

int devlua_check_api(int with_call, char *bad, size_t cap) {
    int skip = with_call ? 0 : 1;   /* devlua_sigs[0] is lua_call */
    return warp_check(devlua_sigs + skip, (int)(sizeof devlua_sigs / sizeof devlua_sigs[0]) - skip, 0, bad, cap);
}

const uint8_t *devlua_dobuffer_orig(void) { return devlua_sigs[1].orig; }

int hle_devlua_install(uint32_t (*frame_fn)(void)) {
    if (!devlua_wanted()) return 0;
    char bad[256];
    if (devlua_check_api(1, bad, sizeof bad)) {
        printf("[lua] not available: signature mismatch at %s (nothing written; --lua/--goto Lua not run)\n", bad);
        return 0;
    }
    uint8_t *stub = plat_alloc_exec(WARP_STUB_MAX);
    if (!stub) return 0;
    frames = frame_fn;
    warp_build_watch_stub(stub, (uint32_t)(uintptr_t)on_call, devlua_sigs[0].orig, DEVLUA_CALL_LEN, DEVLUA_CALL + DEVLUA_CALL_LEN);
    warp_write_jmp((uint8_t *)(uintptr_t)DEVLUA_CALL, DEVLUA_CALL, (uint32_t)(uintptr_t)stub, DEVLUA_CALL_LEN);
    if (opt_console) {
        pthread_t t;
        if (pthread_create(&t, NULL, console_reader, NULL) == 0) pthread_detach(t);
        else opt_console = 0;
    }
    printf("[lua] hook at lua_call (%08x): %d chunk(s)%s%s%s\n", DEVLUA_CALL, njobs, pipe_path ? ", pipe " : "",
           pipe_path ? pipe_path : "", opt_console ? ", console on standard input" : "");
    fflush(stdout);
    return 1;
}
