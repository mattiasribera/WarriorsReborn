/* Lua garbage collector: no nested collection, larger cap (see hle_luagc.h). */
#include "hle_luagc.h"
#include "mem.h"
#include "platform.h"
#include <stdio.h>
#include <string.h>

volatile int32_t *luagc_depth;
volatile uint32_t *luagc_peak;
static int state_level = -1;   /* the level of the current Lua state (-1: no levelNN script yet) */

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

/* The Lua heap's creation as shipped, around the maximum (LUAGC_HEAP_SITE + 1, 4 bytes):
 * push esi; push imm32 | push 0; push 0; mov esi,ecx; call HeapCreate | ... mov [esi+0x60],eax */
static const uint8_t heap_head[] = {0x56, 0x68};
static const uint8_t heap_tail[] = {0x6a, 0x00, 0x6a, 0x00, 0x8b, 0xf1, 0xe8};
static const uint8_t heap_store[] = {0x89, 0x46, 0x60};

#define AT(va) ((uint8_t *)(uintptr_t)(va))

static uint8_t *put32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
    return p + 4;
}
static uint32_t rel(const uint8_t *from_next, uint32_t to) { return to - (uint32_t)(uintptr_t)from_next; }

/* The stub: note the state's size, return while a collection runs, else count it and run the
 * original. The prologue (eax is free at the entry of luaC_collectgarbage):
 *   +0  mov eax,[esp+4]   +4 mov eax,[eax+0x60] (nblocks)   +7 cmp eax,[peak]   +13 jbe +5
 *   +15 mov [peak],eax
 * then, from b = +20:
 *   b+0  cmp dword [depth],0      b+7 jne b+35 (ret)    b+9 lock inc dword [depth]
 *   b+16 push dword [esp+4]       b+20 call b+40        b+25 add esp,4
 *   b+28 lock dec dword [depth]   b+35 ret
 *   b+40 push esi; mov esi,[esp+8]; jmp collect+5       b+52 depth   b+56 peak */
#define STUB_BODY 20
static void build_stub(uint8_t *s) {
    uint8_t *b = s + STUB_BODY;
    uint32_t depth = (uint32_t)(uintptr_t)(b + 52), peak = (uint32_t)(uintptr_t)(b + 56);
    uint8_t *p = s;
    memset(s, 0xcc, LUAGC_STUB_SIZE);
    *p++ = 0x8b; *p++ = 0x44; *p++ = 0x24; *p++ = 0x04;
    *p++ = 0x8b; *p++ = 0x40; *p++ = 0x60;
    *p++ = 0x3b; *p++ = 0x05; p = put32(p, peak);
    *p++ = 0x76; *p++ = 0x05;
    *p++ = 0xa3; p = put32(p, peak);
    *p++ = 0x83; *p++ = 0x3d; p = put32(p, depth); *p++ = 0x00;
    *p++ = 0x75; *p++ = 35 - 9;
    *p++ = 0xf0; *p++ = 0xff; *p++ = 0x05; p = put32(p, depth);
    *p++ = 0xff; *p++ = 0x74; *p++ = 0x24; *p++ = 0x04;
    *p++ = 0xe8; p = put32(p, rel(p + 4, (uint32_t)(uintptr_t)(b + 40)));
    *p++ = 0x83; *p++ = 0xc4; *p++ = 0x04;
    *p++ = 0xf0; *p++ = 0xff; *p++ = 0x0d; p = put32(p, depth);
    *p++ = 0xc3;
    p = b + 40;
    memcpy(p, collect_sig, LUAGC_COLLECT_LEN);
    p += LUAGC_COLLECT_LEN;
    *p++ = 0xe9; put32(p, rel(p + 4, LUAGC_COLLECT + LUAGC_COLLECT_LEN));
    memset(b + 52, 0, 8);
    luagc_depth = (volatile int32_t *)(uintptr_t)depth;
    luagc_peak = (volatile uint32_t *)(uintptr_t)peak;
}

/* "level95" -> 95; anything else -1. */
static int level_of(const char *script) {
    if (strncmp(script, "level", 5) || !script[5]) return -1;
    int n = 0;
    for (const char *c = script + 5; *c; c++) {
        if (*c < '0' || *c > '9' || n > 9999) return -1;
        n = n * 10 + (*c - '0');
    }
    return n;
}

