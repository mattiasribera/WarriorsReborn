/* --cinema-bars test (src/cinema_bars.c): the options, the letterbox bar classifier and the
 * replay reorder that puts the bars after the scene's last draw. */
#include "../src/cinema_bars.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

/* A pre-transformed quad (w 1) from clip (x0, y0) to (x1, y1) at depth z, all vertices colored c. */
static void quad(RVertex v[4], float x0, float y0, float x1, float y1, float z, uint32_t c) {
    memset(v, 0, 4 * sizeof *v);
    const float xy[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (int i = 0; i < 4; i++) {
        v[i].x = xy[i][0], v[i].y = xy[i][1], v[i].z = z, v[i].w = 1;
        v[i].color = c;
    }
}

static void test_args(void) {
    int on = 1;
    CHECK(cinema_bars_arg(&on, "--no-cinema-bars") == 1 && on == 0, "--no-cinema-bars turns it off");
    CHECK(cinema_bars_arg(&on, "--cinema-bars") == 1 && on == 1, "--cinema-bars turns it on");
    CHECK(cinema_bars_arg(&on, "--cinema") == 0 && on == 1, "other options are not consumed");
    CHECK(cinema_bars_on == 1, "on by default");
}

static void test_classify(void) {
    RVertex v[4];
    /* The title's bars as recorded in level51 (clip y -1..-0.76 and 0.76..1, z 0, 0xff000000). */
    quad(v, -1, -1, 1, -0.76f, 0, 0xff000000u);
    CHECK(cinema_bar_draw(v, 4, 0, 0, 0), "top bar");
    quad(v, -1, 0.76f, 1, 1, 0, 0xff000000u);
    CHECK(cinema_bar_draw(v, 4, 0, 0, 0), "bottom bar");
    quad(v, -1.0004f, 0.76f, 1.0004f, 1.0004f, 0, 0x00000000u);
    CHECK(cinema_bar_draw(v, 4, 0, 0, 0), "half-pixel offsets and any alpha");
    quad(v, -1, -1, 1, -0.76f, 0, 0xff000000u);
    CHECK(!cinema_bar_draw(v, 4, 1, 0, 0), "textured: not a bar");
    CHECK(!cinema_bar_draw(v, 4, 0, 1, 0), "vertex program: not a bar");
    CHECK(!cinema_bar_draw(v, 4, 0, 0, 1), "blended: not a bar");
    CHECK(!cinema_bar_draw(v, 3, 0, 0, 0), "three vertices: not a bar");
    quad(v, -1, -1, 1, -0.76f, 0, 0xff010000u);
    CHECK(!cinema_bar_draw(v, 4, 0, 0, 0), "not black: not a bar");
    quad(v, -1, -1, 1, -0.76f, 0.5f, 0xff000000u);
    CHECK(!cinema_bar_draw(v, 4, 0, 0, 0), "not at z 0 (world geometry): not a bar");
    quad(v, -1, -1, 1, 1, 0, 0xff000000u);
    CHECK(!cinema_bar_draw(v, 4, 0, 0, 0), "full-frame black (a fade): not a bar");
    quad(v, -0.5f, -1, 1, -0.76f, 0, 0xff000000u);
    CHECK(!cinema_bar_draw(v, 4, 0, 0, 0), "not across the whole width: not a bar");
    quad(v, -1, -0.2f, 1, 0.2f, 0, 0xff000000u);
    CHECK(!cinema_bar_draw(v, 4, 0, 0, 0), "not at an edge: not a bar");
    quad(v, -1, -1, 1, -0.76f, 0, 0xff000000u);
    v[2].x = 0.3f;
    CHECK(!cinema_bar_draw(v, 4, 0, 0, 0), "not a rectangle: not a bar");
}

static void test_reorder(void) {
    /* ops: 0 clear, 1-3 world, 4 bar, 5 rain layer (scene), 6 bar, 7 subtitles, 8 world, 9 HUD */
    const uint8_t bar[10] = {0, 0, 0, 0, 1, 0, 1, 0, 0, 0};
    const uint8_t scene[10] = {0, 1, 1, 1, 0, 1, 0, 0, 1, 0};
    uint32_t ord[10], tmp[10];
    for (uint32_t i = 0; i < 10; i++) ord[i] = i;
    uint32_t pos = 7;   /* op 7's position */
    uint32_t moved = cinema_bars_reorder(ord, 10, bar, scene, tmp, &pos);
    const uint32_t want[10] = {0, 1, 2, 3, 5, 7, 8, 4, 6, 9};
    CHECK(moved == 2 && !memcmp(ord, want, sizeof want), "bars after the last scene draw, in their order (moved %u)", moved);
    CHECK(pos == 5 && ord[pos] == 7, "a position before the last scene draw is remapped (%u)", pos);

    for (uint32_t i = 0; i < 10; i++) ord[i] = i;
    pos = 9;
    const uint8_t scene2[10] = {0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
    moved = cinema_bars_reorder(ord, 10, bar, scene2, tmp, &pos);
    const uint32_t want2[10] = {0, 1, 2, 3, 5, 4, 6, 7, 8, 9};
    CHECK(moved == 1 && !memcmp(ord, want2, sizeof want2), "only the bars before the last scene draw move (moved %u)", moved);
    CHECK(pos == 9, "a position after it stays (%u)", pos);

    const uint8_t none[10] = {0};
    for (uint32_t i = 0; i < 10; i++) ord[i] = 9 - i;   /* a bloom order: any permutation */
    pos = 10;
    moved = cinema_bars_reorder(ord, 10, none, scene, tmp, &pos);
    CHECK(moved == 0 && ord[0] == 9 && ord[9] == 0 && pos == 10, "no bars: order unchanged");
    /* Bars by op index inside a permuted order: ord = 9..0, bars at ops 4 and 6 (positions 5, 3),
     * the last scene draw op 1 (position 8). */
    moved = cinema_bars_reorder(ord, 10, bar, scene, tmp, &pos);
    const uint32_t want3[10] = {9, 8, 7, 5, 3, 2, 1, 6, 4, 0};
    CHECK(moved == 2 && !memcmp(ord, want3, sizeof want3), "bars found by op index in a permuted order");
}

int main(void) {
    test_args();
    test_classify();
    test_reorder();
    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
