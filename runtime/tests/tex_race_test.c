/* Game-independent test of the texture replay race detector (src/tex_race.c): the replay-time
 * recompute of the record-time texture key, whether a change of the guest bytes between record
 * and replay is seen (inside and outside the sparse checksum), unparsable headers, and the
 * [tex-race] log rate limit. */
#include "../src/tex_race.h"
#include "../src/xtex.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

/* 64x64 swizzled A8R8G8B8 (code 0x06), one level: Format = dim 2, code, 1 level, log2 64 x 64. */
#define FMT (0x6u << 20 | 0x6u << 24 | 1u << 16 | 0x06u << 8 | 2u << 4)
#define DATA 0x00200000u

int main(void) {
    static uint8_t tex[64 * 64 * 4];
    XTexInfo t;
    CHECK(!xtex_info(FMT, 0, &t) && t.w == 64 && t.h == 64 && xtex_total_bytes(&t) == sizeof tex, "test texture parses (64x64 A8R8G8B8)");
    for (size_t i = 0; i < sizeof tex; i++) tex[i] = (uint8_t)(i * 7 + 3);

    const uint64_t rec = tex_race_key(DATA, FMT, 0, tex, sizeof tex);
    CHECK(rec == (xtex_key(DATA, FMT, 0, tex, sizeof tex) | 1), "record key is xtex_key | 1 (%016llx)", (unsigned long long)rec);
    CHECK(rec != 0, "record key is never 0");
    CHECK(tex_race_changed(rec, DATA, FMT, 0, tex) == 0, "same bytes at replay: unchanged");

    /* Streaming reuses the memory: the first bytes, the last bytes, or a sampled dword change. */
    tex[0] ^= 0xFF;
    CHECK(tex_race_changed(rec, DATA, FMT, 0, tex) == 1, "first byte rewritten: changed");
    tex[0] ^= 0xFF;
    tex[sizeof tex - 1] ^= 0x01;
    CHECK(tex_race_changed(rec, DATA, FMT, 0, tex) == 1, "last byte rewritten: changed");
    tex[sizeof tex - 1] ^= 0x01;
    const size_t sample = (size_t)(sizeof tex / 4 / 64) * 4 * 10;   /* the 11th sampled dword */
    tex[sample + 2] ^= 0x10;
    CHECK(tex_race_changed(rec, DATA, FMT, 0, tex) == 1, "sampled dword rewritten: changed");
    tex[sample + 2] ^= 0x10;
    CHECK(tex_race_changed(rec, DATA, FMT, 0, tex) == 0, "restored bytes: unchanged again");
    /* The sparse checksum's limit: a byte it does not sample is not seen (as at record time). */
    tex[sample + 5] ^= 0x10;
    CHECK(tex_race_changed(rec, DATA, FMT, 0, tex) == 0, "unsampled byte rewritten: not detectable (sparse key)");
    tex[sample + 5] ^= 0x10;
    CHECK(tex_race_changed(rec, DATA + 0x1000, FMT, 0, tex) == 1, "other Data address: changed");
    CHECK(tex_race_changed(rec, DATA, 0x00000300u, 0, tex) == -1, "unparsable Format (cube/volume/P8): -1");

    /* Rate limit: TEX_RACE_LOG_BURST lines per TEX_RACE_LOG_WINDOW frames, every check counted. */
    TexRaceStats s;
    memset(&s, 0, sizeof s);
    int logged = 0;
    for (int i = 0; i < 3 * TEX_RACE_LOG_BURST; i++) logged += tex_race_note(&s, 1, 100, 10);
    CHECK(logged == TEX_RACE_LOG_BURST, "a burst in one window logs %d lines (%d)", TEX_RACE_LOG_BURST, logged);
    CHECK(s.races == 3 * TEX_RACE_LOG_BURST && s.suppressed == 2 * TEX_RACE_LOG_BURST, "all counted (%llu), %u suppressed",
          (unsigned long long)s.races, s.suppressed);
    CHECK(tex_race_note(&s, 0, 100, 10) == 0 && s.checks == 3 * TEX_RACE_LOG_BURST + 1 && s.races == 3 * TEX_RACE_LOG_BURST,
          "an unchanged check is counted, not logged (%llu checks)", (unsigned long long)s.checks);
    CHECK(s.check_bytes == 100ull * (3 * TEX_RACE_LOG_BURST + 1), "checked texture bytes summed (%llu)", (unsigned long long)s.check_bytes);
    CHECK(tex_race_note(&s, 1, 100, 10 + TEX_RACE_LOG_WINDOW) == 1, "the next window logs again");
    CHECK(tex_race_note(&s, 1, 100, 5) == 1, "a frame number going back (new run/level) starts a new window");
    CHECK(tex_race_guard == 1, "the guard is on by default");

    /* Skip once per address, then draw: a texture rewritten in place every frame stays visible. */
    TexRaceRecent r;
    memset(&r, 0, sizeof r);
    CHECK(tex_race_repeat(&r, DATA, 100) == 0, "first mismatch of an address: skipped");
    CHECK(tex_race_repeat(&r, DATA, 100) == 0, "another draw of it in the same frame: skipped too");
    CHECK(tex_race_repeat(&r, DATA, 101) == 1, "the same address again on the next frame: drawn with the current bytes");
    CHECK(tex_race_repeat(&r, DATA, 101) == 1, "another draw of it in that frame: drawn too");
    int drawn = 0;
    for (uint32_t f = 102; f < 160; f++) drawn += tex_race_repeat(&r, DATA, f);
    CHECK(drawn == 58, "rewritten every frame: drawn on every later frame (%d of 58)", drawn);
    CHECK(tex_race_repeat(&r, DATA + 0x10000, 160) == 0, "another address: its own first skip");
    CHECK(tex_race_repeat(&r, DATA, 159 + TEX_RACE_REPEAT_FRAMES + 1) == 0, "a mismatch after %d quiet frames: a new first skip",
          TEX_RACE_REPEAT_FRAMES);
    CHECK(tex_race_repeat(&r, DATA, 159 + TEX_RACE_REPEAT_FRAMES + 3) == 1, "then drawn again within the window");
    CHECK(tex_race_repeat(&r, DATA, 50) == 0, "a frame number going back (new run): a new first skip");

    printf(failures ? "FAILED: %d\n" : "ALL OK\n", failures);
    return failures != 0;
}
