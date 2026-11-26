/* Game-independent live game control test (src/gamectl_proto.h): the line parser, the JSON
 * replies, the live option table, the request queue between threads, the capture writers and the
 * input-script end frame. */
#include "../src/gamectl_proto.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

static int parses(const char *line, GcKind kind) {
    GcCommand c;
    char err[256];
    return gc_parse(line, &c, err, sizeof err) == 0 && c.kind == kind;
}
static int refuses(const char *line, const char *msg_part) {
    GcCommand c;
    char err[256] = "";
    int r = gc_parse(line, &c, err, sizeof err);
    if (r == 0) return 0;
    if (msg_part && !strstr(err, msg_part)) { printf("  (message was '%s')\n", err); return 0; }
    return 1;
}

static void test_parse(void) {
    GcCommand c;
    char err[256];
    CHECK(parses("ping", GC_PING), "ping");
    CHECK(parses("  ping \r\n", GC_PING), "ping with blanks and CRLF");
    CHECK(parses("status", GC_STATUS) && parses("help", GC_HELP) && parses("opts", GC_OPTS) && parses("quit", GC_QUIT), "bare verbs");
    CHECK(refuses("ping now", "takes no argument"), "ping with an argument");
    CHECK(refuses("", "empty"), "empty line");
    CHECK(refuses("   \n", "empty"), "blank line");
    CHECK(refuses("dance", "unknown command 'dance'"), "unknown verb");
    gc_parse("dance", &c, err, sizeof err);
    CHECK(c.kind == GC_NONE, "unknown verb: kind none");

    CHECK(gc_parse("lua return GetCheckPoint()", &c, err, sizeof err) == 0 && c.kind == GC_LUA &&
              !strcmp(c.text, "return GetCheckPoint()"), "lua: the chunk");
    CHECK(gc_parse("lua   =1 +  2  ", &c, err, sizeof err) == 0 && !strcmp(c.text, "=1 +  2"), "lua: inner blanks kept");
    CHECK(refuses("lua", "needs a chunk"), "lua without a chunk");
    gc_parse("lua", &c, err, sizeof err);
    CHECK(c.kind == GC_LUA, "lua error: the kind is known for the reply");

    CHECK(gc_parse("input 0:a:6,20:start", &c, err, sizeof err) == 0 && c.kind == GC_INPUT && !strcmp(c.text, "0:a:6,20:start"), "input");
    CHECK(refuses("input", "needs"), "input without steps");

    CHECK(gc_parse("warp m8:3", &c, err, sizeof err) == 0 && c.kind == GC_WARP && !strcmp(c.text, "m8:3"), "warp");
    CHECK(refuses("warp", "needs"), "warp without a target");
    CHECK(refuses("warp m8 m9", "one target"), "warp with two targets");

    CHECK(gc_parse("wait frames 120", &c, err, sizeof err) == 0 && c.kind == GC_WAIT_FRAMES && c.n == 120, "wait frames");
    CHECK(refuses("wait frames 0", "1-1000000"), "wait frames 0");
    CHECK(refuses("wait frames 1000001", "1-1000000"), "wait frames too many");
    CHECK(refuses("wait frames x", "1-1000000"), "wait frames not a number");
    CHECK(refuses("wait frames -5", "1-1000000"), "wait frames negative");
    CHECK(gc_parse("wait level", &c, err, sizeof err) == 0 && c.kind == GC_WAIT_LEVEL && c.n == GC_LEVEL_WAIT_DEFAULT, "wait level default");
    CHECK(gc_parse("wait level 600", &c, err, sizeof err) == 0 && c.kind == GC_WAIT_LEVEL && c.n == 600, "wait level bound");
    CHECK(refuses("wait", "wait frames N"), "wait alone");
    CHECK(refuses("wait seconds 3", "wait frames N"), "wait unknown unit");
    gc_parse("wait", &c, err, sizeof err);
    CHECK(!strcmp(gc_kind_name(c.kind), "wait"), "wait errors reply as wait");

    CHECK(gc_parse("capture /tmp/a.png", &c, err, sizeof err) == 0 && c.kind == GC_CAPTURE && !strcmp(c.text, "/tmp/a.png"), "capture png");
    CHECK(parses("capture shot.BMP", GC_CAPTURE), "capture bmp, any case");
    CHECK(refuses("capture /tmp/a.jpg", ".bmp or .png"), "capture other format");
    CHECK(refuses("capture", "needs"), "capture without a path");

    CHECK(gc_parse("stats 120", &c, err, sizeof err) == 0 && c.kind == GC_STATS && c.n == 120, "stats");
    CHECK(refuses("stats 0", "1-36000"), "stats 0");
    CHECK(refuses("stats", "1-36000"), "stats without frames");

    CHECK(gc_parse("opt turbo 1", &c, err, sizeof err) == 0 && c.kind == GC_OPT && !strcmp(c.name, "turbo") && !strcmp(c.text, "1"), "opt");
    CHECK(gc_parse("opt right-stick 0.5 -1", &c, err, sizeof err) == 0 && !strcmp(c.text, "0.5 -1"), "opt pair value");
    CHECK(gc_parse("opt", &c, err, sizeof err) == 0 && c.kind == GC_OPTS, "opt alone lists");
    CHECK(refuses("opt turbo", "needs a value"), "opt without a value");

    CHECK(gc_parse("save-copy load /tmp/hdd", &c, err, sizeof err) == 0 && c.kind == GC_SAVE_COPY && !strcmp(c.text, "/tmp/hdd"), "save-copy load");
    CHECK(refuses("save-copy /tmp/hdd", "save-copy load DIR"), "save-copy without load");

    char longline[GC_LINE_MAX + 10];
    memset(longline, 'x', sizeof longline - 1);
    longline[sizeof longline - 1] = 0;
    memcpy(longline, "lua ", 4);
    CHECK(refuses(longline, "too long"), "line too long");
}

