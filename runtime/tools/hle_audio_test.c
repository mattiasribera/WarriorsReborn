/* Game-independent test of the HLE audio boundary (src/hle_audio.c). It maps the XBE's
 * audio-library address range, plants each entry's expected first bytes, installs the HLE
 * patches and calls the patched entry points with the guest calling convention, checking
 * return values, out-parameters and that every callee pops exactly its argument bytes. */
#define _GNU_SOURCE
#include "../src/hle_audio.h"
#include "../src/audio.h"
#include "audio_synth.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

/* uint32_t tcall(uint32_t fn, int nargs, const uint32_t *args, int32_t *esp_delta):
 * pushes args (stdcall order), calls fn and reports esp after return relative to before the
 * pushes; 0 means the callee popped exactly nargs*4 bytes. */
__asm__(".text\n.globl tcall\ntcall:\n"
        "push %ebp\npush %ebx\npush %esi\npush %edi\nmov %esp,%ebp\n"
        "mov 20(%ebp),%eax\nmov 24(%ebp),%ecx\nmov 28(%ebp),%esi\n"
        "1: test %ecx,%ecx\njz 2f\ndec %ecx\npushl (%esi,%ecx,4)\njmp 1b\n"
        "2: call *%eax\nmov %esp,%ecx\nsub %ebp,%ecx\nmov 32(%ebp),%edx\nmov %ecx,(%edx)\n"
        "mov %ebp,%esp\npop %edi\npop %esi\npop %ebx\npop %ebp\nret\n");
uint32_t tcall(uint32_t fn, int nargs, const uint32_t *args, int32_t *esp_delta);

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static uint32_t addr_of(const char *name) {
    for (int i = 0; i < hle_audio_entry_count(); i++)
        if (!strcmp(hle_audio_entry_name(i), name)) return hle_audio_entry_addr(i);
    printf("FAIL no entry %s\n", name);
    failures++;
    return 0;
}

static uint32_t call(const char *name, int nargs, const uint32_t *args) {
    int32_t delta = 1;
    uint32_t r = tcall(addr_of(name), nargs, args, &delta);
    CHECK(delta == 0, "%s pops %d bytes (esp delta %d)", name, nargs * 4, delta);
    return r;
}
#define A(...) (const uint32_t[]){__VA_ARGS__}
#define P(x) ((uint32_t)(uintptr_t)(x))

/* ---- parameter controls (XACT "V"/"F") ---- */
/* Mean absolute level over both channels of the next n frames, mixed here (no mixer thread). */
static double mix_level(int n) {
    int16_t *out = calloc((size_t)n * 2, sizeof *out);
    audio_lock();
    audio_mix(out, n);
    audio_unlock();
    double sum = 0;
    for (int i = 0; i < 2 * n; i++) sum += abs(out[i]);
    free(out);
    return sum / (2.0 * n);
}
static int near_ratio(double got, double want) { return fabs(got - want) <= 0.01 * want + 1e-4; }
static double db100(int mb) { return pow(10.0, mb / 2000.0); }

/* Control table and curves (layout in src/xact.h) after the bank's last byte: entry 0 "V",
 * entry 2 "V" and "F" with this title's curves; entry 1 has none. Returns the new length. */
static uint32_t add_controls(uint8_t *xsb, uint32_t len) {
    static const uint16_t v[8] = {655, (uint16_t)-4000, 3932, (uint16_t)-2443, 9830, (uint16_t)-1647, 65535, 0};
    static const uint16_t f[8] = {0, (uint16_t)-6400, 10695, (uint16_t)-2256, 32281, (uint16_t)-473, 65535, 0};
    static const struct { int sound; const char *name; const uint16_t *pts; } c[3] = {{0, "V", v}, {2, "V", v}, {2, "F", f}};
    uint32_t t = (len + 3) & ~3u, cv = t + 3 * 0x18;
    sy_w32(xsb + 0x14, t);
    sy_w16(xsb + 0x20, 3);
    for (int i = 0; i < 3; i++) {
        uint8_t *e = xsb + t + i * 0x18, *p = xsb + cv + i * 0x18;
        memset(e, 0, 0x18);
        memset(p, 0, 0x18);
        sy_w16(e, (uint16_t)c[i].sound);
        e[3] = 1;
        sy_w32(e + 4, cv + i * 0x18);
        e[8] = (uint8_t)c[i].name[0];
        for (int j = 0; j < 8; j++) sy_w16(p + 4 + 2 * j, c[i].pts[j]);
    }
    return cv + 3 * 0x18;
}

