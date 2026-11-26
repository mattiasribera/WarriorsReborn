/* Address space of this process by owner, one [mem-map] line (--mem-map-every; the replay's
 * --mem-map): Linux /proc/self/maps below 4 GB (guest ranges, the GPU driver's libraries, other
 * libraries, device mappings, the brk heap, other anonymous memory, files), Windows VirtualQuery
 * (guest, driver and other images, mapped, private, reserved). Plus the free total and the
 * largest hole. */
#ifndef MEM_MAP_H
#define MEM_MAP_H
/* guest_ranges: count anonymous memory in the runtime's fixed guest ranges as the guest's */
void mem_map_report(const char *why, int guest_ranges);
#endif
