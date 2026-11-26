/* The helper's window, event pump and controllers, used
 * by r64_helper.c when the game's input region carries a window config (R64_IN_CFG_WINDOW).
 *
 * src/video.c is compiled into this file as is: its SDL backend opens the window (size, fullscreen and
 * --refresh-match from the config), handles F11 / Alt+Enter, the mouse capture (click, F1) and the
 * controllers (ports, --pad-slot, --pad-mappings). Where the game's copy of video.c calls hle_input
 * (input_key_event, input_mouse_*, input_release_all, input_set_port_controller), the functions below
 * write records to the input region's event ring instead; the game applies them with the real
 * functions (src/r64_input.c). Escape goes to the game, which owns the menus: the game answers with a
 * capture release or quits. Rumble and capture releases come back on the command ring. A window close
 * is a QUIT record plus the sticky `quit` word; the helper itself exits when the game quits (0) or is
 * gone (4). Without --wire the window shows a clear frame (helper-io).
 *
 * Present pacing ownership (plan 3.5): this loop is the helper's main thread, the one that created the
 * window, so it keeps pumping events also while the renderer runs here. It blocks between pumps (pump_wait): window
 * input wakes it at once; controllers are read every poll_us (default 1000 us) while one is open, else every 4 ms.
 * With --wire (--render-process helper, -w7.md; built with R64_HELPER_RENDER) the
 * window is a Vulkan window when the game presents (R64_IN_CFG_VULKAN), and a render thread decodes the game's
 * call stream into the renderer (src/render_rec.c render_rec_serve): its presents, frame generation's second
 * present and the present-wait thread are all on that thread; the game sends the per-frame show/pause decision
 * in the stream (framegen_decide stays on the game side: it needs hle_sim60_active and the game's clock). After
 * every frame shown this loop calls video_frame_presented (--refresh-match's deferred switch). Without a Vulkan
 * window (SDL's dummy driver, headless) the renderer stays offscreen and reads every presented frame back.
 *
 * --test-inject N[@MS] (tests, SDL's dummy video driver): MS after the window opened (default 50) a thread pushes N key events (SDL_PushEvent) at
 * irregular intervals and the time of each push is sent with its record (R64InEvent.o_*); then a
 * virtual game controller is plugged and its A button pressed and released; a rumble that reaches it
 * is reported as an R64_IN_TEST record; then the window is closed (SDL_QUIT). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* ppoll; video.c sets it too, after <stdio.h> here */
#endif
#include <stdio.h>
static int r64_vprintf_line(const char *fmt, ...) __attribute__((format(__printf__, 1, 2)));
#define printf r64_vprintf_line
#include "../../src/video.c"
#undef printf
#include "../../src/render_ipc.h"
#ifdef R64_HELPER_RENDER
#include "../../src/render_rec.h"
#endif
#include <stdarg.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/resource.h>
#endif

/* Every line video.c prints gets the helper's prefix. */
static int r64_vprintf_line(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t len = strlen(buf);
    while (len && buf[len - 1] == '\n') buf[--len] = 0;
    if (len) r64_log("%s", buf);
    return n;
}

/* ---- the parts of src/platform.h video.c needs (posix.c / win32.c are the i386 game's) ---- */
#ifdef _WIN32
void *plat_dlopen(const char *name) { return (void *)LoadLibraryA(name); }
void *plat_dlsym(void *lib, const char *sym) { return (void *)GetProcAddress((HMODULE)lib, sym); }
const char *plat_dlerror(void) {
    static char b[32];
    snprintf(b, sizeof b, "error %lu", GetLastError());
    return b;
}
const char *plat_program_dir(void) {
    static char d[4096];
    DWORD n = GetModuleFileNameA(NULL, d, sizeof d - 1);
    while (n && d[n - 1] != '\\' && d[n - 1] != '/') n--;
    d[n] = 0;
    return d;
}
#ifdef R64_HELPER_RENDER
/* The pipeline workers' condition variables (--pipeline-async), as src/platform/win32.c: a relative wait, because
 * winpthreads converts an absolute CLOCK_REALTIME deadline at the system tick (up to 15.6 ms late). */
