/* 64-bit off_t and struct stat on 32-bit Linux: disc images are larger than 2 GB. */
#define _FILE_OFFSET_BITS 64
#include "platform.h"
#include "command.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace launcher {

#ifdef _WIN32
static const char SEP = '\\';

static std::wstring wide(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string narrow(const std::wstring &w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static FILE *open_file(const std::string &path, const wchar_t *mode) { return _wfopen(wide(path).c_str(), mode); }

static std::string last_error_text(const char *what) {
    return std::string(what) + " failed (Windows error " + std::to_string(GetLastError()) + ")";
}

bool is_file(const std::string &path) {
    DWORD a = GetFileAttributesW(wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool is_dir(const std::string &path) {
    DWORD a = GetFileAttributesW(wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool make_dir(const std::string &path) { return CreateDirectoryW(wide(path).c_str(), nullptr) || is_dir(path); }

std::string exe_dir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, sizeof buf / sizeof buf[0]);
    return path_dir(narrow(std::wstring(buf, n)));
}

std::string os_language() {
    switch (GetUserDefaultUILanguage() & 0x3FF) { /* the primary language of the UI LANGID */
    case 0x09: return "en";
    case 0x0A: return "es";
    default: return "";
    }
}

std::string user_cache_dir() {
    const wchar_t *base = _wgetenv(L"LOCALAPPDATA");
    return base && *base ? path_join(narrow(base), "warriors-launcher") : "";
}

std::string timestamp() {
    time_t t = time(nullptr);
    struct tm tm;
    localtime_s(&tm, &t);
    char buf[32];
    strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

bool child_spawn(Child &c, const std::vector<std::string> &argv, const std::string &cwd,
                 const std::string &log_path, std::string *err) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE log = CreateFileW(wide(log_path).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        if (err) *err = last_error_text("opening the log");
        return false;
    }
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = si.hStdError = log;
    PROCESS_INFORMATION pi = {};
    std::string cmd = windows_command_line(argv);
    std::wstring wcmd = wide(cmd), wcwd = wide(cwd);
    BOOL ok = CreateProcessW(wide(argv[0]).c_str(), &wcmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             cwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    if (!ok && err) *err = last_error_text("starting the process");
    CloseHandle(log);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    c.process = pi.hProcess;
    c.running = true;
    c.exit_code = 0;
    return true;
}

bool child_poll(Child &c) {
    if (!c.running) return false;
    if (WaitForSingleObject(c.process, 0) != WAIT_OBJECT_0) return true;
    DWORD code = 0;
    GetExitCodeProcess(c.process, &code);
    CloseHandle(c.process);
    c.process = nullptr;
    c.exit_code = (int)code;
    c.running = false;
    return false;
}

void child_wait(Child &c) {
    if (c.running) WaitForSingleObject(c.process, INFINITE);
    child_poll(c);
}

void child_kill(Child &c) {
    if (!c.running) return;
    TerminateProcess(c.process, 1);
    child_wait(c);
}

bool open_with_desktop(const std::string &path) {
    return (INT_PTR)ShellExecuteW(nullptr, L"open", wide(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
}

bool can_pick_paths() { return true; }

static bool pick(const std::string &title, const std::string &start, bool folder, FileKind kind, std::string &out) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); /* once per thread is enough */
    IFileOpenDialog *dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void **)&dlg)))
        return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    dlg->SetTitle(wide(title).c_str());
    if (!folder) {
        COMDLG_FILTERSPEC programs[] = {{L"Programs (*.exe)", L"*.exe"}, {L"All files", L"*.*"}};
        COMDLG_FILTERSPEC images[] = {{L"Disc images (*.iso)", L"*.iso"}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, kind == FileKind::DiscImage ? images : programs);
    }
    IShellItem *item = nullptr;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(wide(start).c_str(), nullptr, IID_IShellItem, (void **)&item))) {
        dlg->SetFolder(item);
        item->Release();
    }
    bool ok = false;
    if (SUCCEEDED(dlg->Show(GetActiveWindow())) && SUCCEEDED(dlg->GetResult(&item))) {
        wchar_t *path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            out = narrow(path);
            CoTaskMemFree(path);
            ok = true;
        }
        item->Release();
    }
    dlg->Release();
    return ok;
}

