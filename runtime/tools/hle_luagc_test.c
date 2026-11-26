/* Game-independent test of the Lua collector guard (hle_luagc.h): the shipped bytes of
 * luaC_checkGC, the four threshold clamps and the head of luaC_collectgarbage are planted at
 * their guest addresses; the planted collectgarbage counts its runs and, on its first run, calls
 * itself once more (a nested collection, as a "gc" tag method's luaD_call does in the title).
 * The memory manager's Lua heap creation (HeapCreate(0, 0, 2 MiB) at 0x245cc0) is planted too. */
#include "../src/hle_luagc.h"
#include "../src/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)
#define AT(a) ((uint8_t *)(uintptr_t)(a))

static volatile uint32_t runs, seen_L;

static void put32(uint32_t va, uint32_t v) { memcpy(AT(va), &v, 4); }

static const uint8_t check_code[] = {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x48, 0x60, 0x3b, 0x48, 0x5c, 0x72,
                                     0x09, 0x89, 0x44, 0x24, 0x04, 0xe9, 0x4b, 0xff, 0xff, 0xff, 0xc3};

static void plant(void) {
    memset(AT(0x22f000), 0xcc, 0x7000);
    /* the clamps: opcode bytes, then 0xa7000 */
    static const struct { uint32_t va; uint8_t n, op[3]; } c[] = {
        {0x22f45b, 1, {0x3d}}, {0x22f465, 3, {0xc7, 0x41, 0x5c}}, {0x233d4f, 2, {0x81, 0xf9}},
        {0x233d5a, 3, {0xc7, 0x46, 0x5c}}, {0x234888, 1, {0x3d}}, {0x23489a, 3, {0xc7, 0x46, 0x5c}},
        {0x234bd3, 1, {0x3d}}, {0x234bdd, 3, {0xc7, 0x46, 0x5c}}};
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        memcpy(AT(c[i].va), c[i].op, c[i].n);
        put32(c[i].va + c[i].n, LUAGC_CAP_SHIPPED);
    }
    memcpy(AT(LUAGC_CHECK), check_code, sizeof check_code);
    /* luaC_collectgarbage: push esi; mov esi,[esp+8]; push edi; mov eax,esi (the shipped head),
     * then: mov [seen_L],esi; inc [runs]; cmp [runs],1; jne done; push esi; call collect; add esp,4;
     * done: pop edi; pop esi; ret */
    uint8_t *p = AT(LUAGC_COLLECT);
    static const uint8_t head[] = {0x56, 0x8b, 0x74, 0x24, 0x08, 0x57, 0x8b, 0xc6};
    memcpy(p, head, sizeof head);
    p += sizeof head;
    *p++ = 0x89; *p++ = 0x35; memcpy(p, &(uint32_t){(uint32_t)(uintptr_t)&seen_L}, 4); p += 4;
    *p++ = 0xff; *p++ = 0x05; memcpy(p, &(uint32_t){(uint32_t)(uintptr_t)&runs}, 4); p += 4;
    *p++ = 0x83; *p++ = 0x3d; memcpy(p, &(uint32_t){(uint32_t)(uintptr_t)&runs}, 4); p += 4; *p++ = 0x01;
    *p++ = 0x75; *p++ = 9;
    *p++ = 0x56;
    *p++ = 0xe8; put32((uint32_t)(uintptr_t)p, LUAGC_COLLECT - ((uint32_t)(uintptr_t)p + 4)); p += 4;
    *p++ = 0x83; *p++ = 0xc4; *p++ = 0x04;
    *p++ = 0x5f; *p++ = 0x5e; *p++ = 0xc3;
}

/* PrivMemoryManager vtable +0x58 as shipped: push esi; push 0x200000; push 0; push 0; mov esi,ecx;
 * call HeapCreate; mov [esi+0x60],eax; pop esi; ret */
static const uint8_t heap_code[] = {0x56, 0x68, 0x00, 0x00, 0x20, 0x00, 0x6a, 0x00, 0x6a, 0x00, 0x8b, 0xf1,
                                    0xe8, 0x93, 0x0c, 0x10, 0x00, 0x89, 0x46, 0x60, 0x5e, 0xc3};
static void plant_heap(void) {
    memset(AT(0x245000), 0xcc, 0x1000);
    memcpy(AT(0x245cc0), heap_code, sizeof heap_code);
}
static uint32_t heap_max(void) {
    uint32_t x;
    memcpy(&x, AT(LUAGC_HEAP_SITE + 1), 4);
    return x;
}

static int caps_are(uint32_t v) {
    static const uint32_t imm[] = {0x22f45c, 0x22f468, 0x233d51, 0x233d5d, 0x234889, 0x23489d, 0x234bd4, 0x234be0};
    for (unsigned i = 0; i < sizeof imm / sizeof imm[0]; i++) {
        uint32_t x;
        memcpy(&x, AT(imm[i]), 4);
        if (x != v) return 0;
    }
    return 1;
}

typedef void (*CollectFn)(uint32_t L);

/* Fake lua_States: only nblocks (+0x60) is read. */
static uint32_t states[3][32];
static uint32_t state(int i, uint32_t nblocks) {
    states[i][0x60 / 4] = nblocks;
    return (uint32_t)(uintptr_t)states[i];
}

