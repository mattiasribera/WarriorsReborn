/* warriors-launcher [profiles.ini]
 *   --smoke <ini>                 load the profiles, draw a few frames, exit 0 (headless CI with
 *                                 SDL_VIDEODRIVER=dummy)
 *   --capture <page> <png> [ini]  draw one page off-screen (software renderer, no window) and
 *                                 write it as a PNG; page = home|display|game|controls|options|wizard1-4
 *                                 (a -es suffix draws it in Spanish)
 *                                 (or the launcher v2 names play, graphics, hud, gameplay, setup,
 *                                 advanced), or the disc image step with a made-up image:
 *                                 wizard2-iso (chosen), wizard2-extract (extracting), setup-iso
 *                                 (the Game page's fold-out, extracting), log (Home with the log
 *                                 viewer of last_log), home-novulkan / home-renderer-none
 *                                 (Home with the no-3D warning). The Vulkan probe's answer
 *                                 is ignored (a driver is assumed). The profiles file is never
 *                                 written.
 *   --print-argv <ini> [profile]  print the argv Play starts, one argument per line
 *   --play <ini> [profile]        start the profile as Play does, wait, exit with its code
 *   --report <ini> [profile]      print the bug report of the last log (Copy report for a bug)
 *   --extract-iso <iso> <dir>     check the disc image's default.xbe, then extract the image into
 *                                 dir (files already there with the same size are skipped);
 *                                 exit 0 done, 1 failed, 3 not The Warriors or not a disc image
 * Without an ini path the profiles live in warriors-launcher.ini next to the launcher, and each
 * Play writes logs/<profile>-<time>.log next to the launcher. The background is the blurred boot
 * splash (or title image) of the profile's own game copy (titleimage.h), cached in the per-user
 * cache folder. */
#include "gui.h"
#include "i18n.h"
#include "iso.h"
#include "imgui.h"
#include "png.h"
#include "theme.h"
#include "titleimage.h"
#include "uiscale.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

using namespace launcher;

#define SMOKE_FRAMES 5
#define WINDOW_W UI_DEFAULT_W
#define WINDOW_H UI_DEFAULT_H
#define CAPTURE_FRAMES 4 /* auto-sized cards settle after two frames */

/* The command line as UTF-8 (main's argv is in the ANSI code page on Windows). */
static std::vector<std::string> utf8_args(int argc, char **argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    (void)argc, (void)argv;
    int n = 0;
    wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 0; i < n; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(len > 0 ? len - 1 : 0, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w[i], -1, &s[0], len, nullptr, nullptr);
        args.push_back(s);
    }
    LocalFree(w);
#else
    for (int i = 0; i < argc; i++) args.push_back(argv[i]);
#endif
    return args;
}

/* Selects the named profile (or the saved one when name is empty). */
static bool select_profile(App &app, const std::string &name) {
    if (name.empty()) return true;
    int i = app_find(app, name);
    if (i < 0) {
        fprintf(stderr, "no profile '%s' in %s\n", name.c_str(), app.ini_path.c_str());
        return false;
    }
    app.selected = i;
    return true;
}

static int headless(App &app, const std::string &mode, const std::string &profile) {
    if (!select_profile(app, profile)) return 2;
    if (mode == "--report") { /* the bug report of the profile's last log, as Copy report puts it on the clipboard */
        const std::string &log = app.file.settings.last_log;
        if (log.empty() || !is_file(log)) {
            fprintf(stderr, "--report: no last log in %s\n", app.ini_path.c_str());
            return 2;
        }
        fputs(app_bug_report(app, log).c_str(), stdout);
        return 0;
    }
    if (mode == "--print-argv") {
        for (const std::string &a : launch_argv(app, *app_profile(app))) printf("%s\n", a.c_str());
        return 0;
    }
    std::string err;
    if (!app_play(app, &err)) {
        fprintf(stderr, "play: %s\n", err.c_str());
        return 2;
    }
    child_wait(app.child);
    printf("log: %s\nexit code: %d\n", app.log_path.c_str(), app.child.exit_code);
    return app.child.exit_code;
}