bool remove_file(const std::string &path) { return DeleteFileW(wide(path).c_str()) != 0; }

std::string user_data_dir() { return user_cache_dir(); }

std::string os_description() {
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
    OSVERSIONINFOW v = {};
    v.dwOSVersionInfoSize = sizeof v;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn get = nt ? (RtlGetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion") : nullptr;
    if (!get || get(&v) != 0) return "Windows";
    char buf[64];
    snprintf(buf, sizeof buf, "Windows %lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    return buf;
}

static std::string env(const wchar_t *name) {
    const wchar_t *v = _wgetenv(name);
    return v ? narrow(v) : "";
}
std::string home_dir() { return env(L"USERPROFILE"); }
std::string user_name() { return env(L"USERNAME"); }
std::string host_name() { return env(L"COMPUTERNAME"); } /* %LOCALAPPDATA%\warriors-launcher */

bool list_dir(const std::string &path, std::vector<DirEntry> &out) {
    out.clear();
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(wide(path_join(path, "*")).c_str(), &d);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        std::string name = narrow(d.cFileName);
        if (name == "." || name == ".." || (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        DirEntry e;
        e.name = name;
        e.dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size = e.dir ? 0 : (uint64_t)d.nFileSizeHigh << 32 | d.nFileSizeLow;
        e.mtime = (int64_t)((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32 | d.ftLastWriteTime.dwLowDateTime);
        out.push_back(e);
    } while (FindNextFileW(h, &d));
    FindClose(h);
    return true;
}

/* CopyFileW keeps the modification time. */
bool copy_file(const std::string &from, const std::string &to) {
    return CopyFileW(wide(from).c_str(), wide(to).c_str(), FALSE) != 0;
}

bool rename_path(const std::string &from, const std::string &to) { return MoveFileW(wide(from).c_str(), wide(to).c_str()) != 0; }

bool remove_dir(const std::string &path) { return RemoveDirectoryW(wide(path).c_str()) != 0; }

bool file_size(const std::string &path, uint64_t &out) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(wide(path).c_str(), GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    out = (uint64_t)d.nFileSizeHigh << 32 | d.nFileSizeLow;
    return true;
}

bool free_space(const std::string &path, uint64_t &out) {
    ULARGE_INTEGER avail;
    if (!GetDiskFreeSpaceExW(wide(path).c_str(), &avail, nullptr, nullptr)) return false;
    out = avail.QuadPart;
    return true;
}

bool InputFile::open(const std::string &path) {
    close();
    HANDLE h = CreateFileW(wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) {
        CloseHandle(h);
        return false;
    }
    handle_ = h;
    size_ = (uint64_t)sz.QuadPart;
    return true;
}

bool InputFile::read_at(uint64_t offset, void *buf, size_t n) {
    if (!handle_ || offset > size_ || n > size_ - offset) return false;
    char *p = (char *)buf;
    while (n) {
        OVERLAPPED ov = {};
        ov.Offset = (DWORD)offset;
        ov.OffsetHigh = (DWORD)(offset >> 32);
        DWORD chunk = n > (1u << 30) ? (1u << 30) : (DWORD)n, got = 0;
        if (!ReadFile((HANDLE)handle_, p, chunk, &got, &ov) || got == 0) return false;
        p += got, offset += got, n -= got;
    }
    return true;
}

void InputFile::close() {
    if (handle_) CloseHandle((HANDLE)handle_);
    handle_ = nullptr;
    size_ = 0;
}

bool OutputFile::open(const std::string &path) {
    close();
    HANDLE h = CreateFileW(wide(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    handle_ = h;
    ok_ = true;
    return true;
}

bool OutputFile::write(const void *buf, size_t n) {
    const char *p = (const char *)buf;
    while (ok_ && n) {
        DWORD got = 0;
        if (!WriteFile((HANDLE)handle_, p, (DWORD)n, &got, nullptr) || got == 0) ok_ = false;
        p += got, n -= got;
    }
    return ok_;
}

bool OutputFile::close() {
    if (!handle_) return ok_;
    if (!CloseHandle((HANDLE)handle_)) ok_ = false;
    handle_ = nullptr;
    return ok_;
}

struct ThreadStart {
    void (*fn)(void *);
    void *arg;
};

static DWORD WINAPI thread_main(LPVOID p) {
    ThreadStart s = *(ThreadStart *)p;
    delete (ThreadStart *)p;
    s.fn(s.arg);
    return 0;
}

bool thread_start(Thread &t, void (*fn)(void *), void *arg) {
    ThreadStart *s = new ThreadStart{fn, arg};
    HANDLE h = CreateThread(nullptr, 0, thread_main, s, 0, nullptr);
    if (!h) {
        delete s;
        return false;
    }
    t.handle = h;
    return true;
}

void thread_join(Thread &t) {
    if (!t.handle) return;
    WaitForSingleObject((HANDLE)t.handle, INFINITE);
    CloseHandle((HANDLE)t.handle);
    t.handle = nullptr;
}

#else
static const char SEP = '/';

static FILE *open_file(const std::string &path, const wchar_t *mode) {
    return fopen(path.c_str(), mode[0] == L'r' ? "rb" : "wb");
}

bool is_file(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool is_dir(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool make_dir(const std::string &path) { return mkdir(path.c_str(), 0755) == 0 || is_dir(path); }

std::string exe_dir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? path_dir(std::string(buf, n)) : std::string(".");
}

std::string os_language() {
    for (const char *var : {"LC_ALL", "LC_MESSAGES", "LANG"}) { /* the first one set decides */
        const char *v = getenv(var);
        if (!v || !*v) continue;
        std::string l(v, strcspn(v, "_.@"));
        return l == "C" || l == "POSIX" ? "en" : l;
    }
    return "";
}

std::string user_cache_dir() {
    const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (xdg && *xdg == '/') return path_join(xdg, "warriors-launcher");
    return home && *home ? path_join(path_join(home, ".cache"), "warriors-launcher") : "";
}

std::string timestamp() {
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

/* The log is opened before the chdir, so a relative log path is the launcher's. */
bool child_spawn(Child &c, const std::vector<std::string> &argv, const std::string &cwd,
                 const std::string &log_path, std::string *err) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());
    std::vector<char *> args;
    for (const std::string &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc) {
        if (err) *err = std::string("starting ") + argv[0] + ": " + strerror(rc);
        return false;
    }
    c.pid = pid;
    c.running = true;
    c.exit_code = 0;
    return true;
}

static void set_exit(Child &c, int status) {
    c.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    c.running = false;
}

bool child_poll(Child &c) {
    int status = 0;
    if (c.running && waitpid(c.pid, &status, WNOHANG) == c.pid) set_exit(c, status);
    return c.running;
}

void child_wait(Child &c) {
    int status = 0;
    if (c.running && waitpid(c.pid, &status, 0) == c.pid) set_exit(c, status);
}

void child_kill(Child &c) {
    if (!c.running) return;
    kill(c.pid, SIGKILL);
    child_wait(c);
}

bool open_with_desktop(const std::string &path) {
    static std::vector<pid_t> openers; /* reaped on the next call */
    for (size_t i = 0; i < openers.size();)
        if (waitpid(openers[i], nullptr, WNOHANG) != 0) openers.erase(openers.begin() + i);
        else i++;
    char *args[] = {const_cast<char *>("xdg-open"), const_cast<char *>(path.c_str()), nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, args, environ)) return false;
    openers.push_back(pid);
    return true;
}


bool remove_file(const std::string &path) { return ::remove(path.c_str()) == 0; }

std::string user_data_dir() {
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg == '/') return path_join(xdg, "warriors-launcher");
    return home && *home ? path_join(path_join(path_join(home, ".local"), "share"), "warriors-launcher") : "";
}

/* lstat: symbolic links are left out (never followed out of the folder). */
bool list_dir(const std::string &path, std::vector<DirEntry> &out) {
    out.clear();
    DIR *d = opendir(path.c_str());
    if (!d) return false;
    while (struct dirent *de = readdir(d)) {
        std::string name = de->d_name;
        struct stat st;
        if (name == "." || name == ".." || lstat(path_join(path, name).c_str(), &st) != 0) continue;
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) continue;
        DirEntry e;
        e.name = name;
        e.dir = S_ISDIR(st.st_mode);
        e.size = e.dir ? 0 : (uint64_t)st.st_size;
        e.mtime = (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
        out.push_back(e);
    }
    closedir(d);
    return true;
}

/* Copies the bytes, then the modification time. */
bool copy_file(const std::string &from, const std::string &to) {
    struct stat st;
    if (stat(from.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    FILE *in = fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE *out = fopen(to.c_str(), "wb");
    if (!out) return fclose(in), false;
    char buf[65536];
    bool ok = true;
    for (size_t n; (n = fread(buf, 1, sizeof buf, in)) > 0;)
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    ok = ok && !ferror(in);
    fclose(in);
    ok = fclose(out) == 0 && ok;
    struct timespec t[2] = {st.st_atim, st.st_mtim};
    return ok && utimensat(AT_FDCWD, to.c_str(), t, 0) == 0;
}

bool rename_path(const std::string &from, const std::string &to) { return ::rename(from.c_str(), to.c_str()) == 0; }

std::string os_description() {
    struct utsname u;
    std::string kernel = uname(&u) == 0 ? std::string(u.sysname) + " " + u.release + ", " + u.machine : "Linux";
    std::string rel, name;
    if (read_file("/etc/os-release", rel, 16384)) {
        size_t p = rel.find("PRETTY_NAME=");
        if (p != std::string::npos && (p == 0 || rel[p - 1] == '\n')) {
            name = rel.substr(p + 12, rel.find('\n', p) == std::string::npos ? std::string::npos : rel.find('\n', p) - p - 12);
            if (name.size() >= 2 && name.front() == '"' && name.back() == '"') name = name.substr(1, name.size() - 2);
        }
    }
    return name.empty() ? kernel : name + " (" + kernel + ")";
}

std::string home_dir() {
    const char *h = getenv("HOME");
    return h ? h : "";
}

std::string user_name() {
    const char *u = getenv("USER");
    if (u && *u) return u;
    struct passwd *pw = getpwuid(getuid());
    return pw && pw->pw_name ? pw->pw_name : "";
}

std::string host_name() {
    char buf[256] = "";
    if (gethostname(buf, sizeof buf - 1) != 0) return "";
    return buf;
}

bool remove_dir(const std::string &path) { return rmdir(path.c_str()) == 0; }

bool file_size(const std::string &path, uint64_t &out) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    out = (uint64_t)st.st_size;
    return true;
}

bool free_space(const std::string &path, uint64_t &out) {
    struct statvfs v;
    if (statvfs(path.c_str(), &v) != 0) return false;
    out = (uint64_t)v.f_bavail * v.f_frsize;
    return true;
}

bool InputFile::open(const std::string &path) {
    close();
    struct stat st;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(f);
        return false;
    }
    handle_ = f;
    size_ = (uint64_t)st.st_size;
    return true;
}

bool InputFile::read_at(uint64_t offset, void *buf, size_t n) {
    if (!handle_ || offset > size_ || n > size_ - offset) return false;
    FILE *f = (FILE *)handle_;
    return fseeko(f, (off_t)offset, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

void InputFile::close() {
    if (handle_) fclose((FILE *)handle_);
    handle_ = nullptr;
    size_ = 0;
}

bool OutputFile::open(const std::string &path) {
    close();
    handle_ = fopen(path.c_str(), "wb");
    ok_ = handle_ != nullptr;
    return ok_;
}

bool OutputFile::write(const void *buf, size_t n) {
    if (ok_ && fwrite(buf, 1, n, (FILE *)handle_) != n) ok_ = false;
    return ok_;
}

bool OutputFile::close() {
    if (!handle_) return ok_;
    if (fclose((FILE *)handle_) != 0) ok_ = false;
    handle_ = nullptr;
    return ok_;
}

struct ThreadStart {
    void (*fn)(void *);
    void *arg;
};

static void *thread_main(void *p) {
    ThreadStart s = *(ThreadStart *)p;
    delete (ThreadStart *)p;
    s.fn(s.arg);
    return nullptr;
}

bool thread_start(Thread &t, void (*fn)(void *), void *arg) {
    ThreadStart *s = new ThreadStart{fn, arg};
    pthread_t th;
    if (pthread_create(&th, nullptr, thread_main, s) != 0) {
        delete s;
        return false;
    }
    t.handle = new pthread_t(th);
    return true;
}

void thread_join(Thread &t) {
    if (!t.handle) return;
    pthread_join(*(pthread_t *)t.handle, nullptr);
    delete (pthread_t *)t.handle;
    t.handle = nullptr;
}

static const char ZENITY[] = "/usr/bin/zenity";

bool can_pick_paths() { return is_file(ZENITY); }

static bool pick(const std::string &title, const std::string &start, bool folder, FileKind kind, std::string &out) {
    if (!can_pick_paths()) return false;
    std::string cmd = std::string(ZENITY) + " --file-selection --title=" + quote_posix(title) +
                      (folder ? " --directory" : "") +
                      (!folder && kind == FileKind::DiscImage ? " --file-filter='Disc images (*.iso) | *.iso *.ISO'"
                                                                  " --file-filter='All files | *'" : "") +
                      (start.empty() ? "" : " --filename=" + quote_posix(path_join(start, ""))) + " 2>/dev/null";
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) return false;
    std::string text;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) text.append(buf, n);
    if (pclose(p) != 0) return false;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (text.empty()) return false;
    out = text;
    return true;
}
#endif

bool pick_folder(const std::string &title, const std::string &start, std::string &out) {
    return pick(title, start, true, FileKind::Program, out);
}

bool pick_file(const std::string &title, const std::string &start, std::string &out, FileKind kind) {
    return pick(title, start, false, kind, out);
}

bool read_file(const std::string &path, std::string &out, size_t max_bytes) {
    FILE *f = open_file(path, L"rb");
    if (!f) return false;
    out.resize(max_bytes);
    out.resize(fread(&out[0], 1, max_bytes, f));
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool write_file(const std::string &path, const std::string &data) {
    FILE *f = open_file(path, L"wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}

bool make_dirs(const std::string &path) {
    if (path.empty() || is_dir(path)) return !path.empty();
    std::string parent = path_dir(path);
    if (!parent.empty() && parent != path && !(parent.size() == 2 && parent[1] == ':')) make_dirs(parent);
    return make_dir(path);
}

std::string path_join(const std::string &dir, const std::string &name) {
    if (dir.empty()) return name;
    char last = dir.back();
    return last == '/' || last == '\\' ? dir + name : dir + SEP + name;
}

std::string path_dir(const std::string &path) {
    size_t s = path.find_last_of("/\\");
    if (s == std::string::npos) return "";
    return s == 0 ? path.substr(0, 1) : path.substr(0, s);
}

} // namespace launcher
