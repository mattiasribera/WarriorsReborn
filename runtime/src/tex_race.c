/* Texture replay race detector (see tex_race.h). */
#include "tex_race.h"
#include "xtex.h"

int tex_race_guard = 1;

uint64_t tex_race_key(uint32_t data, uint32_t format, uint32_t size, const uint8_t *bytes, uint32_t len) {
    return xtex_key(data, format, size, bytes, len) | 1;
}

int tex_race_changed(uint64_t rec_key, uint32_t data, uint32_t format, uint32_t size, const uint8_t *bytes) {
    XTexInfo t;
    if (xtex_info(format, size, &t)) return -1;
    return tex_race_key(data, format, size, bytes, xtex_total_bytes(&t)) != rec_key;
}

int tex_race_note(TexRaceStats *s, int changed, uint32_t len, uint32_t frame) {
    s->checks++;
    s->check_bytes += len;
    if (!changed) return 0;
    s->races++;
    const uint32_t window = frame / TEX_RACE_LOG_WINDOW + 1;
    if (window != s->window) {
        s->window = window;
        s->logged = 0;
    }
    if (s->logged < TEX_RACE_LOG_BURST) {
        s->logged++;
        return 1;
    }
    s->suppressed++;
    return 0;
}

int tex_race_repeat(TexRaceRecent *r, uint32_t addr, uint32_t frame) {
    const uint32_t i = ((addr >> 4) ^ (addr >> 12)) % TEX_RACE_RECENT;
    if (r->addr[i] == addr && frame >= r->first[i] && frame - r->last[i] <= TEX_RACE_REPEAT_FRAMES) {
        r->last[i] = frame;
        return frame > r->first[i];
    }
    r->addr[i] = addr;
    r->first[i] = r->last[i] = frame;
    return 0;
}
