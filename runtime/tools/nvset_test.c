/* Game-independent test of the non-volatile settings the kernel reports to the title
 * (src/nvset.h): language and regions, and the video flags that select 16:9 output. */
#include "../src/nvset.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

int main(void) {
    CHECK(nvset_value(NV_LANGUAGE) == 1, "XC_LANGUAGE: English (%u)", nvset_value(NV_LANGUAGE));
    CHECK(nvset_value(NV_FACTORY_AV_REGION) == 0x00400100u, "XC_FACTORY_AV_REGION: NTSC-M (%08x)", nvset_value(NV_FACTORY_AV_REGION));
    CHECK(nvset_value(NV_FACTORY_GAME_REGION) == 1, "XC_FACTORY_GAME_REGION: North America");
    CHECK(nvset_value(NV_VIDEO_FLAGS) == 0, "XC_VIDEO_FLAGS: 4:3 by default (%08x)", nvset_value(NV_VIDEO_FLAGS));
    CHECK(nvset_value(0x55) == 0, "other settings: 0");
    nvset_widescreen(1);
    uint32_t v = nvset_value(NV_VIDEO_FLAGS);
    /* XGetVideoFlags (0x3400af in this XBE): (value >> 16) & 0x5f, bit 0 = XC_VIDEO_FLAGS_WIDESCREEN. */
    CHECK(v == 0x00010000u && ((v >> 16) & 0x5f) == 1, "widescreen: XGetVideoFlags reads 0x01 (%08x)", v);
    nvset_widescreen(0);
    CHECK(nvset_value(NV_VIDEO_FLAGS) == 0, "widescreen off again");
    /* --language: the code maps to XC_LANGUAGE (1 English, 3 German, 4 French, 5 Spanish, 6 Italian). */
    CHECK(!strcmp(nvset_language_name(), "English"), "language: English by default (%s)", nvset_language_name());
    static const struct { const char *code, *name; uint32_t xc; } langs[] = {
        {"en", "English", 1}, {"es", "Spanish", 5}, {"fr", "French", 4}, {"de", "German", 3}, {"it", "Italian", 6}};
    for (unsigned i = 0; i < sizeof langs / sizeof langs[0]; i++) {
        int ok = nvset_language(langs[i].code);
        CHECK(ok && nvset_value(NV_LANGUAGE) == langs[i].xc && !strcmp(nvset_language_name(), langs[i].name),
              "--language %s: XC_LANGUAGE %u (%s)", langs[i].code, nvset_value(NV_LANGUAGE), nvset_language_name());
    }
    nvset_language("es");
    CHECK(!nvset_language("jp") && nvset_value(NV_LANGUAGE) == 5, "--language jp: rejected, setting unchanged");
    CHECK(!nvset_language("") && !nvset_language("ES") && !nvset_language("esp"), "--language: empty/upper/long rejected");
    CHECK(!strcmp(NV_LANGUAGE_CODES, "en, es, fr, de, it"), "valid codes listed: %s", NV_LANGUAGE_CODES);
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
