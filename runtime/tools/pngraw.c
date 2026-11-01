/* Host tool for tools/texture_upscale.py: decodes a PNG (any color type, via the vendored
 * stb_image in src/texpack.c) and writes "W H\n" followed by W * H RGBA8 texels to stdout.
 * Usage: pngraw FILE.png. Exits 1 when the file cannot be read. */
#include "../src/texpack.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    uint32_t w, h, *px = argc == 2 ? texpack_png_read(argv[1], &w, &h) : NULL;
    if (!px) {
        fprintf(stderr, "usage: pngraw FILE.png (a readable PNG)\n");
        return 1;
    }
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; i++) {   /* 0xAARRGGBB -> R, G, B, A bytes */
        uint32_t c = px[i];
        px[i] = (c & 0xFF00FF00u) | ((c >> 16) & 0xFFu) | ((c & 0xFFu) << 16);
    }
    printf("%u %u\n", w, h);
    fflush(stdout);
    int ok = fwrite(px, 4, n, stdout) == n;
    free(px);
    return ok ? 0 : 1;
}