int plat_cond_init_monotonic(pthread_cond_t *c) { return pthread_cond_init(c, NULL); }
int plat_cond_timedwait_mono(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *deadline) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t ns = (int64_t)(deadline->tv_sec - now.tv_sec) * 1000000000 + (deadline->tv_nsec - now.tv_nsec);
    if (ns < 0) ns = 0;
    const struct timespec rel = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    return pthread_cond_timedwait_relative_np(c, m, &rel);
}
#endif
#else
void *plat_dlopen(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void *plat_dlsym(void *lib, const char *sym) { return dlsym(lib, sym); }
const char *plat_dlerror(void) { const char *e = dlerror(); return e ? e : "unknown error"; }
const char *plat_program_dir(void) {
    static char d[4096];
    ssize_t n = readlink("/proc/self/exe", d, sizeof d - 1);
    if (n < 0) n = 0;
    while (n && d[n - 1] != '/') n--;
    d[n] = 0;
    return d;
}
#ifdef R64_HELPER_RENDER
/* The pipeline workers' condition variables (--pipeline-async), as posix.c / tools/render64/plat_dl.c. */
int plat_cond_init_monotonic(pthread_cond_t *c) {
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    int r = pthread_cond_init(c, &ca);
    pthread_condattr_destroy(&ca);
    return r;
}
int plat_cond_timedwait_mono(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *deadline) {
    return pthread_cond_timedwait(c, m, deadline);
}
#endif
#endif

/* ---- the input sinks: what the game's video.c passes to hle_input, written to the event ring ---- */
static void *in_reg;
static R64InHeader *in_hdr;
static XPad pad_sent[INPUT_PORTS];
static int pad_state[INPUT_PORTS];   /* 0 unknown (send the next state), 1 connected, 2 left */
/* --test-inject: the push time of each injected key event, taken in order by input_key_event */
static uint64_t *inj_t;
static uint32_t inj_n, inj_taken, inj_delay_ms = 50;
static volatile uint32_t inj_pushed;

static void put(uint16_t type, uint16_t arg, const void *v, uint32_t n, uint64_t origin) {
    if (!in_reg) return;
    const int resync = in_hdr->ring[R64_IN_EVENTS].resync;
    if (r64_in_put(in_reg, R64_IN_EVENTS, type, arg, v, n, origin) && resync)
        memset(pad_state, 0, sizeof pad_state);   /* the game released everything: send every controller again */
}
void input_key_event(int key, int down, int repeat) {
    const int32_t v[2] = {key, repeat};
    uint64_t origin = 0;
    if (inj_t && inj_taken < __atomic_load_n(&inj_pushed, __ATOMIC_ACQUIRE)) origin = inj_t[inj_taken++];
    put(R64_IN_KEY, (uint16_t)(down != 0), v, sizeof v, origin);
}
void input_mouse_motion(int dx, int dy) {
    const int32_t v[2] = {dx, dy};
    put(R64_IN_MOTION, 0, v, sizeof v, 0);
}
void input_mouse_wheel(int notches) {
    const int32_t v = notches;
    put(R64_IN_WHEEL, 0, &v, sizeof v, 0);
}
void input_release_all(void) { put(R64_IN_RELEASE_ALL, 0, NULL, 0, 0); }
void input_mouse_reset(void) { put(R64_IN_CAPTURE, (uint16_t)sdl.captured, NULL, 0, 0); }   /* after every capture change */
void input_mouse_frame(double now_s) { (void)now_s; }   /* the game runs mouse look once per pump itself */
void input_pad_apply(const XPad *in, XPad *out) { if (out != in) *out = *in; }   /* the remap is the game's */
void (*input_rumble_hook)(int port, uint16_t left, uint16_t right);
/* The X11 backend's auto-repeat filter: the helper opens the SDL backend only. */
int input_is_repeat_release(const RawKeyEvent *release, const RawKeyEvent *next) { (void)release; (void)next; return 0; }
void input_set_port_controller(int port, const XPad *pad) {
    if (port < 0 || port >= INPUT_PORTS) return;
    if (!pad) {
        if (pad_state[port] == 2) return;
        pad_state[port] = 2;
        const int32_t v = 0;
        put(R64_IN_PAD, (uint16_t)port, &v, sizeof v, 0);
        return;
    }
    if (pad_state[port] == 1 && !memcmp(&pad_sent[port], pad, sizeof *pad)) return;   /* polled every pump: changes only */
    pad_state[port] = 1;
    pad_sent[port] = *pad;
    int32_t v[10] = {1};
    memcpy(&v[1], pad, sizeof *pad);
    put(R64_IN_PAD, (uint16_t)port, v, sizeof v, 0);
}
static int escape_to_game(void) {
    put(R64_IN_ESCAPE, (uint16_t)sdl.captured, NULL, 0, 0);
    return 1;   /* video.c leaves the capture and quitting alone: the game decides */
}

/* ---- --test-inject ---- */
static int (*push_event)(SDL_Event *);
static uint32_t lcg = 12345;
static uint32_t rnd(uint32_t n) {
    lcg = lcg * 1103515245u + 12345u;
    return (lcg >> 8) % n;
}
#ifdef _WIN32
/* nanosleep (winpthreads) and Sleep wake at the system tick (1-15.6 ms): --test-inject's 0.2-2.5 ms gaps would become
 * 15 ms ones. A high-resolution waitable timer (Windows 10 1803+) per thread, as src/platform/win32.c's sleeps; without one,
 * Sleep(1) under timeBeginPeriod(1). */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static void sleep_us(uint32_t us) {
    static __thread HANDLE timer;
    static __thread int tried;
    if (!tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        static LONG period_set;
        if (!timer && !InterlockedExchange(&period_set, 1)) {
            HMODULE winmm = LoadLibraryA("winmm.dll");
            UINT(WINAPI * begin)(UINT) = winmm ? (UINT(WINAPI *)(UINT))(void *)GetProcAddress(winmm, "timeBeginPeriod") : NULL;
            if (begin) begin(1);
        }
    }
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)us * 10;   /* relative, 100 ns units */
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
    else Sleep(us >= 1000 ? us / 1000 : 1);
}
/* CPU time in seconds of this process and of the calling thread (the pump's summary line), as getrusage /
 * CLOCK_THREAD_CPUTIME_ID on Linux. */
