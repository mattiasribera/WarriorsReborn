/* Prints decoded XBE header fields as key=value lines (used by tests). With --fs-sites, prints
 * the fs:[disp32] KPCR accesses that src/fspatch.c rewrites, one "fs=SECTION,VADDR,LEN,DISP"
 * line per site in the code sections. */
#include <stdio.h>
#include <string.h>
#include "../src/fspatch.h"
#include "../src/xbe.h"

static const XbeSection *cur;
static void print_site(const FsSite *s, void *ctx) {
    (void)ctx;
    printf("fs=%s,%08x,%u,%x\n", cur->name, cur->vaddr + s->offset, s->len, s->disp);
}

int main(int argc, char **argv) {
    int fs_sites = argc == 3 && !strcmp(argv[1], "--fs-sites");
    if (argc != 2 && !fs_sites) { fprintf(stderr, "usage: xbeinfo [--fs-sites] <default.xbe>\n"); return 2; }
    uint8_t *data; size_t size; Xbe x; char err[128];
    if (xbe_read_file(argv[argc - 1], &data, &size)) { perror(argv[argc - 1]); return 1; }
    if (xbe_parse(&x, data, size, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    if (fs_sites) {
        for (uint32_t i = 0; i < x.section_count; i++) {
            cur = &x.sections[i];
            if (!fspatch_section_is_code(cur->name, cur->flags)) continue;
            uint32_t n = cur->raw_size < cur->vsize ? cur->raw_size : cur->vsize;
            fspatch_scan(data + cur->raw_addr, n, print_site, NULL);
        }
        return 0;
    }
    printf("base=%08x\nimage_size=%08x\nheaders_size=%08x\nkey=%s\nentry=%08x\nthunk_table=%08x\ntls=%08x\nstack_commit=%08x\n",
           x.base, x.image_size, x.headers_size, x.key, x.entry, x.thunk_table, x.tls_addr, x.stack_commit);
    for (uint32_t i = 0; i < x.section_count; i++) {
        XbeSection *s = &x.sections[i];
        printf("section=%s,%08x,%08x,%08x,%08x,%08x\n", s->name, s->vaddr, s->vsize, s->raw_addr, s->raw_size, s->flags);
    }
    uint32_t ords[512];
    int n = xbe_thunk_ordinals(&x, ords, 512);
    for (int i = 0; i < n; i++) printf("thunk=%u\n", ords[i]);
    return 0;
}
