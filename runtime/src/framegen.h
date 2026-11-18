/* Frame generation (--frame-gen, --frame-gen-of): the pure part.
 *
 * With --frame-gen the renderer builds one frame between every two real frames: the HUD-less frame
 * (the 3D scene after its post effects, before the HUD) is kept for two frames, every pixel of the new
 * one moves half way back along its motion (the camera's reprojection from depth, a skinned
 * character's own motion), and the colours of both frames along that motion are blended; the real
 * frame's HUD goes on top unchanged. The window then shows the generated frame and the real one a
 * refresh apart: 120 frames a second from the 60 Hz game, about 10 ms later than without it.
 *
 * This file holds the options, the rule that decides whether the generated frames are shown (a
 * display that refreshes at a multiple of 120 Hz, the game at 60 Hz) and the slow-source guard (off
 * while the game falls under 60 frames a second, back on once it holds 60 again), so they can be
 * tested without a GPU or a display. */
#ifndef FRAMEGEN_H
#define FRAMEGEN_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int on;   /* --frame-gen */
    int of;   /* --frame-gen-of: optical flow for what has no motion vectors (rain, flicker); implies on */
} FrameGenOptions;

/* The command line's --frame-gen / --frame-gen-of (main.c), read by hle_d3d.c at renderer start. */
extern FrameGenOptions framegen_options;

/* 1 when arg is --frame-gen, --frame-gen-of or --no-frame-gen (and sets *o: --frame-gen-of turns
 * both on, --no-frame-gen both off), 0 otherwise. */
int framegen_parse_arg(const char *arg, FrameGenOptions *o);

/* A display rate (whole hertz as SDL2 reports it; 119 for 119.88) at which 120 frames a second are
 * paced evenly: within 1 Hz of 120, 240, 360, ... (hz <= 0, unknown: 0). */
int framegen_rate_ok(int hz);

typedef enum {
    FRAMEGEN_SHOW,        /* generated frames are presented */
    FRAMEGEN_OFF,         /* --frame-gen not given */
    FRAMEGEN_NO_SIM60,    /* the game does not run its simulation at 60 Hz (--sim60) */
    FRAMEGEN_READBACK,    /* frames are read back (--dump-frame, --present readback): built, not presented */
    FRAMEGEN_RATE,        /* the display does not refresh at a multiple of 120 Hz */
    FRAMEGEN_UNAVAILABLE, /* the renderer cannot build them on this device */
} FrameGenDecision;

/* The decision for the session's current display: opts, the renderer's support, whether frames go to
 * a swapchain, --sim60, the display's rate (0: unknown). line receives a one-line "[frame-gen] ..."
 * report (no newline). FRAMEGEN_READBACK still has the renderer build generated frames (dumps). */
FrameGenDecision framegen_decide(const FrameGenOptions *opts, int supported, int presenting, int sim60, int display_hz, char *line,
                                 unsigned cap);

/* The anchor (the HUD-less frame) goes after the full-screen draws that end the scene: the title's
 * colour tint and frame-feedback blur quads cover the whole screen without depth after the 3D scene,
 * and as part of the HUD they would make every pixel the real frame's. 1 when n vertices (clip x, y,
 * z, w as the first four floats of each, stride bytes apart) all lie in front of the camera (w > 0)
 * and their NDC bounding box covers the screen to within FRAMEGEN_FULL_EDGE of every edge. */
#define FRAMEGEN_FULL_EDGE 0.02f
int framegen_full_screen(const void *v, unsigned stride, unsigned n);

/* Slow-source guard (AMD's advice: frame generation needs the source at 60 fps or more). Fed the
 * interval between consecutive real frames; generated frames stop when the smoothed interval stays
 * above FRAMEGEN_SLOW_MS for FRAMEGEN_HOLD_MS, and resume once it stays under FRAMEGEN_FAST_MS for
 * the resume hold. A gap over FRAMEGEN_GAP_MS (a load, a hitch, the window hidden) restarts the
 * smoothing and does not count. The resume hold doubles with every pause (FRAMEGEN_HOLD_MS, then 4,
 * 8, 16, at most FRAMEGEN_HOLD_MAX_MS): a scene that holds 60 fps only without the generated frames'
 * own GPU cost settles paused instead of flipping every few seconds; FRAMEGEN_CALM_MS shown in a row
 * forgets the pauses. */
#define FRAMEGEN_SLOW_MS 18.0
#define FRAMEGEN_FAST_MS 17.2
#define FRAMEGEN_HOLD_MS 2000.0
#define FRAMEGEN_HOLD_MAX_MS 32000.0
#define FRAMEGEN_CALM_MS 60000.0
#define FRAMEGEN_GAP_MS 250.0
typedef struct {
    double avg;    /* smoothed interval (0: none yet) */
    double held;   /* time the smoothed interval has been on the other side of the threshold */
    int slow;      /* 1: generated frames are off */
    int pauses;    /* pauses since the last FRAMEGEN_CALM_MS shown in a row */
    double shown;  /* time shown since the last resume (or the start) */
} FrameGenPace;
/* The hold before generated frames resume after the given number of pauses (ms). */
double framegen_resume_hold(int pauses);
/* Adds one interval (ms); returns 1 while generated frames should be shown, 0 while the source is slow.
 * *changed (may be NULL) is set to 1 on the frame the state flips. */
int framegen_pace(FrameGenPace *p, double interval_ms, int *changed);

/* Display clock (pacing by present completion, VK_KHR_present_wait). Two presents a frame into a
 * FIFO swapchain whose display takes exactly two a frame leave the queue no slack: once a frame that
 * comes early fills it, it stays full, and every later frame waits on the GPU for a swapchain image
 * the display has not released yet (the [hitch] lines' `wait`). With
 * present waits the renderer keeps at most one frame's pair queued: before it takes images for a new
 * frame it waits until the real frame before the last one is on screen, so the waits fall on the
 * CPU, a refresh at most, at the display's pace. Fed the times those waits return (ms) and whether
 * they waited at all (a wait that returns at once says nothing about the refresh), the clock
 * measures the time between real frames on screen: the display's period times two. */
#define FRAMEGEN_CLOCK_SPAN 0.05   /* accepted relative distance from 1/60 s (within 5 %) */
#define FRAMEGEN_CLOCK_LOCK 60     /* samples before the period is reported */
typedef struct {
    double last;     /* last waited return (0: none) */
    double period;   /* smoothed time between real frames on screen (ms; 0: none yet) */
    int n;           /* intervals taken */
} FrameGenClock;
void framegen_clock_add(FrameGenClock *c, double t_ms, int waited);
/* The measured period (ms between real frames on screen), 0 until FRAMEGEN_CLOCK_LOCK intervals. */
double framegen_clock_period(const FrameGenClock *c);

#ifdef __cplusplus
}
#endif

#endif