static double filetime_s(const FILETIME *k, const FILETIME *u) {
    const uint64_t kk = (uint64_t)k->dwHighDateTime << 32 | k->dwLowDateTime, uu = (uint64_t)u->dwHighDateTime << 32 | u->dwLowDateTime;
    return (double)(kk + uu) / 1e7;
}
static double process_cpu_s(void) {
    FILETIME c, e, k, u;
    return GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ? filetime_s(&k, &u) : 0;
}
static double thread_cpu_s(void) {
    FILETIME c, e, k, u;
    return GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u) ? filetime_s(&k, &u) : 0;
}

/* The pump's wait: MsgWaitForMultipleObjectsEx returns at once for window input
 * (keyboard, mouse, raw input: the messages SDL_PumpEvents reads), for pump_wake (another thread of this process) or
 * when the high-resolution timer of `us` expires (controllers: XInput has no events; the game's commands). Without
 * that timer (before Windows 10 1803) the wait's own timeout, in ms, under timeBeginPeriod(1) (SDL_Init sets it too:
 * SDL_HINT_TIMER_RESOLUTION defaults to 1; the resolution is per process since Windows 10 2004). */
static HANDLE pw_event, pw_timer;
static void pump_wait_init(void) {
    pw_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    pw_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!pw_timer) {
        HMODULE winmm = LoadLibraryA("winmm.dll");
        UINT(WINAPI * begin)(UINT) = winmm ? (UINT(WINAPI *)(UINT))(void *)GetProcAddress(winmm, "timeBeginPeriod") : NULL;
        if (begin) begin(1);
    }
}
static void pump_wake(void) {
    if (pw_event) SetEvent(pw_event);
}
/* 1: woken (input or pump_wake), 0: the time ran out */
static int pump_wait(uint32_t us) {
    HANDLE h[2];
    DWORD n = 0, ms = INFINITE, timer_at = 99;
    if (pw_event) h[n++] = pw_event;
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)us * 10;   /* relative, 100 ns units */
    if (pw_timer && SetWaitableTimer(pw_timer, &due, 0, NULL, NULL, FALSE)) timer_at = n, h[n++] = pw_timer;
    else ms = us >= 1000 ? us / 1000 : 1;
    const DWORD r = MsgWaitForMultipleObjectsEx(n, h, ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    return r != WAIT_TIMEOUT && r != WAIT_OBJECT_0 + timer_at && r != WAIT_FAILED;
}
#else
#include <fcntl.h>
#include <poll.h>
static void sleep_us(uint32_t us) {
    struct timespec t = {us / 1000000, (long)(us % 1000000) * 1000};
    nanosleep(&t, NULL);
}
static double process_cpu_s(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}
static double thread_cpu_s(void) {
    struct timespec t;
    return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) ? 0 : t.tv_sec + t.tv_nsec / 1e9;
}

