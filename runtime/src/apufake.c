/* Experimental (--fake-mmio only): keeps a few MCPX APU / ACI registers in the RAM-backed MMIO
 * window at "ready" values so DirectSound's init polls complete. Not an APU emulation. */
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

#define APU(off) (*(volatile uint32_t *)(uintptr_t)(0xFE800000u + (off)))

static void *apu_thread(void *arg) {
    for (;;) {
        APU(0x20010) = 0x80;   /* polled by CMcpxCore init: (v & ~3) >= 0x20 */
        *(volatile uint32_t *)(uintptr_t)0xFEC00130u |= 0x100;   /* ACI GLOB_STA: primary codec ready */
        usleep(100);
    }
    return arg;
}

void apu_fake_start(void) {
    pthread_t t;
    pthread_create(&t, NULL, apu_thread, NULL);
}
