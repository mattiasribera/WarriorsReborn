/* Host video backends (see video.h). SDL2 is loaded with dlopen so the build needs no i386
 * development packages: the few SDL2 prototypes and constants used here are declared below
 * (values from SDL2's public headers, stable across 2.x). */
#define _GNU_SOURCE
#include "video.h"
#include "input_map.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    const char *name;
    int (*open)(int w, int h, const char *title);
    void (*present)(const uint32_t *px, int w, int h, int pitch);
    int (*pump)(void);
    void (*close)(void);
} VideoBackend;

/* ---- null: accepts frames, never asks to quit ---- */
static int null_open(int w, int h, const char *title) { (void)w; (void)h; (void)title; return 0; }
static void null_present(const uint32_t *px, int w, int h, int pitch) { (void)px; (void)w; (void)h; (void)pitch; }
static int null_pump(void) { return 1; }
static void null_close(void) {}
static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* Pointer capture shared by the windowed backends: a click in the window captures the pointer
 * for mouse look (that click is not passed to the pad), F1 toggles the capture, Escape releases
 * it and quits only when the pointer is free, and losing focus releases it. */
#define CAPTURE_KEY_HELP "click to capture the mouse; F1 toggles, Escape releases"
static int mouse_button_key(int button, const int *map, int n) { return button >= 1 && button <= n ? map[button - 1] : -1; }

static const VideoBackend null_backend = {"null", null_open, null_present, null_pump, null_close};

/* ---- SDL2 (dlopen) ---- */
#define SDL_INIT_VIDEO 0x20u
#define SDL_WINDOWPOS_UNDEFINED 0x1FFF0000
#define SDL_WINDOW_SHOWN 0x4u
#define SDL_WINDOW_RESIZABLE 0x20u
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x1001u
#define SDL_WINDOW_VULKAN 0x10000000u
#define SDLK_F11 (SDLK_F1 + 10)
#define KMOD_ALT 0x0300u
#define SDL_PIXELFORMAT_RGB888 0x16161804u   /* XRGB8888 */
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_QUIT 0x100u
#define SDL_KEYDOWN 0x300u
#define SDL_KEYUP 0x301u
#define SDL_CONTROLLERDEVICEADDED 0x653u
#define SDL_CONTROLLERDEVICEREMOVED 0x654u
#define SDL_INIT_GAMECONTROLLER 0x2000u
#define SDLK_ESCAPE 27
#define SDLK_F1 0x4000003A
#define SDL_MOUSEMOTION 0x400u
#define SDL_MOUSEBUTTONDOWN 0x401u
#define SDL_MOUSEBUTTONUP 0x402u
#define SDLK_KP_ENTER 0x40000058
#define SDLK_RIGHT 0x4000004F   /* RIGHT, LEFT, DOWN, UP follow */
typedef union { uint32_t type; uint8_t pad[56]; } SDL_Event;
/* SDL_KeyboardEvent: type, timestamp, windowID, state/repeat/pad (4 bytes), then SDL_Keysym
 * {scancode, sym, mod, unused}: sym is the dword at byte offset 20. */
#define SDL_EVENT_KEYSYM(e) (*(const int32_t *)((e)->pad + 20))
#define SDL_EVENT_REPEAT(e) ((e)->pad[13])
#define SDL_EVENT_KEYMOD(e) (*(const uint16_t *)((e)->pad + 24))
#define SDL_WINDOWEVENT 0x200u
#define SDL_WINDOWEVENT_FOCUS_LOST 13   /* SDL_WindowEvent.event, byte offset 12 */
/* SDL_MouseMotionEvent: xrel, yrel are the int32s at byte offsets 28 and 32;
 * SDL_MouseButtonEvent: button is the byte at offset 16 (1 left, 2 middle, 3 right, 4/5 X1/X2). */
#define SDL_EVENT_XREL(e) (*(const int32_t *)((e)->pad + 28))
#define SDL_EVENT_YREL(e) (*(const int32_t *)((e)->pad + 32))
#define SDL_EVENT_BUTTON(e) ((e)->pad[16])

static struct {
    void *lib, *win, *ren, *tex;
    int tw, th;
    int (*Init)(uint32_t);
    void (*Quit)(void);
    const char *(*GetError)(void);
    void *(*CreateWindow)(const char *, int, int, int, int, uint32_t);
    void *(*CreateRenderer)(void *, int, uint32_t);
    void *(*CreateTexture)(void *, uint32_t, int, int, int);
    int (*UpdateTexture)(void *, const void *, const void *, int);
    int (*RenderClear)(void *);
    int (*RenderCopy)(void *, void *, const void *, const void *);
    void (*RenderPresent)(void *);
    int (*PollEvent)(SDL_Event *);
    void (*DestroyTexture)(void *);
    void (*DestroyRenderer)(void *);
    void (*DestroyWindow)(void *);
    int (*SetHint)(const char *, const char *);          /* optional */
    int (*RenderSetLogicalSize)(void *, int, int);       /* optional */
    /* Game controllers (optional: keyboard-only when missing), one per port (hle_input.h) */
    struct { void *pad; char name[64]; uint8_t guid[16]; } port[INPUT_PORTS];
    struct { uint8_t guid[16]; int valid; } last[INPUT_PORTS];   /* the last controller seen in each port */
    int (*InitSubSystem)(uint32_t);
    int (*NumJoysticks)(void);
    int (*IsGameController)(int);
    void *(*GameControllerOpen)(int);
    void (*GameControllerClose)(void *);
    int (*GameControllerGetAttached)(void *);
    uint8_t (*GameControllerGetButton)(void *, int);
    int16_t (*GameControllerGetAxis)(void *, int);
    const char *(*GameControllerName)(void *);
    void *(*GameControllerGetJoystick)(void *);                    /* optional (previous port) */
    struct SdlGuid { uint8_t data[16]; } (*JoystickGetGUID)(void *);   /* optional (previous port) */
    int (*GameControllerRumble)(void *, uint16_t, uint16_t, uint32_t);  /* optional (SDL 2.0.9+) */
    void *(*RWFromFile)(const char *, const char *);                     /* optional: mapping files */
    int (*GameControllerAddMappingsFromRW)(void *, int);
    int (*SetRelativeMouseMode)(int);   /* optional: no mouse look without it */
    int captured;
    /* Vulkan window (video_want_vulkan): the renderer presents; optional entry points (SDL 2.0.6+). */
    int vulkan, w, h;
    uint32_t flags;
    int (*Vulkan_GetInstanceExtensions)(void *, unsigned *, const char **);
    int (*Vulkan_CreateSurface)(void *, void *, uint64_t *);
    void (*Vulkan_GetDrawableSize)(void *, int *, int *);
    int (*SetWindowFullscreen)(void *, uint32_t);
    uint32_t (*GetWindowFlags)(void *);
} sdl;
int video_want_vulkan, video_fullscreen;
const char *video_pad_mappings;