/* The pump's wait: poll on the window system's connection (X11 or Wayland, from
 * SDL_GetWindowWMInfo; none with SDL's dummy driver) and on a pipe pump_wake writes, at most `us`. */
static int pw_pipe[2] = {-1, -1}, pw_display = -1;
static void pump_wait_init(void) {
    if (pipe(pw_pipe)) pw_pipe[0] = pw_pipe[1] = -1;
    for (int i = 0; i < 2; i++)
        if (pw_pipe[i] >= 0) fcntl(pw_pipe[i], F_SETFL, O_NONBLOCK), fcntl(pw_pipe[i], F_SETFD, FD_CLOEXEC);
    struct {   /* SDL_SysWMinfo (SDL 2.0.22's view): version, subsystem, the union (x11 / wl: the display first) */
        uint8_t major, minor, patch;
        int subsystem;
        union { void *display; uint8_t pad[128]; } info;
    } wm;
    memset(&wm, 0, sizeof wm);
    wm.major = 2, wm.patch = 22;
    int (*get)(void *, void *);
    *(void **)&get = plat_dlsym(sdl.lib, "SDL_GetWindowWMInfo");
    if (!get || !sdl.win || !get(sdl.win, &wm) || !wm.info.display) return;
    const char *lib = wm.subsystem == 2 ? "libX11.so.6" : wm.subsystem == 6 ? "libwayland-client.so.0" : NULL;
    void *h = lib ? dlopen(lib, RTLD_NOW | RTLD_NOLOAD) : NULL;   /* SDL loaded it */
    int (*fd_of)(void *) = NULL;
    if (h) *(void **)&fd_of = dlsym(h, wm.subsystem == 2 ? "XConnectionNumber" : "wl_display_get_fd");
    if (fd_of) pw_display = fd_of(wm.info.display);
    if (h) dlclose(h);
}
static void pump_wake(void) {
    if (pw_pipe[1] >= 0 && write(pw_pipe[1], "w", 1) < 0) {}   /* full: a wake is pending anyway */
}
static int pump_wait(uint32_t us) {
    struct pollfd p[2] = {{pw_pipe[0], POLLIN, 0}, {pw_display, POLLIN, 0}};
    const nfds_t n = pw_display >= 0 ? 2 : 1;
    if (pw_pipe[0] < 0) {
        sleep_us(us);
        return 0;
    }
    const struct timespec t = {us / 1000000, (long)(us % 1000000) * 1000};
    const int r = ppoll(p, n, &t, NULL);
    if (r > 0 && p[0].revents) {
        char b[64];
        while (read(pw_pipe[0], b, sizeof b) > 0) {}
    }
    /* A connection SDL's pump left readable would wake the pump at once every time (a spin): more than 20000 wakes by
     * it in one second, far above any input device's rate, and the pump waits for the timeout only. */
    static uint64_t display_since;
    static uint32_t display_wakes;
    if (n == 2 && r > 0 && p[1].revents) {
        const uint64_t now = r64_now_ns();
        if (now - display_since > 1000000000ull) display_since = now, display_wakes = 0;
        if (++display_wakes > 20000) {
            r64_log("input pump: the display connection stays readable; waiting %u us at most instead", us);
            pw_display = -1;
        }
    }
    return r > 0;
}
#endif
static void *injector(void *arg) {
    (void)arg;
    sleep_us(inj_delay_ms * 1000);
    for (uint32_t i = 0; i < inj_n; i++) {
        sleep_us(200 + rnd(2300));   /* 0.2-2.5 ms apart */
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = i & 1 ? SDL_KEYUP : SDL_KEYDOWN;
        e.pad[12] = (uint8_t)!(i & 1);                                  /* state */
        *(int32_t *)(e.pad + 20) = (int32_t)('a' + (i / 2) % 26);        /* sym */
        inj_t[i] = r64_now_ns();
        __atomic_store_n(&inj_pushed, i + 1, __ATOMIC_RELEASE);
        push_event(&e);
        pump_wake();   /* as the window system's input wakes the pump (SDL_PushEvent alone does not) */
    }
    return NULL;
}
/* SDL_VirtualJoystickDesc, version 1 (SDL 2.24+), as tools/video_pads_test.c. */
typedef struct {
    uint16_t version, type, naxes, nbuttons, nhats, vendor_id, product_id, padding;
    uint32_t button_mask, axis_mask;
    const char *name;
    void *userdata, *update, *set_player_index;
    int (*rumble)(void *userdata, uint16_t low, uint16_t high);
    void *rumble_triggers, *set_led, *send_effect;
} VirtualDesc;
static int rumble_port = -1;
static int virtual_rumble(void *userdata, uint16_t low, uint16_t high) {
    (void)userdata;
    const int32_t v[3] = {rumble_port, low, high};
    put(R64_IN_TEST, 1, v, sizeof v, 0);
    r64_log("test-inject: rumble %04x %04x reached the virtual controller", low, high);
    return 0;
}
/* The main-thread part of the injection: after the keys, the virtual controller, then the close. */
static struct {
    int step;
    uint64_t at;
    void *joy;
    int (*attach)(const VirtualDesc *);
    void *(*open)(int);
    int (*set_button)(void *, int, uint8_t);
} tj;
static void inject_step(void) {
    const uint64_t now = r64_now_ns();
    switch (tj.step) {
    case 0:   /* the key thread is done and the keys are in the ring */
        if (inj_taken < inj_n) return;
        *(void **)&tj.attach = plat_dlsym(sdl.lib, "SDL_JoystickAttachVirtualEx");
        *(void **)&tj.open = plat_dlsym(sdl.lib, "SDL_JoystickOpen");
        *(void **)&tj.set_button = plat_dlsym(sdl.lib, "SDL_JoystickSetVirtualButton");
        if (tj.attach && tj.open && tj.set_button && sdl.GameControllerOpen) {
            VirtualDesc d = {1, 1 /* SDL_JOYSTICK_TYPE_GAMECONTROLLER */, 6, 15, 0, 0x1234, 0x0177, 0, 0, 0, "r64 test pad",
                             NULL, NULL, NULL, virtual_rumble, NULL, NULL, NULL};
            const int idx = tj.attach(&d);
            tj.joy = idx >= 0 ? tj.open(idx) : NULL;
        }
        if (!tj.joy) {
            r64_log("test-inject: no virtual controller (%s)", sdl.GetError());
            const int32_t v = 0;
            put(R64_IN_TEST, 2, &v, sizeof v, 0);
            tj.step = 4;
        } else
            tj.step = 1;
        tj.at = now + 100000000ull;
        return;
    case 1:   /* press A */
        if (now < tj.at) return;
        tj.set_button(tj.joy, 0 /* SDL_CONTROLLER_BUTTON_A */, 1);
        tj.step = 2;
        tj.at = now + 100000000ull;
        return;
    case 2:   /* release A, then wait for the game's rumble (<= 2 s) */
        if (now < tj.at) return;
        tj.set_button(tj.joy, 0, 0);
        tj.step = 3;
        tj.at = now + 2000000000ull;
        return;
    case 3:
        if (now < tj.at && rumble_port < 0) return;
        tj.step = 4;
        tj.at = now + 50000000ull;
        return;
    case 4: {   /* close the window */
        if (now < tj.at) return;
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = SDL_QUIT;
        push_event(&e);
        tj.step = 5;
        return;
    }
    default: return;
    }
}

