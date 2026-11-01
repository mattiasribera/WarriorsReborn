/* XBE (original Xbox executable) parsing and loading. */
#ifndef XBE_H
#define XBE_H
#include <stddef.h>
#include <stdint.h>

#define XBE_MAX_SECTIONS 64

typedef struct {
    char name[32];
    uint32_t flags, vaddr, vsize, raw_addr, raw_size;
} XbeSection;

typedef struct {
    uint32_t data_start, data_end, index_addr, callback_addr, zero_fill, characteristics;
} XbeTls;

typedef struct {
    const uint8_t *file;   /* whole file contents */
    size_t file_size;
    uint32_t base, headers_size, image_size, cert_addr;
    uint32_t entry, thunk_table;   /* decoded (XOR key removed) */
    const char *key;               /* "retail", "debug" or "sega" */
    uint32_t tls_addr, stack_commit, heap_reserve, heap_commit, init_flags;
    uint32_t section_count;
    XbeSection sections[XBE_MAX_SECTIONS];
} Xbe;

/* Parses headers from an in-memory file; returns 0 or -1 with a message in err. */
int xbe_parse(Xbe *x, const uint8_t *file, size_t size, char *err, size_t errlen);
/* Reads a little-endian dword at a virtual address from the file image (headers or sections). */
int xbe_read_va(const Xbe *x, uint32_t va, uint32_t *out);
/* Kernel import ordinals listed in the thunk table (file view); returns count. */
int xbe_thunk_ordinals(const Xbe *x, uint32_t *out, int max);
int xbe_read_file(const char *path, uint8_t **data, size_t *size);
#endif