static void test_json(void) {
    char buf[512];
    GcJson j;
    gc_json_begin(&j, buf, sizeof buf, 1, "ping");
    gc_json_uint(&j, "frame", 4012);
    gc_json_str(&j, "level", "level81");
    gc_json_bool(&j, "fast", 0);
    gc_json_num(&j, "t", 1.5);
    size_t n = gc_json_end(&j);
    CHECK(!strcmp(buf, "{\"ok\":true,\"cmd\":\"ping\",\"frame\":4012,\"level\":\"level81\",\"fast\":false,\"t\":1.5}\n"), "json: fields");
    CHECK(n == strlen(buf), "json: length");

    gc_json_begin(&j, buf, sizeof buf, 1, "lua");
    gc_json_str(&j, "result", "a\"b\\c\nd\te\x01");
    gc_json_str(&j, "none", NULL);
    gc_json_num(&j, "nan", NAN);
    gc_json_end(&j);
    CHECK(!strcmp(buf, "{\"ok\":true,\"cmd\":\"lua\",\"result\":\"a\\\"b\\\\c\\nd\\te\\u0001\",\"none\":null,\"nan\":null}\n"), "json: escapes, null");

    gc_json_begin(&j, buf, sizeof buf, 1, "stats");
    gc_json_lines(&j, "lines", "[stats] one\n[stats] two\n");
    gc_json_end(&j);
    CHECK(!strcmp(buf, "{\"ok\":true,\"cmd\":\"stats\",\"lines\":[\"[stats] one\",\"[stats] two\"]}\n"), "json: lines");
    gc_json_begin(&j, buf, sizeof buf, 1, "stats");
    gc_json_lines(&j, "lines", "");
    gc_json_end(&j);
    CHECK(!strcmp(buf, "{\"ok\":true,\"cmd\":\"stats\",\"lines\":[]}\n"), "json: no lines");

    gc_json_error(buf, sizeof buf, "opt", "unknown option 'x'");
    CHECK(!strcmp(buf, "{\"ok\":false,\"cmd\":\"opt\",\"error\":\"unknown option 'x'\"}\n"), "json: error reply");

    char small[64];
    gc_json_begin(&j, small, sizeof small, 1, "lua");
    gc_json_str(&j, "result", "0123456789012345678901234567890123456789012345678901234567890123456789");
    n = gc_json_end(&j);
    CHECK(strstr(small, "\"ok\":false") && strstr(small, "too long") && small[n - 1] == '\n' && n < sizeof small, "json: overflow becomes an error");
}