static void imgui_init(float density = 1) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr; /* the window layout is fixed; profiles are our own file */
    theme_init(density);
}


namespace launcher {
extern const unsigned char icon_ico[]; /* assets/icon/wonder-wheel.ico, embedded (build/fonts.cpp) */
extern const unsigned int icon_ico_size;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* The window icon: the 64x64 32-bit DIB of the embedded .ico (rows bottom-up, B G R A). */
static void set_window_icon(SDL_Window *win) {
    const uint8_t *d = icon_ico;
    uint32_t n = icon_ico_size, count = n >= 6 ? (uint32_t)(d[4] | d[5] << 8) : 0;
    const uint32_t row = 64 * 4;
    for (uint32_t i = 0; i < count && 6 + 16 * (i + 1) <= n; i++) {
        const uint8_t *e = d + 6 + 16 * i;
        uint32_t size = le32(e + 8), off = le32(e + 12);
        if (e[0] != 64 || e[1] != 64 || off > n || n - off < size || size < 40 + 64 * row) continue;
        const uint8_t *b = d + off;
        if (le32(b) != 40 || le32(b + 4) != 64 || (b[14] | b[15] << 8) != 32) continue;
        std::vector<uint8_t> px(64 * row);
        for (int y = 0; y < 64; y++) memcpy(&px[y * row], b + 40 + (63 - y) * row, row);
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(px.data(), 64, 64, 32, row, SDL_PIXELFORMAT_BGRA32);
        if (s) SDL_SetWindowIcon(win, s), SDL_FreeSurface(s);
        return;
    }
}

/* The background: the title image texture, drawn to cover the window; flat dark grey without it. */
struct Background {
    SDL_Texture *tex = nullptr;
    int w = 0, h = 0;
    std::string dir; /* the game folder tex was made from */
    bool tried = false;
    /* For the About note, remade when the launcher's language changes: */
    S note = S::bg_plain_no_game; /* bg_plain_no_game, bg_plain_err (err), bg_plain_sdl (err), bg_splash, bg_title */
    std::string err;
    Lang lang = Lang::Count;
};

static void background_note(Background &bg, App &app) {
    bg.lang = ui_lang();
    app.background_note = bg.note == S::bg_splash || bg.note == S::bg_title ? trf(bg.note, bg.w, bg.h)
                          : bg.note == S::bg_plain_no_game                  ? std::string(tr(bg.note))
                                                                              : trf(bg.note, bg.err.c_str());
}

/* Follows the profile's game copy: a new game folder makes a new texture (from the cache). */
static void background_sync(Background &bg, App &app, SDL_Renderer *ren) {
    std::string dir = app_game_dir(app);
    if (bg.tried && dir == bg.dir) {
        if (bg.lang != ui_lang()) background_note(bg, app);
        return;
    }
    bg.tried = true;
    bg.dir = dir;
    if (bg.tex) SDL_DestroyTexture(bg.tex), bg.tex = nullptr;
    bg.err.clear();
    Image im;
    std::string source;
    if (dir.empty()) bg.note = S::bg_plain_no_game;
    else if (!game_background(dir, user_cache_dir(), im, &source, &bg.err)) bg.note = S::bg_plain_err;
    else if (!(bg.tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, im.w, im.h)))
        bg.note = S::bg_plain_sdl, bg.err = SDL_GetError();
    else {
        SDL_UpdateTexture(bg.tex, nullptr, im.rgba.data(), im.w * 4);
        SDL_SetTextureScaleMode(bg.tex, SDL_ScaleModeLinear);
        bg.w = im.w, bg.h = im.h;
        bg.note = source == "boot splash" ? S::bg_splash : S::bg_title;
    }
    background_note(bg, app);
}

