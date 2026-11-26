/* The render helper skeleton: xbrun-render64 (Linux x86_64) /
 * WarriorsRender64.exe (Windows x64). The game starts it (r64_spawn) with the shared channel
 * inherited; it answers the handshake, then runs the echo protocol on the command ring: for every
 * frame it checks the payload's checksum and answers on the back ring. It exits 0 when the game
 * asks it to quit, 3 when the handshake is refused, 4 when the game is gone, 2 on usage errors.
 * With a window config in the input region (the game's --render-process helper-io) it runs the window,
 * event pump and controllers instead (r64_helper_io.c); with --wire too (the
 * game's --render-process helper) it also renders the game's call stream on a render
 * thread into the x86_64 build of vk_renderer.cpp (src/render_rec.c render_rec_serve; Linux builds with
 * R64_HELPER_RENDER, the Windows one too). Every log line starts with [r64]; a crash
 * prints a FAULT line.
 *
 * Test options (render_ipc_test): --test-version N, --test-schema HEX advertise other values. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/render_ipc.h"
#include "../../src/render_wire_gen.h"   /* RW_HELPER_SCHEMA */
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#ifndef XBRUN_VERSION
#define XBRUN_VERSION "unknown"
#endif

/* r64_helper_io.c: the window, event pump and controllers (a window config in the input region). */
int r64_helper_io_run(R64Ipc *x, int argc, char **argv);

static int echo_frames(R64Ipc *x, uint32_t *frames) {
    for (;;) {
        R64EchoFrame h;
        int rc = r64_read(x, R64_RING_CMD, &h, sizeof h);
        if (rc) return rc;
        if (h.tag != R64_ECHO_FRAME || h.bytes & 3) {
            r64_log("bad record %08x (%u bytes) after %u frames", h.tag, h.bytes, *frames);
            return R64_ERR_ARGS;
        }
        R64Sum s;
        r64_sum_init(&s);
        for (uint32_t left = h.bytes; left;) {
            const void *p;
            uint32_t n;
            if ((rc = r64_peek(x, R64_RING_CMD, &p, &n))) return rc;
            if (n > left) n = left;
            n &= ~3u;   /* records are 4-byte aligned, so a span holds whole words except at the very end */
            if (!n) {
                uint32_t w;
                if ((rc = r64_read(x, R64_RING_CMD, &w, 4))) return rc;
                r64_sum_update(&s, &w, 4);
                left -= 4;
                continue;
            }
            r64_sum_update(&s, p, n);
            r64_consume(x, R64_RING_CMD, n);
            left -= n;
        }
        uint32_t want;
        if ((rc = r64_read(x, R64_RING_CMD, &want, 4))) return rc;
        const uint32_t got = r64_sum_final(&s);
        const uint64_t t = r64_now_ns();
        const R64EchoAck a = {R64_ECHO_ACK, h.seq, h.bytes, got, got == want, (uint32_t)t, (uint32_t)(t >> 32), 0};
        if ((rc = r64_write(x, R64_RING_BACK, &a, sizeof a)) || (rc = r64_flush(x, R64_RING_BACK))) return rc;
        R64Control *c = r64_control(x);
        __atomic_store_n(&c->f.frames_done, h.seq + 1, __ATOMIC_RELEASE);
        ++*frames;
    }
}

#ifndef _WIN32
/* A crash in the helper (the renderer, the driver): one FAULT line, then the default action, so the game's death watch
 * reports the signal (its own FAULT line) and exits. */
static void fault(int sig) {
    char line[96];
    const int n = snprintf(line, sizeof line, "[r64] === FAULT (render64 helper: signal %d)\n", sig);
    if (n > 0) {
        ssize_t w = write(1, line, (size_t)n);
        (void)w;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
#else
/* The same on Windows: one FAULT line (the game's outlog gets it through the pipe), then the process ends with the
 * exception code, which the game reports from the process handle. */
static LONG WINAPI fault(EXCEPTION_POINTERS *ep) {
    char line[128];
    const int n = snprintf(line, sizeof line, "[r64] === FAULT (render64 helper: exception %08lx at %p)\n",
                           (unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    DWORD w;
    if (n > 0) WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, (DWORD)n, &w, NULL);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    setvbuf(stdout, NULL, _IONBF, 0);   /* the CRT has no line buffering (_IOLBF is full buffering, size 0 invalid) */
    SetUnhandledExceptionFilter(fault);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);   /* no "has stopped working" dialog: the game reports it */
#else
    setvbuf(stdout, NULL, _IOLBF, 0);
#endif
    R64AttachOpts o = {R64_SKELETON_SCHEMA, 0, "skeleton (no renderer)", 0};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--test-version") && i + 1 < argc) o.test_version = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--test-schema") && i + 1 < argc) o.schema = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--wire")) {   /* the game streams renderer calls (--render-process helper) */
            o.schema = RW_HELPER_SCHEMA;
            o.device = "x86_64 renderer (opens with the game's renderer_init)";
        }
    }
#ifndef _WIN32
    const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++) signal(sigs[i], fault);
#endif
    R64Ipc *x;
    const int rc = r64_attach(argc, argv, &o, &x);
    if (rc == R64_ERR_ARGS) return 2;
    if (rc == R64_ERR_VERSION) return 3;
    if (rc) return 4;
    r64_log("xbrun-render64 %s (%d-bit), channel v%u, game pid %ld", XBRUN_VERSION, (int)(sizeof(void *) * 8), R64_VERSION, r64_peer_pid(x));
    uint32_t in_size;
    const R64InHeader *in = r64_region(x, R64_REG_INPUT, &in_size);
    if (in && in->magic == R64_IN_MAGIC && in->cfg.flags & R64_IN_CFG_WINDOW) {
        const int ir = r64_helper_io_run(x, argc, argv);
        if (ir == 4) {
            r64_shutdown(x, 0, NULL);
            return 4;
        }
        if (ir) r64_mark_dead(x);
        r64_shutdown(x, 0, NULL);
        return ir;
    }
    uint32_t frames = 0;
    const int er = echo_frames(x, &frames);
    if (er == R64_ERR_QUIT) {
        r64_log("quit after %u frames", frames);
        r64_shutdown(x, 0, NULL);
        return 0;
    }
    if (er == R64_ERR_PEER_DEAD) {
        r64_log("the game is gone after %u frames: exiting", frames);
        r64_shutdown(x, 0, NULL);
        return 4;
    }
    r64_mark_dead(x);
    r64_shutdown(x, 0, NULL);
    return 5;
}
