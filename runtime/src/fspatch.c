/* Guest fs:[disp32] instruction patching (see fspatch.h). */
#include "fspatch.h"
#include "platform.h"
#include <string.h>

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* modrm with mod=00, rm=101: an absolute disp32 operand. */
static int abs_modrm(uint8_t m) { return (m & 0xC7) == 0x05; }

int fspatch_decode(const uint8_t *p, uint32_t n, FsSite *s) {
    if (n < 6 || p[0] != 0x64) return 0;
    memset(s, 0, sizeof *s);
    switch (p[1]) {
    case 0xA1: s->kind = FS_LOAD; s->op = 0x8B; s->reg = EAX; s->len = 6; s->disp = rd32(p + 2); break;   /* mov eax, fs:[d] */
    case 0xA3: s->kind = FS_STORE; s->op = 0x89; s->reg = EAX; s->len = 6; s->disp = rd32(p + 2); break;  /* mov fs:[d], eax */
    case 0x8B: case 0x3B:   /* mov r, fs:[d] / cmp r, fs:[d] */
    case 0x89: case 0x39:   /* mov fs:[d], r / cmp fs:[d], r */
        if (n < 7 || !abs_modrm(p[2])) return 0;
        s->kind = (p[1] == 0x8B || p[1] == 0x3B) ? FS_LOAD : FS_STORE;
        s->op = p[1]; s->reg = (p[2] >> 3) & 7; s->len = 7; s->disp = rd32(p + 3);
        if (s->reg == ESP && s->op != 0x89) return 0;   /* only "mov fs:[d], esp" uses esp */
        break;
    case 0x0F:   /* movzx r, byte fs:[d] */
        if (n < 8 || p[2] != 0xB6 || !abs_modrm(p[3])) return 0;
        s->kind = FS_LOAD; s->op = 0xB6; s->reg = (p[3] >> 3) & 7; s->len = 8; s->disp = rd32(p + 4);
        if (s->reg == ESP) return 0;
        break;
    case 0xFF:   /* push dword fs:[d] */
        if (n < 7 || p[2] != 0x35) return 0;
        s->kind = FS_PUSH; s->len = 7; s->disp = rd32(p + 3); break;
    case 0x8F:   /* pop dword fs:[d] */
        if (n < 7 || p[2] != 0x05) return 0;
        s->kind = FS_POP; s->len = 7; s->disp = rd32(p + 3); break;
    default: return 0;
    }
    /* KPCR fields are dword aligned; unaligned matches are text (e.g. "d;\r\n" in XGRPH). */
    return s->disp < FSPATCH_MAX_DISP && !(s->disp & 3) ? s->len : 0;
}

int fspatch_scan(const uint8_t *code, uint32_t size, void (*fn)(const FsSite *, void *), void *ctx) {
    int count = 0;
    for (uint32_t i = 0; i < size;) {
        FsSite s;
        int len = fspatch_decode(code + i, size - i, &s);
        if (!len) { i++; continue; }
        s.offset = i;
        if (fn) fn(&s, ctx);
        count++;
        i += (uint32_t)len;
    }
    return count;
}

static int name_is(const char *a, const char *b) { return !strcmp(a, b); }
static int ends_with(const char *a, const char *suffix) {
    size_t n = strlen(a), m = strlen(suffix);
    return n >= m && !strcmp(a + n - m, suffix);
}

int fspatch_section_is_code(const char *name, uint32_t flags) {
    if (!(flags & 4)) return 0;   /* XBE section flag: executable */
    if (name_is(name, ".rdata") || name_is(name, ".data") || name_is(name, "DOLBY") || !strncmp(name, "$$", 2)) return 0;
    return !ends_with(name, "DATA") && !ends_with(name, "BSS");
}

/* ---- stub emission ---- */
typedef struct { uint8_t *p; } Emit;
static void b(Emit *e, uint8_t v) { *e->p++ = v; }
static void d32(Emit *e, uint32_t v) { memcpy(e->p, &v, 4); e->p += 4; }
static void push_r(Emit *e, int r) { b(e, 0x50 + r); }
static void pop_r(Emit *e, int r) { b(e, 0x58 + r); }
/* op r, [base + disp32] (base != esp) */
static void op_rm(Emit *e, uint8_t op, int r, int base, uint32_t disp) { b(e, op); b(e, 0x80 | r << 3 | base); d32(e, disp); }
/* op r, [esp + disp8] */
static void op_esp8(Emit *e, uint8_t op, int r, int8_t disp) { b(e, op); b(e, 0x40 | r << 3 | 4); b(e, 0x24); b(e, (uint8_t)disp); }
static void lea_esp(Emit *e, int8_t disp) { op_esp8(e, 0x8D, ESP, disp); }   /* lea esp, [esp + disp]: no flags */

