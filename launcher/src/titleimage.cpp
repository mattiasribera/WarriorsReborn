#include "titleimage.h"
#include "platform.h"
#include "png.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace launcher {

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static bool fail(std::string *err, const std::string &what) {
    if (err) *err = what;
    return false;
}

/* ---- XBE ---- */

bool xbe_section(const uint8_t *xbe, size_t size, const char *name, size_t &offset, size_t &length) {
    if (size < 0x178 || memcmp(xbe, "XBEH", 4) != 0) return false;
    uint32_t base = rd32(xbe + 0x104), count = rd32(xbe + 0x11C), headers = rd32(xbe + 0x120);
    if (headers < base || count > 1024) return false;
    size_t at = headers - base, name_len = strlen(name);
    if (at > size || (size - at) / 56 < count) return false;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *h = xbe + at + i * 56;
        uint32_t n = rd32(h + 20);
        if (n < base || n - base >= size || size - (n - base) <= name_len) continue;
        const char *s = (const char *)xbe + (n - base);
        if (memcmp(s, name, name_len) != 0 || s[name_len] != '\0') continue;
        offset = rd32(h + 12);
        length = rd32(h + 16);
        return true;
    }
    return false;
}

bool read_xbe_section(const std::string &path, const char *name, std::string &out, std::string *err) {
    InputFile f;
    if (!f.open(path)) return fail(err, "cannot open " + path);
    uint8_t head[0x200];
    if (f.size() < sizeof head || !f.read_at(0, head, sizeof head) || memcmp(head, "XBEH", 4) != 0)
        return fail(err, "not an XBE file");
    uint32_t header_size = rd32(head + 0x108);
    if (header_size < sizeof head || header_size > (1u << 20) || header_size > f.size())
        return fail(err, "bad XBE header size");
    std::string headers(header_size, '\0');
    if (!f.read_at(0, &headers[0], header_size)) return fail(err, "cannot read the XBE headers");
    size_t offset = 0, length = 0;
    if (!xbe_section((const uint8_t *)headers.data(), headers.size(), name, offset, length))
        return fail(err, std::string("no ") + name + " section");
    if (length == 0 || length > (4u << 20) || offset > f.size() || f.size() - offset < length)
        return fail(err, std::string("bad ") + name + " section");
    out.assign(length, '\0');
    if (!f.read_at(offset, &out[0], length)) return fail(err, std::string("cannot read ") + name);
    return true;
}

/* ---- XPR0 textures ---- */

enum : uint32_t { // the colour format field of an Xbox D3D texture's Format (bits 8-15)
    FMT_A8R8G8B8 = 0x06, FMT_X8R8G8B8 = 0x07, FMT_DXT1 = 0x0C, FMT_DXT3 = 0x0E, FMT_DXT5 = 0x0F,
    FMT_LIN_A8R8G8B8 = 0x12, FMT_LIN_X8R8G8B8 = 0x1E,
};

