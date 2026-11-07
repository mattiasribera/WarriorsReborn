/* Guest address-space management. All guest-visible memory is identity mapped. */
#ifndef MEM_H
#define MEM_H
#include <stdint.h>
#include <stddef.h>

#define GUEST_ARENA_START 0x10000000u   /* NtAllocateVirtualMemory, stacks, pools */
#define GUEST_ARENA_END 0x70000000u
/* The first 512 MB of the arena are reserved at start-up so host allocations (Windows places
 * them bottom-up, inside the arena) cannot take them; the guest stays below 16 MB of it through
 * boot and the start of mission 14 with every option on. The rest stays shared with the host. */
#define GUEST_ARENA_RESERVED_END 0x30000000u
#define CONTIG_BASE 0x80000000u        /* "physical" memory window: VA = 0x80000000 | PA */
#define CONTIG_SIZE 0x08000000u        /* 128 MB (devkit-sized) */

void mem_init(void);
/* Page-granular, zeroed, RWX allocation inside the guest arena. */
void *guest_alloc(size_t size);
/* Maps at a fixed guest address; returns NULL if taken. */
void *guest_alloc_at(uint32_t addr, size_t size);
int guest_free(uint32_t addr, uint32_t *size_out);
/* Finds the allocation containing addr; returns its base (0 if none). */
uint32_t guest_region(uint32_t addr, uint32_t *size_out);
void guest_track(uint32_t base, uint32_t size);
uint32_t contig_alloc(uint32_t size, uint32_t align, uint32_t lowest, uint32_t highest);
uint32_t contig_size(uint32_t addr);
/* Bytes of the contiguous window handed out so far (its bump pointer; it never goes down). */
uint32_t contig_top(void);
int mem_is_mapped(uint32_t addr);
/* Guest arena bytes in tracked regions now, and the arena's size ([crash-context]). */
uint64_t mem_arena_used(void);
uint64_t mem_arena_total(void);
/* Guest and contiguous allocations from this size on leave a breadcrumb (diaglog.h). */
#define MEM_LARGE_ALLOC (4u << 20)
#endif
