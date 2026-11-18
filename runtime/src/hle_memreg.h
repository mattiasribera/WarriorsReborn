/* Memory registry size (always on).
 *
 * The title's memory manager (class "PrivMemoryManager", global object 0x45f070, vtable
 * 0x437380, Memory\MemoryPriv.cpp) keeps every named heap and every loaded resource's memory
 * clump in an address-ordered registry. Each registration (vtable +0x6c, 0x245e20, called e.g.
 * by the ResourceMgr for each loaded resource: "xtxd\<hash>" texture dictionaries, "chars\N",
 * "objs\N", and the level's hashed resources) takes one 0x30-byte node from a fixed node pool
 * at [0x45f084]: {nodes, free stack, capacity +0x8, used +0xc}. The pool is built once by the
 * manager set-up (0x245f10): `push 0x200; mov ecx,eax; call 0x246190` at 0x2460c7, so 512
 * nodes. When the pool is empty the registration takes a NULL node and writes its name to
 * [0+0x2f]: access violation at EIP 0x245e53 (mov [esi+0x2f],bl), no check on the Xbox either.
 *
 * Measured headless (scratch runs, every gang unlocked as on a 100% save, Choose Gangs browsed
 * gang by gang): the title holds 92 registrations, the hub (level95) about 365, and Rumble's
 * Choose Gangs screen keeps the previewed gangs' warchiefs loaded while there is heap for them.
 * With the shipped main heap (45.4 MB) the game evicts previews and the count stays at or below
 * about 480. With --big-heap (implied by --fx-scale above 1, so by the Quality presets) the
 * 77.5 MB heap never forces an eviction, the count climbs by 10 to 25 per gang (510 at the 9th
 * gang, about 760 after all 30) and the 513th registration crashed: the port's larger heap
 * removed the memory pressure that kept the title under its 512 nodes. The mesh pack plays no
 * part (host-side draws only).
 *
 * The fix raises the pushed count to MEMREG_NODES. The pool constructor (0x246190) sizes both
 * of its arrays from that count (count * 0x30 node bytes, count * 4 free-stack bytes) from the
 * manager's own heap at start-up and nothing else reads 512, so the larger pool costs about
 * (MEMREG_NODES - 512) * 0x34 bytes of the main heap once. The instruction bytes around the
 * site are checked first; on any difference nothing is written. */
#ifndef HLE_MEMREG_H
#define HLE_MEMREG_H
#include <stddef.h>
#include <stdint.h>

#define MEMREG_NODES_SHIPPED 0x200
#define MEMREG_NODES 0x800              /* 2048 registry nodes: about 80 KB more of the main heap */
#define MEMREG_SITE 0x002460c7u         /* push 0x200 (68 00 02 00 00) */
#define MEMREG_IMM (MEMREG_SITE + 1)
#define MEMREG_MANAGER_POOL 0x0045f084u /* PrivMemoryManager (0x45f070) +0x14: the node pool */

/* Raises the node count in memory where guest address va is at host address va + bias (bias 0:
 * the identity-mapped guest), after checking the shipped bytes around the site. Writes a
 * "[memreg] ..." line (no newline) into report. Returns 1 when patched, 0 otherwise. */
int memreg_apply(intptr_t bias, char *report, size_t cap);

/* The node pool's used count and capacity, once the manager has built it: 1, or 0 (both 0)
 * before. bias as above. */
int memreg_read(intptr_t bias, uint32_t *used, uint32_t *cap);

/* memreg_apply on the loaded title; prints the line. */
int hle_memreg_install(void);
#endif
