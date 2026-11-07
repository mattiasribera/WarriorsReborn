/* Skip the boot movies (see hle_skipintro.h). */
#include "hle_skipintro.h"
#include <stdio.h>
#include <string.h>

/* main (0x68b40): call PlayMovie (0x24a040) for each boot movie; e8 rel32 as shipped. */
const SkipIntroSite skipintro_sites[SKIPINTRO_SITES] = {
    {"logo", 0x00068beeu, {0xe8, 0x4d, 0x14, 0x1e, 0x00}},
    {"plogo", 0x00068bfau, {0xe8, 0x41, 0x14, 0x1e, 0x00}},
    {"l1_in", 0x00068c06u, {0xe8, 0x35, 0x14, 0x1e, 0x00}},
};

static const uint8_t nop5[5] = {0x0f, 0x1f, 0x44, 0x00, 0x00};   /* nop dword [eax+eax*1+0] */

int skipintro_parse_arg(const char *arg, int *enabled) {
    if (!strcmp(arg, "--skip-intro")) { *enabled = 1; return 1; }
    if (!strcmp(arg, "--no-skip-intro")) { *enabled = 0; return 1; }
    return 0;
}

int skipintro_apply(int enabled, intptr_t bias, char *report, size_t cap) {
    if (!cap) return 0;
    report[0] = 0;
    if (!enabled) { snprintf(report, cap, "[skip-intro] off: the boot movies play"); return 0; }
    for (int i = 0; i < SKIPINTRO_SITES; i++) {
        const SkipIntroSite *s = &skipintro_sites[i];
        if (memcmp((const void *)(uintptr_t)(s->va + bias), s->call, sizeof s->call)) {
            snprintf(report, cap, "[skip-intro] not patched: the %s call at %08x is not the shipped one; the boot movies play",
                     s->movie, s->va);
            return 0;
        }
    }
    for (int i = 0; i < SKIPINTRO_SITES; i++)
        memcpy((void *)(uintptr_t)(skipintro_sites[i].va + bias), nop5, sizeof nop5);
    snprintf(report, cap, "[skip-intro] boot movies skipped: %s, %s, %s (calls at %08x, %08x, %08x)",
             skipintro_sites[0].movie, skipintro_sites[1].movie, skipintro_sites[2].movie,
             skipintro_sites[0].va, skipintro_sites[1].va, skipintro_sites[2].va);
    return SKIPINTRO_SITES;
}

int hle_skipintro_install(int enabled) {
    char report[256];
    int n = skipintro_apply(enabled, 0, report, sizeof report);
    if (enabled) printf("%s\n", report);
    return n;
}