/* One control description entry as the title builds it (0x6dea0). */
static void ctl_entry(uint8_t *e, uint16_t type, uint16_t flags, uint16_t steps, float rate, float value, uint32_t cue,
                      const char *name) {
    memset(e, 0, 0x18);
    sy_w16(e, type);
    sy_w16(e + 2, flags);
    sy_w16(e + 6, steps);
    memcpy(e + 8, &rate, 4);
    memcpy(e + 12, &value, 4);
    sy_w32(e + 16, cue);
    sy_w32(e + 20, (uint32_t)(uintptr_t)name);
}

static void test_controls(uint32_t eng) {
    audio_close();   /* from here the test mixes by itself */
    static uint8_t xwb[16384], xsb[1024];
    uint32_t xwb_len = synth_xwb(xwb, 4000);
    for (int i = 0; i < 4000; i++) sy_w16(xwb + 0x80 + 2 * i, 16000);   /* constant level */
    uint32_t xsb_len = synth_xsb(xsb);
    /* Entry 0 becomes 3D (record 0: min 1, max 40) so the 3D path is covered too. */
    uint32_t t3d = (xsb_len + 3) & ~3u;
    sy_w32(xsb + 0x10, t3d);
    float fl[5] = {1, 40, 1, 1, 1};
    sy_w16(xsb + t3d, 360); sy_w16(xsb + t3d + 2, 360);
    memcpy(xsb + t3d + 8, fl, sizeof fl);
    xsb[0x38 + 3 * 0x14 + 0xb] |= 1;
    xsb_len = add_controls(xsb, t3d + 0x28);
    uint32_t wb = 0, bank = 0, cue = 0, zeros[4] = {0};
    call("IXACTEngine_RegisterWaveBank", 4, A(eng, P(xwb), xwb_len, P(&wb)));
    call("IXACTEngine_CreateSoundBank", 4, A(eng, P(xsb), xsb_len, P(&bank)));
    mix_level(48000);   /* settle the limiter */
    static const char vn[] = "V", fn[] = "F", pn[] = "P";
    static uint8_t ents[4 * 0x18];
    uint32_t desc[2] = {2, P(ents)};

    /* References with the kill switch: every cue at its entry volume. */
    hle_audio_set_xact_controls(0);
    CHECK(!hle_audio_xact_controls(), "kill switch: controls off");
    ctl_entry(ents, 1, 0, 0, 0, 0.18f, 0, vn);
    ctl_entry(ents + 0x18, 1, 0, 0, 0, 1.0f, 0, fn);
    uint32_t pp[4] = {8, 2, 0, P(desc)}, pe[4] = {4, 2, 0, 0};
    call("IXACTSoundBank_Prepare?", 3, A(bank, P(pp), P(&cue)));
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pe), P(&cue)));
    double ref2 = mix_level(480);
    CHECK(ref2 > 1000, "controls off: looping 2D cue 2 plays at its entry volume despite V 0.18 in Prepare (level %.0f)", ref2);
    desc[0] = 1;
    ctl_entry(ents, 1, 1, 0, 0, 0.0f, cue, fn);
    call("IXACTEngine_SetParameterControl?", 2, A(eng, P(desc)));
    CHECK(near_ratio(mix_level(480), ref2), "controls off: SetParameterControl(F 0) is ignored");
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    uint32_t pl[4] = {0, 0, 0, 0}, pl1[4] = {0, 1, 0, 0};
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue)));
    double ref0 = mix_level(480);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    mix_level(48000);   /* the ADPCM cue may engage the limiter: start both runs from rest */
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl1), P(&cue)));
    double ref1 = mix_level(480);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));

    hle_audio_set_xact_controls(1);
    CHECK(hle_audio_xact_controls(), "controls on (the default)");
    /* Prepare with V 0.18 / F 1 (the title's ambient bed): V curve -1589, F curve 0 -> -15.89 dB. */
    desc[0] = 2;
    ctl_entry(ents, 1, 0, 0, 0, 0.18f, 0, vn);
    ctl_entry(ents + 0x18, 1, 0, 0, 0, 1.0f, 0, fn);
    call("IXACTSoundBank_Prepare?", 3, A(bank, P(pp), P(&cue)));
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pe), P(&cue)));
    double got = mix_level(480);
    CHECK(near_ratio(got / ref2, db100(-1589)), "Prepare(V 0.18, F 1): cue at x%.4f of its entry volume (want x%.4f, -15.9 dB)",
          got / ref2, db100(-1589));
    /* The title's fade: SetParameterControl F per cue handle; then an XACT ramp of 3 steps of
     * 0.25 from 0.25, one step per DoWork: F -1787, -467, -234, 0 (+ V -1589). */
    desc[0] = 1;
    ctl_entry(ents, 1, 1, 0, 0, 0.5f, cue, fn);
    call("IXACTEngine_SetParameterControl?", 2, A(eng, P(desc)));
    got = mix_level(480);
    CHECK(near_ratio(got / ref2, db100(-1589 - 467)), "SetParameterControl(F 0.5) -> x%.4f (want x%.4f)", got / ref2, db100(-2056));
    ctl_entry(ents, 1, 1, 3, 0.25f, 0.25f, cue, fn);
    call("IXACTEngine_SetParameterControl?", 2, A(eng, P(desc)));
    static const int ramp[5] = {-1787, -467, -234, 0, 0};
    for (int k = 0; k < 5; k++) {
        if (k) call("XACTEngineDoWork", 0, zeros);
        got = mix_level(480);
        CHECK(near_ratio(got / ref2, db100(-1589 + ramp[k])), "F ramp 0.25 + 0.25 x step, after %d DoWork -> x%.4f (want x%.4f)", k,
              got / ref2, db100(-1589 + ramp[k]));
    }
    /* Names the sound lacks, other cues' handles, unknown handles and global controls change nothing. */
    desc[0] = 4;
    ctl_entry(ents, 1, 1, 0, 0, 0.0f, cue, pn);
    ctl_entry(ents + 0x18, 1, 1, 0, 0, 0.0f, P(zeros), vn);   /* not a cue object */
    ctl_entry(ents + 0x30, 0, 1, 0, 0, 0.0f, 0, "MasterVolume");
    ctl_entry(ents + 0x48, 1, 1, 0, 0, 0.0f, cue, NULL);
    call("IXACTEngine_SetParameterControl?", 2, A(eng, P(desc)));
    CHECK(near_ratio(mix_level(480) / ref2, db100(-1589)), "unmatched name, bad handle, global and nameless controls ignored");
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    /* PlayEx (without Prepare) with V 0.45 on the 3D cue 0: V curve -1066, kept by the per-frame 3D update. */
    desc[0] = 1;
    ctl_entry(ents, 1, 0, 0, 0, 0.45f, 0, vn);
    uint32_t plc[4] = {0, 0, 0, P(desc)};
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(plc), P(&cue)));
    got = mix_level(480);
    CHECK(near_ratio(got / ref0, db100(-1066)), "3D cue, PlayEx(V 0.45) -> x%.4f (want x%.4f)", got / ref0, db100(-1066));
    call("XACTEngineDoWork", 0, zeros);
    got = mix_level(480);
    CHECK(near_ratio(got / ref0, db100(-1066)), "3D cue after DoWork keeps its control volume (x%.4f)", got / ref0);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
    /* A cue whose sound has no controls is unchanged (cue 1, entry 1). */
    mix_level(48000);
    call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl1), P(&cue)));
    got = mix_level(480);
    CHECK(got == ref1, "cue without controls: same mix with controls on and off (%.1f / %.1f)", got, ref1);
    call("IXACTSoundBank_Stop?", 4, A(bank, 0xFFFFFFFFu, 0, 0));
}

