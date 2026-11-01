/* Rewrites the guest's fs:-relative KPCR accesses for hosts where %fs cannot point at the
 * KPCR (Windows: FS is the TEB, and 64-bit Windows has no LDT to give it another base).
 *
 * Xbox code reaches its KPCR only through absolute fs:[disp32] operands (SEH chain fs:[0],
 * StackBase fs:[4], Prcb fs:[0x20], Irql fs:[0x24], CurrentThread fs:[0x28], DPC state
 * fs:[0x58]). Each such instruction is 6-8 bytes, so it is replaced by "jmp stub" + nops; the
 * per-site stub loads this thread's KPCR pointer, performs the same access on [kpcr + disp]
 * and jumps back. Flags are preserved (only the original cmp changes them) and every register
 * the stub borrows is restored. */
#ifndef FSPATCH_H
#define FSPATCH_H
#include <stdint.h>

#define FSPATCH_MAX_DISP 0x300   /* KPCR incl. PrcbData; larger displacements are not KPCR */

typedef struct {
    uint32_t offset;   /* from the start of the scanned buffer */
    uint8_t len;       /* instruction length (6..8) */
    uint8_t kind;      /* FS_LOAD, FS_STORE, ... */
    uint8_t op;        /* opcode byte (0x8B, 0x89, 0x3B, 0x39, 0xB6 for movzx) */
    uint8_t reg;       /* register operand (0-7) */
    uint32_t disp;
} FsSite;

enum { FS_LOAD, FS_STORE, FS_PUSH, FS_POP };

/* Decodes the fs:[disp32] instruction at p (n bytes available); returns its length or 0. */
int fspatch_decode(const uint8_t *p, uint32_t n, FsSite *out);
/* Scans code linearly for decodable sites (disp < FSPATCH_MAX_DISP, dword aligned) and calls fn for each;
 * returns the count. Sites never overlap (the scan resumes after each one). */
int fspatch_scan(const uint8_t *code, uint32_t size, void (*fn)(const FsSite *, void *), void *ctx);
/* XBE section filter: executable sections that hold host-CPU code (the XBE also marks .rdata,
 * .data, DSP images and BINK data executable). */
int fspatch_section_is_code(const char *name, uint32_t flags);

/* Patches every site in [code, code + size). seg_fs: the KPCR pointer is read from
 * fs:[kpcr_ref] (a per-thread slot); otherwise from the absolute address kpcr_ref (tests).
 * Returns the number of patched sites, -1 if stub memory could not be allocated. */
int fspatch_apply(uint8_t *code, uint32_t size, int seg_fs, uint32_t kpcr_ref);
#endif
