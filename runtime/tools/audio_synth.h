/* Synthetic XACT 1 banks for the game-independent audio tests (layouts as documented in
 * src/xact.c). Wave bank "synth": entry 0 = 16-bit PCM mono 24000 Hz (pcm_frames samples of
 * a ramp), entry 1 = Xbox ADPCM stereo 44100 Hz (2 blocks). Sound bank: 3 cues over 3 sound
 * entries naming the wave bank "synth": cue 0 = one track with a play-wave event (entry 0,
 * play once), cue 1 = simple entry (flag 0x08, entry 1), cue 2 = play-wave event looping
 * forever (entry 0), volume 25 * -0.16 dB. */
#ifndef AUDIO_SYNTH_H
#define AUDIO_SYNTH_H
#include <stdint.h>
#include <string.h>

static void sy_w32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void sy_w16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

/* Builds the wave bank into buf (>= 4096 bytes); returns its size. */
static uint32_t synth_xwb(uint8_t *buf, int pcm_frames) {
    memset(buf, 0, 4096);
    memcpy(buf, "WBND", 4);
    sy_w32(buf + 4, 3);
    sy_w32(buf + 8, 0x28);  sy_w32(buf + 0x0c, 0x28);   /* bank data */
    sy_w32(buf + 0x10, 0x50); sy_w32(buf + 0x14, 2 * 0x18);   /* entries */
    uint32_t data = 0x80, pcm_bytes = (uint32_t)pcm_frames * 2, adpcm_bytes = 2 * 72;
    sy_w32(buf + 0x20, data); sy_w32(buf + 0x24, pcm_bytes + adpcm_bytes);
    uint8_t *bd = buf + 0x28;
    sy_w32(bd, 0); sy_w32(bd + 4, 2); memcpy(bd + 8, "synth", 5);
    sy_w32(bd + 0x18, 0x18); sy_w32(bd + 0x1c, 0x40); sy_w32(bd + 0x20, 4);
    uint8_t *e = buf + 0x50;
    sy_w32(e + 4, 0u << 0 | 1u << 2 | 24000u << 5 | 1u << 31);   /* PCM, mono, 24000, 16-bit */
    sy_w32(e + 8, 0); sy_w32(e + 12, pcm_bytes);
    e += 0x18;
    sy_w32(e + 4, 1u << 0 | 2u << 2 | 44100u << 5);              /* ADPCM, stereo, 44100 */
    sy_w32(e + 8, pcm_bytes); sy_w32(e + 12, adpcm_bytes);
    for (int i = 0; i < pcm_frames; i++) sy_w16(buf + data + 2 * i, (uint16_t)(i * 16));
    uint8_t *a = buf + data + pcm_bytes;   /* ADPCM: headers 1000 / -1000, step 10, nibbles 0x1 / 0x9 */
    for (int b = 0; b < 2; b++, a += 72) {
        sy_w16(a, 1000); a[2] = 10; sy_w16(a + 4, (uint16_t)-1000); a[6] = 10;
        for (int k = 0; k < 8; k++) { memset(a + 8 + 8 * k, 0x11, 4); memset(a + 12 + 8 * k, 0x99, 4); }
    }
    return data + pcm_bytes + adpcm_bytes;
}

/* Builds the sound bank into buf (>= 1024 bytes); returns its size. */
static uint32_t synth_xsb(uint8_t *buf) {
    memset(buf, 0, 1024);
    memcpy(buf, "SDBK", 4);
    sy_w16(buf + 4, 11);
    const int cues = 3, entries = 3;
    uint32_t names = 0x38 + (cues + entries) * 0x14, tracks = names + 16, events = tracks + 8;
    sy_w32(buf + 8, names);
    sy_w16(buf + 0x1c, entries); sy_w16(buf + 0x1e, cues); sy_w16(buf + 0x22, 1);
    memcpy(buf + 0x28, "synthsb", 7);
    for (int c = 0; c < cues; c++) sy_w16(buf + 0x38 + c * 0x14 + 2, (uint16_t)c);
    uint8_t *e = buf + 0x38 + cues * 0x14;
    sy_w32(e, tracks); sy_w16(e + 4, 25); e[8] = 1; e[9] = 0xff;            /* entry 0: one track */
    e += 0x14;
    sy_w16(e, 1); sy_w16(e + 2, 0); e[9] = 0xff; e[0xb] = 0x08;               /* entry 1: simple */
    e += 0x14;
    sy_w32(e, tracks + 4); e[8] = 1; e[9] = 0xff;                              /* entry 2: looping track */
    memcpy(buf + names, "synth", 5);
    sy_w32(buf + tracks, 1u | events << 8);
    sy_w32(buf + tracks + 4, 1u | (events + 12) << 8);
    uint8_t *ev = buf + events;                                                /* play wave 0 of bank 0, once */
    ev[4] = 4; sy_w16(ev + 6, 0); sy_w16(ev + 8, 0); sy_w16(ev + 10, 0);
    ev += 12;                                                                   /* play wave 0, loop forever */
    ev[4] = 4; sy_w16(ev + 6, 0xFFFF); sy_w16(ev + 8, 0); sy_w16(ev + 10, 0);
    return events + 24;
}
#endif