#ifdef R64_HELPER_RENDER
/* --wire: the render thread (src/render_rec.c render_rec_serve) */
static struct {
    R64Ipc *x;
    RenderServe o;
    volatile uint32_t presented, done;
    int rc;
} rt;
static void *render_thread(void *arg) {
    (void)arg;
    rt.rc = render_rec_serve(rt.x, &rt.o);
    __atomic_store_n(&rt.done, 1, __ATOMIC_RELEASE);
    return NULL;
}
#endif

#define R64_IDLE_US 4000u   /* no controller: the game's commands (capture), the render thread's frames, peer checks */
static uint32_t pump_period(uint32_t pad_us) {
    for (int p = 0; p < INPUT_PORTS; p++)
        if (sdl.port[p].pad) return pad_us;
    return R64_IDLE_US;
}

int r64_helper_io_run(R64Ipc *x, int argc, char **argv) {
    uint32_t size;
    in_reg = r64_region(x, R64_REG_INPUT, &size);
    in_hdr = in_reg;
    char why[160];
    if (r64_in_check(in_reg, size, why, sizeof why)) {
        r64_log("input region refused: %s", why);
        __atomic_store_n(&in_hdr->helper_state, 2, __ATOMIC_RELEASE);
        in_reg = NULL;
        return 3;
    }
    int wire = 0, hash = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wire")) wire = 1;
        else if (!strcmp(argv[i], "--hash-frames")) hash = 1;
    }
