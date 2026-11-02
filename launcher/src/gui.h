/* Launcher state and the Dear ImGui window: a sidebar of pages (Home, Display, Game, Controls, Options) with
 * the profile, the run status and PLAY at its bottom, and the selected page as frosted cards of
 * settings rows over the background (the blurred title image of the player's own game, drawn by
 * main.cpp). A missing profiles file (or setup_done = 0) opens the first-run wizard instead. The
 * state and Play also back the headless --print-argv / --play modes and the --capture
 * screenshots. */
#ifndef LAUNCHER_GUI_H
#define LAUNCHER_GUI_H
#include <string>
#include <vector>
#include "backup.h"
#include "iso.h"
#include "keymap.h"
#include "padmap.h"
#include "platform.h"
#include "profile.h"
#include "report.h"
#include "xbe_check.h"

namespace launcher {

enum class Page { Home, Display, Game, Controls, Options, Count };
const char *page_key(Page page);                    /* home|display|game|controls|options */
/* Also takes the launcher v2 keys (play, graphics, hud, gameplay, setup, advanced). */
bool page_from_key(const std::string &key, Page &out);

/* Getting the game from the player's own disc image (wizard step 2 and the Game page): the
 * image is read on the UI thread when chosen (only its folder tables and default.xbe header),
 * then extracted by a worker thread. */
struct IsoJob; /* gui.cpp: the worker's state */
struct IsoSetup {
    bool use_folder = false;     /* "I already have an extracted folder" instead */
    bool setup_open = false;     /* the Game page's fold-out */
    std::string iso, dest;       /* the chosen image; the folder it goes to (default <install>/game) */
    std::string opened;          /* the iso path img and xbe were read from */
    bool img_ok = false;
    std::string img_err;
    IsoImage img;
    XbeCheck xbe;
    std::string space_for;       /* the dest need/avail were computed for */
    uint64_t need = 0, avail = 0;
    bool avail_known = false;
    IsoJob *job = nullptr;       /* while a worker extracts */
    std::string result;          /* the last extraction's outcome, in plain language */
    bool result_ok = false;
};

/* An attached controller as main.cpp reads it each frame (SDL_GameController button and axis
 * order: A B X Y Back Guide Start LStick RStick LB RB Up Down Left Right; LX LY RX RY LT RT), or a
 * joystick SDL does not know as a game controller (is_controller false, no button state). */
struct PadView {
    bool connected = false, can_rumble = false;
    bool is_controller = false;
    bool custom = false;              /* the player's line from gamecontrollerdb.txt is in use */
    std::string name, guid;           /* name: SDL's; guid: SDL_JoystickGetGUIDString */
    std::string key;                  /* its ControllerPref key (controller_key: GUID, GUID#K) */
    int nth = 1;                      /* 1 + the controllers with its GUID attached before it */
    bool button[15] = {};
    float axis[6] = {};               /* -1..1 (triggers 0..1) */
    PadRaw raw;                       /* the joystick's own buttons, axes and hats */
};

/* The controller set-up of the Controls page (padmap.h), and its requests to main.cpp. */
struct PadMapState {
    bool active = false;
    int step = 0;                     /* PAD_TARGETS index; PAD_TARGET_COUNT: the save step */
    PadBinding binds[PAD_TARGET_COUNT];
    PadCapture capture;
    std::string guid, name;           /* of the controller being set up */
    std::string last;                 /* the last captured input, in plain words */
    std::string notice;
    bool notice_warn = false;
    std::string apply;                /* main.cpp: a mapping line to add now */
    std::string reset_guid;           /* main.cpp: re-add SDL's own mapping of this GUID */
};

/* The Controls page: the key map of the selected profile's key map file, and a key capture. */
struct ControlsState {
    std::string path;                 /* the file the map was loaded from / is saved to */
    bool loaded = false, file_exists = false;
    KeyMap map;
    int ignored = 0;                  /* invalid lines when it was loaded */
    int capture_control = -1, capture_slot = 0, capture_frames = 0;
    std::string notice;
    bool notice_warn = false;
};

/* The Options page's save backups of the selected profile (backup.h). */
struct BackupState {
    std::string save, root;           /* the save folder and its backups the list is of */
    std::vector<BackupInfo> list;
    double listed = -1;               /* when list was read */
    int confirm = -1;                 /* the entry the restore question is about */
    std::string notice;               /* the last backup or restore, in plain words */
    bool notice_warn = false;
};

/* The Home page's log viewer: the last game log, read back with a level filter. */
struct LogViewState {
    bool open = false;
    std::string path;                 /* the log lines is of */
    std::vector<std::string> lines;
    std::vector<LogLevel> levels;
    int filter = 0;                   /* 0 all, 1 warnings and errors, 2 errors */
    double read = -1;                 /* when lines was read */
    uint64_t size = 0;                /* the log's size then */
    bool to_end = false;              /* scroll to the last line on the next frame */
    std::string notice;
};

struct App {
    std::string ini_path, log_dir;
    ProfileFile file;
    std::vector<std::string> warnings;  /* from loading the profiles file */
    int selected = 0;
    bool dirty = false;                 /* saved once no widget is being edited */
    bool read_only = false;             /* --capture: never write the profiles file */
    std::string message;                /* last error or notice for the sidebar */