static void background_draw(const Background &bg, SDL_Renderer *ren) {
    SDL_SetRenderDrawColor(ren, 30, 32, 36, 255);
    SDL_RenderClear(ren);
    int ow = 0, oh = 0;
    if (!bg.tex || SDL_GetRendererOutputSize(ren, &ow, &oh) != 0 || ow <= 0 || oh <= 0) return;
    float s = (float)ow / bg.w > (float)oh / bg.h ? (float)ow / bg.w : (float)oh / bg.h; /* cover, centred */
    SDL_Rect src;
    src.w = (int)(ow / s), src.h = (int)(oh / s);
    src.x = (bg.w - src.w) / 2, src.y = (bg.h - src.h) / 2;
    SDL_RenderCopy(ren, bg.tex, &src, nullptr);
}

static void background_free(Background &bg) {
    if (bg.tex) SDL_DestroyTexture(bg.tex);
    bg.tex = nullptr;
}

/* Every attached controller for the Controls page's Controllers card, and the selected one for its
 * preview, vibration test and set-up (a joystick SDL knows as no game controller is opened as a
 * joystick). Before a controller is opened, SDL's own mapping of its GUID is remembered and the
 * player's line from gamecontrollerdb.txt next to the game program is added, as the runtime does
 * at start-up. */
struct PadOpen {
    SDL_GameController *pad = nullptr;
    SDL_Joystick *joy = nullptr;            /* the controller's joystick, or a joystick of our own */
    SDL_JoystickID id = -1;
};
struct PadDevice {
    std::vector<PadOpen> open;              /* in the order they were opened */
    std::string db_path;
    std::map<std::string, std::string> defaults;   /* GUID -> SDL's own mapping ("" none) */
    std::set<std::string> custom;           /* GUIDs with the player's line added */
    bool restart = false;
};

static void pad_close(PadOpen &o) {
    if (o.pad) SDL_GameControllerClose(o.pad);
    else if (o.joy) SDL_JoystickClose(o.joy);
    o = PadOpen();
}
static void pad_close(PadDevice &d) {
    for (PadOpen &o : d.open) pad_close(o);
    d.open.clear();
}

static std::string guid_string(SDL_JoystickGUID g) {
    char s[33];
    SDL_JoystickGetGUIDString(g, s, sizeof s);
    return s;
}

static void pad_prepare(PadDevice &d, const std::string &guid) {
    if (d.defaults.count(guid)) return;
    char *own = SDL_GameControllerMappingForGUID(SDL_JoystickGetGUIDFromString(guid.c_str()));
    d.defaults[guid] = own ? own : "";
    if (own) SDL_free(own);
    std::string db;
    std::string line = !d.db_path.empty() && read_file(d.db_path, db, 4 << 20) ? pad_db_find(db, guid, pad_platform()) : "";
    if (!line.empty() && SDL_GameControllerAddMapping(line.c_str()) >= 0) d.custom.insert(guid);
}

/* A saved, removed or other gamecontrollerdb.txt in use at once. Adding a line is not enough: SDL
 * refreshes an open controller only when the line replaces the mapping entry it is bound to, and
 * a GUID with the name's CRC (DirectInput pads on Windows, 0300605b...) whose default entry has
 * none gets a second entry instead; the ImGui backend's handle of the same controller keeps the
 * stale one open across our close and reopen; and SDL 2 cannot remove a line (Reset). So every
 * handle is closed and the joystick subsystems restarted (SDL's own mappings only); the next
 * pad_prepare re-reads the file. Returns false when SDL cannot restart them. */
static bool pad_restart(PadDevice &d) {
    pad_close(d);
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_AutoFirst);   /* closes the backend's; reopened in its next frame */
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
    d.defaults.clear();
    d.custom.clear();
    return SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0;
}

