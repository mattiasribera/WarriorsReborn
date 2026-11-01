#include "iso.h"
#include "platform.h"
#include <cstdio>
#include <cstring>

namespace launcher {

const uint64_t ISO_PARTITION_OFFSETS[4] = {0, 0x18300000, 0x2080000, 0xFD90000};

#define ISO_SECTOR 2048
#define ISO_VD_SECTOR 32
#define ISO_MAGIC "MICROSOFT*XBOX*MEDIA"
#define ISO_MAGIC_LEN 20
#define ISO_MAGIC_TAIL 0x7EC
#define ISO_ATTR_DIR 0x10
#define ISO_DIRENT_HEAD 14
#define ISO_MAX_DIR_TABLE (16u << 20) /* far above any real directory */
#define ISO_MAX_DEPTH 32
#define ISO_MAX_ENTRIES 200000
#define ISO_XBE_HEAD (64 * 1024)      /* xbe_check reads the same header page */
#define ISO_CHUNK (1u << 20)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static bool fail(std::string *err, const std::string &text) {
    if (err) *err = text;
    return false;
}

static bool damaged(std::string *err, const std::string &detail) {
    return fail(err, "The disc image is damaged or not supported (" + detail + ").");
}

/* A name the extracted copy can hold as one path component on Windows and POSIX. */
static bool safe_name(const std::string &name) {
    if (name.empty() || name == "." || name == "..") return false;
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7F || strchr("/\\:*?\"<>|", c)) return false;
    return true;
}

static bool find_partition(InputFile &in, uint64_t &partition, uint32_t &root_sector, uint32_t &root_size) {
    for (uint64_t off : ISO_PARTITION_OFFSETS) {
        uint8_t vd[ISO_SECTOR];
        if (!in.read_at(off + (uint64_t)ISO_VD_SECTOR * ISO_SECTOR, vd, sizeof vd)) continue;
        if (memcmp(vd, ISO_MAGIC, ISO_MAGIC_LEN) || memcmp(vd + ISO_MAGIC_TAIL, ISO_MAGIC, ISO_MAGIC_LEN)) continue;
        partition = off;
        root_sector = rd32(vd + 20);
        root_size = rd32(vd + 24);
        return true;
    }
    return false;
}

struct PendingDir {
    uint32_t sector, size;
    std::string prefix; /* "" for the root, else "a/b/" */
    int depth;
};

/* One directory table: its entries into out, its folders onto pending. */
static bool read_dir(InputFile &in, IsoImage &out, const PendingDir &d, std::vector<PendingDir> &pending,
                     size_t &entries, std::string *err) {
    if (d.size == 0) return true;
    if (d.size > ISO_MAX_DIR_TABLE) return damaged(err, "a folder table is too large");
    uint64_t at = out.partition + (uint64_t)d.sector * ISO_SECTOR;
    if (at > out.image_size || d.size > out.image_size - at) return damaged(err, "a folder lies past the end of the image");
    std::vector<uint8_t> t(d.size);
    if (!in.read_at(at, t.data(), t.size())) return fail(err, "The disc image cannot be read.");
    if (t.size() >= 4 && rd32(t.data()) == 0xFFFFFFFF) return true; /* an empty folder's padding */

    std::vector<char> seen(t.size() / 4 + 1, 0);
    std::vector<uint32_t> stack = {0};
    while (!stack.empty()) {
        uint32_t o = stack.back();
        stack.pop_back();
        if (o % 4 || (uint64_t)o + ISO_DIRENT_HEAD > t.size()) return damaged(err, "a folder entry lies outside its table");
        if (seen[o / 4]) return damaged(err, "a loop in a folder table");
        seen[o / 4] = 1;
        if (++entries > ISO_MAX_ENTRIES) return damaged(err, "too many files");
        const uint8_t *e = t.data() + o;
        uint16_t left = rd16(e), right = rd16(e + 2);
        uint32_t sector = rd32(e + 4), size = rd32(e + 8);
        uint8_t attr = e[12], len = e[13];
        if (len == 0 || (uint64_t)o + ISO_DIRENT_HEAD + len > t.size()) return damaged(err, "a name lies outside its table");
        std::string name((const char *)e + ISO_DIRENT_HEAD, len);
        if (!safe_name(name)) return damaged(err, "an unsafe file name");
        std::string path = d.prefix + name;
        if (attr & ISO_ATTR_DIR) {
            if (d.depth + 1 > ISO_MAX_DEPTH) return damaged(err, "folders nested too deep");
            out.dirs.push_back(path);
            pending.push_back({sector, size, path + "/", d.depth + 1});
        } else {
            IsoFile f;
            f.path = path;
            f.offset = out.partition + (uint64_t)sector * ISO_SECTOR;
            f.size = size;
            if (f.size && (f.offset > out.image_size || f.size > out.image_size - f.offset))
                return damaged(err, "a file lies past the end of the image");
            out.total_bytes += f.size;
            out.files.push_back(f);
        }
        if (right) stack.push_back((uint32_t)right * 4);
        if (left) stack.push_back((uint32_t)left * 4);
    }
    return true;
}

