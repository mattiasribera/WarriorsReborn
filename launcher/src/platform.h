/* Host layer of the launcher: files, the launcher's own folder, and child processes with their
 * stdout and stderr in a log file. All paths are UTF-8 (converted to UTF-16 on Windows). */
#ifndef LAUNCHER_PLATFORM_H
#define LAUNCHER_PLATFORM_H
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace launcher {

bool is_file(const std::string &path);
bool is_dir(const std::string &path);
bool make_dir(const std::string &path); /* true when it exists afterwards */
bool read_file(const std::string &path, std::string &out, size_t max_bytes);
bool write_file(const std::string &path, const std::string &data);

bool make_dirs(const std::string &path);  /* make_dir for each missing parent too */
bool remove_file(const std::string &path);
bool remove_dir(const std::string &path);   /* an empty folder */
bool rename_path(const std::string &from, const std::string &to); /* fails when to exists (Windows) */
/* A copy of a regular file with its modification time (an existing to is replaced). */
bool copy_file(const std::string &from, const std::string &to);

/* One entry of a folder: regular files and folders only (symbolic links and reparse points are
 * left out, so a walk never leaves the folder). mtime is in the host's units (comparable only on
 * the same host). */
struct DirEntry {
    std::string name;
    bool dir = false;
    uint64_t size = 0;
    int64_t mtime = 0;
};
bool list_dir(const std::string &path, std::vector<DirEntry> &out); /* false when it cannot be read */
/* 64-bit sizes: disc images are larger than 4 GB, and the launcher is a 32-bit program. */
bool file_size(const std::string &path, uint64_t &out);
bool free_space(const std::string &path, uint64_t &out); /* bytes free to this user on path's drive */

/* A file opened read-only, read at absolute offsets (never written). */
class InputFile {
public:
    InputFile() = default;
    InputFile(const InputFile &) = delete;
    InputFile &operator=(const InputFile &) = delete;
    ~InputFile() { close(); }
    bool open(const std::string &path);
    bool read_at(uint64_t offset, void *buf, size_t n); /* false unless all n bytes were read */
    uint64_t size() const { return size_; }
    void close();
private:
    void *handle_ = nullptr; /* HANDLE on Windows, FILE* elsewhere */
    uint64_t size_ = 0;
};

/* A new file (an existing one is truncated), written sequentially. */
class OutputFile {
public:
    OutputFile() = default;
    OutputFile(const OutputFile &) = delete;
    OutputFile &operator=(const OutputFile &) = delete;
    ~OutputFile() { close(); }
    bool open(const std::string &path);
    bool write(const void *buf, size_t n);
    bool close(); /* true when every write and the close succeeded */
private:
    void *handle_ = nullptr;
    bool ok_ = true;
};

/* A worker thread (pthreads / CreateThread). */
struct Thread {
    void *handle = nullptr;
};
bool thread_start(Thread &t, void (*fn)(void *), void *arg);
void thread_join(Thread &t);
void thread_yield(); /* lets another thread run (a busy wait in a test) */

std::string exe_dir();                  /* folder of the running launcher */
/* The language of the user's desktop as a two-letter code ("es" for any Spanish), empty when
 * unknown: GetUserDefaultUILanguage on Windows, LC_ALL / LC_MESSAGES / LANG elsewhere. */
std::string os_language();
/* The per-user cache folder (not created): $XDG_CACHE_HOME or ~/.cache, %LOCALAPPDATA% on
 * Windows, + warriors-launcher. Empty when the environment has neither. */
std::string user_cache_dir();
/* The per-user data folder (not created): $XDG_DATA_HOME or ~/.local/share, %LOCALAPPDATA% on
 * Windows (the cache folder's), + warriors-launcher. Empty when the environment has neither. */
std::string user_data_dir();
/* For the bug report: "Windows 10.0.22631" / "Ubuntu 24.04.1 LTS (Linux 6.8.0, x86_64)"; the
 * user's home folder (%USERPROFILE% / $HOME), login name and machine name ("" when unknown). */
std::string os_description();
std::string home_dir();
std::string user_name();
std::string host_name();
std::string path_join(const std::string &dir, const std::string &name);
std::string path_dir(const std::string &path); /* "" when path has no folder part */
std::string timestamp();                /* local time, YYYYmmdd-HHMMSS */

struct Child {
    bool running = false;
    int exit_code = 0;
#ifdef _WIN32
    void *process = nullptr;
#else
    int pid = 0;
#endif
};

/* Starts argv (argv[0] = program path, not searched in PATH) in cwd (empty = inherited) with
 * stdout and stderr appended to log_path. */
bool child_spawn(Child &c, const std::vector<std::string> &argv, const std::string &cwd,
                 const std::string &log_path, std::string *err);
/* Updates c.running / c.exit_code without blocking; returns c.running. */
bool child_poll(Child &c);
void child_wait(Child &c);
/* Stops a running child at once (TerminateProcess / SIGKILL) and reaps it; nothing when it is not
 * running. */
void child_kill(Child &c);

/* Opens a file with the desktop's default application (the log viewer). */
bool open_with_desktop(const std::string &path);

/* The system's folder / file chooser (Windows: the common item dialog; POSIX: zenity, when
 * installed). Blocks until it closes; false when cancelled or unavailable. start is the folder
 * it opens in (may be empty). */
bool can_pick_paths();
bool pick_folder(const std::string &title, const std::string &start, std::string &out);
enum class FileKind { Program, DiscImage }; /* the chooser's filter: *.exe or *.iso */
bool pick_file(const std::string &title, const std::string &start, std::string &out, FileKind kind = FileKind::Program);

} // namespace launcher
#endif
