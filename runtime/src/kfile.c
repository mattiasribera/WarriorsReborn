/* I/O manager exports: files on the extracted disc / host HDD directory, symbolic links. */
#define _GNU_SOURCE
#include "files.h"
#include "hle_textfix.h"
#include "kernel.h"
#include "kobj.h"
#include "platform.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define U16(p, o) (*(uint16_t *)((uintptr_t)(p) + (o)))
#define U32(p, o) (*(uint32_t *)((uintptr_t)(p) + (o)))
#define U64(p, o) (*(uint64_t *)((uintptr_t)(p) + (o)))
#define FILE_ATTRIBUTE_READONLY 0x01
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80

/* Directory handle state (HObj.dir). The Xbox path comes first so relative opens can use it
 * as a string; the rest is the NtQueryDirectoryFile scan: mask and next entry index. */
typedef struct {
    char path[600];
    char mask[256];
    int scanning;      /* a scan was started (mask captured) */
    uint32_t next;     /* index of the next matching entry */
    int queried;       /* a call of this scan already returned (entry or end) */
} DirState;

/* ANSI_STRING { USHORT Length; USHORT MaximumLength; PCHAR Buffer; } */
static void ansi(uint32_t s, char *out, size_t len) {
    uint32_t n = U16(s, 0);
    if (n >= len) n = len - 1;
    memcpy(out, G(U32(s, 4)), n);
    out[n] = 0;
}

/* OBJECT_ATTRIBUTES { RootDirectory; ObjectName; Attributes } -> full Xbox path. */
static int object_path(uint32_t oa, char *out, size_t len) {
    char name[512];
    ansi(U32(oa, 4), name, sizeof name);
    uint32_t root = U32(oa, 0);
    if (root == 0xFFFFFFFDu) {   /* ObDosDevicesDirectory(): XAPI passes "D:\..." relative to \?? */
        snprintf(out, len, "\\??\\%s", name);
    } else if (root) {
        HObj *r = handle_get(root);
        if (!r || r->type != H_FILE || !r->dir) {
            klog("[file] '%s' relative to handle %x, which is not an open directory\n", name, root);
            return -1;
        }
        snprintf(out, len, "%s\\%s", ((DirState *)r->dir)->path, name);
    } else {
        snprintf(out, len, "%s", name);
    }
    return 0;
}

static void io_status(uint32_t iosb, uint32_t status, uint32_t info) {
    if (iosb) { U32(iosb, 0) = status; U32(iosb, 4) = info; }
}

static uint64_t filetime(time_t t) { return (uint64_t)t * 10000000ull + 116444736000000000ull; }

static uint32_t errno_status(void) {
    switch (errno) {
    case ENOENT: return STATUS_OBJECT_NAME_NOT_FOUND;
    case ENOTDIR: return STATUS_OBJECT_PATH_NOT_FOUND;
    case EEXIST: return STATUS_OBJECT_NAME_COLLISION;
    case EACCES: case EROFS: case EPERM: return STATUS_ACCESS_DENIED;
    case EISDIR: return STATUS_FILE_IS_A_DIRECTORY;
    default: return STATUS_UNSUCCESSFUL;
    }
}

/* NtCreateFile(FileHandle*, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize*,
 *   FileAttributes, ShareAccess, CreateDisposition, CreateOptions)
 * NtOpenFile(FileHandle*, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions) */
