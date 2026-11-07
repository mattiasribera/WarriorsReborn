/* Host tool for tools/texture_upscale.py: decodes a PNG (any color type, via the vendored
 * stb_image in src/texpack.c) and writes "W H\n" followed by W * H RGBA8 texels to stdout.
 * Usage: pngraw FILE.png. Exits 1 when the file cannot be read. Built for Linux (build/pngraw)
 * and Windows (build/pngraw.exe). */
#include "../src/texpack.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

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
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);   /* text mode would turn every 0x0A texel byte into CR LF */
#endif
    printf("%u %u\n", w, h);
    fflush(stdout);
    int ok = fwrite(px, 4, n, stdout) == n;
    free(px);
    return ok ? 0 : 1;
}
