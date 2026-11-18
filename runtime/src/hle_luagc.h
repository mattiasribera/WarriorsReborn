/* Lua garbage collector: no nested collection, and a larger collection cap (on with mods).
 *
 * The title's Lua 4.0 is Rockstar's build with a memory budget: every place that sets the
 * collection threshold (L->GCthreshold, lua_State +0x5c; L->nblocks is +0x60) clamps it to
 * 0xa7000 bytes (668 KiB):
 *   lua_setgcthreshold          0x22f450  threshold = min(kb * 1024, cap)
 *   aux_do (after parsing)      0x233d43  threshold += new code, at most cap
 *   callgcTMudata               0x234880  threshold = min(2 * nblocks, cap) "avoid GC in tag methods"
 *   luaC_collectgarbage (end)   0x234bcb  threshold = min(nblocks + 64 KiB, cap)
 * Stock Lua 4.0 has no cap. The retail scripts keep each level's state under it. With the
 * bundled mods loaded (their code, tables and strings live in the same state) the hub and Coney
 * states pass it, and then:
 *   - callgcTMudata (0x234880) runs the userdata "gc" tag methods in the middle of a collection,
 *     after marking and before the tables are swept, with the threshold clamped below nblocks.
 *     The first luaD_call back from a tag method checks the threshold (0x233c88 -> 0x234c00)
 *     and starts a second, nested luaC_collectgarbage (0x234b60) on top of the first one's
 *     marks. A table the outer pass already marked (its mark field is a list link, not itself)
 *     counts as done and is not traversed again, so the nested pass never visits the globals
 *     table, sweeps tables only the globals hold (seen: the game's StringHolder, the temp
 *     string store of global.lua) and frees them while they are still referenced.
 *   - The next write to such a table (luaV_settable 0x23ce00 -> luaH_set -> resize) frees its
 *     node vector a second time: the guest heap of the state is corrupt and the game faults
 *     later in RtlFreeHeap (EIP 0x343e75, a free-list link that is text or a Lua number), in
 *     the table code (EIP 0x23b8ce, a new node vector laid over a live table) or in
 *     luaG_ordererror (EIP 0x23350b, a compare with a garbage type tag).
 *   - After each collection the threshold is the cap, below nblocks: every luaD_call collects.
 * Which mod tips a state over the cap only decides where it shows (police-memory at the hub,
 * protection-money at Coney, any 13 together); a mod with nothing but function definitions
 * did it as well.
 *
 * The fix:
 *   - luaC_collectgarbage (0x234b60, the one entry of every collection: luaC_checkGC's jmp at
 *     0x234c10 and aux_do's call at 0x233d20) goes through a host stub that returns at once
 *     while a collection is running (a depth count), so a collection can never nest. The stub
 *     runs the 5 replaced bytes (push esi; mov esi,[esp+8]) and jumps back to 0x234b65.
 *   - The four clamps take LUAGC_CAP instead of 0xa7000, so a modded state gets Lua's own
 *     thresholds (2 x nblocks during tag methods, nblocks + 64 KiB after a collection) instead
 *     of collecting on every call.
 * Every site's bytes are checked first; on any difference nothing is written. */
#ifndef HLE_LUAGC_H
#define HLE_LUAGC_H
#include <stddef.h>
#include <stdint.h>

#define LUAGC_COLLECT 0x00234b60u   /* luaC_collectgarbage(L): push esi; mov esi,[esp+8] */
#define LUAGC_COLLECT_LEN 5
#define LUAGC_CHECK 0x00234c00u     /* luaC_checkGC(L): ... jmp luaC_collectgarbage */
#define LUAGC_CAP_SHIPPED 0xa7000u
#define LUAGC_CAP 0x400000u         /* 4 MiB: the bundled mods together stay far below */
#define LUAGC_CAP_SITES 8
#define LUAGC_STUB_SIZE 64

/* The stub's collection depth (0 outside a collection). */
extern volatile int32_t *luagc_depth;

/* Checks every site and, when all match, writes the guard (its code in stub, LUAGC_STUB_SIZE
 * bytes of executable memory at a 32-bit address) and the caps. Writes a "[luagc] ..." line
 * (no newline) into report. Returns 1 when patched, 0 when nothing was written. */
int luagc_apply(uint8_t *stub, char *report, size_t cap);

/* luagc_apply on the loaded title (once); prints the line. */
int hle_luagc_install(void);
#endif
