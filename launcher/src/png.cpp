#include "png.h"
#include "platform.h"
#include <cstring>

namespace launcher {

static uint32_t crc32(const uint8_t *p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return c;
}

static void put32(std::string &s, uint32_t v) {
    for (int i = 3; i >= 0; i--) s += (char)((v >> (i * 8)) & 0xFF);
}

static void chunk(std::string &png, const char *type, const std::string &data) {
    put32(png, (uint32_t)data.size());
    std::string body = std::string(type, 4) + data;
    png += body;
    put32(png, crc32((const uint8_t *)body.data(), body.size()) ^ 0xFFFFFFFFu);
}

std::string encode_png_rgba(int w, int h, const uint8_t *pixels, size_t stride) {
    std::string raw; /* filter byte 0 + the row */
    raw.reserve((size_t)h * (w * 4 + 1));
    for (int y = 0; y < h; y++) {
        raw += '\0';
        raw.append((const char *)pixels + (size_t)y * stride, (size_t)w * 4);
    }
    std::string z = "\x78\x01"; /* zlib header, stored blocks of at most 65535 bytes */
    for (size_t pos = 0; pos < raw.size() || pos == 0;) {
        size_t n = raw.size() - pos < 65535 ? raw.size() - pos : 65535;
        z += (char)(pos + n == raw.size() ? 1 : 0);
        z += (char)(n & 0xFF), z += (char)(n >> 8), z += (char)(~n & 0xFF), z += (char)((~n >> 8) & 0xFF);
        z.append(raw, pos, n);
        pos += n;
        if (n == 0) break;
    }
    uint32_t a = 1, b = 0; /* adler32 */
    for (unsigned char c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
    put32(z, (b << 16) | a);

    std::string png = "\x89PNG\r\n\x1a\n", ihdr;
    put32(ihdr, (uint32_t)w);
    put32(ihdr, (uint32_t)h);
    ihdr += std::string("\x08\x06\x00\x00\x00", 5); /* 8-bit RGBA, deflate, no filter choice, no interlace */
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", "");
    return png;
}

bool write_png_rgba(const std::string &path, int w, int h, const uint8_t *pixels, size_t stride) {
    return write_file(path, encode_png_rgba(w, h, pixels, stride));
}

static uint32_t get32(const std::string &s, size_t at) {
    const uint8_t *p = (const uint8_t *)s.data() + at;
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

bool decode_png_rgba(const std::string &png, int &w, int &h, std::vector<uint8_t> &pixels) {
    if (png.size() < 33 || png.compare(0, 8, "\x89PNG\r\n\x1a\n") != 0 || png.compare(12, 4, "IHDR") != 0) return false;
    uint32_t pw = get32(png, 16), ph = get32(png, 20);
    if (!pw || !ph || pw > 8192 || ph > 8192 || png.compare(24, 5, std::string("\x08\x06\x00\x00\x00", 5)) != 0) return false;
    std::string z;
    for (size_t at = 8; at + 12 <= png.size();) { /* the IDAT chunks, CRCs checked */
        uint32_t len = get32(png, at);
        if (len > png.size() - at - 12) return false;
        uint32_t crc = crc32((const uint8_t *)png.data() + at + 4, len + 4) ^ 0xFFFFFFFFu;
        if (crc != get32(png, at + 8 + len)) return false;
        if (png.compare(at + 4, 4, "IDAT") == 0) z.append(png, at + 8, len);
        if (png.compare(at + 4, 4, "IEND") == 0) break;
        at += 12 + len;
    }
    size_t row = (size_t)pw * 4 + 1;
    std::string raw;
    raw.reserve(row * ph);
    size_t pos = 2;
    if (z.size() < 2 || (uint8_t)z[0] != 0x78) return false;
    for (bool last = false; !last;) {
        if (pos + 5 > z.size()) return false;
        uint8_t hdr = (uint8_t)z[pos];
        if (hdr & 6) return false; /* compressed blocks: not one of ours */
        last = hdr & 1;
        size_t n = (uint8_t)z[pos + 1] | (uint8_t)z[pos + 2] << 8, nn = (uint8_t)z[pos + 3] | (uint8_t)z[pos + 4] << 8;
        if ((n ^ 0xFFFF) != nn || pos + 5 + n > z.size()) return false;
        raw.append(z, pos + 5, n);
        pos += 5 + n;
    }
    if (raw.size() != row * ph) return false;
    pixels.resize((size_t)pw * ph * 4);
    for (uint32_t y = 0; y < ph; y++) {
        if (raw[y * row] != 0) return false;
        memcpy(&pixels[(size_t)y * pw * 4], raw.data() + y * row + 1, (size_t)pw * 4);
    }
    w = (int)pw, h = (int)ph;
    return true;
}

} // namespace launcher
