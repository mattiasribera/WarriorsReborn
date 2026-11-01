/* Game-independent test of src/fspatch.c: hand-encoded guest fs:[disp32] instructions are
 * patched and executed against a fake KPCR, once with the KPCR pointer in an absolute host
 * variable and once through the platform's per-thread KPCR slot the way the runtime does
 * (Linux --patch-fs: fs:[0x1C]; Windows: the TEB TLS slot). Also built as fspatch_test.exe. */
#include "../src/fspatch.h"
#include "../src/platform.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t kpcr[0x300] __attribute__((aligned(16)));
static uint32_t kpcr_ptr;   /* absolute-mode slot */
#define K32(o) (*(uint32_t *)(kpcr + (o)))

static uint8_t *code;
static int seg_fs;
static uint32_t ref;

/* Copies a snippet, patches it (expects exactly one site) and runs it; returns eax. */
static uint32_t run(const char *name, const uint8_t *bytes, size_t n) {
    memcpy(code, bytes, n);
    int sites = fspatch_apply(code, (uint32_t)n, seg_fs, ref);
    CHECK(sites == 1, "%s: %d sites patched (expected 1)", name, sites);
    return ((uint32_t(*)(void))code)();
}
#define RUN(name, ...) ({ static const uint8_t b_[] = {__VA_ARGS__}; run(name, b_, sizeof b_); })

static void suite(const char *mode) {
    printf("mode %s\n", mode);
    memset(kpcr, 0, sizeof kpcr);
    K32(0x04) = 0x10203040; K32(0x1C) = (uint32_t)(uintptr_t)kpcr; K32(0x20) = 0xCAFE0028; K32(0x28) = 0x0BADF00D;
    kpcr[0x24] = 2;

    CHECK(RUN("mov eax,fs:20", 0x64, 0xA1, 0x20, 0, 0, 0, 0xC3) == 0xCAFE0028, "%s mov eax,fs:[0x20]", mode);
    /* mov ecx,fs:4 with ebx/esi/edi/edx live: the stub's scratch registers must be restored */
    uint32_t v = RUN("mov ecx,fs:4", 0x53, 0x56, 0x57, 0xBB, 0, 1, 0, 0, 0xBE, 0, 2, 0, 0, 0xBF, 0, 4, 0, 0, 0xBA, 0, 8, 0, 0,
                     0x64, 0x8B, 0x0D, 4, 0, 0, 0, 0x89, 0xC8, 0x01, 0xD8, 0x01, 0xF0, 0x01, 0xF8, 0x01, 0xD0, 0x5F, 0x5E, 0x5B, 0xC3);
    CHECK(v == 0x10203040 + 0xF00, "%s mov ecx,fs:[4] / preserved registers: %08x", mode, v);
    CHECK(RUN("movzx eax,fs:24", 0x64, 0x0F, 0xB6, 0x05, 0x24, 0, 0, 0, 0xC3) == 2, "%s movzx eax,byte fs:[0x24]", mode);
    CHECK(RUN("mov edi,fs:28", 0x57, 0x64, 0x8B, 0x3D, 0x28, 0, 0, 0, 0x89, 0xF8, 0x5F, 0xC3) == 0x0BADF00D, "%s mov edi,fs:[0x28]", mode);
    v = RUN("mov ebx,fs:0", 0x53, 0x64, 0x8B, 0x1D, 0, 0, 0, 0, 0x89, 0xD8, 0x5B, 0xC3);
    CHECK(v == K32(0), "%s mov ebx,fs:[0]", mode);

    v = RUN("mov fs:0,esp", 0x64, 0x89, 0x25, 0, 0, 0, 0, 0x89, 0xE0, 0xC3);
    CHECK(K32(0) == v && v, "%s mov fs:[0],esp stored %08x, esp was %08x", mode, K32(0), v);
    RUN("mov fs:0,ecx", 0xB9, 0x34, 0x12, 0, 0, 0x64, 0x89, 0x0D, 0, 0, 0, 0, 0xC3);
    CHECK(K32(0) == 0x1234, "%s mov fs:[0],ecx", mode);
    RUN("mov fs:0,eax", 0xB8, 0x78, 0x56, 0, 0, 0x64, 0xA3, 0, 0, 0, 0, 0xC3);
    CHECK(K32(0) == 0x5678, "%s mov fs:[0],eax", mode);
    CHECK(RUN("push fs:0", 0x64, 0xFF, 0x35, 0, 0, 0, 0, 0x58, 0xC3) == 0x5678, "%s push fs:[0]; pop eax", mode);
    RUN("pop fs:0", 0x68, 0xAA, 0x99, 0, 0, 0x64, 0x8F, 0x05, 0, 0, 0, 0, 0xC3);   /* unbalanced stack would crash at ret */
    CHECK(K32(0) == 0x99AA, "%s push imm; pop fs:[0]", mode);
    CHECK(RUN("cmp esi,fs:0 eq", 0x56, 0xBE, 0xAA, 0x99, 0, 0, 0x31, 0xC0, 0x64, 0x3B, 0x35, 0, 0, 0, 0, 0x0F, 0x94, 0xC0, 0x5E, 0xC3) == 1,
          "%s cmp esi,fs:[0] (equal)", mode);
    CHECK(RUN("cmp esi,fs:0 ne", 0x56, 0xBE, 0xAB, 0x99, 0, 0, 0x31, 0xC0, 0x64, 0x3B, 0x35, 0, 0, 0, 0, 0x0F, 0x94, 0xC0, 0x5E, 0xC3) == 0,
          "%s cmp esi,fs:[0] (different)", mode);
    /* stc; mov eax,fs:[0x20]; sbb eax,eax: the carry flag survives the stub */
    CHECK(RUN("flags", 0xF9, 0x64, 0xA1, 0x20, 0, 0, 0, 0x19, 0xC0, 0xC3) == 0xFFFFFFFF, "%s flags preserved", mode);
}