static void load_kpcr(Emit *e, int r, int seg_fs, uint32_t ref) {
    if (seg_fs) b(e, 0x64);
    b(e, 0x8B); b(e, (uint8_t)(r << 3 | 5)); d32(e, ref);   /* mov r, [fs:]ref */
}

static int pick(int avoid1, int avoid2) {
    static const int order[] = {EBX, ESI, EDI, EDX, ECX, EAX};
    for (int i = 0; i < 6; i++)
        if (order[i] != avoid1 && order[i] != avoid2) return order[i];
    return EAX;
}

static void emit_stub(Emit *e, const FsSite *s, int seg_fs, uint32_t ref) {
    int S = pick(s->kind == FS_LOAD || s->kind == FS_STORE ? s->reg : -1, -1);
    int T = pick(S, s->reg);
    switch (s->kind) {
    case FS_LOAD:   /* mov/cmp/movzx reg, [kpcr + d] */
        push_r(e, S);
        load_kpcr(e, S, seg_fs, ref);
        if (s->op == 0xB6) b(e, 0x0F);
        op_rm(e, s->op, s->reg, S, s->disp);
        pop_r(e, S);
        break;
    case FS_STORE:
        if (s->reg == ESP) {   /* mov [kpcr + d], esp: the value is esp before our pushes */
            push_r(e, S);
            push_r(e, T);
            b(e, 0x8D); b(e, 0x40 | T << 3 | 4); b(e, 0x24); b(e, 8);   /* lea T, [esp + 8] */
            load_kpcr(e, S, seg_fs, ref);
            op_rm(e, 0x89, T, S, s->disp);
            pop_r(e, T);
            pop_r(e, S);
        } else {
            push_r(e, S);
            load_kpcr(e, S, seg_fs, ref);
            op_rm(e, s->op, s->reg, S, s->disp);
            pop_r(e, S);
        }
        break;
    case FS_PUSH:   /* esp -= 4; [esp] = [kpcr + d] */
        lea_esp(e, -4);
        push_r(e, S);
        load_kpcr(e, S, seg_fs, ref);
        op_rm(e, 0x8B, S, S, s->disp);
        op_esp8(e, 0x89, S, 4);   /* mov [esp + 4], S */
        pop_r(e, S);
        break;
    case FS_POP:    /* [kpcr + d] = [esp]; esp += 4 */
        push_r(e, S);
        push_r(e, T);
        op_esp8(e, 0x8B, T, 8);   /* mov T, [esp + 8] */
        load_kpcr(e, S, seg_fs, ref);
        op_rm(e, 0x89, T, S, s->disp);
        pop_r(e, T);
        pop_r(e, S);
        lea_esp(e, 4);
        break;
    }
}

#define STUB_MAX 48
#define CHUNK 0x10000

typedef struct {
    uint8_t *code, *stubs, *end;
    int seg_fs, failed, count;
    uint32_t ref;
} ApplyCtx;

static void apply_one(const FsSite *s, void *ctx_) {
    ApplyCtx *c = ctx_;
    if (c->failed) return;
    if (!c->stubs || c->end - c->stubs < STUB_MAX) {
        c->stubs = plat_alloc_exec(CHUNK);
        if (!c->stubs) { c->failed = 1; return; }
        c->end = c->stubs + CHUNK;
    }
    uint8_t *site = c->code + s->offset;
    Emit e = {c->stubs};
    emit_stub(&e, s, c->seg_fs, c->ref);
    b(&e, 0xE9);
    d32(&e, (uint32_t)((uintptr_t)(site + s->len) - (uintptr_t)(e.p + 4)));   /* jmp back */
    uint32_t rel = (uint32_t)((uintptr_t)c->stubs - (uintptr_t)(site + 5));
    site[0] = 0xE9;
    memcpy(site + 1, &rel, 4);
    memset(site + 5, 0x90, s->len - 5);
    c->stubs = e.p;
    c->count++;
}

int fspatch_apply(uint8_t *code, uint32_t size, int seg_fs, uint32_t kpcr_ref) {
    ApplyCtx c = {code, NULL, NULL, seg_fs, 0, 0, kpcr_ref};
    fspatch_scan(code, size, apply_one, &c);
    return c.failed ? -1 : c.count;
}