static void test_options(void) {
    float v[2];
    char err[256];
    const GcOptInfo *o = gc_opt_find("turbo");
    CHECK(o && o->cls == GC_OPT_LIVE && o->val == GC_VAL_BOOL, "opt turbo is live");
    CHECK(o && gc_opt_value(o, "1", v, err, sizeof err) == 0 && v[0] == 1, "turbo 1");
    CHECK(o && gc_opt_value(o, "off", v, err, sizeof err) == 0 && v[0] == 0, "turbo off");
    CHECK(o && gc_opt_value(o, "maybe", v, err, sizeof err) < 0 && strstr(err, "0/1"), "turbo maybe refused");
    o = gc_opt_find("taa-sharpness");
    CHECK(o && o->cls == GC_OPT_LIVE && gc_opt_value(o, "0.4", v, err, sizeof err) == 0 && fabsf(v[0] - 0.4f) < 1e-6f, "taa-sharpness 0.4");
    CHECK(o && gc_opt_value(o, "3", v, err, sizeof err) < 0 && strstr(err, "0-1"), "taa-sharpness out of range");
    CHECK(o && gc_opt_value(o, "nan", v, err, sizeof err) < 0, "taa-sharpness nan refused");
    o = gc_opt_find("right-stick");
    CHECK(o && gc_opt_value(o, "0.5 -1", v, err, sizeof err) == 0 && v[0] == 0.5f && v[1] == -1, "right-stick pair");
    CHECK(o && gc_opt_value(o, "0.5", v, err, sizeof err) < 0, "right-stick needs two numbers");
    o = gc_opt_find("idle-timeout");
    CHECK(o && gc_opt_value(o, "60", v, err, sizeof err) == 0 && v[0] == 60, "idle-timeout");
    CHECK(o && gc_opt_value(o, "1.5", v, err, sizeof err) < 0, "idle-timeout integer only");
    o = gc_opt_find("bloom-strength");
    CHECK(o && o->cls == GC_OPT_RESTART, "bloom-strength is restart-only");
    CHECK(o && gc_opt_value(o, "1", v, err, sizeof err) < 0 && strstr(err, "--bloom-strength"), "restart-only names its start option");
    CHECK(!gc_opt_find("no-such"), "unknown option");
    int live = 0;
    for (int i = 0; i < gc_option_count; i++) live += gc_options[i].cls == GC_OPT_LIVE;
    CHECK(live >= 5, "at least five live options");
}

/* ---- queue: three submitter threads, one consumer ---- */
static GcQueue q;
typedef struct { int id; int ok; } Sub;
static void *submitter(void *arg) {
    Sub *s = arg;
    for (int i = 0; i < 50; i++) {
        GcRequest *r = calloc(1, sizeof *r);
        snprintf(r->line, sizeof r->line, "echo %d %d", s->id, i);
        int st = gcq_submit(&q, r);
        char want[64];
        snprintf(want, sizeof want, "reply echo %d %d", s->id, i);
        if (st == 0 && r->done && !strcmp(r->reply, want)) s->ok++;
        free(r);
    }
    return NULL;
}
static void *consumer(void *arg) {
    int *served = arg;
    while (*served < 150) {
        GcRequest *r = gcq_peek(&q);
        if (!r) { usleep(100); continue; }
        char reply[64];
        int n = snprintf(reply, sizeof reply, "reply %s", r->line);
        gcq_finish(&q, r, reply, (size_t)n);
        ++*served;
    }
    return NULL;
}
static void *late_submitter(void *arg) {
    GcRequest *r = arg;
    gcq_submit(&q, r);
    return NULL;
}

static void test_queue(void) {
    gcq_init(&q);
    CHECK(gcq_peek(&q) == NULL && gcq_pending(&q) == 0, "queue: empty");
    pthread_t t[4];
    Sub subs[3] = {{1, 0}, {2, 0}, {3, 0}};
    int served = 0;
    pthread_create(&t[3], NULL, consumer, &served);
    for (int i = 0; i < 3; i++) pthread_create(&t[i], NULL, submitter, &subs[i]);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    CHECK(subs[0].ok == 50 && subs[1].ok == 50 && subs[2].ok == 50, "queue: every submitter got its own reply");
    CHECK(served == 150 && gcq_pending(&q) == 0, "queue: all served");

    /* FIFO: two requests waiting are served in order. */
    GcRequest a = {0}, b = {0};
    strcpy(a.line, "first");
    strcpy(b.line, "second");
    pthread_t ta, tb;
    pthread_create(&ta, NULL, late_submitter, &a);
    while (gcq_pending(&q) < 1) usleep(100);
    pthread_create(&tb, NULL, late_submitter, &b);
    while (gcq_pending(&q) < 2) usleep(100);
    GcRequest *h = gcq_peek(&q);
    CHECK(h == &a, "queue: oldest first");
    CHECK(gcq_peek(&q) == &a, "queue: peek does not remove");
    gcq_finish(&q, h, "r1", 2);
    CHECK(gcq_peek(&q) == &b, "queue: then the next");
    /* close fails the waiting one and refuses new ones */
    gcq_close(&q);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    CHECK(a.done && !strcmp(a.reply, "r1"), "queue: finished before close");
    CHECK(b.done && strstr(b.reply, "\"ok\":false") && strstr(b.reply, "exiting"), "queue: close fails the waiting request");
    GcRequest c = {0};
    strcpy(c.line, "ping");
    CHECK(gcq_submit(&q, &c) < 0 && strstr(c.reply, "exiting"), "queue: closed refuses");
}

