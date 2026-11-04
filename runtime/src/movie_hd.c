/* --movie-hd file-name rule and Bink header check (see movie_hd.h). */
#include "movie_hd.h"
#include <ctype.h>
#include <string.h>

int movie_hd_on = 1;

static int ends_with_ci(const char *s, size_t n, const char *suffix) {
    size_t k = strlen(suffix);
    if (n < k) return 0;
    for (size_t i = 0; i < k; i++)
        if (tolower((unsigned char)s[n - k + i]) != tolower((unsigned char)suffix[i])) return 0;
    return 1;
}

int movie_hd_name(const char *xp, char *out, size_t len) {
    const char *name = strrchr(xp, '\\');
    if (!name) return 0;
    size_t dir = (size_t)(name - xp);   /* the folder must be "bik" or "bik_pal" */
    const char *folder = xp + dir;
    while (folder > xp && folder[-1] != '\\') folder--;
    size_t fl = (size_t)(xp + dir - folder);
    if (!((fl == 3 && ends_with_ci(folder, fl, "bik")) || (fl == 7 && ends_with_ci(folder, fl, "bik_pal")))) return 0;
    name++;
    size_t n = strlen(name);
    if (!ends_with_ci(name, n, ".bik")) return 0;
    n -= 4;
    if (ends_with_ci(name, n, "_hd")) return 0;
    static const char *const suffixes[] = {"_w_pal", "_w", "_pal"};
    for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++)
        if (ends_with_ci(name, n, suffixes[i])) {
            n -= strlen(suffixes[i]);
            break;
        }
    if (!n) return 0;
    size_t need = (size_t)(name - xp) + n + strlen("_hd.bik") + 1;
    if (need > len) return 0;
    memcpy(out, xp, (size_t)(name - xp) + n);
    strcpy(out + (name - xp) + n, "_hd.bik");
    return 1;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int movie_hd_header_ok(const uint8_t *h, size_t len) {
    if (len < MOVIE_HD_HEADER_BYTES || memcmp(h, "BIK", 3)) return 0;
    uint32_t w = le32(h + 20), hh = le32(h + 24);
    return w > 640 && w <= 4096 && (uint64_t)w * 9 == (uint64_t)hh * 16;
}
