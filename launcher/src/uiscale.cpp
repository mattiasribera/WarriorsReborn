#include "uiscale.h"
#include <cmath>

namespace launcher {

float ui_scale_from_setting(const std::string &v) {
    if (v == "1") return 1;
    if (v == "1.25") return 1.25f;
    if (v == "1.5") return 1.5f;
    return 0;
}

const char *ui_scale_setting(float scale) {
    if (scale == 1) return "1";
    if (scale == 1.25f) return "1.25";
    if (scale == 1.5f) return "1.5";
    return "auto";
}

float auto_ui_scale(float dpi, int usable_w, int usable_h) {
    float s = std::floor((dpi > 0 ? dpi : 96) / 96 * 4 + 0.01f) / 4; /* 0.25 steps, rounded down */
    s = s < 1 ? 1 : s > 2 ? 2 : s;
    while (s > 1 && (UI_COMFORT_W * s > usable_w || UI_COMFORT_H * s > usable_h)) s -= 0.25f;
    return s;
}

static int fit(float v, int limit) { return limit > 0 && v > limit ? limit : (int)(v + 0.5f); }

void ui_window_size(float scale, int usable_w, int usable_h, int &w, int &h, int &min_w, int &min_h) {
    w = fit(UI_DEFAULT_W * scale, usable_w);
    h = fit(UI_DEFAULT_H * scale, usable_h);
    min_w = fit(UI_MIN_W * scale, w);
    min_h = fit(UI_MIN_H * scale, h);
}

} // namespace launcher