static void rgb565(uint16_t c, uint8_t *o) {
    o[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
    o[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
    o[2] = (uint8_t)((c & 31) * 255 / 31);
}

/* One 4x4 colour block into px (stride bytes per row); three_colour allows DXT1's 1-bit alpha. */
static void dxt_colour(const uint8_t *b, uint8_t *px, size_t stride, bool three_colour) {
    uint16_t c0 = rd16(b), c1 = rd16(b + 2);
    uint8_t pal[4][4];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
    for (int k = 0; k < 3; k++) {
        if (c0 > c1 || !three_colour) {
            pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
        } else {
            pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    if (c0 <= c1 && three_colour) pal[3][3] = 0;
    uint32_t idx = rd32(b + 4);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) memcpy(px + y * stride + x * 4, pal[(idx >> (2 * (y * 4 + x))) & 3], 4);
}

static void dxt3_alpha(const uint8_t *b, uint8_t *px, size_t stride) {
    for (int i = 0; i < 16; i++) {
        int a = (b[i / 2] >> (4 * (i & 1))) & 15;
        px[(i / 4) * stride + (i % 4) * 4 + 3] = (uint8_t)(a * 17);
    }
}

static void dxt5_alpha(const uint8_t *b, uint8_t *px, size_t stride) {
    int a[8] = {b[0], b[1]};
    for (int i = 2; i < 8; i++)
        a[i] = a[0] > a[1] ? ((8 - i) * a[0] + (i - 1) * a[1]) / 7 : i < 6 ? ((6 - i) * a[0] + (i - 1) * a[1]) / 5 : i == 6 ? 0 : 255;
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) px[(i / 4) * stride + (i % 4) * 4 + 3] = (uint8_t)a[(bits >> (3 * i)) & 7];
}

/* The texel offset of (x, y) in a swizzled (Morton order) w x h texture, both powers of two. */
static uint32_t swizzle(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t off = 0, bit = 1;
    for (uint32_t mw = 1, mh = 1; mw < w || mh < h;) {
        if (mw < w) off |= (x & mw) ? bit : 0, bit <<= 1, mw <<= 1;
        if (mh < h) off |= (y & mh) ? bit : 0, bit <<= 1, mh <<= 1;
    }
    return off;
}

/* A D3D texture resource r (Common, Data, Lock, Format, Size) whose top level starts at
 * data[pos], data being avail bytes long. */
static bool texture_decode(const uint8_t *r, const uint8_t *data, size_t avail, uint32_t pos, Image &out, std::string *err) {
    if (((rd32(r) >> 16) & 7) != 4) return fail(err, "the resource is not a texture");
    uint32_t format = rd32(r + 12), sz = rd32(r + 16);
    uint32_t fmt = (format >> 8) & 0xFF, w, h, pitch = 0;
    if (sz) { /* linear texture with an explicit size */
        w = (sz & 0xFFF) + 1;
        h = ((sz >> 12) & 0xFFF) + 1;
        pitch = (((sz >> 24) & 0xFF) + 1) * 64;
    } else {
        w = 1u << ((format >> 20) & 15);
        h = 1u << ((format >> 24) & 15);
    }
    if (w > 4096 || h > 4096) return fail(err, "texture too large");
    bool dxt = fmt == FMT_DXT1 || fmt == FMT_DXT3 || fmt == FMT_DXT5;
    bool argb = fmt == FMT_A8R8G8B8 || fmt == FMT_X8R8G8B8, lin = fmt == FMT_LIN_A8R8G8B8 || fmt == FMT_LIN_X8R8G8B8;
    if (!dxt && !argb && !lin) {
        char buf[64];
        snprintf(buf, sizeof buf, "unsupported texture format 0x%02X", (unsigned)fmt);
        return fail(err, buf);
    }
    if (lin && !pitch) pitch = w * 4;
    uint64_t need = dxt ? (uint64_t)((w + 3) / 4) * ((h + 3) / 4) * (fmt == FMT_DXT1 ? 8 : 16) : lin ? (uint64_t)pitch * h : (uint64_t)w * h * 4;
    if (pos > avail || avail - pos < need) return fail(err, "texture data past the end");
    const uint8_t *src = data + pos;

    out.w = (int)w, out.h = (int)h;
    out.rgba.assign((size_t)w * h * 4, 0);
    size_t stride = (size_t)w * 4;
    if (dxt) {
        int bw = (int)(w + 3) / 4, bh = (int)(h + 3) / 4, bs = fmt == FMT_DXT1 ? 8 : 16;
        uint8_t tile[4 * 16];
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++, src += bs) {
                const uint8_t *cb = fmt == FMT_DXT1 ? src : src + 8;
                dxt_colour(cb, tile, 16, fmt == FMT_DXT1);
                if (fmt == FMT_DXT3) dxt3_alpha(src, tile, 16);
                if (fmt == FMT_DXT5) dxt5_alpha(src, tile, 16);
                for (int y = 0; y < 4 && by * 4 + y < (int)h; y++)
                    for (int x = 0; x < 4 && bx * 4 + x < (int)w; x++)
                        memcpy(&out.rgba[(by * 4 + y) * stride + (bx * 4 + x) * 4], tile + y * 16 + x * 4, 4);
            }
        return true;
    }
    bool opaque = fmt == FMT_X8R8G8B8 || fmt == FMT_LIN_X8R8G8B8;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *s = src + (lin ? (size_t)y * pitch + x * 4 : (size_t)swizzle(x, y, w, h) * 4); /* B G R A */
            uint8_t *d = &out.rgba[y * stride + x * 4];
            d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = opaque ? 255 : s[3];
        }
    return true;
}

bool xpr_decode(const uint8_t *data, size_t size, Image &out, std::string *err) {
    if (size < 32 || memcmp(data, "XPR0", 4) != 0) return fail(err, "not an XPR0 bundle");
    uint32_t header = rd32(data + 8);
    if (header < 32 || header > size) return fail(err, "bad XPR0 header size");
    /* the first resource follows the 12-byte header; its Data is relative to the header's end */
    return texture_decode(data + 12, data + header, size - header, rd32(data + 16), out, err);
}

/* ---- The boot splash in the game's WAD archives ---- */

