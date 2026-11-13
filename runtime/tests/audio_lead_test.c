/* Game-independent test of the audio output lead (src/audio.c, --audio-lead-ms): the queue
 * target planner the mixer thread runs against the device queue, the lead and device period
 * sizes, and the DirectSound play cursor held back by the frames still queued for the device.
 * Prints "ALL OK" or FAIL lines. */
#define _GNU_SOURCE
#include "../src/audio.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

#define MS(x) ((uint64_t)(x) * 1000000ull)

/* A device that pulls whole periods from the queue on its own clock, driven with the planner the
 * way the mixer thread drives it (one planner call every 2 ms). Returns the underruns seen. */
static int simulate(uint32_t lead, uint32_t period, double device_speed, int ms, uint32_t *min_queued) {
    AudioLead l;
    audio_lead_init(&l, lead, period);
    uint64_t played = 0;
    uint32_t queued = 0;
    int underruns = 0;
    double dev_t = 0;   /* frames the device clock has consumed */
    *min_queued = UINT32_MAX;
    for (int t = 0; t <= ms; t += 2) {
        dev_t += 2 * AUDIO_RATE / 1000.0 * device_speed;
        while (dev_t - played >= period && t > 0) {   /* the device's period callback */
            uint32_t take = queued < period ? queued : period;
            queued -= take;
            played += period;
        }
        if (t > 100 && queued < *min_queued) *min_queued = queued;   /* the lowest level the device left */
        AudioLeadPlan p = audio_lead_plan(&l, queued, MS(t));
        underruns += p.underrun;
        if (p.send) queued += (uint32_t)p.chunks * AUDIO_CHUNK;
    }
    return underruns;
}