/* SDL keycodes: printable keys (and Enter, Backspace, Tab) are their ASCII code, the others
 * are scancode-based (0x40000000 | scancode). */
#define SDLK_LCTRL 0x400000E0   /* LSHIFT, LALT, LGUI, RCTRL, RSHIFT, RALT follow */
static int sdl_key(int32_t sym) {
    if (sym == SDLK_KP_ENTER) return '\r';
    if (sym >= SDLK_F1 && sym <= SDLK_F1 + 11) return INPUT_KEY_F1 + (sym - SDLK_F1);
    if (sym >= SDLK_LCTRL && sym <= SDLK_LCTRL + 6)
        return (const int[]){INPUT_KEY_LCTRL, INPUT_KEY_LSHIFT, INPUT_KEY_LALT, -1, INPUT_KEY_RCTRL, INPUT_KEY_RSHIFT, INPUT_KEY_RALT}[sym - SDLK_LCTRL];
    if (sym >= SDLK_RIGHT && sym <= SDLK_RIGHT + 3) return (const int[]){INPUT_KEY_RIGHT, INPUT_KEY_LEFT, INPUT_KEY_DOWN, INPUT_KEY_UP}[sym - SDLK_RIGHT];
    return sym < 0x80 ? sym : -1;
}

/* Every attached game controller gets a port (hle_input.h): the first one port 0 (with the
 * keyboard and mouse), the next ones ports 1-3. A controller keeps its port while attached; when
 * it is removed the port is freed, and a controller that comes back takes the port it had when
 * that is still free, else the lowest free port. With --pad-slot the player chosen for a
 * controller decides instead (input_pick_port_for), and the controllers with a chosen player are
 * placed first, so one plugged in before them does not take their port. The Kth controller with a
 * GUID is the one that finds K-1 others with it in ports. SDL_GameControllerOpen returns the same
 * handle (one more reference) for a controller that is already open, so rescans skip it. */
static void sdl_pad_open(void) {
    if (!sdl.GameControllerOpen) return;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < sdl.NumJoysticks(); i++) {
            if (!sdl.IsGameController(i)) continue;
            void *pad = sdl.GameControllerOpen(i);
            if (!pad) continue;
            uint32_t used = 0;
            int open = 0;
            for (int p = 0; p < INPUT_PORTS; p++) {
                if (sdl.port[p].pad) used |= 1u << p;
                if (sdl.port[p].pad == pad) open = 1;
            }
            if (open) { sdl.GameControllerClose(pad); continue; }   /* drop the extra reference */
            const char *name = sdl.GameControllerName(pad);
            uint8_t guid[16] = {0};
            void *joy = sdl.GameControllerGetJoystick && sdl.JoystickGetGUID ? sdl.GameControllerGetJoystick(pad) : NULL;
            if (joy) memcpy(guid, sdl.JoystickGetGUID(joy).data, 16);
            if (!pass && !(joy && input_pad_reserved(guid))) { sdl.GameControllerClose(pad); continue; }
            int prev = -1, nth = 1;
            for (int p = 0; joy && p < INPUT_PORTS; p++) {
                if (prev < 0 && sdl.last[p].valid && !(used >> p & 1) && !memcmp(sdl.last[p].guid, guid, 16)) prev = p;
                if (sdl.port[p].pad && !memcmp(sdl.port[p].guid, guid, 16)) nth++;
            }
            int port = joy ? input_pick_port_for(used, prev, guid, nth) : input_pick_port(used, prev);
            if (port < 0) {
                printf("[input] controller \"%s\" ignored: all %d ports in use\n", name ? name : "?", INPUT_PORTS);
                sdl.GameControllerClose(pad);
                continue;
            }
            sdl.port[port].pad = pad;
            snprintf(sdl.port[port].name, sizeof sdl.port[port].name, "%s", name ? name : "?");
            memcpy(sdl.port[port].guid, guid, 16);
            printf("[input] controller \"%s\" -> port %d (player %d)\n", sdl.port[port].name, port, port + 1);
        }
}
/* SDL_GameControllerAddMappingsFromFile (a macro over these two): the number of mappings added
 * from the file, -1 when it cannot be read. Lines for another platform are skipped by SDL. */
static int sdl_mappings_from(const char *path) {
    if (!sdl.RWFromFile || !sdl.GameControllerAddMappingsFromRW) return -1;
    void *rw = sdl.RWFromFile(path, "rb");
    return rw ? sdl.GameControllerAddMappingsFromRW(rw, 1) : -1;
}
/* Before the controllers are opened: the player's own mappings (the launcher's controller set-up
 * writes them), gamecontrollerdb.txt in the program's folder and in the current folder when that
 * is another one, or --pad-mappings FILE instead. */
