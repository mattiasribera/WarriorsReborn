/* --movie-hd: play the disc's 1280x720 Bink movies (bik/<name>_hd.bik).
 *
 * The title picks the movie file by its video flags (0x24a243: [[0x4cfe90] + 4] bit 0x8 -> "%s_hd",
 * bit 0x4 -> "%s_w", bit 0x2 -> "%s_pal"), and the HD bit is set only on an HDTV AV pack in 720p
 * mode, which the runtime never reports. Instead of faking an HDTV (it would also change the
 * title's device and HUD setup), kfile.c redirects the open of a movie to its _hd file when that
 * file exists and its Bink header says 16:9 HD. The movie quad of an HD file is then drawn by the
 * renderer at output resolution (hle_d3d.c draw_vertices_up). */
#ifndef MOVIE_HD_H
#define MOVIE_HD_H
#include <stddef.h>
#include <stdint.h>

/* On by default; --no-movie-hd clears it (the requested file, CPU movie quad as before). */
extern int movie_hd_on;

/* The _hd variant of a movie path: "<dir>\bik\<name>[_w|_pal|_w_pal].bik" (also bik_pal,
 * case-insensitive) -> "<dir>\bik\<name>_hd.bik" in out. Returns 1 when xbox_path is such a movie
 * (and not already an _hd file) and out had room, else 0. */
int movie_hd_name(const char *xbox_path, char *out, size_t len);

/* Bink header bytes (at least 28: "BIK" signature, width at 20, height at 24). Returns 1 when
 * the file is a Bink movie with a 16:9 picture wider than 640 pixels (1280x720 on this disc;
 * trailer_hd.bik is 640x480 and keeps the requested file). */
#define MOVIE_HD_HEADER_BYTES 28
int movie_hd_header_ok(const uint8_t *hdr, size_t len);
#endif
