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

/* Writes a 24-bit uncompressed BMP; returns 0 on success. */
int video_write_bmp(const char *path, const uint32_t *px, int w, int h, int pitch_bytes);
#endif
