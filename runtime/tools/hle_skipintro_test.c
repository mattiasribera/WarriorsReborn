/* Game-independent --skip-intro test: a host buffer stands in for the title's main (0x68b40).
 * Checks the options, that the three boot movie calls become 5-byte NOPs with the bytes around
 * them untouched, that the calls are the shipped PlayMovie calls, that one changed byte at any
 * site leaves all three untouched, and that a disabled skip writes nothing. */
#include "../src/hle_skipintro.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

enum { BASE = 0x00068b40u, SIZE = 0x200 };

/* push 0 / push "logo" / call; push 1 / push "plogo" / call; push 1 / push "l1_in" / call;
 * then mov ecx,[0x5a719c] ... add esp,0x18 (as shipped, 0x68be7-0x68c1c). */
static const uint8_t shipped[] = {
    0x6a, 0x00, 0x68, 0xe0, 0xd9, 0x41, 0x00, 0xe8, 0x4d, 0x14, 0x1e, 0x00,
    0x6a, 0x01, 0x68, 0xe8, 0xd9, 0x41, 0x00, 0xe8, 0x41, 0x14, 0x1e, 0x00,
    0x6a, 0x01, 0x68, 0xf0, 0xd9, 0x41, 0x00, 0xe8, 0x35, 0x14, 0x1e, 0x00,
    0x8b, 0x0d, 0x9c, 0x71, 0x5a, 0x00, 0x8b, 0x15, 0xb4, 0x71, 0x5a, 0x00, 0x8b, 0x01, 0x83, 0xc4, 0x18};
#define SHIPPED_VA 0x00068be7u

static uint8_t mem[SIZE];

static void plant(void) {
    memset(mem, 0xcc, sizeof mem);
    memcpy(mem + (SHIPPED_VA - BASE), shipped, sizeof shipped);
}

int main(void) {
    static const uint8_t nop5[5] = {0x0f, 0x1f, 0x44, 0x00, 0x00};
    intptr_t bias = (intptr_t)((uintptr_t)mem - BASE);
    uint8_t before[SIZE];
    char report[256];

    int on = 7;
    CHECK(skipintro_parse_arg("--skip-intro", &on) == 1 && on == 1, "--skip-intro enables");
    CHECK(skipintro_parse_arg("--no-skip-intro", &on) == 1 && on == 0, "--no-skip-intro disables");
    on = 5;
    CHECK(skipintro_parse_arg("--skip-intros", &on) == 0 && on == 5, "other options are not taken");
    CHECK(skipintro_parse_arg("--skip", &on) == 0 && on == 5, "--skip is not --skip-intro");

    /* The table is the three PlayMovie (0x24a040) calls of main, in order. */
    static const char *names[SKIPINTRO_SITES] = {"logo", "plogo", "l1_in"};
    for (int i = 0; i < SKIPINTRO_SITES; i++) {
        const SkipIntroSite *s = &skipintro_sites[i];
        int32_t rel;
        memcpy(&rel, s->call + 1, 4);
        CHECK(s->call[0] == 0xe8 && s->va + 5u + (uint32_t)rel == 0x0024a040u && !strcmp(s->movie, names[i]),
              "site %d: %s call at %08x targets PlayMovie", i, s->movie, s->va);
    }

    /* Disabled: nothing written, no count. */
    plant();
    memcpy(before, mem, sizeof mem);
    CHECK(skipintro_apply(0, bias, report, sizeof report) == 0 && !memcmp(before, mem, sizeof mem),
          "disabled: memory untouched");

    /* Enabled: the three calls become NOPs, every other byte is unchanged. */
    plant();
    memcpy(before, mem, sizeof mem);
    int n = skipintro_apply(1, bias, report, sizeof report);
    CHECK(n == SKIPINTRO_SITES, "enabled: %d calls removed (%s)", n, report);
    int changed = 0, nops = 1;
    for (int i = 0; i < SIZE; i++) changed += mem[i] != before[i];
    for (int i = 0; i < SKIPINTRO_SITES; i++)
        nops &= !memcmp(mem + (skipintro_sites[i].va - BASE), nop5, 5);
    CHECK(nops && changed <= 15, "calls are 5-byte NOPs, %d bytes changed, nothing else", changed);
    CHECK(mem[SHIPPED_VA - BASE] == 0x6a && mem[sizeof shipped - 3 + (SHIPPED_VA - BASE)] == 0x83,
          "pushes and the caller's add esp,0x18 kept");
    CHECK(strstr(report, "[skip-intro]") && strstr(report, "logo") && strstr(report, "l1_in"), "report names the movies");

    /* Applying twice finds NOPs, not the shipped calls: refused, memory unchanged. */
    memcpy(before, mem, sizeof mem);
    CHECK(skipintro_apply(1, bias, report, sizeof report) == 0 && !memcmp(before, mem, sizeof mem),
          "second pass refused, untouched (%s)", report);

    /* One changed byte at any site: none of the three is patched. */
    for (int s = 0; s < SKIPINTRO_SITES; s++) {
        plant();
        mem[skipintro_sites[s].va - BASE + 2] ^= 0xff;
        memcpy(before, mem, sizeof mem);
        int r = skipintro_apply(1, bias, report, sizeof report);
        CHECK(r == 0 && !memcmp(before, mem, sizeof mem) && strstr(report, "not patched"),
              "site %d differs: all untouched (%s)", s, report);
    }

    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
