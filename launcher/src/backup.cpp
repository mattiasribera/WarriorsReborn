#include "backup.h"
#include "platform.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace launcher {

/* Inside a backup folder: the copied save and the description. */
static const char SAVE_SUBDIR[] = "save", INFO_FILE[] = "backup.txt", PARTIAL[] = ".partial";

static uint64_t fnv(uint64_t h, const void *data, size_t n) {
    const unsigned char *b = (const unsigned char *)data;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
static const uint64_t FNV_BASIS = 1469598103934665603ull;

static bool ends_with(const std::string &s, const char *suffix) {
    size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string save_folder(const std::string &hdd_dir) {
    return path_join(path_join(path_join(hdd_dir, "partition1"), "UDATA"), SAVE_TITLE_ID);
}

std::string backup_root(const std::string &data_dir, const std::string &save_dir) {
    char name[32];
    snprintf(name, sizeof name, "save-%016llx", (unsigned long long)fnv(FNV_BASIS, save_dir.data(), save_dir.size()));
    return path_join(path_join(data_dir, "backups"), name);
}

/* Sorted entries, so the signature does not depend on the order the host lists them in. */
static bool sorted_dir(const std::string &path, std::vector<DirEntry> &out) {
    if (!list_dir(path, out)) return false;
    std::sort(out.begin(), out.end(), [](const DirEntry &a, const DirEntry &b) { return a.name < b.name; });
    return true;
}

static void scan(const std::string &dir, const std::string &rel, SaveScan &s) {
    std::vector<DirEntry> list;
    if (!sorted_dir(dir, list)) return;
    for (const DirEntry &e : list) {
        std::string r = rel.empty() ? e.name : rel + "/" + e.name;
        s.signature = fnv(s.signature, r.c_str(), r.size() + 1);
        if (e.dir) {
            scan(path_join(dir, e.name), r, s);
            continue;
        }
        s.files++;
        s.bytes += e.size;
        s.signature = fnv(fnv(s.signature, &e.size, sizeof e.size), &e.mtime, sizeof e.mtime);
    }
}

SaveScan scan_save(const std::string &save_dir) {
    SaveScan s;
    if (!is_dir(save_dir)) return s;
    s.exists = true;
    s.signature = FNV_BASIS;
    scan(save_dir, "", s);
    if (!s.files) s.signature = 0;
    return s;
}

static bool copy_tree(const std::string &from, const std::string &to) {
    std::vector<DirEntry> list;
    if (!make_dir(to) || !list_dir(from, list)) return false;
    for (const DirEntry &e : list) {
        std::string a = path_join(from, e.name), b = path_join(to, e.name);
        if (e.dir ? !copy_tree(a, b) : !copy_file(a, b)) return false;
    }
    return true;
}

/* Only used on the launcher's own folders: backups, unfinished copies, the restore's temporary
 * folders. */
static void remove_tree(const std::string &path) {
    std::vector<DirEntry> list;
    if (!list_dir(path, list)) return;
    for (const DirEntry &e : list)
        if (e.dir) remove_tree(path_join(path, e.name));
        else remove_file(path_join(path, e.name));
    remove_dir(path);
}

static bool read_info(const std::string &dir, BackupInfo &b) {
    std::string text;
    if (!read_file(path_join(dir, INFO_FILE), text, 4096) || !is_dir(path_join(dir, SAVE_SUBDIR))) return false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        size_t eq = line.find(" = ");
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 3);
        if (k == "kind") b.kind = v;
        else if (k == "files") b.files = atoi(v.c_str());
        else if (k == "bytes") b.bytes = strtoull(v.c_str(), nullptr, 10);
        else if (k == "signature") b.signature = strtoull(v.c_str(), nullptr, 16);
    }
    return true;
}

std::vector<BackupInfo> list_backups(const std::string &root) {
    std::vector<BackupInfo> out;
    std::vector<DirEntry> list;
    if (!list_dir(root, list)) return out;
    for (const DirEntry &e : list) {
        BackupInfo b;
        b.name = e.name;
        b.path = path_join(root, e.name);
        if (e.dir && !ends_with(e.name, PARTIAL) && read_info(b.path, b)) out.push_back(b);
    }
    std::sort(out.begin(), out.end(), [](const BackupInfo &a, const BackupInfo &b) { return a.name > b.name; });
    return out;
}