int main(void) {
    /* lead and period sizes */
    CHECK(audio_lead_period(50) == 1024, "50 ms lead requests 1024-frame device periods (got %u)", audio_lead_period(50));
    CHECK(audio_lead_period(20) == 256, "20 ms lead requests 256-frame periods (got %u)", audio_lead_period(20));
    CHECK(audio_lead_frames(50, 1024) == 2400, "50 ms lead = 2400 frames (got %u)", audio_lead_frames(50, 1024));
    CHECK(audio_lead_frames(20, 1024) == 2048, "a lead under two device periods is raised to two (got %u)",
          audio_lead_frames(20, 1024));
    CHECK(audio_lead_frames(0, 1024) == 0, "0 ms = no lead (the host-clocked queue)");

    /* prefill: the first call fills the queue up to the lead at once */
    AudioLead l;
    audio_lead_init(&l, 2400, 1024);
    AudioLeadPlan p = audio_lead_plan(&l, 0, MS(0));
    CHECK(p.chunks == 10 && p.send && !p.underrun, "prefill: 10 chunks queued, no underrun (chunks %d send %d underrun %d)",
          p.chunks, p.send, p.underrun);
    p = audio_lead_plan(&l, 2400, MS(2));
    CHECK(p.chunks == 0, "queue at the lead: nothing mixed (chunks %d)", p.chunks);
    /* the device pulled one period: mix ahead up to the lead again */
    p = audio_lead_plan(&l, 2400 - 1024, MS(22));
    CHECK(p.chunks == 5 && p.send && !p.underrun, "after a 1024-frame pull: 5 chunks top the queue up (chunks %d)", p.chunks);
    /* underrun: the queue ran dry; it is reported once and refilled to the lead in one go */
    p = audio_lead_plan(&l, 0, MS(80));
    CHECK(p.underrun && p.chunks == 10 && p.send, "empty queue: underrun reported, refilled with 10 chunks (underrun %d chunks %d)",
          p.underrun, p.chunks);

    /* stalled device: queue full and not consumed for longer than the stall limit -> the mixer
     * keeps the host clock (DirectSound cursors and cue ends advance) without queueing */
    audio_lead_init(&l, 2400, 1024);
    p = audio_lead_plan(&l, 0, MS(0));
    uint64_t mixed = (uint64_t)p.chunks * AUDIO_CHUNK;
    p = audio_lead_plan(&l, 2400, MS(100));
    CHECK(p.chunks == 0, "queue full, 100 ms without consumption: still waiting (chunks %d)", p.chunks);
    p = audio_lead_plan(&l, 2400, MS(300));
    CHECK(p.chunks == (int)((2400 + 300 * 48 - mixed) / AUDIO_CHUNK) && !p.send && !p.underrun,
          "stalled 300 ms: host-clocked chunks mixed and not queued (chunks %d send %d)", p.chunks, p.send);
    mixed += (uint64_t)p.chunks * AUDIO_CHUNK;
    p = audio_lead_plan(&l, 2400 - 1024, MS(302));
    CHECK(p.chunks == 5 && p.send, "the device resumes: top-up to the lead again (chunks %d send %d)", p.chunks, p.send);

    /* a second stall after a resume: the host-clock fallback must not count the frames mixed
     * host-only during the first stall (10 s stall, 1 s of real-time consumption, 2 s stall) */
    {
        audio_lead_init(&l, 2400, 1024);
        uint64_t m = 0, t;
        uint32_t q = 0;
        p = audio_lead_plan(&l, q, 0);
        m += (uint64_t)p.chunks * AUDIO_CHUNK;
        q = 2400;
        for (t = 100; t <= 10000; t += 100) { p = audio_lead_plan(&l, q, MS(t)); m += (uint64_t)p.chunks * AUDIO_CHUNK; }
        uint64_t t0 = t;
        for (; t <= t0 + 1000; t += 2) {
            q = q > 96 ? q - 96 : 0;
            p = audio_lead_plan(&l, q, MS(t));
            m += (uint64_t)p.chunks * AUDIO_CHUNK;
            if (p.send) q += (uint32_t)p.chunks * AUDIO_CHUNK;
        }
        uint64_t m1 = m, t1 = t;
        for (; t <= t1 + 2000; t += 2) { p = audio_lead_plan(&l, q, MS(t)); m += (uint64_t)p.chunks * AUDIO_CHUNK; }
        CHECK(m - m1 >= 84000 && m - m1 <= 96000 + 2400,
              "second 2 s stall after a 10 s stall and a resume: %llu frames mixed on the host clock (want ~96000)",
              (unsigned long long)(m - m1));
    }

    /* simulated devices over 10 s: on time, 0.5 % slow, 0.5 % fast: never an underrun, never a drop */
    static const double speeds[3] = {1.0, 0.995, 1.005};
    for (int i = 0; i < 3; i++) {
        uint32_t lo;
        int u = simulate(2400, 1024, speeds[i], 10000, &lo);
        CHECK(u == 0 && lo >= 2400 - 1024, "device at x%.3f for 10 s: %d underruns, queue never below %u frames", speeds[i], u, lo);
    }

    /* DirectSound play cursor held back by the queued frames */
    CHECK(audio_cursor_behind(40000, 65536, 4, 2400, 100000, 1) == 40000 - 9600, "looping: 2400 frames behind = 9600 bytes");
    CHECK(audio_cursor_behind(4000, 65536, 4, 2400, 100000, 1) == 65536 + 4000 - 9600, "looping: wraps to the buffer end");
    CHECK(audio_cursor_behind(4000, 65536, 4, 2400, 1000, 1) == 0,
          "looping: never further back than what was consumed since Play (1000 frames -> 0)");
    CHECK(audio_cursor_behind(4000, 65536, 4, 2400, 100000, 0) == 0, "one-shot: clamps at 0");
    CHECK(audio_cursor_behind(40000, 65536, 4, 0, 100000, 1) == 40000, "no lag: the mixer's cursor");
    CHECK(audio_cursor_behind(40000, 0, 4, 2400, 100000, 1) == 0, "empty buffer -> 0");
    CHECK(audio_cursor_behind(40002, 65538, 2, 3, 100000, 1) == 39996, "16-bit mono: 3 frames = 6 bytes");

    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures != 0;
}