int main(void) {
    const uint32_t lo = 0x3a7000, hi = 0x3db000;   /* DSOUND .. end of XACTENG */
    uint8_t *m = mmap((void *)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m != (void *)lo) { perror("mmap"); return 1; }
    memset(m, 0xCC, hi - lo);
    int n = hle_audio_entry_count();
    for (int i = 0; i < n; i++) {
        uint32_t a = hle_audio_entry_addr(i);
        CHECK(a >= lo && a + HLE_SIG_LEN <= hi, "%s address %08x inside the audio libraries", hle_audio_entry_name(i), a);
        memcpy((void *)(uintptr_t)a, hle_audio_entry_sig(i), HLE_SIG_LEN);
    }
    /* A mismatching entry must be left alone. */
    uint8_t *bad = (uint8_t *)(uintptr_t)addr_of("IDirectSoundBuffer_SetMixBins");
    bad[0] ^= 0xFF;
    uint8_t before[HLE_SIG_LEN];
    memcpy(before, bad, HLE_SIG_LEN);
    hle_audio_log_limit = 2;
    int patched = hle_audio_install(NULL);
    CHECK(patched == n - 1, "install patched %d of %d entries (one planted mismatch)", patched, n);
    CHECK(!memcmp(before, bad, HLE_SIG_LEN), "mismatching entry left unpatched");

    /* Every patched entry survives NULL/zero arguments and balances the stack. */
    uint32_t zeros[16] = {0};
    for (int i = 0; i < n; i++) {
        if (!strcmp(hle_audio_entry_name(i), "IDirectSoundBuffer_SetMixBins")) continue;
        call(hle_audio_entry_name(i), hle_audio_entry_argbytes(i) / 4, zeros);
    }

    /* XACT engine, synthetic banks, sources, cues and notifications. */
    CHECK(!strcmp(audio_open("null", NULL), "null"), "mixer started with the null backend");
    static uint8_t xwb[4096], xsb[1024];
    uint32_t xwb_len = synth_xwb(xwb, 500), xsb_len = synth_xsb(xsb);
    uint32_t params[8] = {0}, eng = 0, bank = 0, wb = 0, src = 0, cue = 0, cue2 = 0, status = 0xFFFFFFFF;
    CHECK(call("XACTEngineCreate", 2, A(P(params), P(&eng))) == 0 && eng, "XACTEngineCreate -> engine %08x", eng);
    CHECK(call("IXACTEngine_RegisterWaveBank", 4, A(eng, P(xwb), xwb_len, P(&wb))) == 0 && wb, "RegisterWaveBank(synthetic) -> %08x", wb);
    CHECK(call("IXACTEngine_CreateSoundBank", 4, A(eng, P(xsb), xsb_len, P(&bank))) == 0 && bank && bank != wb,
          "CreateSoundBank(synthetic) -> distinct bank %08x", bank);
    CHECK(call("IXACTEngine_CreateSoundSource", 3, A(eng, 2, P(&src))) == 0 && src, "CreateSoundSource -> %08x", src);
    uint32_t notif[10], any3[6] = {0x20003, bank, 0xFFFFFFFF, 0, 0, 0};
    CHECK(call("IXACTEngine_GetNotification", 3, A(eng, P(any3), P(notif))) == 0x80004005u,
          "GetNotification -> E_FAIL (no notification pending)");
    CHECK(call("IXACTSoundSource_GetStatus?", 2, A(src, P(&status))) == 0 && status == 0, "SoundSource GetStatus -> idle");
    /* Persistent "data available" registration (type 3, flags 0x8002) as the title makes it. */
    uint32_t reg3[6] = {0x80020003, bank, 0xFFFFFFFF, 0, 0, 0};
    call("IXACTEngine_RegisterNotification", 2, A(eng, P(reg3)));
    /* The title prepares without a source and names the source in PlayEx(flag 4); the cue must
     * then belong to that source, or GetStatus reports it idle and the title stops the cue at once. */
    uint32_t pp[4] = {8, 1, 0, 0};
    CHECK(call("IXACTSoundBank_Prepare?", 3, A(bank, P(pp), P(&cue))) == 0 && cue, "Prepare(cue 1) -> cue %08x", cue);
    CHECK(call("IXACTEngine_GetNotification", 3, A(eng, P(any3), P(notif))) == 0x80004005u,
          "prepared notification is not posted inside Prepare");
    call("XACTEngineDoWork", 0, zeros);
    memset(notif, 0xAB, sizeof notif);
    CHECK(call("IXACTEngine_GetNotification", 3, A(eng, P(any3), P(notif))) == 0 && notif[0] == 0x20003 && notif[1] == bank &&
          notif[2] == 1 && notif[3] == cue && notif[9] == 0, "after DoWork: type 3 notification for the prepared cue");
    CHECK(call("IXACTEngine_GetNotification", 3, A(eng, P(any3), P(notif))) == 0x80004005u, "the notification is consumed");
    uint32_t pe[4] = {4, 1, src, 0}, played = cue;
    CHECK(call("IXACTSoundBank_PlayEx", 3, A(bank, P(pe), P(&played))) == 0 && played == cue, "PlayEx(flag 4) plays the prepared cue");
    uint32_t reg1[6] = {0x40001, bank, 1, cue, 0, 0};
    call("IXACTEngine_RegisterNotification", 2, A(eng, P(reg1)));
    call("IXACTSoundSource_GetStatus?", 2, A(src, P(&status)));
    CHECK(status == 1, "SoundSource GetStatus while its cue plays -> 1");
    nanosleep(&(struct timespec){0, 60 * 1000 * 1000}, NULL);   /* cue 1: 128 frames at 44.1 kHz */
    CHECK(call("IXACTEngine_GetNotification", 3, A(eng, P(reg1), P(notif))) == 0 && notif[0] == 0x40001 && notif[3] == cue,
          "type 1 (stop) notification once the cue's wave ended");
    call("IXACTSoundSource_GetStatus?", 2, A(src, P(&status)));
    CHECK(status == 0, "SoundSource GetStatus after the cue ended -> 0");
    uint32_t pl[4] = {0, 2, src, 0};
    CHECK(call("IXACTSoundBank_PlayEx", 3, A(bank, P(pl), P(&cue2))) == 0 && cue2 && cue2 != cue, "PlayEx(cue 2, looping) -> %08x", cue2);
    nanosleep(&(struct timespec){0, 60 * 1000 * 1000}, NULL);
    call("IXACTSoundSource_GetStatus?", 2, A(src, P(&status)));
    CHECK(status == 1, "a looping cue keeps playing");
    call("IXACTSoundBank_Stop?", 4, A(bank, 2, 0, 0));
    call("IXACTSoundSource_GetStatus?", 2, A(src, P(&status)));
    CHECK(status == 0, "Stop(bank, cue index) stops it");
    CHECK(call("IXACTSoundBank_PlayEx", 3, A(bank, P(params), 0)) == 0, "PlayEx without ppCue (cue 0)");
    /* Cue properties as the XBE's own XACT code computes them (0x3d12a7): duration in ms at
     * +0x34, wave at +0x30, largest loop count at +0x24, track count at +0x28, volume at +0x08. */
    uint32_t props[25];
    memset(props, 0xAB, sizeof props);
    call("IXACTSoundBank_GetSoundCueProperties?", 3, A(bank, 0, P(props)));
    CHECK(props[0x34 / 4] == 20 && props[0x30 / 4] == 0 && props[0x24 / 4] == 0 && props[0x28 / 4] == 1 && props[0x08 / 4] == (uint32_t)-400 &&
          props[0] == 0 && props[24] == 0,
          "GetSoundCueProperties(cue 0): duration %u ms, wave %u, loops %u, tracks %u, volume %d", props[0x34 / 4], props[0x30 / 4],
          props[0x24 / 4], props[0x28 / 4], (int)props[0x08 / 4]);
    call("IXACTSoundBank_GetSoundCueProperties?", 3, A(bank, 2, P(props)));
    CHECK(props[0x24 / 4] == 0xFFFF && props[0x34 / 4] == 20, "GetSoundCueProperties(cue 2): loop count %x, duration %u ms",
          props[0x24 / 4], props[0x34 / 4]);
    CHECK(call("IXACTEngine_UnRegisterWaveBank", 2, A(eng, wb)) == 0, "UnRegisterWaveBank");

    /* DirectSound: a looping buffer whose play cursor advances with wall-clock time. */
    uint32_t ds = 0, buf = 0, wfx[5] = {0x00020001, 48000, 48000 * 4, 0x00100004, 0};
    uint32_t dsbd[6] = {24, 0, 0x10000, P(wfx), 0, 0};
    CHECK(call("DirectSoundCreate", 3, A(0, P(&ds), 0)) == 0 && ds, "DirectSoundCreate -> %08x", ds);
    CHECK(call("IDirectSound_CreateSoundBuffer", 4, A(ds, P(dsbd), P(&buf), 0)) == 0 && buf, "CreateSoundBuffer -> %08x", buf);
    uint32_t p1 = 0, n1 = 0, p2 = 1, n2 = 1;
    CHECK(call("IDirectSoundBuffer_Lock", 8, A(buf, 0, 0, P(&p1), P(&n1), P(&p2), P(&n2), 2)) == 0 && p1 && n1 == 0x10000 && !p2 && !n2,
          "Lock(entire buffer) -> %08x/%x", p1, n1);
    memset((void *)(uintptr_t)p1, 0x55, n1);   /* the region must be writable */
    call("IDirectSoundBuffer_Unlock", 5, A(buf, p1, n1, 0, 0));
    call("IDirectSoundBuffer_Play", 4, A(buf, 0, 0, 1));
    call("IDirectSoundBuffer_GetStatus", 2, A(buf, P(&status)));
    CHECK(status == 5, "GetStatus while looping -> PLAYING|LOOPING (%x)", status);
    uint32_t mixbins[2] = {0, 0}, pairs[4] = {0, 0, 1, 0};
    mixbins[0] = 2; mixbins[1] = P(pairs);
    CHECK(call("IDirectSoundBuffer_SetMixBinVolumes_8", 2, A(buf, P(mixbins))) == 0, "SetMixBinVolumes(front left/right)");
    call("IDirectSoundBuffer_SetVolume", 2, A(buf, (uint32_t)-600));
    nanosleep(&(struct timespec){0, 50 * 1000 * 1000}, NULL);
    uint32_t play = 0, write = 0;
    call("IDirectSoundBuffer_GetCurrentPosition", 3, A(buf, P(&play), P(&write)));
    CHECK(play > 0 && play < 0x10000 && play % 4 == 0, "play cursor advanced with the mixer to %x (frame aligned)", play);
    call("IDirectSoundBuffer_Stop", 1, A(buf));
    call("IDirectSoundBuffer_GetStatus", 2, A(buf, P(&status)));
    CHECK(status == 0, "GetStatus after Stop -> 0");

    /* --sfx-3d-volume <dB>: -40..+12, decimals allowed; junk and out-of-range values rejected. */
    static const struct { const char *s; int ok; float db; } dbs[] = {
        {"0", 1, 0}, {"-6", 1, -6}, {"-6.5", 1, -6.5f}, {"+3", 1, 3}, {"12", 1, 12}, {"-40", 1, -40}, {"-10.0", 1, -10},
        {"12.1", 0, 0}, {"-40.5", 0, 0}, {"100", 0, 0}, {"", 0, 0}, {"abc", 0, 0}, {"-6dB", 0, 0}, {"6 ", 0, 0},
        {"nan", 0, 0}, {"inf", 0, 0}, {"-inf", 0, 0},
    };
    for (size_t i = 0; i < sizeof dbs / sizeof *dbs; i++) {
        float db = 123;
        int r = hle_audio_parse_db(dbs[i].s, &db);
        CHECK(dbs[i].ok ? r == 0 && db == dbs[i].db : r != 0, "parse --sfx-3d-volume '%s' -> %s", dbs[i].s,
              dbs[i].ok ? "accepted" : "rejected");
    }
    hle_audio_set_sfx_3d_volume(-6);
    CHECK(fabsf(hle_audio_sfx_3d_gain() - 0.50119f) < 1e-4f, "-6 dB -> 3D gain x%.4f", hle_audio_sfx_3d_gain());
    hle_audio_set_sfx_3d_volume(0);
    CHECK(hle_audio_sfx_3d_gain() == 1.0f, "0 dB -> 3D gain x1");

    test_controls(eng);
    hle_audio_summary();
    printf(failures ? "FAILED: %d\n" : "ALL OK\n", failures);
    return failures != 0;
}