int main(void) {
    plat_init("fspatch_test");   /* Windows: CRT error handlers and fspatch_test.log next to the exe */
    printf("fspatch_test: start\n");
    FsSite s;
    static const uint8_t far_disp[] = {0x64, 0x8B, 0x0D, 0x00, 0x10, 0x00, 0x00};   /* fs:[0x1000]: not KPCR */
    static const uint8_t reg_base[] = {0x64, 0x8B, 0x4D, 0x00, 0, 0, 0};           /* mov ecx, fs:[ebp] */
    static const uint8_t short_buf[] = {0x64, 0xA1, 0x20, 0x00};
    static const uint8_t text[] = {0x64, 0x3B, 0x0D, 0x0A, 0x00, 0x00, 0x00};       /* "d;\r\n": unaligned disp */
    CHECK(!fspatch_decode(text, sizeof text, &s), "unaligned fs:[0xa] decoded");
    CHECK(!fspatch_decode(far_disp, sizeof far_disp, &s), "fs:[0x1000] decoded");
    CHECK(!fspatch_decode(reg_base, sizeof reg_base, &s), "fs:[ebp] decoded");
    CHECK(!fspatch_decode(short_buf, sizeof short_buf, &s), "truncated instruction decoded");
    static const uint8_t two[] = {0x90, 0x64, 0xA1, 0, 0, 0, 0, 0x64, 0x89, 0x25, 0, 0, 0, 0, 0x64, 0x8B, 0x0D, 0, 0x10, 0, 0};
    CHECK(fspatch_scan(two, sizeof two, NULL, NULL) == 2, "scan found %d sites (expected 2)", fspatch_scan(two, sizeof two, NULL, NULL));
    CHECK(fspatch_section_is_code(".text", 0x16) && fspatch_section_is_code("DSOUND", 7) && !fspatch_section_is_code(".rdata", 6) &&
              !fspatch_section_is_code("BINKDATA", 7) && !fspatch_section_is_code("DOLBY", 6) && !fspatch_section_is_code("$$XTIMAGE", 0x38) &&
              !fspatch_section_is_code("D3D", 1),
          "section filter");

    code = plat_alloc_exec(0x1000);
    if (!code) { printf("cannot allocate code\n"); return 1; }
    seg_fs = 0;
    kpcr_ptr = (uint32_t)(uintptr_t)kpcr;
    ref = (uint32_t)(uintptr_t)&kpcr_ptr;
    suite("absolute");
    /* The runtime's path: Linux %fs = KPCR, read SelfPcr at fs:[0x1C]; Windows reads the
     * KPCR pointer from its TEB TLS slot (plat_kpcr_init checks the TEB layout). */
    uint32_t disp;
    plat_kpcr_init();
    plat_kpcr_bind(kpcr, 0, 0, 0);
    plat_fs_patch_required(&disp);
    seg_fs = 1;
    ref = disp;
    suite("fs");
    printf(failures ? "fspatch_test: %d failures\n" : "fspatch_test: all checks passed\n", failures);
    plat_flush_output();
    return failures != 0;
}
