/* `--render64-selftest`: a check of the render64 helper on this machine in a few seconds,
 * without the game's data. The game program (main.c, before anything else) starts the x64 / x86_64 helper and:
 *   1. reference: a small synthetic scene (a base image, two textures, blended quads) rendered for a few frames by
 *      this process's own renderer, read back;
 *   2. echo: 200 frames of 2 MB through the shared command ring, each checksummed by the helper (the raw channel:
 *      handshake, events, throughput, round trip);
 *   3. wire: the same frames through `--render-process helper` (SDL's dummy video driver: the helper opens no window
 *      and reads its frames back): the pixels that come back must be the reference's bytes (same hash), the texture
 *      handles stable, and 120 presented frames must not wait for the helper;
 *   4. the helper exits with code 0 when asked to quit.
 * Every line starts with "[render64] selftest:"; the last one is "... PASS (...)" or "... FAIL: <the first failed
 * check>". The helper's own lines ([r64]) are in the same log. */
#ifndef R64_SELFTEST_H
#define R64_SELFTEST_H

/* 0 PASS, 1 FAIL, 77 when this process has no Vulkan device (nothing to compare with). helper: the helper program. */
int r64_selftest_run(const char *helper);

#endif