std::string backup_date(const std::string &n) {
    bool stamp = n.size() >= 15 && n[8] == '-';
    for (int i = 0; stamp && i < 15; i++) stamp = i == 8 || (n[i] >= '0' && n[i] <= '9');
    if (!stamp) return n;
    return n.substr(0, 4) + "-" + n.substr(4, 2) + "-" + n.substr(6, 2) + " " + n.substr(9, 2) + ":" + n.substr(11, 2) + ":" + n.substr(13, 2);
}

BackupResult backup_save(const std::string &save_dir, const std::string &root, const std::string &stamp, int keep,
                         const char *kind, bool force, std::string *made, std::string *err) {
    if (made) made->clear();
    SaveScan sc = scan_save(save_dir);
    if (!sc.exists || !sc.files) return BackupResult::NoSave;
    if (!make_dirs(root)) {
        if (err) *err = "cannot create " + root;
        return BackupResult::Failed;
    }
    std::vector<DirEntry> entries;
    list_dir(root, entries);
    for (const DirEntry &e : entries) /* left by a launcher that was closed mid-copy */
        if (e.dir && ends_with(e.name, PARTIAL)) remove_tree(path_join(root, e.name));
    std::vector<BackupInfo> list = list_backups(root);
    if (!force && !list.empty() && list[0].signature == sc.signature) return BackupResult::Unchanged;

    std::string name = stamp;
    for (int n = 2; is_dir(path_join(root, name)); n++) name = stamp + "-" + std::to_string(n);
    std::string tmp = path_join(root, name + PARTIAL), dest = path_join(root, name);
    char info[256];
    snprintf(info, sizeof info, "kind = %s\nfiles = %d\nbytes = %llu\nsignature = %016llx\n", kind, sc.files,
             (unsigned long long)sc.bytes, (unsigned long long)sc.signature);
    if (!make_dir(tmp) || !copy_tree(save_dir, path_join(tmp, SAVE_SUBDIR)) || !write_file(path_join(tmp, INFO_FILE), info) ||
        !rename_path(tmp, dest)) {
        remove_tree(tmp);
        if (err) *err = "cannot copy the save to " + dest;
        return BackupResult::Failed;
    }
    if (made) *made = dest;
    if (keep > 0) {
        list = list_backups(root);
        for (size_t i = (size_t)keep; i < list.size(); i++) remove_tree(list[i].path);
    }
    return BackupResult::Done;
}

bool restore_backup(const std::string &save_dir, const std::string &root, const BackupInfo &b, const std::string &stamp,
                    std::string *undo, std::string *err) {
    if (undo) undo->clear();
    std::string src = path_join(b.path, SAVE_SUBDIR), fresh = save_dir + ".restore-new", old = save_dir + ".restore-old";
    if (!is_dir(src)) {
        if (err) *err = "the backup " + b.path + " is gone";
        return false;
    }
    /* A restore interrupted between the two renames left the save under the old name. */
    if (!is_dir(save_dir) && is_dir(old)) rename_path(old, save_dir);
    std::string before;
    if (backup_save(save_dir, root, stamp, 0, "restore", true, &before, err) == BackupResult::Failed) return false;
    remove_tree(fresh);
    remove_tree(old);
    if (!make_dirs(path_dir(save_dir)) || !copy_tree(src, fresh)) {
        remove_tree(fresh);
        if (err) *err = "cannot copy the backup next to " + save_dir;
        return false;
    }
    bool had = is_dir(save_dir);
    if (had && !rename_path(save_dir, old)) {
        remove_tree(fresh);
        if (err) *err = "cannot move " + save_dir + " (is the game running?)";
        return false;
    }
    if (!rename_path(fresh, save_dir)) {
        if (had) rename_path(old, save_dir);
        remove_tree(fresh);
        if (err) *err = "cannot put the backup in place of " + save_dir;
        return false;
    }
    if (had) remove_tree(old);
    if (undo) *undo = before;
    return true;
}

} // namespace launcher