int luagc_on_script(uint32_t L, const char *script, int new_state, char *out, size_t cap) {
    int wrote = 0;
    if (cap) out[0] = 0;
    if (!luagc_peak) return 0;
    if (new_state) {
        if (state_level >= 0 && *luagc_peak && cap) {
            snprintf(out, cap, "[luagc] level %d: peak Lua memory %u KiB (cap %u)", state_level,
                     (*luagc_peak + 1023) / 1024, LUAGC_CAP / 1024);
            wrote = 1;
        }
        *luagc_peak = 0;
        state_level = -1;
    }
    if (state_level < 0 && script) state_level = level_of(script);
    if (L) {
        const uint32_t n = *(const uint32_t *)(uintptr_t)(L + 0x60);
        if (n > *luagc_peak) *luagc_peak = n;
    }
    return wrote;
}

/* Raises the Lua heap's maximum when its creation is the shipped code: 1, else 0 (nothing written). */
static int heap_apply(void) {
    uint32_t v;
    memcpy(&v, AT(LUAGC_HEAP_SITE + 1), 4);
    if (memcmp(AT(LUAGC_HEAP_CREATE), heap_head, sizeof heap_head) || v != LUAGC_HEAP_SHIPPED ||
        memcmp(AT(LUAGC_HEAP_SITE + 5), heap_tail, sizeof heap_tail) ||
        memcmp(AT(LUAGC_HEAP_SITE + 16), heap_store, sizeof heap_store))
        return 0;
    put32(AT(LUAGC_HEAP_SITE + 1), LUAGC_HEAP);
    return 1;
}

/* "; Lua heap ..." after the collector's part of the report. */
static void heap_report(int raised, char *report, size_t cap) {
    size_t n = strlen(report);
    if (raised)
        snprintf(report + n, cap - n, "; Lua heap %u -> %u KiB at %08x", LUAGC_HEAP_SHIPPED / 1024, LUAGC_HEAP / 1024,
                 LUAGC_HEAP_SITE);
    else
        snprintf(report + n, cap - n, "; Lua heap not raised: %08x is not the shipped code", LUAGC_HEAP_SITE);
}

int luagc_apply(uint8_t *stub, char *report, size_t cap) {
    if (!cap) return 0;
    report[0] = 0;
    const int heap = heap_apply();
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
        heap_report(heap, report, cap);
        return 0;
    }
    if (!stub) {
        snprintf(report, cap, "[luagc] not patched: no stub memory; Lua collections as shipped");
        heap_report(heap, report, cap);
        return 0;
    }
    build_stub(stub);
    for (int i = 0; i < LUAGC_CAP_SITES; i++) put32(AT(caps[i].va + caps[i].n), LUAGC_CAP);
    uint8_t jmp[LUAGC_COLLECT_LEN] = {0xe9};
    put32(jmp + 1, (uint32_t)(uintptr_t)stub - (LUAGC_COLLECT + LUAGC_COLLECT_LEN));
    memcpy(AT(LUAGC_COLLECT), jmp, sizeof jmp);
    snprintf(report, cap, "[luagc] Lua collections at %08x no longer nest; collection cap %u -> %u KiB (%d sites)",
             LUAGC_COLLECT, LUAGC_CAP_SHIPPED / 1024, LUAGC_CAP / 1024, LUAGC_CAP_SITES);
    heap_report(heap, report, cap);
    return 1;
}

void luagc_report_exit(void) {
    char line[128];
    if (luagc_on_script(0, NULL, 1, line, sizeof line)) printf("%s (at exit)\n", line);
    fflush(stdout);
}

int hle_luagc_install(void) {
    static int done;
    if (done++) return 0;
    char report[256];
    int n = luagc_apply(plat_alloc_exec(LUAGC_STUB_SIZE), report, sizeof report);
    uint32_t heap;
    memcpy(&heap, AT(LUAGC_HEAP_SITE + 1), 4);
    if (heap == LUAGC_HEAP || heap == LUAGC_HEAP_SHIPPED) mem_keep_last_freed(heap);   /* late reads of a closed state (hle_luagc.h) */
    printf("%s\n", report);
    return n;
}
