/* Host tool for tools/texture_upscale.py and tools/texture_basis.py: decodes a PNG (any color type,
 * via the vendored stb_image in src/texpack.c) and writes "W H\n" followed by W * H RGBA8 texels to
 * stdout. A .dds (BC1, BC3 or BC7 with a full mip chain, as texture packs hold them) is decoded
 * level by level with the runtime's own BC decoders: "W H\n" + texels for each level, level 0
 * first. Usage: pngraw FILE.png | FILE.dds. Exits 1 when the file cannot be read. Built for Linux
 * (build/pngraw) and Windows (build/pngraw.exe). */
#include "../src/texpack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/* B8G8R8A8 (0xAARRGGBB) texels to R, G, B, A bytes, after a "W H" line. */
static int put(const uint32_t *src, uint32_t w, uint32_t h) {
    size_t n = (size_t)w * h;
    uint32_t *px = malloc(n * 4);
    if (!px) return 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = src[i];
        px[i] = (c & 0xFF00FF00u) | ((c >> 16) & 0xFFu) | ((c & 0xFFu) << 16);
    }
    printf("%u %u\n", w, h);
    fflush(stdout);
    int ok = fwrite(px, 4, n, stdout) == n;
    free(px);
    return ok;
}

static int is_dds(const char *p) {
    size_t n = strlen(p);
    return n > 4 && p[n - 4] == '.' && (p[n - 3] | 32) == 'd' && (p[n - 2] | 32) == 'd' && (p[n - 1] | 32) == 's';
}

int main(int argc, char **argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);   /* text mode would turn every 0x0A texel byte into CR LF */
#endif
    if (argc == 2 && is_dds(argv[1])) {
        TexpackImage img;
        const char *why = NULL;
        if (texpack_load_image(argv[1], &img, &why) || img.format == TEXPACK_BGRA8) {
            fprintf(stderr, "pngraw: %s: %s\n", argv[1], why ? why : "not BC blocks");
            return 1;
        }
        int ok = 1;
        size_t at = 0;
        for (uint32_t l = 0; ok && l < img.levels; l++) {
            const uint32_t w = img.w >> l ? img.w >> l : 1, h = img.h >> l ? img.h >> l : 1;
            TexpackImage lv = {img.format, w, h, 1, (uint8_t *)img.data + at};
            uint32_t *px = texpack_image_bgra(&lv);
            ok = px && put(px, w, h);
            free(px);
            at += texpack_level_bytes(img.format, w, h);
        }
        free(img.data);
        return ok ? 0 : 1;
    }
    uint32_t w, h, *px = argc == 2 ? texpack_png_read(argv[1], &w, &h) : NULL;
    if (!px) {
        fprintf(stderr, "usage: pngraw FILE.png | FILE.dds (a readable PNG, or a BC1/BC3/BC7 .dds)\n");
        return 1;
    }
    int ok = put(px, w, h);
    free(px);
    return ok ? 0 : 1;
}
