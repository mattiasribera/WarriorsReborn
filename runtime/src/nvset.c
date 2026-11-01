#include "nvset.h"
#include <string.h>

/* XC_LANGUAGE values (Xbox kernel) for the string tables on the disc; the first is the default. */
static const struct { const char *code, *name; uint32_t xc; } languages[] = {
    {"en", "English", 1}, {"es", "Spanish", 5}, {"fr", "French", 4}, {"de", "German", 3}, {"it", "Italian", 6}};

static uint32_t video_flags;
static unsigned language;   /* index into languages[] */

uint32_t nvset_value(uint32_t index) {
    switch (index) {
    case NV_LANGUAGE: return languages[language].xc;
    case NV_VIDEO_FLAGS: return video_flags;
    case NV_FACTORY_AV_REGION: return 0x00400100u;  /* NTSC-M */
    case NV_FACTORY_GAME_REGION: return 1;          /* North America */
    default: return 0;
    }
}

void nvset_widescreen(int on) { video_flags = on ? NV_VIDEO_WIDESCREEN : 0; }

int nvset_language(const char *code) {
    for (unsigned i = 0; i < sizeof languages / sizeof languages[0]; i++)
        if (!strcmp(code, languages[i].code)) { language = i; return 1; }
    return 0;
}

const char *nvset_language_name(void) { return languages[language].name; }
