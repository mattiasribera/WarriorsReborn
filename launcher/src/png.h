/* A minimal PNG writer (8-bit RGBA, stored deflate blocks: no compression) for the launcher's
 * --capture screenshots and its cached background, and the reader for those files. */
#ifndef LAUNCHER_PNG_H
#define LAUNCHER_PNG_H
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace launcher {

/* The PNG file for w x h RGBA pixels, rows stride bytes apart. */
std::string encode_png_rgba(int w, int h, const uint8_t *pixels, size_t stride);
bool write_png_rgba(const std::string &path, int w, int h, const uint8_t *pixels, size_t stride);
/* Reads a PNG as this writer makes it (8-bit RGBA, no interlace, stored deflate blocks, filter
 * byte 0); false for anything else, so a foreign or damaged file is simply made again. */
bool decode_png_rgba(const std::string &png, int &w, int &h, std::vector<uint8_t> &pixels);

} // namespace launcher
#endif