#ifndef R64_HELPER_RENDER
    if (wire) {
        r64_log("this helper was built without the renderer (R64_HELPER_RENDER)");
        __atomic_store_n(&in_hdr->helper_state, 2, __ATOMIC_RELEASE);
        return 3;
    }
    (void)hash;
#endif
    for (int i = 1; i + 1 < argc; i++) {
        char err[160];
        if (!strcmp(argv[i], "--pad-slot") && input_pad_slot_parse(argv[i + 1], err, sizeof err)) r64_log("--pad-slot: %s", err);
        else if (!strcmp(argv[i], "--pad-mappings")) video_pad_mappings = argv[i + 1];
        else if (!strcmp(argv[i], "--test-inject")) {   /* N[@MS]: N keys, starting MS after the window opened (default 50) */
            char *at;
            inj_n = (uint32_t)strtoul(argv[i + 1], &at, 0);
            if (*at == '@') inj_delay_ms = (uint32_t)strtoul(at + 1, NULL, 0);
        }
    }
    const R64InConfig cfg = in_hdr->cfg;
    video_fullscreen = !!(cfg.flags & R64_IN_CFG_FULLSCREEN);
    video_refresh_match = !(cfg.flags & R64_IN_CFG_NO_REFRESH_MATCH);
    video_want_vulkan = wire && cfg.flags & R64_IN_CFG_VULKAN;   /* the game presents: into this window */
    const int w = cfg.w ? (int)cfg.w : 640, h = cfg.h ? (int)cfg.h : 480;
    if (sdl_open(w, h, cfg.title[0] ? cfg.title : "WarriorsReborn")) {
        __atomic_store_n(&in_hdr->helper_state, 2, __ATOMIC_RELEASE);
        return 5;
    }
    cur = &sdl_backend;
    video_escape_hook = escape_to_game;
    *(void **)&push_event = plat_dlsym(sdl.lib, "SDL_PushEvent");
    void (*get_size)(void *, int *, int *);
    *(void **)&get_size = plat_dlsym(sdl.lib, "SDL_GetWindowSize");
    pump_wait_init();
    pthread_t inj_thread;
    int inj_on = 0;
    if (inj_n && push_event && (inj_t = calloc(inj_n, sizeof *inj_t))) inj_on = !pthread_create(&inj_thread, NULL, injector, NULL);
    else tj.step = 5;
