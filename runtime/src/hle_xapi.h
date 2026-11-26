/* HLE of XAPI timing: the title's statically linked QueryPerformanceCounter reads the CPU
 * time-stamp counter (rdtsc) while its QueryPerformanceFrequency returns the constant
 * 733333333, the Xbox's 733 MHz CPU clock. Run natively on a host CPU, rdtsc ticks at the host
 * rate (several GHz), so every QueryPerformanceCounter-based duration in the title came out
 * several times too long. The replacement derives the counter from the host monotonic clock at
 * the Xbox rate. By default the counter is frame-locked (src/pacing.h, hle_xapi.c): it advances
 * one 60 Hz period per presented frame while the host keeps up, so the title's fixed-step loop
 * runs its step pair on every other presented frame. */
#ifndef HLE_XAPI_H
#define HLE_XAPI_H
#include "hle_audio.h"   /* HleLookup */
#include <stdint.h>

#define XBOX_TSC_FREQUENCY 733333333u    /* XAPILIB QueryPerformanceFrequency (0x340109) */
#define XAPI_QPC_ADDR 0x003400f8u        /* XAPILIB__QueryPerformanceCounter */

uint64_t xapi_tsc_from_time(uint64_t sec, uint32_t nsec);   /* host time -> Xbox TSC ticks */
int hle_xapi_install(HleLookup lookup);                      /* number of entry points patched */
extern int hle_xapi_frame_lock;                              /* 1 (default): frame-locked counter; --no-frame-lock: 0 */
/* The title's clock in seconds: what QueryPerformanceCounter returns (frame-locked by default). */
double hle_xapi_seconds(void);
void hle_xapi_frame_presented(void);                         /* Swap: a frame was presented (after the 60 Hz pacing) */
int hle_xapi_frame_lock_stats(uint64_t *presents, uint64_t *resyncs);   /* 0 when the lock is off */
/* 1: presents come unpaced (--fast-boot, src/gamectl.h): with the frame lock the counter still
 * advances exactly one period per present (a virtual clock ahead of real time; it stays ahead when
 * this goes back to 0). */
extern int hle_xapi_unthrottled;
/* Photo mode (src/hle_photo.h): 1 holds the title's clock where it is (the game thread is frozen),
 * 0 lets it run on from that value: the time held is taken out of the clock, so the game sees no
 * jump when it resumes. */
void hle_xapi_clock_hold(int on);
#endif