static PadView pad_view(const PadDevice &d, const PadOpen &o) {
    PadView v;
    v.connected = true;
    v.is_controller = o.pad != nullptr;
    v.guid = guid_string(SDL_JoystickGetGUID(o.joy));
    v.custom = d.custom.count(v.guid) > 0;
    const char *name = o.pad ? SDL_GameControllerName(o.pad) : SDL_JoystickName(o.joy);
    v.name = name ? name : "Game controller";
    for (int b = 0; b < SDL_JoystickNumButtons(o.joy); b++) v.raw.buttons.push_back(SDL_JoystickGetButton(o.joy, b) != 0);
    for (int a = 0; a < SDL_JoystickNumAxes(o.joy); a++) v.raw.axes.push_back(SDL_JoystickGetAxis(o.joy, a));
    for (int h = 0; h < SDL_JoystickNumHats(o.joy); h++) v.raw.hats.push_back(SDL_JoystickGetHat(o.joy, h));
    if (!o.pad) return v;
#if SDL_VERSION_ATLEAST(2, 0, 18)
    v.can_rumble = SDL_GameControllerHasRumble(o.pad);
#else
    v.can_rumble = true;
#endif
    for (int b = 0; b < 15; b++) v.button[b] = SDL_GameControllerGetButton(o.pad, (SDL_GameControllerButton)b);
    for (int a = 0; a < 6; a++) v.axis[a] = SDL_GameControllerGetAxis(o.pad, (SDL_GameControllerAxis)a) / 32767.0f;
    return v;
}

/* app.pads: every attached controller with its key (the Kth with a GUID in the order they were
 * opened, as the runtime counts them at start-up); app.pad: the selected one, else the first. */
static void pad_views(App &app, const PadDevice &d) {
    app.pads.clear();
    for (const PadOpen &o : d.open) {
        PadView v = pad_view(d, o);
        for (const PadView &other : app.pads) v.nth += other.guid == v.guid;
        v.key = controller_key(v.guid, v.nth);
        app.pads.push_back(v);
    }
    size_t sel = 0;
    for (size_t i = 0; i < app.pads.size(); i++)
        if (app.pads[i].key == app.pad_selected) sel = i;
    app.pad = sel < app.pads.size() ? app.pads[sel] : PadView();
    if (app.rumble_test && sel < d.open.size() && d.open[sel].pad) SDL_GameControllerRumble(d.open[sel].pad, 0xa000, 0xa000, 400);
    app.rumble_test = false;
}

static void pad_update(App &app, PadDevice &d) {
    Profile *prof = app_profile(app);
    std::string db = prof ? pad_db_path(*prof) : "";
    if (db != d.db_path) {   /* another game program: its own file (lines of the old one dropped) */
        d.restart = !d.custom.empty() || !d.open.empty();
        d.defaults.clear();
        d.db_path = db;
    }
    PadMapState &m = app.padmap;
    if (!m.apply.empty()) {   /* the set-up saved its line to the file: in use at once */
        m.apply.clear();
        d.restart = true;
    }
    bool reset = !m.reset_guid.empty();   /* the line was removed from the file */
    m.reset_guid.clear();
    if (reset) d.restart = true;
    if (d.restart) {
        bool live = pad_restart(d);
        d.restart = false;
        if (!live) fprintf(stderr, "SDL_InitSubSystem(game controller): %s\n", SDL_GetError());
        if (reset || !live) {
            m.notice = tr(live ? S::padmap_reset_done : S::padmap_reset_restart);
            m.notice_warn = !live;
        }
    }
    for (size_t i = 0; i < d.open.size();) {   /* unplugged: closed (a reconnected one is opened anew) */
        PadOpen &o = d.open[i];
        if (o.pad ? SDL_GameControllerGetAttached(o.pad) : SDL_JoystickGetAttached(o.joy)) {
            i++;
            continue;
        }
        pad_close(o);
        d.open.erase(d.open.begin() + i);
    }
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        bool have = id < 0;
        for (const PadOpen &o : d.open) have |= o.id == id;
        if (have) continue;
        pad_prepare(d, guid_string(SDL_JoystickGetDeviceGUID(i)));
        PadOpen o;
        if (SDL_IsGameController(i)) o.pad = SDL_GameControllerOpen(i);
        o.joy = o.pad ? SDL_GameControllerGetJoystick(o.pad) : SDL_JoystickOpen(i);
        if (!o.joy) {
            if (o.pad) SDL_GameControllerClose(o.pad);
            continue;
        }
        o.id = id;
        d.open.push_back(o);
    }
    pad_views(app, d);
}