bool wad_find(const std::string &disc_dir, uint32_t name_hash, std::string &wad_path, uint64_t &offset, uint32_t &size,
              std::string *err) {
    std::string idx;
    if (!read_file(path_join(disc_dir, "XBoxWad.idx"), idx, 4u << 20)) return fail(err, "no XBoxWad.idx");
    const uint8_t *d = (const uint8_t *)idx.data();
    if (idx.size() < 20) return fail(err, "bad XBoxWad.idx");
    uint32_t wads = rd32(d), files = rd32(d + 4), wad_table = rd32(d + 12), file_table = rd32(d + 16);
    if (wads > 256 || files > (1u << 20) || wad_table > idx.size() || (idx.size() - wad_table) / 36 < wads ||
        file_table > idx.size() || (idx.size() - file_table) / 16 < files)
        return fail(err, "bad XBoxWad.idx");
    for (uint32_t i = 0; i < files; i++) {
        const uint8_t *f = d + file_table + i * 16;
        if (rd32(f + 12) != name_hash) continue;
        uint32_t loc = rd32(f), wad = loc >> 24;
        if (wad >= wads) return fail(err, "bad XBoxWad.idx entry");
        const char *name = (const char *)d + wad_table + wad * 36;
        wad_path = path_join(disc_dir, std::string(name, strnlen(name, 32)));
        offset = (uint64_t)(loc & 0xFFFFFF) << 10;
        size = rd32(f + 4);
        return true;
    }
    return fail(err, "no splash entry in XBoxWad.idx");
}

bool splash_decode(const uint8_t *data, size_t size, Image &out, std::string *err) {
    if (size < 0xA0 + 20) return fail(err, "splash too short");
    uint32_t bytes = rd32(data + 0x2C);
    if (bytes > size - 0xA0 - 20) return fail(err, "bad splash header");
    if (!texture_decode(data + 0xA0 + bytes, data + 0xA0, bytes, 0, out, err)) return false;
    int top = out.h, bottom = 0; /* the picture: the rows with any opaque texel (the rest is padding) */
    for (int y = 0; y < out.h; y++)
        for (int x = 0; x < out.w; x++)
            if (out.rgba[((size_t)y * out.w + x) * 4 + 3]) {
                if (y < top) top = y;
                bottom = y + 1;
                break;
            }
    if (top >= bottom) return fail(err, "the splash is empty");
    out.rgba = std::vector<uint8_t>(out.rgba.begin() + (size_t)top * out.w * 4, out.rgba.begin() + (size_t)bottom * out.w * 4);
    out.h = bottom - top;
    return true;
}

bool read_splash(const std::string &disc_dir, std::string &out, std::string *err) {
    std::string wad;
    uint64_t offset = 0;
    uint32_t size = 0;
    if (!wad_find(disc_dir, SPLASH_HASH, wad, offset, size, err)) return false;
    InputFile f;
    if (!f.open(wad)) return fail(err, "cannot open " + wad);
    if (size == 0 || size > (4u << 20) || offset > f.size() || f.size() - offset < size) return fail(err, "bad splash entry");
    out.assign(size, '\0');
    if (!f.read_at(offset, &out[0], size)) return fail(err, "cannot read the splash");
    return true;
}

/* ---- The background look ---- */

/* One pass of a normalised Gaussian along rows (step 1) or columns (step w), clamped at the edges. */
static void gauss_pass(const std::vector<float> &in, std::vector<float> &out, int w, int h, bool rows,
                       const std::vector<float> &kernel) {
    int r = (int)kernel.size() - 1, n = rows ? w : h, lines = rows ? h : w;
    for (int l = 0; l < lines; l++)
        for (int i = 0; i < n; i++) {
            float acc[3] = {0, 0, 0};
            for (int k = -r; k <= r; k++) {
                int j = i + k < 0 ? 0 : i + k >= n ? n - 1 : i + k;
                const float *s = &in[((rows ? l * w + j : j * w + l)) * 3];
                float kw = kernel[k < 0 ? -k : k];
                acc[0] += s[0] * kw, acc[1] += s[1] * kw, acc[2] += s[2] * kw;
            }
            float *d = &out[(rows ? l * w + i : i * w + l) * 3];
            d[0] = acc[0], d[1] = acc[1], d[2] = acc[2];
        }
}

