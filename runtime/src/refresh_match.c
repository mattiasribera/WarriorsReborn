/* --refresh-match: the target display mode and its log line (see refresh_match.h). */
#include "refresh_match.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int refresh_match_parse_arg(const char *arg, int *enabled) {
    if (!strcmp(arg, "--refresh-match")) *enabled = 1;
    else if (!strcmp(arg, "--no-refresh-match")) *enabled = 0;
    else return 0;
    return 1;
}

/* The multiple of 60 nearest hz (at least 1). */
static int nearest_k(int hz) { return hz < 30 ? 1 : (hz + 30) / 60; }

int refresh_match_is_multiple(int hz) {
    return hz > 0 && abs(hz - 60 * nearest_k(hz)) <= REFRESH_MATCH_TOLERANCE_HZ;
}

int refresh_match_pick(const RefreshMode *modes, int n, const RefreshMode *desktop) {
    int best = -1, best_k = 0, best_off = 0;
    for (int i = 0; i < n; i++) {
        const RefreshMode *m = &modes[i];
        if (m->w != desktop->w || m->h != desktop->h || !refresh_match_is_multiple(m->hz)) continue;
        int k = nearest_k(m->hz), off = abs(m->hz - 60 * k);
        if (best < 0 || k > best_k || (k == best_k && off < best_off)) best = i, best_k = k, best_off = off;
    }
    return best;
}

RefreshDecision refresh_match_decide(int enabled, int fullscreen, const RefreshMode *desktop, const RefreshMode *modes, int n,
                                     int *pick, char *line, size_t cap) {
    const int hz = desktop->hz;
    *pick = -1;
    if (!enabled) {
        snprintf(line, cap, "[video] display refresh left at %d Hz (--no-refresh-match)", hz);
        return REFRESH_OFF;
    }
    if (!fullscreen) {
        snprintf(line, cap, "[video] display refresh left at %d Hz (windowed: --refresh-match only switches in fullscreen)", hz);
        return REFRESH_WINDOWED;
    }
    if (hz <= 0) {
        snprintf(line, cap, "[video] display refresh rate unknown: left as is");
        return REFRESH_UNKNOWN;
    }
    if (refresh_match_is_multiple(hz)) {
        snprintf(line, cap, "[video] display %d Hz is a multiple of 60: refresh left as is", hz);
        return REFRESH_ALREADY;
    }
    int i = refresh_match_pick(modes, n, desktop);
    if (i < 0) {
        snprintf(line, cap,
                 "[video] display %d Hz is not a multiple of 60, but it has no %dx%d mode at a multiple of 60: "
                 "refresh left at %d Hz (frames alternate between refresh counts)",
                 hz, desktop->w, desktop->h, hz);
        return REFRESH_NO_MODE;
    }
    *pick = i;
    snprintf(line, cap, "[video] display %d Hz is not a multiple of 60: switched to %dx%d @ %d Hz (--no-refresh-match keeps %d)", hz,
             modes[i].w, modes[i].h, modes[i].hz, hz);
    return REFRESH_SWITCH;
}

RefreshWatch refresh_match_watch(double elapsed_s, int frames, int minimized, int focus_lost) {
    if (minimized && !focus_lost) return REFRESH_WATCH_MINIMIZED;
    if (elapsed_s < REFRESH_MATCH_WATCH_S) return REFRESH_WATCH_WAIT;
    return frames > 0 ? REFRESH_WATCH_OK : REFRESH_WATCH_NO_FRAME;
}

const char *refresh_match_watch_reason(RefreshWatch w) {
    switch (w) {
    case REFRESH_WATCH_NO_FRAME: return "no frame presented within 3 s of the switch";
    case REFRESH_WATCH_MINIMIZED: return "the window was minimized without a focus change";
    case REFRESH_WATCH_OK: return "ok";
    default: return "watching";
    }
}