static void test_images(void) {
    uint32_t px[2 * 3] = {0xff0000, 0x00ff00, 0x0000ff, 0xffffff, 0x123456, 0x000000};
    char err[256];
    char dir[] = "/tmp/gamectl_testXXXXXX";
    if (!mkdtemp(dir)) { CHECK(0, "mkdtemp"); return; }
    char bmp[256], png[256], bad[256];
    snprintf(bmp, sizeof bmp, "%s/a.bmp", dir);
    snprintf(png, sizeof png, "%s/a.PNG", dir);
    snprintf(bad, sizeof bad, "%s/missing/a.bmp", dir);
    CHECK(gc_write_image(bmp, px, 3, 2, 3, err, sizeof err) == 0, "bmp written");
    FILE *f = fopen(bmp, "rb");
    unsigned char h[64] = {0};
    size_t n = f ? fread(h, 1, sizeof h, f) : 0;
    if (f) fclose(f);
    /* 54-byte header + 2 rows of 3 pixels padded to 12 bytes */
    CHECK(n == 54 + 24 - 14 || n == 64, "bmp: read (64 of 78 bytes)");
    CHECK(h[0] == 'B' && h[1] == 'M' && h[2] == 54 + 24 && h[18] == 3 && h[22] == 2 && h[28] == 24, "bmp: header");
    /* bottom-up: the first pixel row in the file is image row 1 (white, 123456, black); BGR */
    CHECK(h[54] == 0xff && h[55] == 0xff && h[56] == 0xff && h[57] == 0x56 && h[58] == 0x34 && h[59] == 0x12, "bmp: bottom row first, BGR");

    CHECK(gc_write_image(png, px, 3, 2, 3, err, sizeof err) == 0, "png written");
    f = fopen(png, "rb");
    unsigned char p[256] = {0};
    n = f ? fread(p, 1, sizeof p, f) : 0;
    if (f) fclose(f);
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    CHECK(n > 8 + 25 + 12 && !memcmp(p, sig, 8) && !memcmp(p + 12, "IHDR", 4) && p[19] == 3 && p[23] == 2 && p[24] == 8 && p[25] == 2,
          "png: signature and IHDR (3x2, 8-bit RGB)");
    CHECK(!memcmp(p + n - 8, "IEND", 4), "png: IEND last");
    /* IHDR CRC (zlib crc32 of "IHDR" + 13 data bytes) */
    CHECK(p[29] == 0x12 && p[30] == 0x16 && p[31] == 0xf1 && p[32] == 0x4d, "png: IHDR crc");
    if (access("/usr/bin/python3", X_OK) == 0) {
        char cmd[512];
        snprintf(cmd, sizeof cmd,
                 "python3 -c \"import zlib,struct,sys;d=open('%s','rb').read();i=d.index(b'IDAT');l=struct.unpack('>I',d[i-4:i])[0];"
                 "r=zlib.decompress(d[i+4:i+4+l]);sys.exit(0 if r==bytes([0,255,0,0,0,255,0,0,0,255,0,255,255,255,0x12,0x34,0x56,0,0,0]) else 1)\"",
                 png);
        CHECK(system(cmd) == 0, "png: IDAT inflates to the filtered rows");
    }
    CHECK(gc_write_image(bad, px, 3, 2, 3, err, sizeof err) < 0 && strstr(err, "cannot"), "unwritable path refused");
    CHECK(gc_write_image("x.gif", px, 3, 2, 3, err, sizeof err) < 0, "unknown extension refused");
    unlink(bmp);
    unlink(png);
    rmdir(dir);

    uint32_t q1[4] = {1, 2, 3, 4}, q2[4] = {0xff000001, 2, 3, 4}, q3[4] = {1, 2, 3, 5};
    CHECK(gc_frame_hash(q1, 2, 2, 2) == gc_frame_hash(q2, 2, 2, 2), "hash: alpha ignored");
    CHECK(gc_frame_hash(q1, 2, 2, 2) != gc_frame_hash(q3, 2, 2, 2), "hash: a pixel changes it");
    uint32_t wide[6] = {1, 2, 99, 3, 4, 77};
    CHECK(gc_frame_hash(wide, 2, 2, 3) == gc_frame_hash(q1, 2, 2, 2), "hash: pitch padding ignored");
}

static void test_script_end(void) {
    CHECK(gc_script_end("3200:start,3400:start,3600:a,3720:a,3840:a") == 3846, "script end: default 6 frames");
    CHECK(gc_script_end("100:a:50,10:b") == 150, "script end: the latest end");
    CHECK(gc_script_end("5:p1/plug") == 5, "script end: plug step");
    CHECK(gc_script_end("") == 0 && gc_script_end(NULL) == 0, "script end: none");
}

int main(void) {
    test_parse();
    test_json();
    test_options();
    test_queue();
    test_images();
    test_script_end();
    printf("%s\n", fails ? "gamectl_test: FAILED" : "gamectl_test: ALL OK");
    return fails != 0;
}
