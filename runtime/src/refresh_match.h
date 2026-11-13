/* --refresh-match (default) / --no-refresh-match: in fullscreen, a display whose refresh rate is
 * not a multiple of 60 Hz (144, 165, 75, ...) is switched to a mode of the same size whose rate
 * is, the highest one (120 before 60), so every 60 Hz frame stays on screen for the same number
 * of refreshes instead of alternating between 2 and 3 (the cadence hitch of 60 fps on 144 Hz).
 * The SDL side lives in video.c; this unit is the pure choice and its [video] log line.
 *
 * Rates are whole hertz as SDL2 reports them: 59.94 Hz modes read 59 (or 60), 119.88 read 119,
 * so a rate within REFRESH_MATCH_TOLERANCE_HZ of a multiple of 60 counts as one. */
#ifndef REFRESH_MATCH_H
#define REFRESH_MATCH_H
#include <stddef.h>

#define REFRESH_MATCH_TOLERANCE_HZ 1

typedef struct {
    int w, h, hz;   /* hz 0: unknown */
} RefreshMode;

typedef enum {
    REFRESH_SWITCH,      /* switch the display to modes[*pick] */
    REFRESH_OFF,         /* --no-refresh-match */
    REFRESH_WINDOWED,    /* not fullscreen: the desktop's rate is left alone */
    REFRESH_UNKNOWN,     /* the display reports no rate */
    REFRESH_ALREADY,     /* the rate is already a multiple of 60 */
    REFRESH_NO_MODE,     /* no mode of the desktop's size has a multiple of 60 */
} RefreshDecision;

/* 1 when arg is --refresh-match (sets *enabled) or --no-refresh-match (clears it), 0 otherwise. */
int refresh_match_parse_arg(const char *arg, int *enabled);

/* 1 when hz is within REFRESH_MATCH_TOLERANCE_HZ of 60, 120, 180, ... (hz <= 0: 0). */
int refresh_match_is_multiple(int hz);

/* Among the modes of the desktop's size whose rate is a multiple of 60: the highest multiple,
 * then the rate nearest that exact multiple (120 before 119), then the first listed. Returns its
 * index, or -1 when there is none. */
int refresh_match_pick(const RefreshMode *modes, int n, const RefreshMode *desktop);

/* The whole decision for a window about to go fullscreen (or not) on a display whose desktop
 * mode is *desktop. *pick is the mode index for REFRESH_SWITCH, else -1. line receives the
 * one-line "[video] ..." report (no newline); for REFRESH_SWITCH it is the line to print once
 * the switch has succeeded. */
RefreshDecision refresh_match_decide(int enabled, int fullscreen, const RefreshMode *desktop, const RefreshMode *modes, int n,
                                     int *pick, char *line, size_t cap);

/* The exclusive switch is deferred until the window has presented a frame in desktop fullscreen;
 * after it, a watchdog runs for REFRESH_MATCH_WATCH_S seconds and reverts to desktop fullscreen
 * when the switch left the game without a visible frame (a minimized exclusive window has no
 * swapchain: frames are rendered but never presented, which looks like a hang). */
#define REFRESH_MATCH_WATCH_S 3.0

typedef enum {
    REFRESH_WATCH_WAIT,        /* keep watching */
    REFRESH_WATCH_OK,          /* frames were presented: the watchdog stops */
    REFRESH_WATCH_NO_FRAME,    /* revert: no frame presented within the window */
    REFRESH_WATCH_MINIMIZED,   /* revert: minimized although the window never lost focus */
} RefreshWatch;

/* elapsed_s since the switch, frames presented (visible window) since it, whether the window is
 * minimized now, and whether it lost focus since the switch (Alt+Tab, Win+D, another program
 * taking the foreground). A minimize without a focus loss reverts at once; with one it waits,
 * but a window that showed no frame by the end of the window reverts in every case. */
RefreshWatch refresh_match_watch(double elapsed_s, int frames, int minimized, int focus_lost);
/* The reason text of the [video] revert line ("ok" / "watching" for the others). */
const char *refresh_match_watch_reason(RefreshWatch w);
#endif