int main(void) {
    void *m = mmap(AT(0x22f000), 0x7000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m != AT(0x22f000)) { perror("mmap"); return 1; }
    m = mmap(AT(0x245000), 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m != AT(0x245000)) { perror("mmap"); return 1; }
    CollectFn collect = (CollectFn)(uintptr_t)LUAGC_COLLECT;
    char report[192];

    plant();
    runs = 0;
    collect(state(0, 0x1000));
    CHECK(runs == 2, "unpatched: the planted collection nests once (%u runs)", runs);

    /* any byte off: nothing is written */
    plant();
    AT(0x234bd3)[0] = 0x3c;
    uint8_t *stub = plat_alloc_exec(LUAGC_STUB_SIZE);
    CHECK(luagc_apply(stub, report, sizeof report) == 0 && strstr(report, "not patched") && strstr(report, "00234bd3"),
          "a different clamp is refused: %s", report);
    CHECK(AT(LUAGC_COLLECT)[0] == 0x56 && caps_are(LUAGC_CAP_SHIPPED), "nothing written after a refusal");
    plant();
    AT(LUAGC_CHECK + 16)[0] = 0xe8;
    CHECK(luagc_apply(stub, report, sizeof report) == 0 && strstr(report, "luaC_checkGC"), "a different checkGC is refused: %s", report);
    CHECK(caps_are(LUAGC_CAP_SHIPPED) && AT(LUAGC_COLLECT)[0] == 0x56, "caps and head unchanged");
    plant();
    CHECK(luagc_apply(NULL, report, sizeof report) == 0 && caps_are(LUAGC_CAP_SHIPPED), "no stub memory: nothing written");

    /* the Lua heap: a different creation is left alone, the collector is still patched */
    plant();
    plant_heap();
    AT(0x245cc0 + 17)[0] = 0x90;
    CHECK(luagc_apply(stub, report, sizeof report) == 1 && strstr(report, "Lua heap not raised") && strstr(report, "00245cc1"),
          "a different heap creation is refused: %s", report);
    CHECK(heap_max() == LUAGC_HEAP_SHIPPED, "the heap maximum is unchanged after a refusal (%x)", heap_max());

    /* patched */
    plant();
    plant_heap();
    CHECK(luagc_apply(stub, report, sizeof report) == 1, "patched: %s", report);
    CHECK(heap_max() == LUAGC_HEAP, "the Lua heap's maximum now reads %x", heap_max());
    CHECK(!memcmp(AT(0x245cc0), heap_code, 2) && !memcmp(AT(0x245cc6), heap_code + 6, sizeof heap_code - 6),
          "only the heap maximum changed");
    CHECK(strstr(report, "Lua heap 2048 -> 8192 KiB") != NULL, "the report names the heap: %s", report);
    CHECK(caps_are(LUAGC_CAP), "all 8 clamps now read %u", LUAGC_CAP);
    CHECK(AT(LUAGC_COLLECT)[0] == 0xe9, "collectgarbage enters the stub");
    runs = 0;
    seen_L = 0;
    const uint32_t L1 = state(1, 0x150000);
    collect(L1);
    CHECK(runs == 1, "the nested collection returns at once (%u runs)", runs);
    CHECK(seen_L == L1, "the collection gets its lua_State (%08x)", seen_L);
    CHECK(*luagc_peak == 0x150000, "the peak is the state's nblocks at the collection (%x)", *luagc_peak);
    CHECK(*luagc_depth == 0, "depth back to 0 after the collection");
    runs = 1;   /* the planted body nests only on its first run: now a plain collection */
    const uint32_t L2 = state(2, 0x90000);
    collect(L2);
    CHECK(runs == 2 && seen_L == L2 && *luagc_depth == 0, "a later collection runs normally");
    CHECK(*luagc_peak == 0x150000, "a smaller state keeps the peak (%x)", *luagc_peak);

    /* per-level lines: a state's first levelNN script names it; the next state's boot logs it */
    char line[128];
    *luagc_peak = 0;
    const uint32_t A = state(1, 0x10000);
    CHECK(luagc_on_script(A, "config_normal", 1, line, sizeof line) == 0 && !line[0], "the first state: no line yet");
    luagc_on_script(A, "global", 0, line, sizeof line);
    luagc_on_script(A, "level95", 0, line, sizeof line);
    luagc_on_script(A, "level95_chapter12", 0, line, sizeof line);
    states[1][0x60 / 4] = 0x2a0400;
    collect(A);
    luagc_on_script(A, "tag5", 0, line, sizeof line);
    const uint32_t B = state(2, 0x8000);
    CHECK(luagc_on_script(B, "config_normal", 1, line, sizeof line) == 1 &&
              !strcmp(line, "[luagc] level 95: peak Lua memory 2689 KiB (cap 4096)"), "next state logs: %s", line);
    CHECK(*luagc_peak == 0x8000, "the new state starts from its own size (%x)", *luagc_peak);
    CHECK(luagc_on_script(state(0, 0x4000), "enum_preload", 1, line, sizeof line) == 0 && !line[0],
          "a state with no levelNN script logs nothing");
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures != 0;
}
