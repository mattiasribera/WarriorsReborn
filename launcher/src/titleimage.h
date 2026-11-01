/* The launcher's background: art of the player's own installed game, read at run time (nothing
 * of the game is in the launcher). First choice is the boot splash, a 512x512 DXT3 texture (a
 * 640x480 picture in its top 512x384) in the game's WAD archives; second, the title image an XBE
 * carries in its $$XTIMAGE section as an XPR0 resource bundle (The Warriors: 128x128 DXT1). The
 * art is decoded, scaled, Gaussian-blurred and darkened, then cached as a PNG in the per-user
 * cache folder so the blur runs once per image. */
#ifndef LAUNCHER_TITLEIMAGE_H
#define LAUNCHER_TITLEIMAGE_H
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace launcher {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba; /* w * h * 4 bytes, rows top to bottom */
};

/* The raw bytes of the named section of an XBE file (base address at 0x104, section count and
 * headers at 0x11C/0x120, 56-byte headers: raw address +12, raw size +16, name address +20). */
bool xbe_section(const uint8_t *xbe, size_t size, const char *name, size_t &offset, size_t &length);
bool read_xbe_section(const std::string &path, const char *name, std::string &out, std::string *err);

/* The first texture of an XPR0 bundle ("XPR0", total size, header size, then 20-byte D3D
 * resources; texture data at header size + Data). Formats: DXT1, DXT3, DXT5, and 32-bit ARGB /
 * XRGB swizzled or linear. Only the top mip level is decoded. */
bool xpr_decode(const uint8_t *data, size_t size, Image &out, std::string *err);

/* The WAD archive file with the given name hash (CRC32 of the lowercased "ee_files\..." path)
 * in disc_dir/XBoxWad.idx: {u32 wads, files, ?, wad table, file table}, 36-byte wad entries
 * (name[32], handle), 16-byte file entries (loc, size, 0, name hash); wad = loc >> 24, offset =
 * (loc & 0xFFFFFF) << 10. */
bool wad_find(const std::string &disc_dir, uint32_t name_hash, std::string &wad_path, uint64_t &offset, uint32_t &size,
              std::string *err);

/* The boot splash with the autosave notice (the WAD entry has no recovered name). Its 0xA0-byte
 * header holds the texture data size at 0x2C; the data follows the header and the D3D resource
 * (Common, Data, Lock, Format, Size) follows the data. */
constexpr uint32_t SPLASH_HASH = 0xFDF31D9F;
bool read_splash(const std::string &disc_dir, std::string &out, std::string *err);
/* Decodes a splash entry and crops it to its rows with opaque texels (the picture). */
bool splash_decode(const uint8_t *data, size_t size, Image &out, std::string *err);

/* The background look: scaled (bilinear) so its longer side is size, Gaussian blur of the given
 * sigma (in output pixels), each colour multiplied by dark and moved toward its grey by desat. */
Image blur_darken(const Image &in, int size, float sigma, float dark, float desat);

/* The background from one source: cache_dir/bg-<look>-<hash of the source bytes>.png when it is
 * there, else decoded, blurred and written to that file (a failed write is not an error). */
bool title_background(const std::string &xbe_path, const std::string &cache_dir, Image &out, std::string *err);
bool splash_background(const std::string &disc_dir, const std::string &cache_dir, Image &out, std::string *err);
/* The splash, else the title image of disc_dir/default.xbe; *source says which. */
bool game_background(const std::string &disc_dir, const std::string &cache_dir, Image &out, std::string *source,
                     std::string *err);

} // namespace launcher
#endif
