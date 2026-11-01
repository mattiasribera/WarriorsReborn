/* The launcher's interface size (Options, "Interface size"; ui_scale in [launcher]): Auto, 1x,
 * 1.25x or 1.5x. The window is laid out in logical pixels (the 1x layout) and drawn at the scale,
 * with the fonts rasterized at it, so everything grows together and stays sharp. Auto follows the
 * display's DPI (96 = 1x) in steps of 0.25 up to 2x, lowered until the comfortable window
 * (UI_COMFORT_W x UI_COMFORT_H logical pixels) fits the usable screen area. */
#ifndef LAUNCHER_UISCALE_H
#define LAUNCHER_UISCALE_H
#include <string>

namespace launcher {

/* The window's default and comfortable size, and the smallest logical size the layout is made for
 * (a 1366x768 screen at 1.5x leaves about 910 x 465). */
constexpr int UI_DEFAULT_W = 1120, UI_DEFAULT_H = 760;
constexpr int UI_COMFORT_W = 1000, UI_COMFORT_H = 680;
constexpr int UI_MIN_W = 880, UI_MIN_H = 440;

/* "auto" (or empty, or an unknown value) = 0; "1", "1.25", "1.5" = that scale. */
float ui_scale_from_setting(const std::string &value);
const char *ui_scale_setting(float scale); /* "auto" for 0, else "1" / "1.25" / "1.5" */
/* Auto: dpi <= 0 (unknown) counts as 96. */
float auto_ui_scale(float dpi, int usable_w, int usable_h);
/* The window size (default, minimum) in screen pixels at a scale, kept inside the usable area. */
void ui_window_size(float scale, int usable_w, int usable_h, int &w, int &h, int &min_w, int &min_h);

} // namespace launcher
#endif