#ifdef R64_HELPER_RENDER
    pthread_t render;
    int render_on = 0;
    if (wire) {
        memset(&rt, 0, sizeof rt);
        rt.x = x;
        rt.o.hash = hash;
        rt.o.presented = &rt.presented;
        if (sdl.vulkan) {   /* a Vulkan window: the renderer presents into it */
            rt.o.instance_ext = video_vulkan_extensions(&rt.o.instance_ext_count);
            rt.o.create_surface = video_vulkan_surface;
            rt.o.drawable_size = video_vulkan_drawable;
        }
        render_on = !pthread_create(&render, NULL, render_thread, NULL);
        if (!render_on) {
            r64_log("cannot start the render thread");
            __atomic_store_n(&in_hdr->helper_state, 2, __ATOMIC_RELEASE);
            video_close();
            return 5;
        }
    }
#endif
    __atomic_store_n(&in_hdr->helper_state, 1, __ATOMIC_RELEASE);
    /* The pump waits for window input; controllers (no events: XInput) and the game's commands (rumble, capture) are
     * read every pad_us while a controller is open, else every R64_IDLE_US. */
    const uint32_t pad_us = cfg.poll_us ? cfg.poll_us : 1000;
    r64_log("window %dx%d%s, input pump: waits for window input, every %u us with a controller, else every %u us%s%s", w, h,
            video_fullscreen ? " (fullscreen)" : "", pad_us, R64_IDLE_US,
            inj_on ? ", test injection" : "",
            !wire ? "" : sdl.vulkan ? "; the renderer presents into it (a render thread)" : video_want_vulkan ? "; no Vulkan window here: the renderer reads every presented frame back (headless)" : "; the game reads its frames back");

    static uint32_t clear[16 * 9];   /* a black 16:9 frame, scaled to the window */
    const uint64_t t0 = r64_now_ns();
    const double cpu0 = thread_cpu_s();
    uint64_t next_present = t0, next_window = t0;
    int32_t win[4] = {-1, -1, -1, -1};
    uint32_t loops = 0, shown = 0, woken = 0;
    const int headless_hz = wire && getenv("R64_HEADLESS_HZ") ? atoi(getenv("R64_HEADLESS_HZ")) : 0;
    int rc = 0, closed = 0;
    for (;; loops++) {
        if (!video_pump() && !closed) {   /* SDL_QUIT: the window was closed */
            closed = 1;
            put(R64_IN_QUIT, 0, NULL, 0, 0);   /* the record first: a game that sees `quit` has it in the ring */
            __atomic_store_n(&in_hdr->quit, 1, __ATOMIC_RELEASE);
            r64_log("window closed: quit requested");
        }
        R64InEvent c;
        while (r64_in_get(in_reg, R64_IN_CMDS, &c)) {
            if (c.type == R64_IN_RUMBLE) {
                rumble_port = c.arg;
                sdl_rumble(c.arg, (uint16_t)c.v[0], (uint16_t)c.v[1]);
            } else if (c.type == R64_IN_SET_CAPTURE)
                sdl_capture(c.arg);
        }
        const uint64_t now = r64_now_ns();
        if (now >= next_window) {   /* size, fullscreen, focus and display rate, sent when they change */
            next_window = now + 16000000ull;
            int32_t v[4] = {w, h, 0, video_display_hz()};
            if (headless_hz && !sdl.vulkan) v[3] = headless_hz;   /* R64_HEADLESS_HZ (tests): the display rate a headless present reports */
            if (get_size) get_size(sdl.win, &v[0], &v[1]);
            const uint32_t f = sdl.GetWindowFlags ? sdl.GetWindowFlags(sdl.win) : 0;
            v[2] = (int32_t)((f & SDL_WINDOW_FULLSCREEN ? R64_IN_WIN_FULLSCREEN : 0) | (f & 0x200u /* INPUT_FOCUS */ ? R64_IN_WIN_FOCUS : 0) |
                             (f & SDL_WINDOW_MINIMIZED ? R64_IN_WIN_MINIMIZED : 0));
            if (memcmp(v, win, sizeof v)) {
                memcpy(win, v, sizeof v);
                put(R64_IN_WINDOW, 0, v, sizeof v, 0);
            }
        }
        if (!wire && now >= next_present) {
            next_present = now + 250000000ull;   /* a placeholder (the game renders): 4 Hz, not the pump's CPU */
            video_present(clear, 16, 9, 16 * 4);
        }
        if (tj.step < 5) inject_step();
#ifdef R64_HELPER_RENDER
        if (render_on) {   /* the render thread owns the channel's rings; it ends when the game quits or is gone */
            for (const uint32_t p = __atomic_load_n(&rt.presented, __ATOMIC_ACQUIRE); shown != p; shown++) video_frame_presented();
            if (__atomic_load_n(&rt.done, __ATOMIC_ACQUIRE)) {
                pthread_join(render, NULL);
                render_on = 0;
                rc = rt.rc;
                if (rc == 4) r64_log("the game is gone: exiting");
                break;
            }
            woken += pump_wait(pump_period(pad_us));
            continue;
        }
#endif
        if (r64_quit_requested(x)) break;
        if (r64_poll(x)) {
            r64_log("the game is gone: exiting");
            rc = 4;
            break;
        }
        woken += pump_wait(pump_period(pad_us));
    }
    (void)shown;
    if (inj_on) pthread_join(inj_thread, NULL);
    const double secs = (double)(r64_now_ns() - t0) / 1e9;
    const double cpu = process_cpu_s(), pump_cpu = thread_cpu_s() - cpu0;
    r64_log("input pump: %u loops in %.1f s (%.0f us apart, %u woken by input), %u records, %u dropped; cpu: pump thread %.1f%%, "
            "whole process %.1f%% of one core", loops, secs, loops ? secs * 1e6 / loops : 0.0, woken, in_hdr->ring[R64_IN_EVENTS].seq,
            r64_in_dropped(in_reg, R64_IN_EVENTS), secs > 0 ? 100.0 * pump_cpu / secs : 0.0, secs > 0 ? 100.0 * cpu / secs : 0.0);
    if (tj.joy && sdl.lib) {
        void (*jclose)(void *);
        *(void **)&jclose = plat_dlsym(sdl.lib, "SDL_JoystickClose");
        if (jclose) jclose(tj.joy);
    }
    video_close();
    in_reg = NULL;
    free(inj_t);
    return rc;
}
