/* Game-independent test of the Lua collector guard (hle_luagc.h): the shipped bytes of
 * luaC_checkGC, the four threshold clamps and the head of luaC_collectgarbage are planted at
 * their guest addresses; the planted collectgarbage counts its runs and, on its first run, calls
 * itself once more (a nested collection, as a "gc" tag method's luaD_call does in the title). */
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

int main(void) {
    void *m = mmap(AT(0x22f000), 0x7000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m != AT(0x22f000)) { perror("mmap"); return 1; }
    CollectFn collect = (CollectFn)(uintptr_t)LUAGC_COLLECT;
    char report[192];

    plant();
    runs = 0;
    collect(0x1234);
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

    /* patched */
    plant();
    CHECK(luagc_apply(stub, report, sizeof report) == 1, "patched: %s", report);
    CHECK(caps_are(LUAGC_CAP), "all 8 clamps now read %u", LUAGC_CAP);
    CHECK(AT(LUAGC_COLLECT)[0] == 0xe9, "collectgarbage enters the stub");
    runs = 0;
    seen_L = 0;
    collect(0x5678);
    CHECK(runs == 1, "the nested collection returns at once (%u runs)", runs);
    CHECK(seen_L == 0x5678, "the collection gets its lua_State (%08x)", seen_L);
    CHECK(*luagc_depth == 0, "depth back to 0 after the collection");
    runs = 1;   /* the planted body nests only on its first run: now a plain collection */
    collect(0x9abc);
    CHECK(runs == 2 && seen_L == 0x9abc && *luagc_depth == 0, "a later collection runs normally");
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures != 0;
}
