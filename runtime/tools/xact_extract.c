/* Helper of the audio extractor (tools/audio_extract): the XACT work that must match what the
 * runtime plays, done with the runtime's own parser and wave reader (src/xact.c).
 *
 *   xact_extract cues BANK.xsb   one line per wave of each cue: "cue<TAB>wave bank<TAB>wave"
 *   xact_extract decode          jobs on stdin, one per line: "BANK.xwb<TAB>index<TAB>OUT.wav";
 *                                one result line per job on stdout, in order:
 *                                "ok<TAB>frames<TAB>channels<TAB>rate" or "err<TAB>message"
 *
 * Output WAVs are 16-bit PCM (Xbox ADPCM decoded, 8-bit PCM widened), the whole wave once (loop
 * regions are not repeated). The parent folder of each output must exist. Banks are read with
 * positioned reads (streamed), so a 900 MB bank is never loaded whole. Consecutive jobs on the
 * same bank reuse its open handle. */
#include "../src/xact.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef O_BINARY
#define O_BINARY 0
#endif

typedef struct {
    uint32_t frames, rate;
    int channels;
} XxInfo;

/* Lists cue -> (wave bank, wave); returns the number of cues or -1 when the bank is unreadable. */
static int xx_cues(const char *path, FILE *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = len > 0 ? malloc((size_t)len) : NULL;
    int ok = d && fread(d, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    XsbBank sb;
    char err[128];
    if (!ok || xsb_parse(d, (size_t)len, &sb, err, sizeof err)) { free(d); return -1; }
    for (int c = 0; c < sb.cues; c++) {
        uint32_t rng = 1;   /* the cues of this title have one wave each; a random cue lists one pick */
        XsbSound s;
        if (xsb_cue_sound(&sb, (uint32_t)c, &rng, &s)) continue;
        for (int i = 0; i < s.nwaves; i++) {
            char name[17];
            fprintf(out, "%d\t%s\t%d\n", c, xsb_bank_name(&sb, s.w[i].bank, name), s.w[i].wave);
        }
    }
    free(d);
    return sb.cues;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static void wav_header(uint8_t h[44], int ch, uint32_t rate, uint32_t frames) {
    uint32_t data = frames * (uint32_t)ch * 2;
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + data); memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, (uint32_t)ch); put32(h + 24, rate);
    put32(h + 28, rate * (uint32_t)ch * 2); put16(h + 32, (uint32_t)ch * 2); put16(h + 34, 16);
    memcpy(h + 36, "data", 4); put32(h + 40, data);
}

/* The open bank of the last job (reused while jobs stay on the same file). */
static struct { char path[1024]; int fd; XwbBank wb; } cur = {"", -1, {0}};

static int open_bank(const char *path, char *err, size_t errlen) {
    if (cur.fd >= 0 && !strcmp(cur.path, path)) return 0;
    if (cur.fd >= 0) { xwb_free(&cur.wb); close(cur.fd); cur.fd = -1; }
    int fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) { snprintf(err, errlen, "cannot open %s", path); return -1; }
    if (xwb_open_stream(fd, 0, &cur.wb, err, errlen)) { close(fd); return -1; }
    cur.fd = fd;
    snprintf(cur.path, sizeof cur.path, "%s", path);
    return 0;
}

/* Decodes wave index of bank path into a 16-bit WAV at out; returns 0 or -1 with err set. */
static int xx_decode(const char *path, int index, const char *out, XxInfo *info, char *err, size_t errlen) {
    memset(info, 0, sizeof *info);
    if (open_bank(path, err, errlen)) return -1;
    if (index < 0 || index >= cur.wb.count) { snprintf(err, errlen, "wave %d out of range (%d)", index, cur.wb.count); return -1; }
    const XwbEntry *e = &cur.wb.entries[index];
    if ((e->codec != XWB_PCM && e->codec != XWB_XADPCM) || e->channels < 1 || e->channels > 2 || !e->rate) {
        snprintf(err, errlen, "unsupported wave (codec %d, %d channels)", e->codec, e->channels);
        return -1;
    }
    FILE *f = fopen(out, "wb");
    if (!f) { snprintf(err, errlen, "cannot write %s", out); return -1; }
    uint8_t h[44];
    wav_header(h, e->channels, e->rate, 0);
    fwrite(h, 1, sizeof h, f);   /* rewritten with the real length below */
    WaveReader *r = malloc(sizeof *r);
    if (!r) { fclose(f); snprintf(err, errlen, "out of memory"); return -1; }
    wave_reader_init(r, &cur.wb, e, 0);
    static int16_t buf[4096 * 2];
    uint8_t le[sizeof buf];
    uint32_t frames = 0;
    int got;
    while ((got = wave_reader_fill(r, buf, 4096)) > 0) {
        for (int i = 0; i < got * e->channels; i++) put16(le + 2 * i, (uint16_t)buf[i]);
        fwrite(le, 2, (size_t)got * e->channels, f);
        frames += (uint32_t)got;
    }
    free(r);
    wav_header(h, e->channels, e->rate, frames);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
    if (ferror(f) | fclose(f)) { snprintf(err, errlen, "write error on %s", out); return -1; }
    info->frames = frames;
    info->channels = e->channels;
    info->rate = e->rate;
    return 0;
}

static void xx_decode_jobs(FILE *in, FILE *out) {
    char line[4096];
    while (fgets(line, sizeof line, in)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!*line) continue;
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        char err[512] = "malformed job";
        XxInfo in_;
        if (t1 && t2) {
            *t1 = *t2 = 0;
            if (!xx_decode(line, atoi(t1 + 1), t2 + 1, &in_, err, sizeof err)) {
                fprintf(out, "ok\t%u\t%d\t%u\n", in_.frames, in_.channels, in_.rate);
                fflush(out);
                continue;
            }
        }
        for (char *p = err; *p; p++) if (*p == '\t' || *p == '\n') *p = ' ';
        fprintf(out, "err\t%s\n", err);
        fflush(out);
    }
}

#ifndef XACT_EXTRACT_LIB
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "cues")) return xx_cues(argv[2], stdout) < 0 ? 1 : 0;
    if (argc == 2 && !strcmp(argv[1], "decode")) { xx_decode_jobs(stdin, stdout); return 0; }
    fprintf(stderr, "usage: %s cues BANK.xsb | decode < jobs\n", argv[0]);
    return 2;
}
#endif
