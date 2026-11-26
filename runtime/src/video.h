/* Host video output behind a tiny interface so the backend can be swapped (SDL2 window today,
 * Vulkan later, "null" for tests and headless runs). Frames are 32-bit XRGB8888 pixels
 * (Xbox D3DFMT_LIN_X8R8G8B8 / A8R8G8B8 memory layout: B, G, R, X bytes). All video_* calls
 * except video_write_bmp must come from one host thread (the runtime's main thread). */
#ifndef VIDEO_H
#define VIDEO_H
#include <stdint.h>

/* Opens the output. pref: "sdl" (falls back to "null" when SDL2 cannot be loaded or
 * initialized) or "null". Returns the backend name actually opened. */
const char *video_open(const char *pref, int w, int h, const char *title);
void video_present(const uint32_t *px, int w, int h, int pitch_bytes);
int video_pump(void);       /* handles window events and feeds keyboard/controller state to hle_input;
                             * returns 0 once the user asked to quit */
void video_close(void);

/* Direct Vulkan present. Set before video_open: video_want_vulkan
 * makes the SDL window a Vulkan window without an SDL renderer (the host renderer presents to it);
 * video_fullscreen starts in desktop fullscreen (F11 / Alt+Enter toggle it in the SDL window). */
extern int video_want_vulkan, video_fullscreen;
/* Escape pressed (SDL and X11 backends): a mod menu or the settings menu takes it first (src/hle_modmenu.h
 * hle_modmenu_escape, src/hle_settings.h: 1 = consumed, the mouse capture and quit are left alone). NULL = none. */
extern int (*video_escape_hook)(void);
/* --refresh-match (1, default) / --no-refresh-match (0): a window that starts in fullscreen on a
 * display whose rate is not a multiple of 60 switches the display to the desktop size at the
 * highest multiple of 60 it offers, in exclusive fullscreen (refresh_match.h); set before video_open. */
extern int video_refresh_match;
/* Called after a frame reached the window through the renderer's own swapchain (video_present
 * counts its frames itself): the deferred --refresh-match switch and its watchdog wait for shown
 * frames. Frames of a minimized window do not count. */
void video_frame_presented(void);
/* The refresh rate (whole hertz, as SDL2 reports it) of the display the window is on now, read again
 * at most every half second; 0 without a window or when it is unknown. --frame-gen shows its frames
 * only at a multiple of 120 Hz (framegen.h). */
int video_display_hz(void);
/* --pad-mappings FILE: SDL controller mappings read instead of gamecontrollerdb.txt in the
 * program's and the current folder (set before video_open). */
extern const char *video_pad_mappings;
/* Instance extensions the window's surface needs (NULL when the window is not a Vulkan window),
 * the surface for a VkInstance (0 on success; *surface is a VkSurfaceKHR), and the drawable size. */
const char *const *video_vulkan_extensions(uint32_t *n);
int video_vulkan_surface(void *instance, uint64_t *surface);
void video_vulkan_drawable(int *w, int *h);
/* Replaces a Vulkan window by a regular one presented through video_present (after the renderer
 * released its surface). Returns -1 when no window could be made. */
int video_vulkan_fallback(void);

/* Largest rectangle of aspect sw:sh centered in dw x dh: r = {x, y, w, h} (pillarbox or
 * letterbox borders around it). */
void video_fit(int sw, int sh, int dw, int dh, int r[4]);

/* An input source that replaces the window backends (set before video_open; NULL: none): the render64
 * helper's window and input (--render-process helper-io, src/r64_input.h). video_open tries it first
 * (open: 0 on success, else the usual backends), video_pump calls its pump, video_close its close,
 * video_display_hz its display_hz; frames given to video_present are dropped. */
typedef struct {
    const char *name;
    int (*open)(int w, int h, const char *title);
    int (*pump)(void);
    void (*close)(void);
    int (*display_hz)(void);
    /* 1: the source's process renders and presents (--render-process helper): with
     * video_want_vulkan, video_vulkan_extensions answers an empty list, so the renderer's configuration says the game
     * presents (into the helper's window) */
    int remote_present;
} VideoInputSource;
extern const VideoInputSource *video_input_source;

/* Writes a 24-bit uncompressed BMP; returns 0 on success. */
int video_write_bmp(const char *path, const uint32_t *px, int w, int h, int pitch_bytes);
#endif