/* --capture: the page drawn into a software surface and written as a PNG. */
static int capture(App &app, std::string page, const std::string &png) {
    app.read_only = true;
    app.sync_jobs = true; /* the page as it looks with the GPU probe and the pack scans done */
    /* page:HEIGHT draws a taller window, to see below the fold; page:WxH another size */
    int width = WINDOW_W, height = WINDOW_H;
    size_t colon = page.find(':');
    if (colon != std::string::npos) {
        const char *size = page.c_str() + colon + 1;
        if (strchr(size, 'x')) width = atoi(size), height = atoi(strchr(size, 'x') + 1);
        else height = atoi(size);
        page.resize(colon);
        if (height < 200 || height > 4000) height = WINDOW_H;
        if (width < 400 || width > 4000) width = WINDOW_W;
    }
    float scale = 1; /* page@1.5: drawn at that interface scale */
    size_t at = page.find('@');
    if (at != std::string::npos) {
        scale = ui_scale_from_setting(page.substr(at + 1));
        page.resize(at);
        if (scale <= 0) scale = 1;
    }
    app.ui_scale = scale;
    app.desktop_res = "2560x1440";
    app.vulkan_driver = 1; /* no probe: the same picture on any machine */
    if (page.size() > 3 && page.compare(page.size() - 3, 3, "-es") == 0) {   /* e.g. controls-es: in Spanish */
        page.resize(page.size() - 3);
        set_ui_lang(Lang::Es);
    }
    bool padmap = page == "padmap" || page == "padmap-save";
    if (page == "controls" || padmap) { /* made-up controllers (no device is opened) */
        app.pad.connected = app.pad.can_rumble = app.pad.is_controller = true;
        app.pad.custom = page == "controls";
        app.pad.name = "Xbox Series X Controller (preview)";
        app.pad.guid = "03000000aaaa0000bbbb000000000000";
        app.pad.key = controller_key(app.pad.guid, 1);
        app.pad.button[0] = app.pad.button[9] = true;
        app.pad.axis[0] = 0.7f;
        app.pad.axis[1] = -0.5f;
        app.pad.axis[5] = 0.6f;
        PadView b; /* two identical generic pads, the first renamed and both with a player */
        b.connected = b.is_controller = true;
        b.name = "Generic USB Gamepad";
        b.guid = "03000000cccc00000100000000000000";
        b.key = controller_key(b.guid, 1);
        PadView c = b;
        c.nth = 2;
        c.key = controller_key(c.guid, 2);
        app.pads = {app.pad, b, c};
        app.pad_selected = app.pad.key;
        controller_pref(app.file, b.key).name = "Controller A";
        set_controller_player(app.file, b.key, 1);
        set_controller_player(app.file, app.pad.key, 2);
    }
    Page pg;
    if (padmap) { /* the controller set-up at LT (A to RB done), or at its last step */
        app.wizard = false;
        app.page = Page::Controls;
        PadMapState &m = app.padmap;
        m.active = true;
        m.guid = app.pad.guid;
        m.name = app.pad.name;
        m.capture.start(app.pad.raw);
        m.step = page == "padmap" ? 6 : PAD_TARGET_COUNT;
        for (int i = 0; i < m.step; i++) m.binds[i].kind = PadBinding::Button, m.binds[i].index = i;
        m.last = trf(S::padmap_last, "RB", (trf(S::padmap_raw_button, 5) + " (b5)").c_str());
    } else if (page == "wizard2-iso" || page == "wizard2-extract" || page == "setup-iso") {
        app.wizard = page != "setup-iso";
        app.wizard_step = 1;
        app.page = Page::Game;
        app.iso.setup_open = true;
        app_capture_iso_demo(app, page != "wizard2-iso");
    } else if (page.compare(0, 6, "wizard") == 0 && page.size() == 7 && page[6] >= '1' && page[6] <= '4') {
        app.wizard = true;
        app.wizard_step = page[6] - '1';
    } else if (page == "home-novulkan" || page == "home-renderer-none") { /* Home with the no-3D warning */
        app.wizard = false;
        app.page = Page::Home;
        app.vulkan_driver = page == "home-novulkan" ? 0 : 1;
        if (page == "home-renderer-none") app.file.settings.show_advanced = false, app_profile(app)->renderer = "none";
    } else if (page == "log") { /* Home with the log viewer open (the profile file's last_log) */
        app.wizard = false;
        app.page = Page::Home;
        app.log_view.open = true;
    } else if (page_from_key(page, pg)) {
        app.wizard = false;
        app.page = pg;
        if (page == "advanced" || page == "options") app.file.settings.show_advanced = true;
        if (page == "hud" || page == "display") app.hud_customize = true;
    } else {
        fprintf(stderr, "--capture: unknown page '%s'\n", page.c_str());
        return 2;
    }
    if (SDL_Init(0) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ABGR8888);
    SDL_Renderer *ren = surf ? SDL_CreateSoftwareRenderer(surf) : nullptr;
    if (!ren) {
        fprintf(stderr, "software renderer: %s\n", SDL_GetError());
        return 1;
    }
    imgui_init(scale);
    ImGui_ImplSDLRenderer2_Init(ren);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(width / scale, height / scale); /* logical pixels */
    io.DeltaTime = 1.0f / 60;
    Background bg;
    for (int frame = 0; frame < CAPTURE_FRAMES; frame++) {
        background_sync(bg, app, ren);
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui::NewFrame();
        app_draw(app, frame / 60.0);
        ImGui::Render();
        SDL_RenderSetScale(ren, 1, 1);
        background_draw(bg, ren);
        SDL_RenderSetScale(ren, scale, scale);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
        SDL_RenderPresent(ren);
    }
    bool ok = write_png_rgba(png, surf->w, surf->h, (const uint8_t *)surf->pixels, (size_t)surf->pitch);
    app_shutdown(app);
    if (!ok) fprintf(stderr, "--capture: cannot write %s\n", png.c_str());
    else printf("capture: %s -> %s\n", page.c_str(), png.c_str());
    background_free(bg);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_FreeSurface(surf);
    SDL_Quit();
    return ok ? 0 : 1;
}