static void sdl_pad_mappings(void) {
    if (video_pad_mappings) {
        int n = sdl_mappings_from(video_pad_mappings);
        if (n < 0) printf("[input] --pad-mappings %s: cannot read it (%s)\n", video_pad_mappings, sdl.GetError());
        else printf("[input] %d controller mappings from %s\n", n, video_pad_mappings);
        return;
    }
    const char *dir = plat_program_dir();
    char path[4200], cwd[4096];
    snprintf(path, sizeof path, "%sgamecontrollerdb.txt", dir);
    int n = dir[0] ? sdl_mappings_from(path) : -1;
    if (n >= 0) printf("[input] %d controller mappings from gamecontrollerdb.txt (program folder)\n", n);
    if (!getcwd(cwd, sizeof cwd - 1)) return;
    size_t len = strlen(cwd);
#ifdef _WIN32
    if (len && cwd[len - 1] != '\\' && cwd[len - 1] != '/') strcat(cwd, "\\");
    if (!strcasecmp(cwd, dir)) return;
#else
    if (len && cwd[len - 1] != '/') strcat(cwd, "/");
    if (!strcmp(cwd, dir)) return;
#endif
    n = sdl_mappings_from("gamecontrollerdb.txt");
    if (n >= 0) printf("[input] %d controller mappings from gamecontrollerdb.txt (current folder)\n", n);
}

/* SDL_GameControllerButton / SDL_GameControllerAxis values (stable across SDL 2.x). */
/* input_rumble_hook: the title's motor speeds to that port's controller (left = low frequency).
 * The title stops the motors itself; the 10 s limit only bounds a request it never clears. */
static void sdl_rumble(int port, uint16_t left, uint16_t right) {
    if (port >= 0 && port < INPUT_PORTS && sdl.port[port].pad && sdl.GameControllerRumble)
        sdl.GameControllerRumble(sdl.port[port].pad, left, right, 10000);
}

static void sdl_pad_read_port(int port) {
    void *pad = sdl.port[port].pad;
    if (!sdl.GameControllerGetAttached(pad)) {
        sdl.GameControllerClose(pad);
        sdl.port[port].pad = NULL;
        memcpy(sdl.last[port].guid, sdl.port[port].guid, 16);
        sdl.last[port].valid = 1;
        input_set_port_controller(port, NULL);
        printf("[input] controller \"%s\" removed from port %d\n", sdl.port[port].name, port);
        sdl_pad_open();   /* a controller left without a port (all were in use) can take this one */
        return;
    }
    static const int8_t bits[15] = {   /* A B X Y BACK GUIDE START LSTICK RSTICK LB RB UP DOWN LEFT RIGHT */
        IN_A, IN_B, IN_X, IN_Y, IN_BACK, -1, IN_START, IN_LTHUMB, IN_RTHUMB, IN_WHITE, IN_BLACK,
        IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT};
    uint32_t mask = 0;
    for (int b = 0; b < 15; b++)
        if (bits[b] >= 0 && sdl.GameControllerGetButton(pad, b)) mask |= 1u << bits[b];
    XPad p;
    input_from_controls(mask, &p);
    int16_t ax[6];
    for (int a = 0; a < 6; a++) ax[a] = sdl.GameControllerGetAxis(pad, a);
    p.lx = ax[0];
    p.ly = (int16_t)(ax[1] == -32768 ? 32767 : -ax[1]);   /* SDL: down is positive */
    p.rx = ax[2];
    p.ry = (int16_t)(ax[3] == -32768 ? 32767 : -ax[3]);
    p.analog[IN_LT - IN_A] = (uint8_t)(ax[4] > 0 ? ax[4] * 255 / 32767 : 0);
    p.analog[IN_RT - IN_A] = (uint8_t)(ax[5] > 0 ? ax[5] * 255 / 32767 : 0);
    input_set_port_controller(port, &p);
}
static void sdl_pad_read(void) {
    for (int port = 0; port < INPUT_PORTS; port++)
        if (sdl.port[port].pad) sdl_pad_read_port(port);
}