Image blur_darken(const Image &in, int size, float sigma, float dark, float desat) {
    Image out;
    if (in.w <= 0 || in.h <= 0 || size <= 0) return out;
    int w = in.w >= in.h ? size : (int)((int64_t)size * in.w / in.h), h = in.w >= in.h ? (int)((int64_t)size * in.h / in.w) : size;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    std::vector<float> a((size_t)w * h * 3), b(a.size());
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) { /* bilinear, sample centres aligned */
            float sx = (x + 0.5f) * in.w / w - 0.5f, sy = (y + 0.5f) * in.h / h - 0.5f;
            int x0 = (int)floorf(sx), y0 = (int)floorf(sy);
            float fx = sx - x0, fy = sy - y0;
            for (int c = 0; c < 3; c++) {
                float v = 0;
                for (int k = 0; k < 4; k++) {
                    int px = x0 + (k & 1), py = y0 + (k >> 1);
                    px = px < 0 ? 0 : px >= in.w ? in.w - 1 : px;
                    py = py < 0 ? 0 : py >= in.h ? in.h - 1 : py;
                    float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
                    v += in.rgba[((size_t)py * in.w + px) * 4 + c] * wgt;
                }
                a[((size_t)y * w + x) * 3 + c] = v;
            }
        }
    if (sigma > 0) {
        int r = (int)ceilf(sigma * 3);
        std::vector<float> kernel(r + 1);
        float sum = 0;
        for (int k = 0; k <= r; k++) sum += (kernel[k] = expf(-(float)(k * k) / (2 * sigma * sigma))) * (k ? 2 : 1);
        for (float &k : kernel) k /= sum;
        gauss_pass(a, b, w, h, true, kernel);
        gauss_pass(b, a, w, h, false, kernel);
    }
    out.w = w, out.h = h;
    out.rgba.resize((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        float *s = &a[i * 3], grey = 0.299f * s[0] + 0.587f * s[1] + 0.114f * s[2];
        for (int c = 0; c < 3; c++) {
            float v = (s[c] + (grey - s[c]) * desat) * dark;
            out.rgba[i * 4 + c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f);
        }
        out.rgba[i * 4 + 3] = 255;
    }
    return out;
}

/* Each look's parameters; its key names them in the cache file, so a change makes a new file. */
struct Look {
    const char *key;
    int size;
    float sigma, dark, desat;
};
static const Look SPLASH_LOOK = {"splash-v2", 768, 7, 0.5f, 0.1f}; /* 512x384 art: lighter blur */
static const Look TITLE_LOOK = {"title-v2", 384, 13, 0.6f, 0.15f};  /* 128x128: heavy blur */

typedef bool (*Decoder)(const uint8_t *data, size_t size, Image &out, std::string *err);

/* The cached look of the source bytes, or decoded, blurred and cached. */
static bool cached_look(const std::string &src, Decoder decode, const Look &look, const std::string &cache_dir, Image &out,
                        std::string *err) {
    uint32_t hash = 2166136261u; /* FNV-1a */
    for (unsigned char c : src) hash = (hash ^ c) * 16777619u;
    char name[64];
    snprintf(name, sizeof name, "bg-%s-%08x.png", look.key, (unsigned)hash);
    std::string cached = cache_dir.empty() ? "" : path_join(cache_dir, name), png;
    if (!cached.empty() && read_file(cached, png, 16u << 20) && decode_png_rgba(png, out.w, out.h, out.rgba) && out.w > 0)
        return true;
    Image art;
    if (!decode((const uint8_t *)src.data(), src.size(), art, err)) return false;
    if (art.w < 16 || art.h < 16) return fail(err, "the image is too small");
    out = blur_darken(art, look.size, look.sigma, look.dark, look.desat);
    if (!cached.empty() && make_dirs(cache_dir)) write_png_rgba(cached, out.w, out.h, out.rgba.data(), (size_t)out.w * 4);
    return true;
}

bool title_background(const std::string &xbe_path, const std::string &cache_dir, Image &out, std::string *err) {
    std::string xpr;
    return read_xbe_section(xbe_path, "$$XTIMAGE", xpr, err) && cached_look(xpr, xpr_decode, TITLE_LOOK, cache_dir, out, err);
}

bool splash_background(const std::string &disc_dir, const std::string &cache_dir, Image &out, std::string *err) {
    std::string splash;
    return read_splash(disc_dir, splash, err) && cached_look(splash, splash_decode, SPLASH_LOOK, cache_dir, out, err);
}

bool game_background(const std::string &disc_dir, const std::string &cache_dir, Image &out, std::string *source,
                     std::string *err) {
    std::string splash_err, title_err;
    const char *from = nullptr;
    if (splash_background(disc_dir, cache_dir, out, &splash_err)) from = "boot splash";
    else if (title_background(path_join(disc_dir, "default.xbe"), cache_dir, out, &title_err)) from = "title image";
    if (from) {
        if (source) *source = from;
        return true;
    }
    return fail(err, "splash: " + splash_err + "; title image: " + title_err);
}

} // namespace launcher
