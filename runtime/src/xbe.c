#include "xbe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* XOR keys, as used by ghidra-xbe XbeLoader.java (originally radare2 bin_xbe.c). */
#define ENTRY_RETAIL 0xA8FC57ABu
#define ENTRY_DEBUG 0x94859D4Bu
#define ENTRY_SEGA 0x40B5C16Eu
#define KTHUNK_RETAIL 0x5B6D40B6u
#define KTHUNK_DEBUG 0xEFB1F152u
#define KTHUNK_SEGA 0x2290059Du

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int xbe_read_file(const char *path, uint8_t **data, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *data = malloc(n);
    if (!*data || fread(*data, 1, n, f) != (size_t)n) { fclose(f); return -1; }
    fclose(f);
    *size = n;
    return 0;
}

/* Maps a virtual address to a file offset (headers or raw section data). */
static long va_to_off(const Xbe *x, uint32_t va) {
    if (va >= x->base && va < x->base + x->headers_size) return va - x->base;
    for (uint32_t i = 0; i < x->section_count; i++) {
        const XbeSection *s = &x->sections[i];
        if (va >= s->vaddr && va < s->vaddr + s->raw_size) return s->raw_addr + (va - s->vaddr);
    }
    return -1;
}

int xbe_read_va(const Xbe *x, uint32_t va, uint32_t *out) {
    long off = va_to_off(x, va);
    if (off < 0 || (size_t)off + 4 > x->file_size) return -1;
    *out = rd32(x->file + off);
    return 0;
}

static int in_image(const Xbe *x, uint32_t va) { return va >= x->base && va < x->base + x->image_size; }

int xbe_parse(Xbe *x, const uint8_t *f, size_t size, char *err, size_t errlen) {
    memset(x, 0, sizeof *x);
    if (size < 0x178 || memcmp(f, "XBEH", 4)) { snprintf(err, errlen, "not an XBE"); return -1; }
    x->file = f;
    x->file_size = size;
    x->base = rd32(f + 0x104);
    x->headers_size = rd32(f + 0x108);
    x->image_size = rd32(f + 0x10C);
    x->cert_addr = rd32(f + 0x118);
    x->section_count = rd32(f + 0x11C);
    uint32_t sec_hdrs = rd32(f + 0x120);
    x->init_flags = rd32(f + 0x124);
    uint32_t entry = rd32(f + 0x128);
    x->tls_addr = rd32(f + 0x12C);
    x->stack_commit = rd32(f + 0x130);
    x->heap_reserve = rd32(f + 0x134);
    x->heap_commit = rd32(f + 0x138);
    uint32_t thunk = rd32(f + 0x158);
    if (x->section_count > XBE_MAX_SECTIONS) { snprintf(err, errlen, "too many sections"); return -1; }
    for (uint32_t i = 0; i < x->section_count; i++) {
        uint32_t off = sec_hdrs - x->base + i * 0x38;
        if (off + 0x38 > size) { snprintf(err, errlen, "section header out of file"); return -1; }
        XbeSection *s = &x->sections[i];
        s->flags = rd32(f + off);
        s->vaddr = rd32(f + off + 4);
        s->vsize = rd32(f + off + 8);
        s->raw_addr = rd32(f + off + 12);
        s->raw_size = rd32(f + off + 16);
        uint32_t name_off = rd32(f + off + 20) - x->base;
        if (name_off < size) snprintf(s->name, sizeof s->name, "%.31s", (const char *)f + name_off);
    }
    /* Pick the key whose decoded entry falls inside the image. */
    static const struct { const char *name; uint32_t e, k; } keys[] = {
        {"retail", ENTRY_RETAIL, KTHUNK_RETAIL}, {"debug", ENTRY_DEBUG, KTHUNK_DEBUG}, {"sega", ENTRY_SEGA, KTHUNK_SEGA}};
    for (int i = 0; i < 3; i++) {
        if (in_image(x, entry ^ keys[i].e) && in_image(x, thunk ^ keys[i].k)) {
            x->entry = entry ^ keys[i].e;
            x->thunk_table = thunk ^ keys[i].k;
            x->key = keys[i].name;
            return 0;
        }
    }
    snprintf(err, errlen, "no XOR key decodes entry/thunk inside the image");
    return -1;
}

int xbe_thunk_ordinals(const Xbe *x, uint32_t *out, int max) {
    int n = 0;
    uint32_t v;
    while (n < max && xbe_read_va(x, x->thunk_table + 4 * n, &v) == 0 && v) out[n++] = v & 0x7FFFFFFF;
    return n;
}
