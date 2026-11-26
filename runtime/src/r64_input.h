/* The game side of the render64 helper's window and input.
 * With `--render-process helper-io` (a test mode before W6; Windows since W7) the game starts the x86_64 helper
 * (tools/render64/r64_helper.c) with a window config; the helper owns the SDL window, its event pump
 * and the controllers and writes what video.c's SDL backend would have passed to hle_input into the
 * input region's event ring (src/render_ipc.h r64_in_*). video.c's "helper" backend drains that ring
 * at the point its own SDL pump runs (video_pump) into the same input_key_event, input_mouse_*,
 * input_release_all and input_set_port_controller calls; rumble and the mouse-capture release go back
 * on the command ring. The game keeps rendering in-process (offscreen: it opens no window of its
 * own). A helper that dies ends the game with a FAULT line (video_pump returns 0). */
#ifndef R64_INPUT_H
#define R64_INPUT_H
#include "render_ipc.h"
#include <stddef.h>
#include <stdint.h>

extern int r64_input_helper_io;           /* --render-process helper-io */
extern const char *r64_input_helper_path; /* --render64-helper PATH (default: next to the program, then ../build64/; Windows: x64\ next to it) */
/* The helper to start: given, or the default above (the result is in buf or is given). */
const char *r64_input_find_helper(const char *given, char *buf, size_t n);
extern uint32_t r64_input_pad_poll_us;   /* --render64-pad-poll-us N: the helper reads open controllers every N us (0: 1000) */
extern const char *r64_input_inject;      /* --render64-inject N[@MS] (tests): the helper injects N key events (after MS), a pad and a close */
/* main.c: 1 when argv[*i] was one of the options above (consumed), -1 on an error (message printed), 0 otherwise. */
int r64_input_option(int argc, char **argv, int *i);

typedef struct {
    const char *helper_path;   /* NULL: the default lookup */
    int w, h, fullscreen, refresh_match;
    const char *title;
    const char *inject;        /* NULL, or the helper's --test-inject value */
    uint32_t poll_us;          /* 0: the helper's default */
} R64InputOpen;
/* Starts the helper and waits (<= 5 s) for its window. 0, or -1 with one line in err. */
int r64_input_open(const R64InputOpen *o, char *err, size_t n);
int r64_input_active(void);
/* Drains the event ring into hle_input (main thread). 0 once the window was closed, Escape asked to
 * quit, or the helper died (after its FAULT line). */
int r64_input_pump(void);
/* Asks the helper to quit, waits <= 1 s, prints the input statistics. */
void r64_input_close(void);
int r64_input_display_hz(void);   /* the helper window's display refresh, 0 unknown */
long r64_input_helper_pid(void);
extern int r64_input_last_exit;   /* the helper's exit code at the last r64_input_close (-1: it had died) */

typedef struct {
    uint32_t events, keys, pads, resyncs, dropped, rumbles;
    uint32_t n_wait, n_origin;    /* samples below */
    double wait_p50, wait_p95, wait_max;       /* ms: written by the helper -> read by the game */
    double origin_p50, origin_p95, origin_max; /* ms: the input happened (when known) -> read by the game */
} R64InputStats;
void r64_input_stats(R64InputStats *s);
/* Tests: sees every record before it is applied, with the game's read time. */
extern void (*r64_input_test_hook)(const R64InEvent *e, uint64_t now_ns);
#endif