    CopyCheck check;                    /* of the selected profile, refreshed on edits and timed */
    double check_time = -1;

    Page page = Page::Home;
    bool wizard = false;                /* the first-run wizard replaces the pages */
    int wizard_step = 0;                /* 0-3 */
    bool hud_customize = false;         /* the HUD bars' customize rows (Display page) */
    std::string desktop_res;            /* "WxH" of the desktop, empty when unknown */
    float ui_scale = 1;                 /* interface scale in use (main.cpp; uiscale.h) */
    float ui_scale_auto = 1;            /* what Auto picks on this screen */
    bool quit = false;                  /* set by Play with "Close launcher when the game starts" */

    IsoSetup iso;
    ControlsState controls;
    BackupState backups;
    LogViewState log_view;
    std::vector<PadView> pads;          /* every attached controller, in the order they were opened (main.cpp) */
    PadView pad;                        /* the one the Controller card shows and sets up (main.cpp) */
    std::string pad_selected;           /* its key (Test in the Controllers card); empty: the first */
    bool pad_scroll = false;            /* bring the Controller card into view on the next frame */
    PadMapState padmap;
    bool rumble_test = false;           /* set by the Controls page, consumed by main.cpp */
    std::string background_note;        /* where the background came from, or why it is plain */

    Child child;
    bool launched = false;
    std::string run_profile, log_path;
};

/* Loads ini_path (a missing file gives one recommended profile and the wizard) and selects the
 * saved profile and page. */
void app_load(App &app, const std::string &ini_path, const std::string &log_dir);
bool app_save(App &app);
Profile *app_profile(App &app);
int app_find(const App &app, const std::string &name); /* -1 when absent */

/* The game folder the background is made from: the profile's game copy once it checks out as The
 * Warriors, else empty (plain background). */
std::string app_game_dir(App &app);

/* The argv Play starts: build_argv with the controllers' players (--pad-slot) and the xbrun path
 * resolved (xbrun_path), run in the xbrun binary's folder. */
std::vector<std::string> launch_argv(const App &app, const Profile &p);
bool app_play(App &app, std::string *err);
/* The selected profile's save folder (as the game sees its hdd) and its backup folder (empty when
 * the user has no data folder). */
std::string app_save_dir(App &app);
std::string app_backup_root(App &app);
/* "Copy report for a bug" of a log (scrubbed, see report.h); also --report. */
std::string app_bug_report(App &app, const std::string &log_path);

/* Cancels and waits for a running extraction (before the launcher exits). */
void app_shutdown(App &app);
/* --capture of the disc image step: a chosen image (fake numbers, no file read) and, when
 * extracting, a worker part-way through. */
void app_capture_iso_demo(App &app, bool extracting);

/* One frame of the launcher window. */
void app_draw(App &app, double now);

} // namespace launcher
#endif
