/* The player's own Xbox disc image (.iso), read with the launcher's own XDVDFS reader and extracted
 * once into a folder the runtime can use. The image is only ever opened for reading.
 *
 * XDVDFS: 2048-byte sectors counted from the start of the game partition, which sits at 0 in a
 * trimmed xiso and further in on a full (Redump-style) image; the partition is the first of
 * ISO_PARTITION_OFFSETS with "MICROSOFT*XBOX*MEDIA" at the start and at 0x7EC of its sector 32,
 * where the root directory's sector (+20) and size (+24) follow the magic. A directory is a table
 * of entries forming a binary tree: u16 left and right subtree offsets (in 4-byte units from the
 * table start, 0 = none), u32 start sector, u32 size, u8 attributes (0x10 = folder), u8 name
 * length, the name; entries are 4-byte aligned. Names, offsets and sizes are validated: a name
 * with a path separator, a drive colon or a control character, "." or "..", a subtree outside its
 * table, a cycle, a folder nested too deep or data past the end of the image is an error. */
#ifndef LAUNCHER_ISO_H
#define LAUNCHER_ISO_H
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include "xbe_check.h"

namespace launcher {

extern const uint64_t ISO_PARTITION_OFFSETS[4]; /* 0, 0x18300000, 0x2080000, 0xFD90000 */

struct IsoFile {
    std::string path;    /* relative, '/'-separated, as named in the image */
    uint64_t offset = 0; /* absolute byte offset in the image */
    uint64_t size = 0;
};

struct IsoImage {
    std::string path;
    uint64_t image_size = 0, partition = 0;
    std::vector<std::string> dirs; /* parents before their children */
    std::vector<IsoFile> files;
    uint64_t total_bytes = 0;
};

/* Finds the partition and reads the whole directory tree. false with *err in plain language. */
bool iso_open(const std::string &path, IsoImage &out, std::string *err);
/* The file at rel ('/'-separated, case-insensitive like the Xbox), or null. */
const IsoFile *iso_find(const IsoImage &img, const std::string &rel);
/* The title check of the image's default.xbe (Missing when the image has none). */
XbeCheck iso_check_xbe(const IsoImage &img);

/* Bytes still to write into dest: files that are not already there with the same size. */
uint64_t iso_bytes_needed(const IsoImage &img, const std::string &dest);

/* Shared with the GUI while a worker thread extracts; bytes and files count skipped files too. */
struct IsoProgress {
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint32_t> files{0}, skipped{0};
    std::atomic<bool> cancel{false};
};

enum class IsoResult { Ok, Cancelled, Failed };

/* Extracts every folder and file into dest (created when missing). A file already there with the
 * same size is skipped, so an interrupted extraction resumes; a file cut short by a cancel or an
 * error is removed. */
IsoResult iso_extract(const IsoImage &img, const std::string &dest, IsoProgress *progress, std::string *err);

std::string format_bytes(uint64_t bytes); /* "5.2 GB", "980 MB", "12 KB" */

} // namespace launcher
#endif