bool iso_open(const std::string &path, IsoImage &out, std::string *err) {
    out = IsoImage();
    out.path = path;
    InputFile in;
    if (!in.open(path)) return fail(err, "The disc image cannot be opened.");
    out.image_size = in.size();
    uint32_t root_sector = 0, root_size = 0;
    if (!find_partition(in, out.partition, root_sector, root_size))
        return fail(err, "This file is not an Xbox disc image (no Xbox game partition found).");
    std::vector<PendingDir> pending = {{root_sector, root_size, "", 0}};
    size_t entries = 0;
    while (!pending.empty()) {
        PendingDir d = pending.back();
        pending.pop_back();
        if (!read_dir(in, out, d, pending, entries, err)) return false;
    }
    return true;
}

static bool same_ci(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        unsigned char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}

const IsoFile *iso_find(const IsoImage &img, const std::string &rel) {
    for (const IsoFile &f : img.files)
        if (same_ci(f.path, rel)) return &f;
    return nullptr;
}

XbeCheck iso_check_xbe(const IsoImage &img) {
    XbeCheck r;
    const IsoFile *f = iso_find(img, "default.xbe");
    if (!f) return r;
    InputFile in;
    std::vector<uint8_t> head((size_t)(f->size < ISO_XBE_HEAD ? f->size : ISO_XBE_HEAD));
    if (!in.open(img.path) || !in.read_at(f->offset, head.data(), head.size())) {
        r.status = XbeStatus::Unreadable;
        return r;
    }
    return check_xbe_bytes(head.data(), head.size());
}

/* dest + the '/'-separated relative path, with the host's separator. */
static std::string dest_path(const std::string &dest, const std::string &rel) {
    std::string out = dest;
    size_t start = 0;
    while (start <= rel.size()) {
        size_t slash = rel.find('/', start);
        if (slash == std::string::npos) slash = rel.size();
        out = path_join(out, rel.substr(start, slash - start));
        start = slash + 1;
    }
    return out;
}

static bool already_there(const std::string &path, uint64_t size) {
    uint64_t have = 0;
    return file_size(path, have) && have == size;
}

uint64_t iso_bytes_needed(const IsoImage &img, const std::string &dest) {
    uint64_t need = 0;
    for (const IsoFile &f : img.files)
        if (!already_there(dest_path(dest, f.path), f.size)) need += f.size;
    return need;
}

IsoResult iso_extract(const IsoImage &img, const std::string &dest, IsoProgress *progress, std::string *err) {
    IsoProgress local;
    IsoProgress &pr = progress ? *progress : local;
    if (!make_dirs(dest)) return fail(err, "The folder " + dest + " cannot be created."), IsoResult::Failed;
    for (const std::string &d : img.dirs)
        if (!make_dir(dest_path(dest, d)))
            return fail(err, "The folder " + dest_path(dest, d) + " cannot be created."), IsoResult::Failed;
    InputFile in;
    if (!in.open(img.path) || in.size() != img.image_size)
        return fail(err, "The disc image cannot be opened."), IsoResult::Failed;
    std::vector<uint8_t> buf(ISO_CHUNK);
    for (const IsoFile &f : img.files) {
        if (pr.cancel) return IsoResult::Cancelled;
        std::string path = dest_path(dest, f.path);
        if (already_there(path, f.size)) {
            pr.bytes += f.size;
            pr.files++;
            pr.skipped++;
            continue;
        }
        OutputFile o;
        if (!o.open(path)) return fail(err, "The file " + path + " cannot be written."), IsoResult::Failed;
        uint64_t done = 0;
        IsoResult r = IsoResult::Ok;
        while (done < f.size) {
            if (pr.cancel) {
                r = IsoResult::Cancelled;
                break;
            }
            size_t n = f.size - done < ISO_CHUNK ? (size_t)(f.size - done) : ISO_CHUNK;
            if (!in.read_at(f.offset + done, buf.data(), n)) {
                fail(err, "The disc image cannot be read at " + f.path + ".");
                r = IsoResult::Failed;
                break;
            }
            if (!o.write(buf.data(), n)) {
                fail(err, "Writing " + path + " failed (is the drive full?).");
                r = IsoResult::Failed;
                break;
            }
            done += n;
            pr.bytes += n;
        }
        if (!o.close() && r == IsoResult::Ok) {
            fail(err, "Writing " + path + " failed (is the drive full?).");
            r = IsoResult::Failed;
        }
        if (r != IsoResult::Ok) {
            remove_file(path); /* a short file would otherwise look like a different file on resume */
            return r;
        }
        pr.files++;
    }
    return IsoResult::Ok;
}

std::string format_bytes(uint64_t bytes) {
    static const char *const units[] = {"bytes", "KB", "MB", "GB", "TB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024 && u < 4) v /= 1024, u++;
    char buf[32];
    if (u == 0) snprintf(buf, sizeof buf, "%llu bytes", (unsigned long long)bytes);
    else snprintf(buf, sizeof buf, v < 10 ? "%.1f %s" : "%.0f %s", v, units[u]);
    return buf;
}

} // namespace launcher
