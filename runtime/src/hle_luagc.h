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
 * Every site's bytes are checked first; on any difference nothing is written.
 *
 * Lua heap: the memory manager (PrivMemoryManager 0x45f070) creates each Lua state's heap with
 * vtable +0x58 (0x245cc0, called at 0x2927d0): HeapCreate(0, 0, 0x200000), a growable heap of at
 * most 2 MiB, at manager +0x60; +0x5c (0x2458b0) destroys it with the state. Lua's allocator
 * luaM_realloc (0x236a00) takes every block from it and, when HeapAlloc fails, writes to address 0
 * on purpose (`mov byte [0],0` at 0x236a46): the title's out-of-Lua-memory abort. The retail
 * scripts stay under 700 KiB, but the collection cap above lets a modded state grow to 4 MiB, and
 * the bundled mods' own data already pass 2 MiB at the front end: all 15 together fault at
 * 0x236a46 right after config_preload2 (the 73 KB code block of the next script cannot be
 * allocated; peak 1954 KiB of Lua blocks, plus the heap's headers and fragmentation). Which mod's
 * tables tip it over only decides when it shows (Dirty Fights 1.4's knock-off poses with More hats
 * fly were the first). The fix raises the pushed maximum to LUAGC_HEAP (the heap reserves that
 * range from the guest arena once per Lua state and frees it with the state; pages the state never
 * touches take no host memory). Its bytes are checked on their own; on any difference the heap is
 * left as shipped and the collector patch still applies.
 *
 * Closed states are still read: going from a Rumble match (level102) to the hub (level95), the
 * text measure 0x964a0 (called from the text draw 0x96660) reads the opponent gang's name, a
 * string of the Rumble state ("BASEBALL FURIES", 15 bytes), from about frame 25 to 55 after the
 * hub's state was created, long after HeapDestroy released the Rumble heap (NtFreeVirtualMemory
 * MEM_RELEASE: guest_free unmaps it). That faults at EIP 0x96548 (2 of 3 runs with all 15 mods
 * and the raised heap). The shipped 2 MiB heap is its own region as well and reads the same way
 * (seen with 8 mods and the heap left as shipped); it only stayed unnoticed. So the last freed
 * Lua heap (its size as patched or as shipped) stays mapped with its bytes until the next Lua
 * heap is freed (mem_keep_last_freed, mem.h): one extra heap-sized range of address space,
 * holding only the pages the old state touched.
 *
 * Headroom log: the stub also keeps the largest L->nblocks (+0x60, the state's Lua memory in bytes)
 * seen at the start of a collection, the moment the state is fullest; the mods hook adds a sample
 * at every script load. When the next Lua state boots (a new level), the old one's peak is logged:
 * `[luagc] level N: peak Lua memory X KiB (cap 4096)` (N from the state's first levelNN script;
 * level100 = the menus); the last state's line is written at exit, ending in "(at exit)". */
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
#define LUAGC_STUB_SIZE 96
#define LUAGC_HEAP_CREATE 0x00245cc0u /* PrivMemoryManager vtable +0x58: creates the Lua heap */
#define LUAGC_HEAP_SITE 0x00245cc1u   /* push 0x200000 (68 00 00 20 00): HeapCreate's dwMaximumSize */
#define LUAGC_HEAP_SHIPPED 0x200000u
#define LUAGC_HEAP 0x800000u          /* 8 MiB: twice the collection cap */

/* The stub's collection depth (0 outside a collection) and the largest nblocks it saw. */
extern volatile int32_t *luagc_depth;
extern volatile uint32_t *luagc_peak;

/* After the game loaded `script` in L (new_state: the first script of that state): the previous
 * state's peak line goes to out (returns 1; else 0 and out is ""), then L's nblocks is sampled. */
int luagc_on_script(uint32_t L, const char *script, int new_state, char *out, size_t cap);
/* At the game's exit (the frame limit or a window close): the last state's line, "(at exit)". */
void luagc_report_exit(void);

/* Checks every site and, when all match, writes the guard (its code in stub, LUAGC_STUB_SIZE
 * bytes of executable memory at a 32-bit address) and the caps; the Lua heap's maximum is raised
 * when its own bytes match. Writes a "[luagc] ..." line (no newline) into report. Returns 1 when
 * the collector was patched, 0 when it was not. */
int luagc_apply(uint8_t *stub, char *report, size_t cap);

/* luagc_apply on the loaded title (once); prints the line. */
int hle_luagc_install(void);
#endif