/* The SDL renderer and streaming texture of the window (frames presented by sdl_present). */
static int sdl_make_renderer(void) {
    sdl.ren = sdl.CreateRenderer(sdl.win, -1, 0);
    if (sdl.ren && sdl.RenderSetLogicalSize) sdl.RenderSetLogicalSize(sdl.ren, sdl.w, sdl.h);
    if (sdl.ren) sdl.tex = sdl.CreateTexture(sdl.ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, sdl.w, sdl.h);
    sdl.tw = sdl.w;
    sdl.th = sdl.h;
    return sdl.tex ? 0 : -1;
}
static int sdl_open(int w, int h, const char *title) {
    sdl.lib = plat_dlopen(PLAT_LIB_SDL2);
    if (!sdl.lib) { printf("[video] cannot load SDL2: %s\n", plat_dlerror()); return -1; }
#define SYM(f) if (!(*(void **)&sdl.f = plat_dlsym(sdl.lib, "SDL_" #f))) { printf("[video] SDL_%s missing\n", #f); return -1; }
    SYM(Init) SYM(Quit) SYM(GetError) SYM(CreateWindow) SYM(CreateRenderer) SYM(CreateTexture) SYM(UpdateTexture)
    SYM(RenderClear) SYM(RenderCopy) SYM(RenderPresent) SYM(PollEvent) SYM(DestroyTexture) SYM(DestroyRenderer)
    SYM(DestroyWindow)
#undef SYM
    if (sdl.Init(SDL_INIT_VIDEO)) { printf("[video] SDL_Init: %s\n", sdl.GetError()); return -1; }
    /* Scaled presentation: bilinear filtering and the frame's aspect ratio (letterboxed) when
     * the window (or fullscreen) is larger than the frame. */
    *(void **)&sdl.SetHint = plat_dlsym(sdl.lib, "SDL_SetHint");
    *(void **)&sdl.RenderSetLogicalSize = plat_dlsym(sdl.lib, "SDL_RenderSetLogicalSize");
    if (sdl.SetHint) sdl.SetHint("SDL_RENDER_SCALE_QUALITY", "linear");
    if (sdl.SetHint) sdl.SetHint("SDL_MOUSE_RELATIVE_SCALING", "0");   /* mouse look speed independent of window size */
#define OPTV(f) *(void **)&sdl.f = plat_dlsym(sdl.lib, "SDL_" #f);
    OPTV(Vulkan_GetInstanceExtensions) OPTV(Vulkan_CreateSurface) OPTV(Vulkan_GetDrawableSize) OPTV(SetWindowFullscreen) OPTV(GetWindowFlags)
#undef OPTV
    sdl.w = w;
    sdl.h = h;
    sdl.flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | (video_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (video_want_vulkan && sdl.Vulkan_GetInstanceExtensions && sdl.Vulkan_CreateSurface && sdl.Vulkan_GetDrawableSize) {
        sdl.win = sdl.CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h, sdl.flags | SDL_WINDOW_VULKAN);
        if (sdl.win) sdl.vulkan = 1;
        else printf("[video] no Vulkan window (%s)\n", sdl.GetError());
    } else if (video_want_vulkan) {
        printf("[video] no Vulkan window (SDL2 without SDL_Vulkan_*)\n");
    }
    if (!sdl.win) sdl.win = sdl.CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h, sdl.flags);
    if (!sdl.win || (!sdl.vulkan && sdl_make_renderer())) {
        printf("[video] SDL window/renderer: %s\n", sdl.GetError());
        sdl.Quit();
        return -1;
    }
#define OPT(f) *(void **)&sdl.f = plat_dlsym(sdl.lib, "SDL_" #f);
    OPT(InitSubSystem) OPT(NumJoysticks) OPT(IsGameController) OPT(GameControllerOpen) OPT(GameControllerClose)
    OPT(GameControllerGetAttached) OPT(GameControllerGetButton) OPT(GameControllerGetAxis) OPT(GameControllerName)
    OPT(GameControllerGetJoystick) OPT(JoystickGetGUID) OPT(GameControllerRumble) OPT(RWFromFile)
    OPT(GameControllerAddMappingsFromRW)
