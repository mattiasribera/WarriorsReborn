/* Developer Lua: run Lua text in the game's own Lua state (off by default; nothing is patched
 * unless one of these options is given).
 *
 *   --lua FILE          run the file's text once
 *   --lua-exec CODE     run CODE once (repeatable; runs in the order given, after --lua files)
 *   --lua-at FRAME      the following --lua / --lua-exec wait until presented frame FRAME
 *   --lua-retry         the following --lua / --lua-exec run again (once per frame and Lua state)
 *                       until the chunk returns a value other than nil (e.g. `if menu then ... return 1 end`)
 *   --lua-pipe PATH     a text file polled once per frame: every line appended to it runs as a
 *                       chunk (headless scripting: `echo 'Foo()' >> PATH`); a truncated file restarts
 *   --dev-console       every line typed on standard input runs as a chunk
 * A line or chunk starting with '=' is `return <rest>`. Every chunk logs `[lua] <name>: ...` with
 * its results (tostring of each; tables, functions and userdata by type) or its error message.
 *
 * Hook (title code): lua_call (0x233cc0, `int lua_call(lua_State *L, int nargs, int nresults)`),
 * the engine's only C -> Lua call, used by every scheduled function, message handler, callback and
 * level script start, in the front end and in every level. Its first 7 bytes (sub esp,8;
 * mov eax,[esp+0x10]) jump to a host stub that calls devlua_on_call() with the caller's state and
 * then resumes lua_call. A chunk runs there with lua_dobuffer (0x233f80): the call being made is
 * already on the Lua stack, the chunk's results are popped (lua_settop back to the entry top), so
 * the engine's call proceeds unchanged. The hook is a C boundary (not inside the VM), the same
 * place where the engine's own C functions run Lua, so the state is consistent. Errors: around the
 * chunk _ERRORMESSAGE is a Lua function that stores the message (restored afterwards). Nested
 * lua_calls made by the chunk itself are not hooked again.
 * Every site is signature-checked first; on any mismatch nothing is written and the options are
 * reported as unavailable. */
#ifndef HLE_DEVLUA_H
#define HLE_DEVLUA_H
#include <stddef.h>
#include <stdint.h>

#define DEVLUA_CALL 0x00233cc0u       /* lua_call: sub esp,8; mov eax,[esp+0x10] (7 bytes replaced) */
#define DEVLUA_CALL_LEN 7
#define DEVLUA_DOBUFFER 0x00233f80u   /* lua_dobuffer(L, buf, size, name) -> status */
#define DEVLUA_GETTOP 0x0022e890u
#define DEVLUA_SETTOP 0x0022e8a0u
#define DEVLUA_PUSHVALUE 0x0022e9b0u
#define DEVLUA_TYPE 0x0022ea00u
#define DEVLUA_TOSTRING 0x0022eca0u
#define DEVLUA_GETGLOBAL 0x0022ef80u
#define DEVLUA_SETGLOBAL 0x0022f210u

/* The title's Lua 4.0 C API (cdecl). Tests pass fakes. */
typedef struct {
    int (*gettop)(uint32_t L);
    void (*settop)(uint32_t L, int idx);
    void (*pushvalue)(uint32_t L, int idx);
    int (*type)(uint32_t L, int idx);              /* Lua 4: 0 userdata, 1 nil, 2 number, 3 string, 4 table, 5 function */
    const char *(*tostring)(uint32_t L, int idx);
    void (*getglobal)(uint32_t L, const char *name);
    void (*setglobal)(uint32_t L, const char *name);
    int (*dobuffer)(uint32_t L, const char *buf, size_t size, const char *name);
} DevLuaApi;

/* Parses one option at argv[*i] (consuming its value). 1 = handled, 0 = not ours, -1 = invalid
 * (message in err). */
int devlua_parse_arg(int argc, char **argv, int *i, char *err, size_t cap);
/* Queues a chunk (the text is copied). at: earliest presented frame; retry: run again until it
 * returns a non-nil value. Returns 0, or -1 when out of memory or the queue is full. */
int devlua_add(const char *code, size_t len, const char *name, uint32_t at, int retry);
/* Any chunk, --lua-pipe or --dev-console given. */
int devlua_wanted(void);
/* Pending chunks (not yet run, or retrying). */
int devlua_pending(void);
/* Clears every option and chunk (tests). */
void devlua_reset(void);

/* Runs one chunk in L (dobuffer + result logging + stack restore). Returns the load/run status
 * (0 = ok) and sets *truthy when the first result is not nil. log: print the [lua] lines. */
int devlua_run(const DevLuaApi *api, uint32_t L, const char *code, size_t len, const char *name, int *truthy);

/* devlua_run with the chunk name given as is (Lua's "=name" / "@file" form): name = the [lua] log
 * name, NULL = log nothing; err (may be NULL) gets "<kind> error: <message>" on failure. */
int devlua_run_ex(const DevLuaApi *api, uint32_t L, const char *code, size_t len, const char *chunk, const char *name,
                  int *truthy, char *err, size_t errcap);

/* devlua_run with the results returned: res gets them as logged ("3, \"x\"", tables by type; empty
 * without results), err (may be NULL) the error text as devlua_run_ex. */
int devlua_run_result(const DevLuaApi *api, uint32_t L, const char *code, size_t len, const char *name, char *res, size_t rescap,
                      char *err, size_t errcap);
/* Live control (src/gamectl.h): called at the end of every hooked lua_call, after the queued
 * chunks (NULL: none). Setting it before hle_devlua_install installs the hook. */
extern void (*devlua_host_tick)(const DevLuaApi *api, uint32_t L, uint32_t frame);

/* Nonzero while a host chunk (--lua, a mod) runs: lua_dobuffer calls made then are not game loads. */
extern int devlua_host_chunks;
/* The title's API (valid once devlua_check_api passed). */
extern const DevLuaApi devlua_title_api;
/* Signature-checks the C API in the title (and lua_call when with_call; the mod hook skips it, as
 * --lua may already have patched it): 0 = all match, else the first bad site in bad. */
int devlua_check_api(int with_call, char *bad, size_t cap);
/* lua_dobuffer's checked first bytes (at least 12). */
const uint8_t *devlua_dobuffer_orig(void);

/* Host side of the lua_call hook: L = the state of the engine's call, frame = presented frames. */
void devlua_tick(const DevLuaApi *api, uint32_t L, uint32_t frame);

/* Signature-checks lua_call and the C API, patches the hook, starts the --dev-console reader.
 * frame_fn: presented frame count. Returns 1 when installed; nothing when nothing was asked for. */
int hle_devlua_install(uint32_t (*frame_fn)(void));
#endif