static uint64_t k_NtCreateFile(KFrame *f, const uint32_t *a) {
    int is_open = f->ordinal == 202;
    uint32_t disposition = is_open ? 1 : a[7], options = is_open ? a[5] : a[8];
    char xp[600], host[1400];
    if (object_path(a[2], xp, sizeof xp)) { io_status(a[3], STATUS_INVALID_HANDLE, 0); return STATUS_INVALID_HANDLE; }
    int ro;
    if (files_resolve(xp, host, sizeof host, &ro)) {
        klog("[file] unmapped path '%s'\n", xp);
        io_status(a[3], STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    struct stat st;
    int exists = stat(host, &st) == 0;
    int is_dir = exists ? S_ISDIR(st.st_mode) : (options & 1);
    uint32_t info = exists ? 1 /* FILE_OPENED */ : 2 /* FILE_CREATED */;
    if (!exists && (disposition == 1 || disposition == 4)) {
        klog("[file] not found '%s' -> %s\n", xp, host);
        io_status(a[3], STATUS_OBJECT_NAME_NOT_FOUND, 0);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (exists && disposition == 2) { io_status(a[3], STATUS_OBJECT_NAME_COLLISION, 0); return STATUS_OBJECT_NAME_COLLISION; }
    if (exists && is_dir && (options & 0x40)) { io_status(a[3], STATUS_FILE_IS_A_DIRECTORY, 0); return STATUS_FILE_IS_A_DIRECTORY; }
    if (exists && !is_dir && (options & 1)) { io_status(a[3], STATUS_NOT_A_DIRECTORY, 0); return STATUS_NOT_A_DIRECTORY; }
    int fd;
    if (is_dir) {
        if (!exists && (ro || plat_mkdir(host))) { uint32_t s = errno_status(); io_status(a[3], s, 0); return s; }
        fd = plat_open_dir(host);
    } else {
        int flags = ro ? O_RDONLY : O_RDWR;
        if (!ro && disposition != 1) flags |= O_CREAT;
        if (!ro && (disposition == 0 || disposition == 4 || disposition == 5)) { flags |= O_TRUNC; if (exists) info = 3; }
        if (ro && disposition != 1 && disposition != 3) { io_status(a[3], STATUS_ACCESS_DENIED, 0); return STATUS_ACCESS_DENIED; }
        fd = open(host, flags | PLAT_O_BINARY, 0644);
    }
    if (fd < 0) { uint32_t s = errno_status(); io_status(a[3], s, 0); return s; }
    uint32_t h = handle_new(H_FILE, 0);
    HObj *o = handle_get(h);
    o->fd = fd;
    o->path = strdup(host);
    if (is_dir) {   /* Xbox path for relative opens, directory scan state */
        DirState *d = calloc(1, sizeof *d);
        snprintf(d->path, sizeof d->path, "%s", xp);
        o->dir = d;
    }
    U32(a[0], 0) = h;
    io_status(a[3], STATUS_SUCCESS, info);
    if (klog_enabled) klog("[file] open '%s' -> %s (h=%x)\n", xp, host, h);
    return STATUS_SUCCESS;
}

static void complete_io(KFrame *f, uint32_t event, uint32_t apc) {
    if (apc) kfatal(f, "asynchronous I/O with an APC completion routine is not supported");
    if (event) {
        HObj *e = handle_get(event);
        if (e && e->body) kobj_set_signal(e->body, 1);
    }
}

/* Text fixes (hle_textfix.h) read the bytes of a match outside the guest's read with this. */
static long textfix_read_at(void *ctx, uint64_t off, void *dst, size_t len) {
    return (long)plat_pread(*(int *)ctx, dst, len, off);
}

/* NtReadFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset*) */
static uint64_t k_NtReadFile(KFrame *f, const uint32_t *a) {
    HObj *o = handle_get(a[0]);
    if (!o || o->type != H_FILE) return STATUS_INVALID_HANDLE;
    uint64_t at = a[7] ? U64(a[7], 0) : textfix_active ? (uint64_t)lseek(o->fd, 0, SEEK_CUR) : 0;
    ssize_t n = a[7] ? plat_pread(o->fd, G(a[5]), a[6], at) : read(o->fd, G(a[5]), a[6]);
    if (textfix_active && n > 0 && o->path) {
        textfix_apply(o->path, at, G(a[5]), (size_t)n, textfix_read_at, &o->fd);
        if (!a[7]) lseek(o->fd, (off_t)(at + (uint64_t)n), SEEK_SET);   /* a Win32 pread moves the position */
    }
    if (a[7] && n >= 0) lseek(o->fd, (off_t)at + n, SEEK_SET);
    uint32_t st = n < 0 ? errno_status() : (n == 0 && a[6] ? STATUS_END_OF_FILE : STATUS_SUCCESS);
    io_status(a[4], st, n < 0 ? 0 : (uint32_t)n);
    complete_io(f, a[1], a[2]);
    return st;
}
static uint64_t k_NtWriteFile(KFrame *f, const uint32_t *a) {
    HObj *o = handle_get(a[0]);
    if (!o || o->type != H_FILE) return STATUS_INVALID_HANDLE;
    ssize_t n = a[7] ? plat_pwrite(o->fd, G(a[5]), a[6], U64(a[7], 0)) : write(o->fd, G(a[5]), a[6]);
    if (a[7] && n >= 0) lseek(o->fd, (off_t)U64(a[7], 0) + n, SEEK_SET);
    uint32_t st = n < 0 ? errno_status() : STATUS_SUCCESS;
    io_status(a[4], st, n < 0 ? 0 : (uint32_t)n);
    complete_io(f, a[1], a[2]);
    return st;
}
static uint64_t k_NtFlushBuffersFile(KFrame *f, const uint32_t *a) { io_status(a[1], 0, 0); return 0; }

/* Directory handles have no real fd on Windows (plat_open_dir), so they stat the host path. */
static int obj_stat(const HObj *o, struct stat *st) { return o->dir ? stat(o->path, st) : fstat(o->fd, st); }

static void fill_network_info(uint32_t p, const struct stat *st) {
    U64(p, 0) = filetime(st->st_ctime);
    U64(p, 8) = filetime(st->st_atime);
    U64(p, 16) = filetime(st->st_mtime);
    U64(p, 24) = filetime(st->st_mtime);
    U64(p, 32) = ((uint64_t)st->st_size + 0xFFF) & ~0xFFFull;
    U64(p, 40) = (uint64_t)st->st_size;
    U32(p, 48) = S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

/* NtQueryInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass) */
static uint64_t k_NtQueryInformationFile(KFrame *f, const uint32_t *a) {
    HObj *o = handle_get(a[0]);
    if (!o || o->type != H_FILE) return STATUS_INVALID_HANDLE;
    struct stat st;
    obj_stat(o, &st);
    uint32_t p = a[2], len;
    switch (a[4]) {
    case 4:   /* FileBasicInformation */
        U64(p, 0) = filetime(st.st_ctime); U64(p, 8) = filetime(st.st_atime);
        U64(p, 16) = filetime(st.st_mtime); U64(p, 24) = filetime(st.st_mtime);
        U32(p, 32) = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        len = 36; break;
    case 5:   /* FileStandardInformation */
        U64(p, 0) = ((uint64_t)st.st_size + 0xFFF) & ~0xFFFull; U64(p, 8) = st.st_size;
        U32(p, 16) = 1; *(uint8_t *)G(p + 20) = 0; *(uint8_t *)G(p + 21) = S_ISDIR(st.st_mode);
        len = 24; break;
    case 14:  /* FilePositionInformation */
        U64(p, 0) = (uint64_t)lseek(o->fd, 0, SEEK_CUR); len = 8; break;
    case 34:  /* FileNetworkOpenInformation */
        fill_network_info(p, &st); len = 56; break;
    default:
        kfatal(f, "NtQueryInformationFile class %u not implemented", a[4]);
    }
    io_status(a[1], STATUS_SUCCESS, len);
    return STATUS_SUCCESS;
}

/* NtSetInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass) */
static uint64_t k_NtSetInformationFile(KFrame *f, const uint32_t *a) {
    HObj *o = handle_get(a[0]);
    if (!o || o->type != H_FILE) return STATUS_INVALID_HANDLE;
    uint32_t st = STATUS_SUCCESS;
    switch (a[4]) {
    case 4: break;   /* FileBasicInformation: timestamps ignored */
    case 14: lseek(o->fd, (off_t)U64(a[2], 0), SEEK_SET); break;
    case 19: case 20: if (ftruncate(o->fd, (off_t)U64(a[2], 0))) st = errno_status(); break;
    case 13: if (*(uint8_t *)G(a[2]) && unlink(o->path) && rmdir(o->path)) st = errno_status(); break;
    default: kfatal(f, "NtSetInformationFile class %u not implemented", a[4]);
    }
    io_status(a[1], st, 0);
    return st;
}

/* NtQueryFullAttributesFile(ObjectAttributes, FILE_NETWORK_OPEN_INFORMATION*) */
static uint64_t k_NtQueryFullAttributesFile(KFrame *f, const uint32_t *a) {
    char xp[600], host[1400];
    int ro;
    struct stat st;
    if (object_path(a[0], xp, sizeof xp) || files_resolve(xp, host, sizeof host, &ro)) return STATUS_OBJECT_PATH_NOT_FOUND;
    if (stat(host, &st)) return STATUS_OBJECT_NAME_NOT_FOUND;
    fill_network_info(a[1], &st);
    return STATUS_SUCCESS;
}

/* NtQueryVolumeInformationFile(FileHandle, IoStatusBlock, FsInformation, Length, FsInformationClass) */
static uint64_t k_NtQueryVolumeInformationFile(KFrame *f, const uint32_t *a) {
    HObj *o = handle_get(a[0]);
    if (!o || o->type != H_FILE) return STATUS_INVALID_HANDLE;
    uint64_t total = 0, avail = 0;
    plat_disk_space(o->path, &total, &avail);
    total /= 16384;
    avail /= 16384;
    if (total > 0x7FFFFFFF / 16) total = 0x7FFFFFFF / 16;   /* report a plausible Xbox-sized volume */
    if (avail > total) avail = total;
    /* Cluster size: from the FATX superblock XAPI wrote when formatting the raw partition
     * image ("FATX", VolumeID, SectorsPerCluster), else 16 KB. */
    uint32_t spc = 32;
    char img[1500];
    snprintf(img, sizeof img, "%s.img", o->path);
    FILE *fi = fopen(img, "rb");
    if (fi) {
        uint8_t sb[12];
        if (fread(sb, 1, 12, fi) == 12 && !memcmp(sb, "FATX", 4)) memcpy(&spc, sb + 8, 4);
        fclose(fi);
    }
    total = total * 32 / spc;
    avail = avail * 32 / spc;
    uint32_t p = a[2], len;
    switch (a[4]) {
    case 3:   /* FileFsSizeInformation */
        U64(p, 0) = total; U64(p, 8) = avail; U32(p, 16) = spc; U32(p, 20) = 512; len = 24; break;
    case 7:   /* FileFsFullSizeInformation */
        U64(p, 0) = total; U64(p, 8) = avail; U64(p, 16) = avail; U32(p, 24) = spc; U32(p, 28) = 512; len = 32; break;
    default:
        kfatal(f, "NtQueryVolumeInformationFile class %u not implemented", a[4]);
    }
    io_status(a[1], STATUS_SUCCESS, len);
    return STATUS_SUCCESS;
}

/* Case-insensitive DOS-style wildcard match: '*' any run, '?' one character. */
static int mask_match(const char *m, const char *n) {
    if (!*m) return !*n;
    if (*m == '*') {
        for (;; n++) {
            if (mask_match(m + 1, n)) return 1;
            if (!*n) return 0;
        }
    }
    if (!*n) return 0;
    if (*m != '?' && tolower((unsigned char)*m) != tolower((unsigned char)*n)) return 0;
    return mask_match(m + 1, n + 1);
}

/* NtQueryDirectoryFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
 *   FileInformation, Length, FileInformationClass, FileName (POBJECT_STRING, ANSI), RestartScan)
 * Xbox signature and FILE_DIRECTORY_INFORMATION layout from nxdk lib/xboxkrnl/xboxkrnl.h: unlike
 * NT there is no ReturnSingleEntry argument and FileName is an ANSI OBJECT_STRING. Layout:
 * NextEntryOffset 0, FileIndex 4, Creation/LastAccess/LastWrite/ChangeTime 8..32, EndOfFile 40,
 * AllocationSize 48, FileAttributes 56, FileNameLength 60, FileName (ANSI, no NUL) 64.
 * One entry is returned per call (XAPI FindFirstFile/FindNextFile pass a one-entry buffer).
 * The mask is captured on the first call or on RestartScan; an empty or absent mask, or
 * "*.*", matches everything. "." and ".." are not reported (FATX has no such entries). */
static uint64_t k_NtQueryDirectoryFile(KFrame *f, const uint32_t *a) {
    HObj *o = handle_get(a[0]);
    if (!o || o->type != H_FILE) return STATUS_INVALID_HANDLE;
    if (!o->dir) { io_status(a[4], STATUS_INVALID_PARAMETER, 0); return STATUS_INVALID_PARAMETER; }
    if (a[7] != 1) kfatal(f, "NtQueryDirectoryFile class %u not implemented", a[7]);
    uint32_t p = a[5], len = a[6];
    if (len < 64) { io_status(a[4], STATUS_INFO_LENGTH_MISMATCH, 0); return STATUS_INFO_LENGTH_MISMATCH; }
    DirState *d = o->dir;
    if (!d->scanning || (a[9] & 0xFF)) {
        d->mask[0] = 0;
        if (a[8] && U16(a[8], 0)) ansi(a[8], d->mask, sizeof d->mask);
        if (!strcmp(d->mask, "*.*")) d->mask[0] = 0;
        d->scanning = 1;
        d->next = 0;
        d->queried = 0;
    }
    DIR *dp = opendir(o->path);
    if (!dp) { uint32_t s = errno_status(); io_status(a[4], s, 0); return s; }
    struct dirent *e;
    uint32_t idx = 0;
    char host[1400];
    struct stat st;
    int found = 0;
    while ((e = readdir(dp))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (d->mask[0] && !mask_match(d->mask, e->d_name)) continue;
        if (idx++ != d->next) continue;
        snprintf(host, sizeof host, "%s/%s", o->path, e->d_name);
        if (stat(host, &st)) { d->next++; continue; }   /* vanished or dangling: skip it */
        found = 1;
        break;
    }
    if (!found) {
        closedir(dp);
        uint32_t s = d->queried ? STATUS_NO_MORE_FILES : STATUS_NO_SUCH_FILE;
        d->queried = 1;
        io_status(a[4], s, 0);
        complete_io(f, a[1], a[2]);
        return s;
    }
    uint32_t n = strlen(e->d_name), copy = n < len - 64 ? n : len - 64;
    U32(p, 0) = 0;
    U32(p, 4) = 0;
    U64(p, 8) = filetime(st.st_ctime);
    U64(p, 16) = filetime(st.st_atime);
    U64(p, 24) = filetime(st.st_mtime);
    U64(p, 32) = filetime(st.st_mtime);
    U64(p, 40) = (uint64_t)st.st_size;
    U64(p, 48) = ((uint64_t)st.st_size + 0xFFF) & ~0xFFFull;
    U32(p, 56) = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    U32(p, 60) = n;
    memcpy(G(p + 64), e->d_name, copy);
    closedir(dp);
    d->next++;
    d->queried = 1;
    uint32_t s = copy < n ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
    io_status(a[4], s, 64 + copy);
    complete_io(f, a[1], a[2]);
    if (klog_enabled) klog("[file] query dir %s mask '%s' -> '%.*s'\n", o->path, d->mask, (int)copy, (char *)G(p + 64));
    return s;
}

/* IoCreateSymbolicLink(SymbolicLinkName, DeviceName) -- both PSTRING */
static uint64_t k_IoCreateSymbolicLink(KFrame *f, const uint32_t *a) {
    char name[256], target[256];
    ansi(a[0], name, sizeof name);
    ansi(a[1], target, sizeof target);
    files_add_symlink(name, target);
    if (klog_enabled) klog("[file] symlink '%s' -> '%s'\n", name, target);
    return STATUS_SUCCESS;
}
static uint64_t k_IoDeleteSymbolicLink(KFrame *f, const uint32_t *a) {
    char name[256];
    ansi(a[0], name, sizeof name);
    files_remove_symlink(name);
    return STATUS_SUCCESS;
}
/* NtOpenSymbolicLinkObject(LinkHandle*, ObjectAttributes) */
static uint64_t k_NtOpenSymbolicLinkObject(KFrame *f, const uint32_t *a) {
    char name[600];
    object_path(a[1], name, sizeof name);
    const char *t = files_symlink_target(name);
    if (!t) return STATUS_OBJECT_NAME_NOT_FOUND;
    uint32_t h = handle_new(H_SYMLINK, 0);
    handle_get(h)->path = strdup(t);
    U32(a[0], 0) = h;
    return STATUS_SUCCESS;
}
/* NtQuerySymbolicLinkObject(LinkHandle, LinkTarget (PSTRING), ReturnedLength*) */
static uint64_t k_NtQuerySymbolicLinkObject(KFrame *f, const uint32_t *a) {
    HObj *o = handle_get(a[0]);
    if (!o || o->type != H_SYMLINK) return STATUS_INVALID_HANDLE;
    uint32_t n = strlen(o->path);
    if (a[2]) U32(a[2], 0) = n;
    if (n > U16(a[1], 2)) return STATUS_BUFFER_TOO_SMALL;
    memcpy(G(U32(a[1], 4)), o->path, n);
    U16(a[1], 0) = n;
    return STATUS_SUCCESS;
}

/* NtDeviceIoControlFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
 *   IoControlCode, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength) */
static uint64_t k_NtDeviceIoControlFile(KFrame *f, const uint32_t *a) {
    HObj *o = handle_get(a[0]);
    if (!o || o->type != H_FILE) return STATUS_INVALID_HANDLE;
    struct stat st;
    obj_stat(o, &st);
    uint32_t out = a[8], info = 0;
    switch (a[5]) {
    case 0x70000:   /* IOCTL_DISK_GET_DRIVE_GEOMETRY */
        U64(out, 0) = (uint64_t)st.st_size / (512 * 63 * 255); U32(out, 8) = 12 /* FixedMedia */;
        U32(out, 12) = 255; U32(out, 16) = 63; U32(out, 20) = 512; info = 24; break;
    case 0x74004:   /* IOCTL_DISK_GET_PARTITION_INFO */
        memset(G(out), 0, 32);
        U64(out, 0) = 0; U64(out, 8) = (uint64_t)st.st_size; U32(out, 16) = 0; U32(out, 20) = 0;
        *(uint8_t *)G(out + 24) = 0x0B; *(uint8_t *)G(out + 26) = 1; info = 32; break;
    default:
        kfatal(f, "NtDeviceIoControlFile ioctl %x on %s not implemented", a[5], o->path);
    }
    io_status(a[4], STATUS_SUCCESS, info);
    complete_io(f, a[1], a[2]);
    return STATUS_SUCCESS;
}

/* NtFsControlFile: same argument layout as NtDeviceIoControlFile. */
static uint64_t k_NtFsControlFile(KFrame *f, const uint32_t *a) {
    HObj *o = handle_get(a[0]);
    if (!o || o->type != H_FILE) return STATUS_INVALID_HANDLE;
    switch (a[5]) {
    case 0x90018: case 0x9001C: case 0x90020: break;   /* FSCTL_LOCK/UNLOCK/DISMOUNT_VOLUME */
    default: kfatal(f, "NtFsControlFile code %x on %s not implemented", a[5], o->path);
    }
    io_status(a[4], STATUS_SUCCESS, 0);
    complete_io(f, a[1], a[2]);
    return STATUS_SUCCESS;
}

void kfile_register(void) {
    kernel_register(200, k_NtFsControlFile);
    kernel_register(196, k_NtDeviceIoControlFile);
    kernel_register(190, k_NtCreateFile);
    kernel_register(202, k_NtCreateFile);
    kernel_register(219, k_NtReadFile);
    kernel_register(236, k_NtWriteFile);
    kernel_register(198, k_NtFlushBuffersFile);
    kernel_register(211, k_NtQueryInformationFile);
    kernel_register(226, k_NtSetInformationFile);
    kernel_register(210, k_NtQueryFullAttributesFile);
    kernel_register(218, k_NtQueryVolumeInformationFile);
    kernel_register(207, k_NtQueryDirectoryFile);
    kernel_register(67, k_IoCreateSymbolicLink);
    kernel_register(69, k_IoDeleteSymbolicLink);
    kernel_register(203, k_NtOpenSymbolicLinkObject);
    kernel_register(215, k_NtQuerySymbolicLinkObject);
}

#ifdef KFILE_TEST
/* Game-independent test (make build/kfile_test): kernel and handle stubs, a temporary host
 * directory as the hard disk, and the guest calling convention replaced by direct calls
 * (guest addresses are host addresses in this 32-bit process). */
#include <stdarg.h>

int klog_enabled;
void klog(const char *fmt, ...) { (void)fmt; }
void kfatal(KFrame *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("FAIL kfatal: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    exit(1);
}
void kernel_register(uint32_t ordinal, KHandler h) { (void)ordinal; (void)h; }
int32_t kobj_set_signal(uint32_t obj, int32_t value) { (void)obj; return value; }
static HObj thandles[64];
uint32_t handle_new(HType type, uint32_t body) {
    for (int i = 1; i < 64; i++)
        if (thandles[i].type == H_FREE) { thandles[i] = (HObj){.type = type, .body = body, .fd = -1, .refs = 1}; return i * 4; }
    return 0;
}
HObj *handle_get(uint32_t h) { return h / 4 && h / 4 < 64 && thandles[h / 4].type != H_FREE ? &thandles[h / 4] : NULL; }
static void tclose(uint32_t h) {
    HObj *o = handle_get(h);
    if (!o) return;
    close(o->fd); free(o->path); free(o->dir);
    o->type = H_FREE;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define P(x) ((uint32_t)(uintptr_t)(x))

typedef struct { uint16_t len, max; uint32_t buf; } TStr;
static TStr tstr(const char *s) { return (TStr){(uint16_t)strlen(s), (uint16_t)strlen(s), P(s)}; }

/* NtOpenFile(&h, access, oa, iosb, share, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT). */
static uint32_t open_path(const char *xp, uint32_t options, uint32_t *h) {
    TStr name = tstr(xp);
    uint32_t oa[3] = {0, P(&name), 0x40}, iosb[2] = {0, 0};
    uint32_t a[6] = {P(h), 0x100001, P(oa), P(iosb), 3, options};
    KFrame f = {.ordinal = 202};
    return (uint32_t)k_NtCreateFile(&f, a);
}

static uint8_t buf[0x148];
static uint32_t iosb[2];
/* NtQueryDirectoryFile(h, 0, 0, 0, iosb, buf, len, FileDirectoryInformation, mask, restart) */
static uint32_t query(uint32_t h, uint32_t len, const char *mask, int restart) {
    TStr m = mask ? tstr(mask) : (TStr){0, 0, 0};
    memset(buf, 0xCC, sizeof buf);
    iosb[0] = iosb[1] = 0xDEAD;
    uint32_t a[10] = {h, 0, 0, 0, P(iosb), P(buf), len, 1, mask ? P(&m) : 0, (uint32_t)restart};
    KFrame f = {.ordinal = 207};
    return (uint32_t)k_NtQueryDirectoryFile(&f, a);
}
static const char *entry_name(char *out) {
    uint32_t n = U32(buf, 60);
    memcpy(out, buf + 64, n);
    out[n] = 0;
    return out;
}

/* Enumerates the whole directory with mask; returns a '|'-joined sorted-insensitive set. */
static int enumerate(uint32_t h, const char *mask, char seen[][64], int max) {
    int n = 0;
    uint32_t st = query(h, sizeof buf, mask, 1);
    while (st == STATUS_SUCCESS && n < max) {
        entry_name(seen[n++]);
        st = query(h, sizeof buf, NULL, 0);
    }
    CHECK(st == (n ? STATUS_NO_MORE_FILES : STATUS_NO_SUCH_FILE), "mask '%s' ended with %x after %d entries", mask ? mask : "(null)", st, n);
    return n;
}
static int has(char seen[][64], int n, const char *name) {
    for (int i = 0; i < n; i++) if (!strcmp(seen[i], name)) return 1;
    return 0;
}

int main(void) {
    char root[] = "/tmp/kfile_test_XXXXXX", disc[600], hdd[600], p[1400], host[1400];
    if (!mkdtemp(root)) { printf("FAIL mkdtemp\n"); return 1; }
    snprintf(disc, sizeof disc, "%s/disc", root);
    snprintf(hdd, sizeof hdd, "%s/hdd", root);
    mkdir(disc, 0755);
    files_init(disc, hdd);
    files_add_symlink("\\??\\U:", "\\Device\\Harddisk0\\Partition1\\UDATA\\5454009b");
    int ro;
    CHECK(files_resolve("\\Device\\Harddisk0\\Partition1\\UDATA", host, sizeof host, &ro) == 0, "resolve UDATA");
    mkdir(host, 0755);
    CHECK(files_resolve("\\??\\U:\\", host, sizeof host, &ro) == 0, "resolve U:");
    mkdir(host, 0755);   /* the title's save directory, empty */

    /* 1. Empty directory: the first scan finds nothing (STATUS_NO_SUCH_FILE), later scans
     *    report STATUS_NO_MORE_FILES. This is the title's case with no saved games. */
    uint32_t h = 0, st;
    st = open_path("\\??\\U:\\", 0x4021, &h);
    CHECK(st == STATUS_SUCCESS && h, "open U:\\ -> %x", st);
    st = query(h, sizeof buf, "", 0);
    CHECK(st == STATUS_NO_SUCH_FILE, "empty first scan -> %x", st);
    CHECK(iosb[0] == STATUS_NO_SUCH_FILE && iosb[1] == 0, "empty iosb %x %x", iosb[0], iosb[1]);
    st = query(h, sizeof buf, NULL, 0);
    CHECK(st == STATUS_NO_MORE_FILES, "empty second query -> %x", st);
    tclose(h);

    /* 2. A save directory, a file and a subdirectory. */
    snprintf(p, sizeof p, "%s/DEADBEEF0001", host); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/SaveMeta.xbx", host);
    FILE *fp = fopen(p, "wb"); fwrite("0123456789", 1, 10, fp); fclose(fp);
    snprintf(p, sizeof p, "%s/abc", host);
    fp = fopen(p, "wb"); fclose(fp);
    h = 0;
    CHECK(open_path("\\??\\U:\\", 0x4021, &h) == STATUS_SUCCESS, "reopen U:\\");
    char seen[8][64];
    int n = enumerate(h, "", seen, 8);
    CHECK(n == 3 && has(seen, n, "DEADBEEF0001") && has(seen, n, "SaveMeta.xbx") && has(seen, n, "abc"),
          "empty mask gave %d entries", n);
    n = enumerate(h, NULL, seen, 8);
    CHECK(n == 3, "null mask gave %d entries", n);
    n = enumerate(h, "*", seen, 8);
    CHECK(n == 3, "'*' gave %d entries", n);
    n = enumerate(h, "*.*", seen, 8);
    CHECK(n == 3, "'*.*' gave %d entries", n);
    n = enumerate(h, "*.XBX", seen, 8);
    CHECK(n == 1 && !strcmp(seen[0], "SaveMeta.xbx"), "'*.XBX' gave %d entries", n);
    n = enumerate(h, "A?C", seen, 8);
    CHECK(n == 1 && !strcmp(seen[0], "abc"), "'A?C' gave %d entries", n);
    n = enumerate(h, "nothing*", seen, 8);
    CHECK(n == 0, "'nothing*' gave %d entries", n);

    /* 3. Layout of FILE_DIRECTORY_INFORMATION for the file entry. */
    st = query(h, sizeof buf, "savemeta.xbx", 1);
    CHECK(st == STATUS_SUCCESS, "single file -> %x", st);
    char name[64];
    CHECK(U32(buf, 0) == 0, "NextEntryOffset %u", U32(buf, 0));
    CHECK(U64(buf, 40) == 10, "EndOfFile %llu", (unsigned long long)U64(buf, 40));
    CHECK(U64(buf, 48) >= 10, "AllocationSize %llu", (unsigned long long)U64(buf, 48));
    CHECK(U32(buf, 56) == FILE_ATTRIBUTE_NORMAL, "file attributes %x", U32(buf, 56));
    CHECK(U64(buf, 24) > 116444736000000000ull, "LastWriteTime");
    CHECK(U32(buf, 60) == 12 && !strcmp(entry_name(name), "SaveMeta.xbx"), "name '%s'", entry_name(name));
    CHECK(iosb[0] == STATUS_SUCCESS && iosb[1] == 64 + 12, "iosb %x %u", iosb[0], iosb[1]);
    st = query(h, sizeof buf, NULL, 0);
    CHECK(st == STATUS_NO_MORE_FILES, "after single file -> %x", st);
    st = query(h, sizeof buf, "DEAD*", 1);
    CHECK(st == STATUS_SUCCESS && U32(buf, 56) == FILE_ATTRIBUTE_DIRECTORY, "directory entry attrs %x", U32(buf, 56));

    /* 4. Without RestartScan the mask of the running scan is kept. */
    st = query(h, sizeof buf, "abc", 1);
    CHECK(st == STATUS_SUCCESS, "restart abc -> %x", st);
    st = query(h, sizeof buf, "*", 0);
    CHECK(st == STATUS_NO_MORE_FILES, "new mask without restart -> %x", st);

    /* 5. A buffer too small for the name: STATUS_BUFFER_OVERFLOW with the full name length;
     *    smaller than the fixed part: STATUS_INFO_LENGTH_MISMATCH. */
    st = query(h, 64 + 4, "SaveMeta.xbx", 1);
    CHECK(st == STATUS_BUFFER_OVERFLOW && U32(buf, 60) == 12 && !memcmp(buf + 64, "Save", 4) && buf[68] == 0xCC,
          "small buffer -> %x", st);
    st = query(h, 32, "", 1);
    CHECK(st == STATUS_INFO_LENGTH_MISMATCH, "tiny buffer -> %x", st);
    tclose(h);

    /* 6. A file handle is not a directory. */
    uint32_t fh = 0;
    CHECK(open_path("\\??\\U:\\abc", 0x20, &fh) == STATUS_SUCCESS, "open file");
    st = query(fh, sizeof buf, "", 1);
    CHECK(st == STATUS_INVALID_PARAMETER, "query on a file -> %x", st);
    tclose(fh);

    /* 7. Path mapping refuses '..' components (no escape from the disc or hdd roots). */
    CHECK(files_resolve("\\??\\U:\\..\\..\\..\\..", host, sizeof host, &ro) == -1, "'..' resolved to %s", host);
    CHECK(files_resolve("\\Device\\CdRom0\\..\\x", host, sizeof host, &ro) == -1, "cdrom '..' resolved to %s", host);
    CHECK(files_resolve("\\Device\\Harddisk0\\Partition1\\a\\..\\b", host, sizeof host, &ro) == -1, "inner '..' resolved");
    CHECK(files_resolve("\\Device\\Harddisk0\\Partition1\\a..b", host, sizeof host, &ro) == 0, "'a..b' is a plain name");
    h = 0;
    st = open_path("\\??\\U:\\..", 0x4021, &h);
    CHECK(st != STATUS_SUCCESS, "open U:\\.. -> %x", st);

    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd)) printf("warning: could not remove %s\n", root);
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("ALL OK\n");
    return 0;
}
#endif