#undef OPT
    if (!sdl.InitSubSystem || !sdl.NumJoysticks || !sdl.IsGameController || !sdl.GameControllerOpen || !sdl.GameControllerClose ||
        !sdl.GameControllerGetAttached || !sdl.GameControllerGetButton || !sdl.GameControllerGetAxis || !sdl.GameControllerName ||
        sdl.InitSubSystem(SDL_INIT_GAMECONTROLLER)) {
        printf("[input] no SDL game controller support (keyboard only)\n");
        sdl.GameControllerOpen = NULL;
    } else {
        if (sdl.GameControllerRumble) input_rumble_hook = sdl_rumble;
        sdl_pad_mappings();
    }
    sdl_pad_open();
    *(void **)&sdl.SetRelativeMouseMode = plat_dlsym(sdl.lib, "SDL_SetRelativeMouseMode");
    printf("[input] mouse look: %s\n", sdl.SetRelativeMouseMode ? CAPTURE_KEY_HELP : "unavailable (no SDL_SetRelativeMouseMode)");
    return 0;
}
static void sdl_present(const uint32_t *px, int w, int h, int pitch) {
    if (!sdl.tex) return;   /* a Vulkan window: the renderer presents */
    if (w != sdl.tw || h != sdl.th) {   /* resolution change: recreate the streaming texture */
        sdl.DestroyTexture(sdl.tex);
        sdl.tex = sdl.CreateTexture(sdl.ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (sdl.RenderSetLogicalSize) sdl.RenderSetLogicalSize(sdl.ren, w, h);
        sdl.tw = w;
        sdl.th = h;
        if (!sdl.tex) return;
    }
    sdl.UpdateTexture(sdl.tex, NULL, px, pitch);
    sdl.RenderClear(sdl.ren);
    sdl.RenderCopy(sdl.ren, sdl.tex, NULL, NULL);   /* scaled to the window: linear, letterboxed (logical size) */
    sdl.RenderPresent(sdl.ren);
}
static void sdl_capture(int on) {
    if (!sdl.SetRelativeMouseMode || on == sdl.captured) return;
    if (on && sdl.SetRelativeMouseMode(1)) { printf("[input] mouse capture failed: %s\n", sdl.GetError()); return; }
    if (!on) sdl.SetRelativeMouseMode(0);
    sdl.captured = on;
    input_mouse_reset();
    printf("[input] mouse %s\n", on ? "captured (F1 or Escape releases)" : "released");
}
static int sdl_pump(void) {
    static const int buttons[5] = {INPUT_MOUSE_LEFT, INPUT_MOUSE_MIDDLE, INPUT_MOUSE_RIGHT, INPUT_MOUSE_X1, INPUT_MOUSE_X2};
    SDL_Event e;
    while (sdl.PollEvent(&e)) {
        if (e.type == SDL_QUIT) return 0;
        if (e.type == SDL_KEYDOWN && SDL_EVENT_KEYSYM(&e) == SDLK_ESCAPE) {
            if (SDL_EVENT_REPEAT(&e)) continue;   /* a held Escape releases the mouse, then quits only on a new press */
            if (!sdl.captured) return 0;
            sdl_capture(0);
            continue;
        }
        /* F11 or Alt+Enter: desktop fullscreen on/off (the renderer follows the new size). */
        if (e.type == SDL_KEYDOWN && (SDL_EVENT_KEYSYM(&e) == SDLK_F11 || (SDL_EVENT_KEYSYM(&e) == '\r' && (SDL_EVENT_KEYMOD(&e) & KMOD_ALT)))) {
            if (!SDL_EVENT_REPEAT(&e) && sdl.SetWindowFullscreen && sdl.GetWindowFlags) {
                int full = (sdl.GetWindowFlags(sdl.win) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP;
                sdl.SetWindowFullscreen(sdl.win, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
            continue;
        }
        if (e.type == SDL_KEYDOWN && SDL_EVENT_KEYSYM(&e) == SDLK_F1) {
            if (!SDL_EVENT_REPEAT(&e)) sdl_capture(!sdl.captured);
            continue;
        }
        if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP)   /* held keys: SDL repeats are flagged and ignored */
            input_key_event(sdl_key(SDL_EVENT_KEYSYM(&e)), e.type == SDL_KEYDOWN, SDL_EVENT_REPEAT(&e));
        if (e.type == SDL_MOUSEBUTTONDOWN && !sdl.captured) { sdl_capture(1); continue; }   /* the capturing click */
        if ((e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) && sdl.captured)
            input_key_event(mouse_button_key(SDL_EVENT_BUTTON(&e), buttons, 5), e.type == SDL_MOUSEBUTTONDOWN, 0);
        if (e.type == SDL_MOUSEMOTION && sdl.captured) input_mouse_motion(SDL_EVENT_XREL(&e), SDL_EVENT_YREL(&e));
        if (e.type == SDL_WINDOWEVENT && e.pad[12] == SDL_WINDOWEVENT_FOCUS_LOST) { sdl_capture(0); input_release_all(); }
        if (e.type == SDL_CONTROLLERDEVICEADDED) sdl_pad_open();
    }
    if (sdl.captured) input_mouse_frame(now_s());
    sdl_pad_read();   /* also notices a removed controller (SDL_CONTROLLERDEVICEREMOVED) */
    return 1;
}
static void sdl_close(void) {
    for (int p = 0; p < INPUT_PORTS; p++)
        if (sdl.port[p].pad) sdl.GameControllerClose(sdl.port[p].pad);
    if (sdl.tex) sdl.DestroyTexture(sdl.tex);
    if (sdl.ren) sdl.DestroyRenderer(sdl.ren);
    if (sdl.win) sdl.DestroyWindow(sdl.win);
    sdl.Quit();
}
static const VideoBackend sdl_backend = {"sdl", sdl_open, sdl_present, sdl_pump, sdl_close};

const char *const *video_vulkan_extensions(uint32_t *n) {
    static const char *names[16];
    unsigned c = 16;
    *n = 0;
    if (!sdl.vulkan || !sdl.Vulkan_GetInstanceExtensions(sdl.win, &c, names)) return NULL;
    *n = c;
    return names;
}
int video_vulkan_surface(void *instance, uint64_t *surface) {
    if (!sdl.vulkan || !sdl.Vulkan_CreateSurface(sdl.win, instance, surface)) {
        printf("[video] SDL_Vulkan_CreateSurface: %s\n", sdl.vulkan ? sdl.GetError() : "no Vulkan window");
        return -1;
    }
    return 0;
}
void video_vulkan_drawable(int *w, int *h) {
    *w = *h = 0;
    if (sdl.vulkan) sdl.Vulkan_GetDrawableSize(sdl.win, w, h);
}
int video_vulkan_fallback(void) {
    if (!sdl.vulkan) return 0;
    sdl.vulkan = 0;
    sdl.DestroyWindow(sdl.win);
    sdl.win = sdl.CreateWindow("The Warriors (xbrun)", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, sdl.w, sdl.h, sdl.flags);
    if (sdl.win && !sdl_make_renderer()) return 0;
    printf("[video] SDL window/renderer: %s\n", sdl.GetError());
    return -1;
}

/* ---- plain Xlib (dlopen), for forwarded displays ----
 * SDL2's X11 framebuffer path attaches MIT-SHM whenever the server advertises it and only
 * tolerates BadAccess on failure; servers reached through `ssh -X` answer BadRequest and the
 * default Xlib handler exits the process. This backend sends frames with XPutImage only. */
#define X_KeyPress 2
#define X_KeyRelease 3
#define X_ButtonPress 4
#define X_ButtonRelease 5
#define X_MotionNotify 6
#define X_FocusOut 10
#define X_ButtonPressMask (1L << 2)
#define X_ButtonReleaseMask (1L << 3)
#define X_PointerMotionMask (1L << 6)
#define X_GrabModeAsync 1
#define X_GrabSuccess 0
#define X_KeyReleaseMask (1L << 1)
#define X_FocusChangeMask (1L << 21)
#define X_ClientMessage 33
#define X_ZPixmap 2
#define X_KeyPressMask (1L << 0)
#define X_StructureNotifyMask (1L << 17)
#define XK_Escape 0xff1b
#define XK_BackSpace 0xff08
#define XK_Return 0xff0d
#define XK_Left 0xff51   /* Up, Right, Down follow */
#define XK_KP_Enter 0xff8d
#define XK_F1 0xffbe   /* F2..F12 follow */
#define XK_Tab 0xff09
#define XK_Shift_L 0xffe1   /* Shift_R, Control_L, Control_R follow */
#define XK_Alt_L 0xffe9     /* Alt_R follows */
/* XKeyEvent (Xlib.h); XButtonEvent has the same layout with `button` in place of `keycode`. */
typedef struct {
    int type;
    unsigned long serial;
    int send_event;
    void *display;
    unsigned long window, root, subwindow, time;
    int x, y, x_root, y_root;
    unsigned state, keycode;
    int same_screen;
} XKeyEvent;
typedef union { int type; XKeyEvent key; long pad[24]; } XEvent;
typedef struct { unsigned long pixel; unsigned short red, green, blue; char flags, pad; } XColor;

static struct {
    void *lib, *dpy, *visual, *gc;
    unsigned long win;
    int depth, ww, wh;
    void *(*OpenDisplay)(const char *);
    int (*DefaultScreen)(void *);
    void *(*DefaultVisual)(void *, int);
    int (*DefaultDepth)(void *, int);
    void *(*DefaultGC)(void *, int);
    unsigned long (*RootWindow)(void *, int);
    unsigned long (*BlackPixel)(void *, int);
    unsigned long (*CreateSimpleWindow)(void *, unsigned long, int, int, unsigned, unsigned, unsigned,
                                        unsigned long, unsigned long);
    int (*StoreName)(void *, unsigned long, const char *);
    int (*SelectInput)(void *, unsigned long, long);
    unsigned long (*InternAtom)(void *, const char *, int);
    int (*SetWMProtocols)(void *, unsigned long, unsigned long *, int);
    int (*MapWindow)(void *, unsigned long);
    int (*ResizeWindow)(void *, unsigned long, unsigned, unsigned);
    void *(*CreateImage)(void *, void *, unsigned, int, int, char *, unsigned, unsigned, int, int);
    int (*PutImage)(void *, unsigned long, void *, void *, int, int, int, int, unsigned, unsigned);
    int (*Free)(void *);
    int (*Flush)(void *);
    int (*Pending)(void *);
    int (*NextEvent)(void *, XEvent *);
    int (*PeekEvent)(void *, XEvent *);
    int (*XkbSetDetectableAutoRepeat)(void *, int, int *);   /* optional */
    int filter_repeat;   /* no detectable auto-repeat: drop synthetic release/press pairs */
    /* Pointer capture (optional symbols: no mouse look without them). */
    int (*GrabPointer)(void *, unsigned long, int, unsigned, int, int, unsigned long, unsigned long, unsigned long);
    int (*UngrabPointer)(void *, unsigned long);
    int (*WarpPointer)(void *, unsigned long, unsigned long, int, int, unsigned, unsigned, int, int);
    unsigned long (*CreateBitmapFromData)(void *, unsigned long, const char *, unsigned, unsigned);
    unsigned long (*CreatePixmapCursor)(void *, unsigned long, unsigned long, XColor *, XColor *, unsigned, unsigned);
    unsigned long blank;   /* invisible cursor while captured */
    int captured, last_x, last_y, warp_pending;
    int esc_down, f1_down;   /* held Escape / F1: repeated presses (Xkb detectable auto-repeat) do not re-trigger */
    unsigned long (*LookupKeysym)(XEvent *, int);
    int (*DestroyWindow)(void *, unsigned long);
    int (*CloseDisplay)(void *);
} x11;

static int x11_open(int w, int h, const char *title) {
    x11.lib = plat_dlopen("libX11.so.6");
    if (!x11.lib) { printf("[video] cannot load libX11: %s\n", plat_dlerror()); return -1; }
#define SYM(f) if (!(*(void **)&x11.f = plat_dlsym(x11.lib, "X" #f))) { printf("[video] X%s missing\n", #f); return -1; }
    SYM(OpenDisplay) SYM(DefaultScreen) SYM(DefaultVisual) SYM(DefaultDepth) SYM(DefaultGC) SYM(RootWindow)
    SYM(BlackPixel) SYM(CreateSimpleWindow) SYM(StoreName) SYM(SelectInput) SYM(InternAtom) SYM(SetWMProtocols)
    SYM(MapWindow) SYM(ResizeWindow) SYM(CreateImage) SYM(PutImage) SYM(Free) SYM(Flush) SYM(Pending)
    SYM(NextEvent) SYM(PeekEvent) SYM(LookupKeysym) SYM(DestroyWindow) SYM(CloseDisplay)
#undef SYM
    x11.dpy = x11.OpenDisplay(NULL);
    if (!x11.dpy) { printf("[video] cannot open X display\n"); return -1; }
    int scr = x11.DefaultScreen(x11.dpy);
    x11.depth = x11.DefaultDepth(x11.dpy, scr);
    if (x11.depth != 24 && x11.depth != 32) {   /* frames are XRGB8888 */
        printf("[video] unsupported X depth %d\n", x11.depth);
        x11.CloseDisplay(x11.dpy);
        return -1;
    }
    x11.visual = x11.DefaultVisual(x11.dpy, scr);
    x11.gc = x11.DefaultGC(x11.dpy, scr);
    x11.win = x11.CreateSimpleWindow(x11.dpy, x11.RootWindow(x11.dpy, scr), 0, 0, w, h, 0,
                                     x11.BlackPixel(x11.dpy, scr), x11.BlackPixel(x11.dpy, scr));
    x11.StoreName(x11.dpy, x11.win, title);
    x11.SelectInput(x11.dpy, x11.win, X_KeyPressMask | X_KeyReleaseMask | X_FocusChangeMask | X_StructureNotifyMask |
                                          X_ButtonPressMask | X_ButtonReleaseMask | X_PointerMotionMask);
    unsigned long del = x11.InternAtom(x11.dpy, "WM_DELETE_WINDOW", 0);
    x11.SetWMProtocols(x11.dpy, x11.win, &del, 1);
    /* Held keys: with detectable auto-repeat the server sends only repeated KeyPress events
     * (ignored as presses of a held key); otherwise x11_pump drops the release/press pairs. */
    *(void **)&x11.XkbSetDetectableAutoRepeat = plat_dlsym(x11.lib, "XkbSetDetectableAutoRepeat");
    int supported = 0;
    if (x11.XkbSetDetectableAutoRepeat) x11.XkbSetDetectableAutoRepeat(x11.dpy, 1, &supported);
    x11.filter_repeat = !supported;
    printf("[input] x11 key auto-repeat: %s\n", supported ? "detectable (Xkb)" : "filtered (release/press pairs)");
#define OPT(f) *(void **)&x11.f = plat_dlsym(x11.lib, "X" #f);
    OPT(GrabPointer) OPT(UngrabPointer) OPT(WarpPointer) OPT(CreateBitmapFromData) OPT(CreatePixmapCursor)
#undef OPT
    if (x11.GrabPointer && x11.UngrabPointer && x11.WarpPointer && x11.CreateBitmapFromData && x11.CreatePixmapCursor) {
        static const char bits[8];
        XColor black = {0};
        unsigned long pix = x11.CreateBitmapFromData(x11.dpy, x11.win, bits, 8, 8);
        x11.blank = pix ? x11.CreatePixmapCursor(x11.dpy, pix, pix, &black, &black, 0, 0) : 0;
    }
    printf("[input] mouse look: %s\n", x11.blank ? CAPTURE_KEY_HELP : "unavailable (pointer grab symbols missing)");
    x11.MapWindow(x11.dpy, x11.win);
    x11.Flush(x11.dpy);
    x11.ww = w;
    x11.wh = h;
    return 0;
}
static void x11_present(const uint32_t *px, int w, int h, int pitch) {
    if (w != x11.ww || h != x11.wh) {
        x11.ResizeWindow(x11.dpy, x11.win, w, h);
        x11.ww = w;
        x11.wh = h;
    }
    /* The image only borrows px; XFree releases the XImage header, not the pixels. */
    void *img = x11.CreateImage(x11.dpy, x11.visual, x11.depth, X_ZPixmap, 0, (char *)px, w, h, 32, pitch);
    if (!img) return;
    x11.PutImage(x11.dpy, x11.win, x11.gc, img, 0, 0, 0, 0, w, h);
    x11.Free(img);
    x11.Flush(x11.dpy);
}
/* Keysyms of printable keys are their Latin-1 code (index 0: unshifted, lowercase letters). */
static int x11_key(unsigned long ks) {
    if (ks == XK_Return || ks == XK_KP_Enter) return '\r';
    if (ks == XK_BackSpace) return '\b';
    if (ks == XK_Tab) return '\t';
    if (ks >= XK_F1 && ks <= XK_F1 + 11) return INPUT_KEY_F1 + (int)(ks - XK_F1);
    if (ks >= XK_Shift_L && ks <= XK_Shift_L + 3)
        return (const int[]){INPUT_KEY_LSHIFT, INPUT_KEY_RSHIFT, INPUT_KEY_LCTRL, INPUT_KEY_RCTRL}[ks - XK_Shift_L];
    if (ks == XK_Alt_L || ks == XK_Alt_L + 1) return ks == XK_Alt_L ? INPUT_KEY_LALT : INPUT_KEY_RALT;
    if (ks >= XK_Left && ks <= XK_Left + 3) return (const int[]){INPUT_KEY_LEFT, INPUT_KEY_UP, INPUT_KEY_RIGHT, INPUT_KEY_DOWN}[ks - XK_Left];
    return ks >= 0x20 && ks < 0x7f ? (int)ks : -1;
}
/* Mouse look under X11: the pointer is grabbed (confined to the window, invisible cursor) and
 * warped back to the window centre whenever it leaves the middle half, and motion is the change
 * from the previous pointer position. The warp's own MotionNotify lands exactly on the centre
 * and only resets the reference point, so motion still in flight before the warp is counted
 * against the old position. */
static void x11_capture(int on) {
    if (!x11.blank || on == x11.captured) return;
    if (on) {
        int r = x11.GrabPointer(x11.dpy, x11.win, 1, X_ButtonPressMask | X_ButtonReleaseMask | X_PointerMotionMask,
                                X_GrabModeAsync, X_GrabModeAsync, x11.win, x11.blank, 0);
        if (r != X_GrabSuccess) { printf("[input] pointer grab failed (%d)\n", r); return; }
        x11.last_x = x11.ww / 2;
        x11.last_y = x11.wh / 2;
        x11.warp_pending = 1;
        x11.WarpPointer(x11.dpy, 0, x11.win, 0, 0, 0, 0, x11.last_x, x11.last_y);
    } else {
        x11.UngrabPointer(x11.dpy, 0);
    }
    x11.Flush(x11.dpy);
    x11.captured = on;
    input_mouse_reset();
    printf("[input] mouse %s\n", on ? "captured (F1 or Escape releases)" : "released");
}
static void x11_motion(int x, int y) {
    int cx = x11.ww / 2, cy = x11.wh / 2;
    if (x11.warp_pending && x == cx && y == cy) { x11.warp_pending = 0; x11.last_x = x; x11.last_y = y; return; }
    input_mouse_motion(x - x11.last_x, y - x11.last_y);
    x11.last_x = x;
    x11.last_y = y;
    if (!x11.warp_pending && (abs(x - cx) > x11.ww / 4 || abs(y - cy) > x11.wh / 4)) {
        x11.warp_pending = 1;
        x11.WarpPointer(x11.dpy, 0, x11.win, 0, 0, 0, 0, cx, cy);
    }
}
static int x11_pump(void) {
    static const int buttons[9] = {INPUT_MOUSE_LEFT, INPUT_MOUSE_MIDDLE, INPUT_MOUSE_RIGHT, -1, -1, -1, -1,
                                   INPUT_MOUSE_X1, INPUT_MOUSE_X2};   /* 4-7: wheel */
    XEvent e;
    while (x11.Pending(x11.dpy)) {
        x11.NextEvent(x11.dpy, &e);
        if (e.type == X_ClientMessage) return 0;   /* only WM_DELETE_WINDOW is registered */
        if (e.type == X_KeyRelease && x11.filter_repeat && x11.Pending(x11.dpy)) {
            XEvent n;
            x11.PeekEvent(x11.dpy, &n);
            RawKeyEvent rel = {0, e.key.keycode, e.key.time}, next = {n.type == X_KeyPress, n.key.keycode, n.key.time};
            if (input_is_repeat_release(&rel, &next)) { x11.NextEvent(x11.dpy, &n); continue; }
        }
        unsigned long ks = e.type == X_KeyPress || e.type == X_KeyRelease ? x11.LookupKeysym(&e, 0) : 0;
        if (ks == XK_Escape || ks == XK_F1) {
            int *down = ks == XK_Escape ? &x11.esc_down : &x11.f1_down, fresh = e.type == X_KeyPress && !*down;
            *down = e.type == X_KeyPress;
            if (fresh && ks == XK_F1) x11_capture(!x11.captured);
            if (fresh && ks == XK_Escape) {
                if (!x11.captured) return 0;
                x11_capture(0);
            }
            continue;
        }
        if (e.type == X_ButtonPress && !x11.captured) { x11_capture(1); continue; }   /* the capturing click */
        if ((e.type == X_ButtonPress || e.type == X_ButtonRelease) && x11.captured)
            input_key_event(mouse_button_key((int)e.key.keycode, buttons, 9), e.type == X_ButtonPress, 0);
        if (e.type == X_MotionNotify && x11.captured) x11_motion(e.key.x, e.key.y);
        if (ks) input_key_event(x11_key(ks), e.type == X_KeyPress, 0);
        if (e.type == X_FocusOut) { x11_capture(0); input_release_all(); x11.esc_down = x11.f1_down = 0; }
    }
    if (x11.captured) input_mouse_frame(now_s());
    return 1;
}
static void x11_close(void) {
    x11.DestroyWindow(x11.dpy, x11.win);
    x11.CloseDisplay(x11.dpy);
}
static const VideoBackend x11_backend = {"x11", x11_open, x11_present, x11_pump, x11_close};

/* A DISPLAY with a host part ("localhost:10.0") is a forwarded or network server. */
static int display_is_remote(void) {
    const char *d = getenv("DISPLAY");
    return d && d[0] && d[0] != ':' && strncmp(d, "unix:", 5) != 0;
}

static const VideoBackend *cur;

const char *video_open(const char *pref, int w, int h, const char *title) {
    cur = &null_backend;
    if (pref && !strcmp(pref, "sdl") && display_is_remote() && x11_backend.open(w, h, title) == 0) cur = &x11_backend;
    else if (pref && !strcmp(pref, "sdl") && sdl_backend.open(w, h, title) == 0) cur = &sdl_backend;
    else if (pref && !strcmp(pref, "x11") && x11_backend.open(w, h, title) == 0) cur = &x11_backend;
    else null_backend.open(w, h, title);
    printf("[video] backend %s, %dx%d\n", cur->name, w, h);
    return cur->name;
}
void video_present(const uint32_t *px, int w, int h, int pitch) { if (cur) cur->present(px, w, h, pitch); }
int video_pump(void) { return cur ? cur->pump() : 1; }
void video_close(void) { if (cur) cur->close(); cur = NULL; }

void video_fit(int sw, int sh, int dw, int dh, int r[4]) {
    r[0] = r[1] = r[2] = r[3] = 0;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    if ((int64_t)dw * sh >= (int64_t)dh * sw) {   /* destination wider: full height, pillarbox */
        r[3] = dh;
        r[2] = (int)(((int64_t)dh * sw * 2 + sh) / (2 * sh));
    } else {                                     /* taller: full width, letterbox */
        r[2] = dw;
        r[3] = (int)(((int64_t)dw * sh * 2 + sw) / (2 * sw));
    }
    r[0] = (dw - r[2]) / 2;
    r[1] = (dh - r[3]) / 2;
}

int video_write_bmp(const char *path, const uint32_t *px, int w, int h, int pitch) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int row = (w * 3 + 3) & ~3;
    uint32_t data = (uint32_t)row * h, file = 54 + data;
    uint8_t hdr[54] = {'B', 'M'};
#define PUT32(o, v) do { uint32_t v_ = (v); memcpy(hdr + (o), &v_, 4); } while (0)
    PUT32(2, file); PUT32(10, 54); PUT32(14, 40); PUT32(18, w); PUT32(22, h);
    hdr[26] = 1; hdr[28] = 24; PUT32(34, data);
#undef PUT32
    fwrite(hdr, 1, sizeof hdr, f);
    uint8_t *line = calloc(1, row);
    for (int y = h - 1; y >= 0; y--) {   /* bottom-up rows */
        const uint32_t *src = (const uint32_t *)((const uint8_t *)px + (size_t)y * pitch);
        for (int x = 0; x < w; x++) {
            line[x * 3] = src[x] & 0xFF;
            line[x * 3 + 1] = (src[x] >> 8) & 0xFF;
            line[x * 3 + 2] = (src[x] >> 16) & 0xFF;
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    return fclose(f) ? -1 : 0;
}