/* The usable area of the window's display (the desktop without the task bar, less a title bar)
 * and its DPI (0 when unknown). */
static void display_area(SDL_Window *win, int &w, int &h, float &dpi) {
    int d = win ? SDL_GetWindowDisplayIndex(win) : 0;
    SDL_Rect r;
    w = h = 0;
    if (SDL_GetDisplayUsableBounds(d < 0 ? 0 : d, &r) == 0) w = r.w, h = r.h - 32;
    dpi = 0;
    if (SDL_GetDisplayDPI(d < 0 ? 0 : d, &dpi, nullptr, nullptr) != 0) dpi = 0;
#ifndef _WIN32
    /* X servers without a physical screen size report none: the desktop's own scale settings */
    for (const char *var : {"GDK_SCALE", "QT_SCALE_FACTOR"}) {
        const char *v = getenv(var);
        if (dpi <= 0 && v && atof(v) > 0) dpi = (float)(96 * atof(v));
    }
#endif
}

/* The interface scale the settings ask for (Auto: from the display); a change rebuilds the fonts
 * at the new density and sets the window's minimum size for it. */
static void ui_scale_sync(App &app, SDL_Window *win, bool first) {
    int uw = 0, uh = 0;
    float dpi = 0;
    display_area(win, uw, uh, dpi);
    app.ui_scale_auto = auto_ui_scale(dpi, uw, uh);
    float want = ui_scale_from_setting(app.file.settings.ui_scale);
    if (want <= 0) want = app.ui_scale_auto;
    if (!first && want == app.ui_scale) return;
    app.ui_scale = want;
    theme_fonts(want);
    ImGui_ImplSDLRenderer2_DestroyFontsTexture();
    ImGui_ImplSDLRenderer2_CreateFontsTexture();
    int w, h, mw, mh;
    ui_window_size(want, uw, uh, w, h, mw, mh);
    SDL_SetWindowMinimumSize(win, mw, mh);
    if (first) SDL_SetWindowSize(win, w, h), SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

static int run_gui(App &app, int smoke_frames) {
#ifdef SDL_HINT_WINDOWS_DPI_AWARENESS
    /* Windows: real pixels on a scaled desktop (no blurry bitmap stretch); the interface scale
     * (Auto: the display's DPI) sizes the window and its contents instead. */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("The Warriors \xE2\x80\x94 Launcher", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       WINDOW_W, WINDOW_H, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (win) set_window_icon(win);
    SDL_DisplayMode mode;
    if (win && SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(win), &mode) == 0 && mode.w > 0 && mode.h > 0)
        app.desktop_res = std::to_string(mode.w) + "x" + std::to_string(mode.h);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (win && !ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        fprintf(stderr, "SDL window/renderer: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    imgui_init();
    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);
    ImGuiIO &io = ImGui::GetIO();
    ui_scale_sync(app, win, true);
    /* The GPU probe on a worker from now: it can take seconds (a Vulkan instance wakes a laptop's
     * discrete GPU), and a window that stops taking messages is "Not responding" on Windows. */
    app_gpu_start(app);
    if (smoke_frames) {
        int uw, uh, ww, wh;
        float dpi;
        display_area(win, uw, uh, dpi);
        SDL_GetWindowSize(win, &ww, &wh);
        printf("smoke: interface scale %g (auto %g at %.0f dpi, usable %dx%d), window %dx%d\n", app.ui_scale, app.ui_scale_auto, dpi, uw,
               uh, ww, wh);
    }

    Background bg;
    PadDevice pad;
    for (int frame = 0; !smoke_frames || frame < smoke_frames; frame++) {
        SDL_Event e;
        bool quit = false;
        const float s = app.ui_scale;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            /* ImGui works in logical pixels: the window position divided by the scale */
            if (e.type == SDL_MOUSEMOTION && s != 1) io.AddMousePosEvent(e.motion.x / s, e.motion.y / s);
            quit |= e.type == SDL_QUIT;
        }
        if (quit) break;
        pad_update(app, pad);
        background_sync(bg, app, ren);
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        /* Logical pixels: the window's size and the mouse (also the backend's focused-window
         * fallback position) divided by the scale; drawn at the scale below. */
        float fb = io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1;
        io.DisplaySize = ImVec2(io.DisplaySize.x / s, io.DisplaySize.y / s);
        int mx, my;
        if (s != 1 && SDL_GetMouseFocus() == win) SDL_GetMouseState(&mx, &my), io.AddMousePosEvent(mx / s, my / s);
        ImGui::NewFrame();
        app_draw(app, SDL_GetTicks() / 1000.0);
        ImGui::Render();
        SDL_RenderSetScale(ren, 1, 1);
        background_draw(bg, ren);
        SDL_RenderSetScale(ren, s * fb, s * fb);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
        SDL_RenderPresent(ren);
        if (app.quit) break; /* Play with "Close launcher when the game starts" */
        ui_scale_sync(app, win, false); /* Options changed the interface size: applied from the next frame */
    }
    pad_close(pad);
    SDL_HideWindow(win); /* gone at once while app_shutdown waits for a worker */
    app_shutdown(app); /* a running extraction is cancelled (it resumes next time), the GPU probe waited for */
    if (app.dirty) app_save(app);

    background_free(bg);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

struct ExtractJob {
    IsoImage img;
    std::string dest, err;
    IsoProgress progress;
    IsoResult result = IsoResult::Failed;
    std::atomic<bool> done{false};
};

static void extract_worker(void *arg) {
    ExtractJob *j = (ExtractJob *)arg;
    j->result = iso_extract(j->img, j->dest, &j->progress, &j->err);
    j->done = true;
}

static int extract_iso(const std::string &iso, const std::string &dest) {
    ExtractJob job;
    job.dest = dest;
    std::string err;
    if (!iso_open(iso, job.img, &err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 3;
    }
    XbeCheck xbe = iso_check_xbe(job.img);
    printf("image: partition at 0x%llx, %zu files, %zu folders, %s; default.xbe: %s\n",
           (unsigned long long)job.img.partition, job.img.files.size(), job.img.dirs.size(),
           format_bytes(job.img.total_bytes).c_str(), xbe.title.empty() ? xbe_status_text(xbe.status) : xbe.title.c_str());
    if (xbe.status != XbeStatus::Ok) {
        fprintf(stderr, "this disc image is not The Warriors (%s)\n", xbe_status_text(xbe.status));
        return 3;
    }
    uint64_t need = iso_bytes_needed(job.img, dest), avail = 0;
    std::string probe = dest;
    while (!probe.empty() && !is_dir(probe) && path_dir(probe) != probe) probe = path_dir(probe);
    if (free_space(probe.empty() ? "." : probe, avail) && avail < need) {
        fprintf(stderr, "not enough free space: %s needed, %s free\n", format_bytes(need).c_str(), format_bytes(avail).c_str());
        return 1;
    }
    Thread t;
    if (!thread_start(t, extract_worker, &job)) {
        fprintf(stderr, "cannot start the extraction thread\n");
        return 1;
    }
    int last = -1;
    while (!job.done) {
        int pct = job.img.total_bytes ? (int)(job.progress.bytes * 100 / job.img.total_bytes) : 100;
        if (pct / 10 != last / 10) printf("extract: %d%%\n", pct), fflush(stdout), last = pct;
        SDL_Delay(200);
    }
    thread_join(t);
    if (job.result != IsoResult::Ok) {
        fprintf(stderr, "extract: %s\n", job.err.c_str());
        return 1;
    }
    printf("extract: done, %u files (%u already there), %s -> %s\n", (unsigned)job.progress.files,
           (unsigned)job.progress.skipped, format_bytes(job.img.total_bytes).c_str(), dest.c_str());
    return 0;
}

int main(int argc, char **argv) {
    std::vector<std::string> args = utf8_args(argc, argv);
    if (args.size() == 4 && args[1] == "--extract-iso") return extract_iso(args[2], args[3]);
    if (args.size() >= 4 && args[1] == "--capture" && args.size() <= 5) {
        std::string ini = args.size() == 5 ? args[4] : path_join(exe_dir(), "warriors-launcher.ini");
        App app;
        app_load(app, ini, path_join(exe_dir(), "logs"));
        return capture(app, args[2], args[3]);
    }
    std::string mode, ini = path_join(exe_dir(), "warriors-launcher.ini"), profile;
    size_t i = 1;
    if (i < args.size() && args[i].compare(0, 2, "--") == 0) mode = args[i++];
    if (i < args.size()) ini = args[i++];
    if (i < args.size()) profile = args[i++];
    bool known = mode.empty() || mode == "--smoke" || mode == "--print-argv" || mode == "--play" || mode == "--report";
    if (!known || i < args.size() || (!mode.empty() && args.size() < 3)) {
        fprintf(stderr, "usage: warriors-launcher [profiles.ini] | --smoke <ini> | --print-argv <ini> [profile] |"
                        " --play <ini> [profile] | --report <ini> [profile] | --capture <page> <png> [ini] |"
                        " --extract-iso <iso> <dir>\n");
        return 2;
    }
    if (mode == "--smoke" && !is_file(ini)) {
        fprintf(stderr, "--smoke: no file %s\n", ini.c_str());
        return 2;
    }

    App app;
    app_load(app, ini, path_join(exe_dir(), "logs"));
    for (const std::string &w : app.warnings) fprintf(stderr, "%s: %s\n", ini.c_str(), w.c_str());
    if (mode == "--print-argv" || mode == "--play" || mode == "--report") return headless(app, mode, profile);
    if (mode == "--smoke") printf("smoke: %zu profiles from %s\n", app.file.profiles.size(), ini.c_str());
    return run_gui(app, mode == "--smoke" ? SMOKE_FRAMES : 0);
}
