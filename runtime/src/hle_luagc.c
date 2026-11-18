/* Lua garbage collector: no nested collection, larger cap (see hle_luagc.h). */
#include "hle_luagc.h"
#include "platform.h"
#include <stdio.h>
#include <string.h>

volatile int32_t *luagc_depth;

/* The first bytes of luaC_collectgarbage as shipped (the 5 replaced, then push edi; mov eax,esi). */
static const uint8_t collect_sig[] = {0x56, 0x8b, 0x74, 0x24, 0x08, 0x57, 0x8b, 0xc6};
/* luaC_checkGC as shipped: collect when nblocks (+0x60) >= threshold (+0x5c), by a jmp. */
static const uint8_t check_sig[] = {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x48, 0x60, 0x3b, 0x48, 0x5c, 0x72,
                                    0x09, 0x89, 0x44, 0x24, 0x04, 0xe9, 0x4b, 0xff, 0xff, 0xff};

/* Each clamp: the instruction's address, its opcode bytes, then the 4-byte 0xa7000. */
static const struct {
    uint32_t va;
    uint8_t n, op[3];
} caps[LUAGC_CAP_SITES] = {
    {0x0022f45bu, 1, {0x3d}},             /* lua_setgcthreshold: cmp eax,cap */
    {0x0022f465u, 3, {0xc7, 0x41, 0x5c}}, /*                     mov [ecx+0x5c],cap */
    {0x00233d4fu, 2, {0x81, 0xf9}},       /* aux_do: cmp ecx,cap */
    {0x00233d5au, 3, {0xc7, 0x46, 0x5c}}, /*         mov [esi+0x5c],cap */
    {0x00234888u, 1, {0x3d}},             /* callgcTMudata: cmp eax,cap */
    {0x0023489au, 3, {0xc7, 0x46, 0x5c}}, /*                mov [esi+0x5c],cap */
    {0x00234bd3u, 1, {0x3d}},             /* luaC_collectgarbage: cmp eax,cap */
    {0x00234bddu, 3, {0xc7, 0x46, 0x5c}}, /*                      mov [esi+0x5c],cap */
};

#define AT(va) ((uint8_t *)(uintptr_t)(va))

static uint8_t *put32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
    return p + 4;
}
static uint32_t rel(const uint8_t *from_next, uint32_t to) { return to - (uint32_t)(uintptr_t)from_next; }

/* The stub: return while a collection runs, else count it and run the original.
 *   +0  cmp dword [depth],0      +7 jne +35 (ret)      +9 lock inc dword [depth]
 *   +16 push dword [esp+4]       +20 call +40          +25 add esp,4
 *   +28 lock dec dword [depth]   +35 ret
 *   +40 push esi; mov esi,[esp+8]; jmp collect+5       +52 depth */
static void build_stub(uint8_t *s) {
    uint32_t depth = (uint32_t)(uintptr_t)(s + 52);
    uint8_t *p = s;
    memset(s, 0xcc, LUAGC_STUB_SIZE);
    *p++ = 0x83; *p++ = 0x3d; p = put32(p, depth); *p++ = 0x00;
    *p++ = 0x75; *p++ = 35 - 9;
    *p++ = 0xf0; *p++ = 0xff; *p++ = 0x05; p = put32(p, depth);
    *p++ = 0xff; *p++ = 0x74; *p++ = 0x24; *p++ = 0x04;
    *p++ = 0xe8; p = put32(p, rel(p + 4, (uint32_t)(uintptr_t)(s + 40)));
    *p++ = 0x83; *p++ = 0xc4; *p++ = 0x04;
    *p++ = 0xf0; *p++ = 0xff; *p++ = 0x0d; p = put32(p, depth);
    *p++ = 0xc3;
    p = s + 40;
    memcpy(p, collect_sig, LUAGC_COLLECT_LEN);
    p += LUAGC_COLLECT_LEN;
    *p++ = 0xe9; put32(p, rel(p + 4, LUAGC_COLLECT + LUAGC_COLLECT_LEN));
    memset(s + 52, 0, 4);
    luagc_depth = (volatile int32_t *)(uintptr_t)depth;
}

int luagc_apply(uint8_t *stub, char *report, size_t cap) {
    if (!cap) return 0;
    report[0] = 0;
    const char *bad = NULL;
    uint32_t bad_va = 0;
    if (memcmp(AT(LUAGC_COLLECT), collect_sig, sizeof collect_sig)) bad = "luaC_collectgarbage", bad_va = LUAGC_COLLECT;
    else if (memcmp(AT(LUAGC_CHECK), check_sig, sizeof check_sig)) bad = "luaC_checkGC", bad_va = LUAGC_CHECK;
    for (int i = 0; !bad && i < LUAGC_CAP_SITES; i++) {
        uint32_t v;
        memcpy(&v, AT(caps[i].va + caps[i].n), 4);
        if (memcmp(AT(caps[i].va), caps[i].op, caps[i].n) || v != LUAGC_CAP_SHIPPED) bad = "a collection cap", bad_va = caps[i].va;
    }
    if (bad) {
        snprintf(report, cap, "[luagc] not patched: %s at %08x is not the shipped code; Lua collections as shipped", bad, bad_va);
        return 0;
    }
    if (!stub) {
        snprintf(report, cap, "[luagc] not patched: no stub memory; Lua collections as shipped");
        return 0;
    }
    build_stub(stub);
    for (int i = 0; i < LUAGC_CAP_SITES; i++) put32(AT(caps[i].va + caps[i].n), LUAGC_CAP);
    uint8_t jmp[LUAGC_COLLECT_LEN] = {0xe9};
    put32(jmp + 1, (uint32_t)(uintptr_t)stub - (LUAGC_COLLECT + LUAGC_COLLECT_LEN));
    memcpy(AT(LUAGC_COLLECT), jmp, sizeof jmp);
    snprintf(report, cap, "[luagc] Lua collections at %08x no longer nest; collection cap %u -> %u KiB (%d sites)",
             LUAGC_COLLECT, LUAGC_CAP_SHIPPED / 1024, LUAGC_CAP / 1024, LUAGC_CAP_SITES);
    return 1;
}

int hle_luagc_install(void) {
    static int done;
    if (done++) return 0;
    char report[192];
    int n = luagc_apply(plat_alloc_exec(LUAGC_STUB_SIZE), report, sizeof report);
    printf("%s\n", report);
    return n;
}
