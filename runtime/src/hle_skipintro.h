/* Skip the boot movies (--skip-intro; off by default, the launcher turns it on).
 *
 * The title's main (0x68b40, called once from the start-up code at 0x34787f) starts loading
 * "level1" (the title-screen backdrop), then plays three Bink movies through the blocking movie
 * player PlayMovie (0x24a040, Movie\PlayMovie.cpp: open D:\bik\<name>.bik, play until the end or
 * a button, close):
 *   0x68bee  call PlayMovie("logo", 0)    Rockstar Toronto / publisher logos
 *   0x68bfa  call PlayMovie("plogo", 1)   "Rockstar Games presents"
 *   0x68c06  call PlayMovie("l1_in", 1)   the opening movie (the attract loop plays it too)
 * and then enters the front end (0x32ac60), which shows the title screen (PRESS START). The three
 * movies take about 3200 frames (about 54 s) before the title screen shows.
 *
 * The skip replaces those three call instructions (5 bytes each, e8 rel32) with a 5-byte NOP.
 * The arguments are still pushed and the caller's own add esp,0x18 at 0x68c19 pops them, so the
 * stack stays balanced. Nothing else changes: PlayMovie itself is untouched, so every other movie
 * plays as shipped: the level intros and outros ("l%d_in" / "l%d_out", from 0x7c276 and 0x75a60;
 * story mission 1 opens l99_in) and the title screen's attract loop, which after about a minute
 * idle plays "Rockstar Games presents" and l1_in again through the script binding at 0x27e84c.
 * The skip is keyed to the call sites, not to movie names, for that reason.
 *
 * All three sites are checked before anything is written; if any differs (another build of the
 * title), nothing is patched and the line says so. */
#ifndef HLE_SKIPINTRO_H
#define HLE_SKIPINTRO_H
#include <stddef.h>
#include <stdint.h>

#define SKIPINTRO_SITES 3

typedef struct {
    const char *movie;               /* the movie name the call plays */
    uint32_t va;                     /* the call instruction */
    uint8_t call[5];                 /* its bytes as shipped (e8 rel32 to PlayMovie 0x24a040) */
} SkipIntroSite;

extern const SkipIntroSite skipintro_sites[SKIPINTRO_SITES];

/* 1 when arg is --skip-intro or --no-skip-intro (sets *enabled), 0 otherwise. */
int skipintro_parse_arg(const char *arg, int *enabled);

/* With enabled, NOPs the boot movie calls in memory where guest address va is at host address
 * va + bias (bias 0: the identity-mapped guest), all or none. Writes a "[skip-intro] ..." line
 * (no newline) into report. Returns the number of calls removed (SKIPINTRO_SITES or 0). */
int skipintro_apply(int enabled, intptr_t bias, char *report, size_t cap);

/* skipintro_apply on the loaded title; prints the line when enabled. */
int hle_skipintro_install(int enabled);
#endif
