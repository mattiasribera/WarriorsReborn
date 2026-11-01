/* Host implementation of the Xbox kernel (xboxkrnl.exe) imports. */
#ifndef KERNEL_H
#define KERNEL_H
#include <stdint.h>
#include <stddef.h>

#define KEXPORT_COUNT 379

typedef enum { CC_UNKNOWN, CC_STDCALL, CC_FASTCALL, CC_CDECL, CC_DATA } CallConv;

typedef struct {
    const char *name;
    CallConv conv;
    int arg_dwords;   /* argument size in dwords (64-bit by-value args count twice) */
} KExport;

extern const KExport kexports[KEXPORT_COUNT];

/* Stack frame built by kentry_common (thunk.S): pushal block, then the stub frame. */
typedef struct {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t stub_ret, ordinal, ret_addr;
    uint32_t args[];
} KFrame;

/* A handler receives logical arguments (fastcall: ecx, edx, stack...) and returns edx:eax. */
typedef uint64_t (*KHandler)(KFrame *f, const uint32_t *a);

void kernel_init(void);
/* Returns the address to place in the thunk slot for an ordinal (stub or data). */
uint32_t kernel_resolve(uint32_t ordinal);
void kernel_dispatch(KFrame *f);
void kernel_register(uint32_t ordinal, KHandler h);
void kernel_register_data(uint32_t ordinal, void *data);
int kernel_is_implemented(uint32_t ordinal);
extern int klog_enabled;        /* per-call log (kernel calls, file opens); off unless --trace */
extern int klog_limit;

/* Subsystem registration (each file registers its own handlers). */
void kmem_register(void);
void kobj_register(void);
void kfile_register(void);
void kmisc_register(void);
void kxc_register(void);

/* Recent-call ring buffer for diagnostics. */
typedef struct { uint32_t ordinal, caller, tid, args[4]; } KCallRec;
void kring_dump(int max);
uint32_t ksym_nearest(uint32_t addr, const char **name);

#define STATUS_SUCCESS 0x00000000u
#define STATUS_TIMEOUT 0x00000102u
#define STATUS_PENDING 0x00000103u
#define STATUS_ALERTED 0x00000101u
#define STATUS_USER_APC 0x000000C0u
#define STATUS_ABANDONED 0x00000080u
#define STATUS_BUFFER_OVERFLOW 0x80000005u
#define STATUS_NO_MORE_FILES 0x80000006u
#define STATUS_UNSUCCESSFUL 0xC0000001u
#define STATUS_NOT_IMPLEMENTED 0xC0000002u
#define STATUS_INFO_LENGTH_MISMATCH 0xC0000004u
#define STATUS_INVALID_HANDLE 0xC0000008u
#define STATUS_INVALID_PARAMETER 0xC000000Du
#define STATUS_NO_SUCH_FILE 0xC000000Fu
#define STATUS_END_OF_FILE 0xC0000011u
#define STATUS_NO_MEMORY 0xC0000017u
#define STATUS_ACCESS_DENIED 0xC0000022u
#define STATUS_BUFFER_TOO_SMALL 0xC0000023u
#define STATUS_OBJECT_TYPE_MISMATCH 0xC0000024u
#define STATUS_OBJECT_NAME_INVALID 0xC0000033u
#define STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define STATUS_OBJECT_NAME_COLLISION 0xC0000035u
#define STATUS_OBJECT_PATH_NOT_FOUND 0xC000003Au
#define STATUS_MUTANT_NOT_OWNED 0xC0000046u
#define STATUS_SEMAPHORE_LIMIT_EXCEEDED 0xC0000047u
#define STATUS_FILE_IS_A_DIRECTORY 0xC00000BAu
#define STATUS_NOT_A_DIRECTORY 0xC0000103u

#define G(p) ((void *)(uintptr_t)(p))      /* guest address -> host pointer (identity) */
#define GA(p) ((uint32_t)(uintptr_t)(p))   /* host pointer -> guest address */

/* Diagnostics: abort with caller info. */
void kfatal(KFrame *f, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif
