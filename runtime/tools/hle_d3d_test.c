/* Game-independent test of the HLE D3D boundary (src/hle_d3d.c). It maps the XBE's D3D8 library
 * range (code and globals), plants each entry's expected first bytes, installs the HLE patches
 * and calls the patched entry points with the guest calling convention: stack balance, the
 * device and resource objects (Xbox D3DResource layout), Clear + Swap and the posted frame, all
 * with the null video backend. Usage: hle_d3d_test [frame.bmp] */
#define _GNU_SOURCE
#include "../src/hle_d3d.h"
#include "../src/mem.h"
#include "../src/video.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

/* Same helper as hle_audio_test.c: pushes args (stdcall order), calls fn and reports the esp
 * change across the call; 0 means the callee popped exactly nargs*4 bytes. */
__asm__(".text\n.globl tcall\ntcall:\n"
        "push %ebp\npush %ebx\npush %esi\npush %edi\nmov %esp,%ebp\n"
        "mov 20(%ebp),%eax\nmov 24(%ebp),%ecx\nmov 28(%ebp),%esi\n"
        "1: test %ecx,%ecx\njz 2f\ndec %ecx\npushl (%esi,%ecx,4)\njmp 1b\n"
        "2: call *%eax\nmov %esp,%ecx\nsub %ebp,%ecx\nmov 32(%ebp),%edx\nmov %ecx,(%edx)\n"
        "mov %ebp,%esp\npop %edi\npop %esi\npop %ebx\npop %ebp\nret\n");
uint32_t tcall(uint32_t fn, int nargs, const uint32_t *args, int32_t *esp_delta);
/* Register-argument call (ecx, edx; no stack arguments), as SetRenderState_Simple takes them. */
__asm__(".text\n.globl rcall\nrcall:\n"
        "push %ebp\npush %ebx\npush %esi\npush %edi\n"
        "mov 20(%esp),%eax\nmov 24(%esp),%ecx\nmov 28(%esp),%edx\ncall *%eax\n"
        "pop %edi\npop %esi\npop %ebx\npop %ebp\nret\n");
uint32_t rcall(uint32_t fn, uint32_t ecx, uint32_t edx);

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static uint32_t addr_of(const char *name) {
    for (int i = 0; i < hle_d3d_entry_count(); i++)
        if (!strcmp(hle_d3d_entry_name(i), name)) return hle_d3d_entry_addr(i);
    printf("FAIL no entry %s\n", name);
    failures++;
    return 0;
}
static uint32_t call(const char *name, int nargs, const uint32_t *args) {
    int32_t delta = 1;
    uint32_t a = addr_of(name);
    if (!a) return 0;
    uint32_t r = tcall(a, nargs, args, &delta);
    CHECK(delta == 0, "%s pops %d bytes (esp delta %d)", name, nargs * 4, delta);
    return r;
}
#define A(...) (const uint32_t[]){__VA_ARGS__}
#define P(x) ((uint32_t)(uintptr_t)(x))
#define U32(a, off) (*(uint32_t *)(uintptr_t)((a) + (off)))
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* Host loop on its own thread: starts servicing after 50 ms. */
static void *late_host(void *arg) {
    (void)arg;
    struct timespec d = {0, 50000000};
    nanosleep(&d, NULL);
    for (int i = 0; i < 20; i++) hle_d3d_service();
    return NULL;
}

int main(int argc, char **argv) {
    const uint32_t lo = 0x35c000, hi = 0x381000;   /* D3D8 code .. end of its globals */
    uint8_t *m = mmap((void *)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (m != (void *)lo) { perror("mmap"); return 1; }
    memset(m, 0xCC, 0x37d000 - lo);   /* code: int3; globals stay zero */
    mem_init();

    /* Table sanity: unique addresses inside the D3D library, plausible stdcall sizes. */
    int n = hle_d3d_entry_count(), dup = 0, range = 1, sizes = 1;
    for (int i = 0; i < n; i++) {
        uint32_t a = hle_d3d_entry_addr(i);
        int b = hle_d3d_entry_argbytes(i);
        range &= a >= lo && a + HLE_SIG_LEN <= 0x37d000;
        sizes &= b >= 0 && b <= 28 && b % 4 == 0;
        for (int j = 0; j < i; j++) dup += hle_d3d_entry_addr(j) == a;
        memcpy((void *)(uintptr_t)a, hle_d3d_entry_sig(i), HLE_SIG_LEN);
    }
    CHECK(n >= 80, "%d entry points in the table", n);
    CHECK(!dup, "entry addresses are unique (%d duplicates)", dup);
    CHECK(range, "entry addresses inside the D3D8 library code");
    CHECK(sizes, "argument byte counts are dword multiples <= 28");
    hle_d3d_log_limit = 1;
    hle_d3d_video = "null";
    int patched = hle_d3d_install(NULL);
    CHECK(patched == n, "install patched %d of %d entries", patched, n);

    /* Device creation through the guest out-pointer, globals as the original code leaves them. */
    uint32_t pp[16] = {640, 480, 0x1E, 1, 0x11, 1, 0, 0, 1, 0x2A, 0, 60, 0};
    uint32_t dev = 0;
    CHECK(call("Direct3D_CreateDevice", 6, A(0, 1, 0, 0x40, P(pp), P(&dev))) == 0 && dev == D3D_DEVICE,
          "CreateDevice -> device %08x", dev);
    CHECK(U32(D3D_G_PDEVICE, 0) == D3D_DEVICE, "D3D_g_pDevice -> %08x", U32(D3D_G_PDEVICE, 0));
    uint32_t put = U32(D3D_DEVICE, 0), end = U32(D3D_DEVICE, 4);
    CHECK(put && end > put, "push-buffer put/end point at a scratch buffer (%08x..%08x)", put, end);

    uint32_t zeros[8] = {0};
    /* Back buffer, depth buffer and render target (pointer-returning "2" variants). */
    uint32_t bb = call("D3DDevice_GetBackBuffer2", 1, A(0));
    uint32_t front = call("D3DDevice_GetBackBuffer2", 1, A(0xFFFFFFFF));
    uint32_t zs = call("D3DDevice_GetDepthStencilSurface2", 0, zeros);
    uint32_t rt = call("D3DDevice_GetRenderTarget2", 0, zeros);
    CHECK(bb && front && bb != front && zs && rt == bb, "back %08x, front %08x, depth %08x, render target %08x", bb, front, zs, rt);
    CHECK((U32(bb, 0) & 0x70000) == 0x50000 && U32(bb, 4) < CONTIG_SIZE && ((U32(bb, 0xc) >> 8) & 0xFF) == 0x1E,
          "back buffer: surface, physical Data %08x, format LIN_X8R8G8B8", U32(bb, 4));
    uint32_t size = U32(bb, 0x10);
    CHECK((size & 0xFFF) + 1 == 640 && ((size >> 12) & 0xFFF) + 1 == 480 && ((size >> 24) + 1) * 64 == 2560,
          "back buffer Size %08x = 640x480, pitch 2560", size);
    uint32_t desc[7] = {0};
    call("D3DSurface_GetDesc", 2, A(bb, P(desc)));
    CHECK(desc[0] == 0x1E && desc[1] == 1 && desc[5] == 640 && desc[6] == 480, "GetDesc -> fmt %x type %u %ux%u",
          desc[0], desc[1], desc[5], desc[6]);
    CHECK(call("D3DResource_GetType", 1, A(bb)) == 1, "GetType(back buffer) -> D3DRTYPE_SURFACE");

    /* Every patched entry survives zero arguments and balances the stack. */
    for (int i = 0; i < n; i++) {
        const char *nm = hle_d3d_entry_name(i);
        if (!strcmp(nm, "Direct3D_CreateDevice") || !strcmp(nm, "D3DDevice_Swap")) continue;
        call(nm, hle_d3d_entry_argbytes(i) / 4, zeros);
    }

    call("D3DDevice_SetRenderTarget", 2, A(bb, zs));   /* the sweep set (NULL, NULL) */

    /* Output state for the renderer: NV2A methods through SetRenderState_Simple (ecx = 0x40000 |
     * method, edx = value), D3DRS_CULLMODE through SetRenderState_CullMode. */
    RState rs;
    static const uint32_t simple[][2] = {{0x304, 1}, {0x344, 0x302}, {0x348, 0x303}, {0x350, 0x800B}, {0x34c, 0x80402010u},
                                         {0x358, 0x00010001u}, {0x35c, 0}, {0x354, 0x206}, {0x300, 1}};
    uint32_t simple_fn = addr_of("D3DDevice_SetRenderState_Simple");
    for (unsigned i = 0; i < sizeof simple / sizeof *simple; i++) rcall(simple_fn, 0x40000u | simple[i][0], simple[i][1]);
    call("D3DDevice_SetRenderState_CullMode", 1, A(0x900));
    hle_d3d_render_state(&rs);
    CHECK(rs.blend_enable == 1 && rs.blend_src == 0x302 && rs.blend_dst == 0x303 && rs.blend_eq == 0x800B &&
              rs.blend_color == 0x80402010u && rs.color_mask == 0x00010001u && rs.depth_write == 0 && rs.depth_func == 0x206 &&
              rs.cull == 0x900,
          "SetRenderState_Simple / CullMode -> blend %u %x %x eq %x color %08x mask %08x zwrite %u zfunc %x cull %x", rs.blend_enable,
          rs.blend_src, rs.blend_dst, rs.blend_eq, rs.blend_color, rs.color_mask, rs.depth_write, rs.depth_func, rs.cull);
    rcall(simple_fn, 0x40304u, 0);
    call("D3DDevice_SetRenderState_CullMode", 1, A(0));
    hle_d3d_render_state(&rs);
    CHECK(rs.blend_enable == 0 && rs.cull == 0 && rs.blend_src == 0x302, "BLEND_ENABLE 0 and CULL_NONE keep the other state");


    /* Texture, its level-0 surface and LockRect. */
    uint32_t tex = call("D3DDevice_CreateTexture2", 7, A(512, 256, 1, 1, 0, 0x12, 3));
    CHECK(tex && (U32(tex, 0) & 0x70000) == 0x40000 && call("D3DResource_GetType", 1, A(tex)) == 3,
          "CreateTexture2(512x256 LIN_A8R8G8B8) -> texture %08x", tex);
    uint32_t lvl = call("D3DTexture_GetSurfaceLevel2", 2, A(tex, 0));
    CHECK(lvl && U32(lvl, 4) == U32(tex, 4) && U32(lvl, 0x14) == tex, "GetSurfaceLevel2 -> %08x shares Data, Parent = texture", lvl);
    uint32_t lr[2] = {0};
    CHECK(call("D3DTexture_LockRect", 5, A(tex, 0, P(lr), 0, 0)) == 0 && lr[0] == 2048 && lr[1] == (U32(tex, 4) | 0x80000000u),
          "LockRect -> pitch %u, bits %08x", lr[0], lr[1]);
    memset((void *)(uintptr_t)lr[1], 0x5A, 2048 * 256);   /* the whole level must be writable */
    uint32_t dxt = call("D3DDevice_CreateTexture2", 7, A(256, 256, 1, 0, 0, 0x0C, 3));
    CHECK(dxt && ((U32(dxt, 0xc) >> 16) & 0xF) == 9 && ((U32(dxt, 0xc) >> 20) & 0xF) == 8,
          "CreateTexture2(256x256 DXT1, full chain) -> 9 levels, log2 width 8 (format %08x)", U32(dxt, 0xc));
    uint32_t ib = call("D3DDevice_CreateIndexBuffer2", 1, A(64));
    CHECK(ib && U32(ib, 0) == 0x1010001 && U32(ib, 4) == ib + 12, "CreateIndexBuffer2 -> %08x, Data follows the header", ib);
    CHECK(call("D3DResource_Release", 1, A(ib)) == 0, "Release(index buffer) -> refcount 0");
    uint32_t vbh[3] = {0, 0x1000, 0};
    CHECK(call("D3DVertexBuffer_Lock2", 2, A(P(vbh), 0)) == 0x80001000u, "VertexBuffer_Lock2 -> Data | 0x80000000");
    call("D3DResource_Register", 2, A(P(vbh), 0x80020000u));
    CHECK(vbh[1] == 0x21000, "Register adds the base and masks to a physical address (%08x)", vbh[1]);
    uint32_t vsh = 0;
    CHECK(call("D3DDevice_CreateVertexShader", 4, A(0, 0, P(&vsh), 0)) == 0 && (vsh & 1), "CreateVertexShader -> handle %08x", vsh);

    /* Clear fills the render target; Swap posts the back buffer as a frame and counts it. */
    call("D3DDevice_Clear", 6, A(0, 0, 0xF3, 0xFF336699u, fbits(1.0f), 0));
    uint32_t *pix = (uint32_t *)(uintptr_t)(U32(bb, 4) | 0x80000000u);
    CHECK((pix[0] & 0xFFFFFF) == 0x336699 && (pix[640 * 480 - 1] & 0xFFFFFF) == 0x336699, "Clear filled the back buffer (%08x)", pix[0]);
    CHECK(hle_d3d_clear_color() == 0xFF336699u, "clear color recorded");
    uint32_t rect[4] = {10, 10, 20, 20};
    call("D3DDevice_Clear", 6, A(1, P(rect), 0xF0, 0xFF00FF00u, 0, 0));
    CHECK(pix[15 * 640 + 15] == 0xFF00FF00u && (pix[0] & 0xFFFFFF) == 0x336699, "Clear with a rect only touches the rect");
    CHECK(call("D3DDevice_Swap", 1, A(0)) == 1 && hle_d3d_frame_count() == 1, "Swap -> swap count 1");
    CHECK(call("D3DDevice_Swap", 1, A(5)) == 2 && hle_d3d_frame_count() == 2, "Swap -> swap count 2");
    int w = 0, h = 0;
    uint32_t *frame = hle_d3d_last_frame(&w, &h);
    CHECK(frame && w == 640 && h == 480 && (frame[0] & 0xFFFFFF) == 0x336699 && frame[15 * 640 + 15] == 0xFF00FF00u,
          "posted frame %dx%d carries the cleared pixels", w, h);
    if (frame && argc > 1) {
        struct stat st;
        CHECK(video_write_bmp(argv[1], frame, w, h, w * 4) == 0 && stat(argv[1], &st) == 0 && st.st_size == 54 + 640 * 480 * 3,
              "BMP written (%s)", argv[1]);
    }
    free(frame);

    /* DrawVerticesUP outside the movie path is recorded for the renderer like the indexed draws:
     * the title's full-screen tint quad (FVF 0x44 = XYZRHW |
     * DIFFUSE, QUADLIST, stride 20) and a world-space fan (FVF 0x142 = XYZ | DIFFUSE | TEX1). */
    {
        const RVertex *rv;
        uint32_t nv, ni, draws0 = hle_d3d_recorded(&rv, &nv, &ni);
        static const uint32_t vp[6] = {0, 0, 640, 480, 0, 0x3f800000};
        call("D3DDevice_SetViewport", 1, A(P(vp)));
        call("D3DDevice_SetVertexShader", 1, A(0x44));
        uint32_t *q = guest_alloc(4096);
        const float xy[4][2] = {{0, 0}, {0, 480}, {640, 480}, {640, 0}};
        for (int i = 0; i < 4; i++) {
            memcpy(&q[5 * i], &xy[i][0], 4);
            memcpy(&q[5 * i + 1], &xy[i][1], 4);
            q[5 * i + 2] = 0;
            q[5 * i + 3] = fbits(1);
            q[5 * i + 4] = 0x2b07141eu;
        }
        call("D3DDevice_DrawVerticesUP", 4, A(8, 4, P(q), 20));
        uint32_t draws = hle_d3d_recorded(&rv, &nv, &ni);
        CHECK(draws == draws0 + 1 && nv == 4 && ni == 6, "FVF 0x44 quad recorded (%u draws, %u vertices, %u indices)", draws - draws0, nv, ni);
        static const RVertex none[4];
        if (draws == draws0) rv = none;
        CHECK(rv[0].x == -1 && rv[0].y == 1 && rv[2].x == 1 && rv[2].y == -1 && rv[0].color == 0x2b07141eu,
              "quad covers the viewport in clip space with its diffuse color (%g,%g)-(%g,%g) %08x", rv[0].x, rv[0].y, rv[2].x, rv[2].y,
              rv[0].color);
        static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        for (uint32_t t = 0; t < 7; t++) call("D3DDevice_SetTransform", 2, A(t, P(ident)));
        call("D3DDevice_SetVertexShader", 1, A(0x142));
        const float fan[5][3] = {{0, 0, 0.5f}, {0.5f, 0, 0.5f}, {0.5f, 0.5f, 0.5f}, {0, 0.5f, 0.5f}, {-0.5f, 0.25f, 0.5f}};
        for (int i = 0; i < 5; i++) {
            memcpy(&q[6 * i], fan[i], 12);
            q[6 * i + 3] = 0xFF00FF00u;
            q[6 * i + 4] = fbits(0.25f * i);
            q[6 * i + 5] = fbits(1);
        }
        call("D3DDevice_DrawVerticesUP", 4, A(7, 5, P(q), 24));
        draws0 = draws;
        draws = hle_d3d_recorded(&rv, &nv, &ni);
        CHECK(draws == draws0 + 1 && nv == 5 && ni == 9, "FVF 0x142 fan recorded (%u draws, %u vertices, %u indices)", draws - draws0, nv, ni);
        if (draws == draws0) rv = none;
        CHECK(rv[2].x == 0.5f && rv[2].y == 0.5f && rv[2].w == 1 && rv[3].tex[0][0] == 0.75f && rv[1].color == 0xFF00FF00u,
              "fan vertices transformed with their color and texture coordinates");

        /* Fixed-function lighting of an FVF draw (D3DRS_LIGHTING in D3D_g_RenderState): one
         * directional light straight down, material diffuse 0.5, global ambient 0x40 (0.25),
         * emissive from the vertex diffuse (D3DRS_EMISSIVEMATERIALSOURCE = COLOR1, COLORVERTEX). */
        uint32_t *rs = (uint32_t *)(uintptr_t)0x37d6e0u;
        rs[102] = 1;            /* LIGHTING */
        rs[105] = 1;            /* COLORVERTEX */
        rs[113] = 1;            /* EMISSIVEMATERIALSOURCE = D3DMCS_COLOR1 */
        rs[115] = 0x40404040u;  /* AMBIENT */
        float light[26] = {0}, mat[17] = {0};
        memcpy(&light[0], &(uint32_t){3}, 4);   /* D3DLIGHT_DIRECTIONAL; then Diffuse, Specular, Ambient, Position, Direction */
        light[1] = light[2] = light[3] = 1;
        light[17] = -1;   /* Direction.y */
        mat[0] = mat[1] = mat[2] = 0.5f;
        mat[3] = 1;
        mat[4] = mat[5] = mat[6] = mat[7] = 1;   /* ambient */
        call("D3DDevice_SetLight", 2, A(0, P(light)));
        call("D3DDevice_LightEnable", 2, A(0, 1));
        call("D3DDevice_SetMaterial", 1, A(P(mat)));
        call("D3DDevice_SetVertexShader", 1, A(0x152));   /* XYZ | NORMAL | DIFFUSE | TEX1 */
        const float tri[3][3] = {{0, 0, 0.5f}, {0.5f, 0, 0.5f}, {0, 0.5f, 0.5f}};
        for (int i = 0; i < 3; i++) {
            memcpy(&q[9 * i], tri[i], 12);
            q[9 * i + 3] = 0;
            q[9 * i + 4] = fbits(1);   /* normal (0, 1, 0) */
            q[9 * i + 5] = 0;
            q[9 * i + 6] = 0xFF102030u;
            q[9 * i + 7] = q[9 * i + 8] = 0;
        }
        call("D3DDevice_DrawVerticesUP", 4, A(5, 3, P(q), 36));
        draws0 = draws;
        draws = hle_d3d_recorded(&rv, &nv, &ni);
        if (draws == draws0) rv = none;
        /* 0x10 + 0x40 + 0x80 = 0xD0, 0x20 + 0xC0 = 0xE0, 0x30 + 0xC0 = 0xF0; alpha = material 1.0. */
        CHECK(rv[0].color == 0xFFD0E0F0u || rv[0].color == 0xFFCFDFEFu, "lit FVF vertex: emissive + ambient + N.L diffuse (%08x)", rv[0].color);
        call("D3DDevice_LightEnable", 2, A(0, 0));
        call("D3DDevice_DrawVerticesUP", 4, A(5, 3, P(q), 36));
        hle_d3d_recorded(&rv, &nv, &ni);
        CHECK(rv[0].color == 0xFF506070u || rv[0].color == 0xFF4F5F6Fu, "disabled light: emissive + ambient only (%08x)", rv[0].color);
        rs[102] = 0;
        call("D3DDevice_DrawVerticesUP", 4, A(5, 3, P(q), 36));
        hle_d3d_recorded(&rv, &nv, &ni);
        CHECK(rv[0].color == 0xFF102030u, "LIGHTING off keeps the vertex diffuse (%08x)", rv[0].color);
        CHECK(rv[0].fog == 0, "fog off: fog amount 0 (%g)", rv[0].fog);
        /* Linear table fog over view depth 0..1: the triangle at z = 0.5 is half fogged. */
        rs[92] = 1;   /* FOGENABLE */
        rs[93] = 3;   /* FOGTABLEMODE = D3DFOG_LINEAR */
        rs[94] = fbits(0);
        rs[95] = fbits(1);
        call("D3DDevice_DrawVerticesUP", 4, A(5, 3, P(q), 36));
        hle_d3d_recorded(&rv, &nv, &ni);
        CHECK(fabsf(rv[0].fog - 0.5f) < 1e-5f, "linear fog at view depth 0.5 of 0..1: amount 0.5 (%g)", rv[0].fog);
        rs[92] = 0;

        /* Title render targets: a depth-only pass into the depth
         * surface of a LIN_D16 texture (the color target has no memory, as the title's shadow
         * pass), recorded with its target; the texture bound on stage 1 afterwards samples that
         * surface, with the D3D library's PROJECT2D -> PROJECT3D rewrite for depth textures and
         * the shadow compare function of SetRenderState_ShadowFunc (D3DCMP - 0x200). */
        {
            uint32_t ztex = call("D3DDevice_CreateTexture2", 7, A(64, 32, 1, 1, 2, 0x30, 3));
            uint32_t zsurf = call("D3DTexture_GetSurfaceLevel2", 2, A(ztex, 0));
            uint32_t ctex = call("D3DDevice_CreateTexture2", 7, A(64, 32, 1, 1, 1, 0x11, 3));
            uint32_t csurf = call("D3DTexture_GetSurfaceLevel2", 2, A(ctex, 0));
            U32(csurf, 4) = 0;   /* no color memory */
            HleD3dDrawInfo di;
            call("D3DDevice_SetRenderTarget", 2, A(csurf, zsurf));
            call("D3DDevice_Clear", 6, A(0, 0, 1, 0, fbits(1.0f), 0));
            call("D3DDevice_SetVertexShader", 1, A(0x142));
            draws0 = hle_d3d_recorded(&rv, &nv, &ni);
            call("D3DDevice_DrawVerticesUP", 4, A(7, 5, P(q), 24));
            draws = hle_d3d_recorded(&rv, &nv, &ni);
            int got = hle_d3d_last_draw(&di);
            CHECK(draws == draws0 + 1 && got && di.target_depth == U32(ztex, 4) && di.target_color == 0 && di.vp_w == 64 && di.vp_h == 32,
                  "draw into a title render target recorded with its depth surface %08x (color %08x, viewport %ux%u)", di.target_depth,
                  di.target_color, di.vp_w, di.vp_h);
            call("D3DDevice_SetRenderTarget", 2, A(bb, zs));
            uint32_t *def = guest_alloc(0x100);
            memset(def, 0, 0xF0);
            def[0xD4 / 4] = 1;                   /* one stage */
            def[0x88 / 4] = 0x09200000u;         /* r0 = t1 x 1 */
            def[0xB4 / 4] = 0xC0u;
            def[0xD8 / 4] = 1 | 1 << 5;          /* PROJECT2D on stages 0 and 1 */
            uint32_t pshader = 0;
            call("D3DDevice_CreatePixelShader", 2, A(P(def), P(&pshader)));
            call("D3DDevice_SetPixelShader", 1, A(pshader));
            call("D3DDevice_SetTexture", 2, A(1, ztex));
            call("D3DDevice_SetRenderState_ShadowFunc", 1, A(0x204));
            call("D3DDevice_DrawVerticesUP", 4, A(7, 5, P(q), 24));
            got = hle_d3d_last_draw(&di);
            CHECK(got && di.target_depth == 0 && di.stage_surface[1] == U32(ztex, 4) && ((di.tex_modes >> 5) & 31) == 2 &&
                      (di.tex_modes & 31) == 1 && di.shadow_func == 4,
                  "stage 1 samples the rendered depth surface %08x, PROJECT3D (modes %x), shadow func %u", di.stage_surface[1], di.tex_modes,
                  di.shadow_func);
            call("D3DDevice_SetTexture", 2, A(1, 0));
            call("D3DDevice_SetPixelShader", 1, A(0));
        }
    }

    /* HD output size: the renderer target for the 640x480 back buffer. */
    {
        int ow, oh, r[4];
        hle_d3d_output_size(640, 480, &ow, &oh);
        CHECK(ow == 640 && oh == 480, "no options: the back-buffer size (%dx%d)", ow, oh);
        hle_d3d_render_scale = 3;
        hle_d3d_output_size(640, 480, &ow, &oh);
        CHECK(ow == 1920 && oh == 1440, "--render-scale 3: 1920x1440 (%dx%d)", ow, oh);
        hle_d3d_widescreen = 1;
        hle_d3d_output_size(640, 480, &ow, &oh);
        CHECK(ow == 2560 && oh == 1440, "--render-scale 3 --widescreen: 16:9 at the scaled height (%dx%d)", ow, oh);
        hle_d3d_render_scale = 0;
        hle_d3d_res_w = 1920;
        hle_d3d_res_h = 1080;
        hle_d3d_output_size(640, 480, &ow, &oh);
        CHECK(ow == 1920 && oh == 1080, "--resolution 1920x1080 --widescreen: 1920x1080 (%dx%d)", ow, oh);
        hle_d3d_widescreen = 0;
        hle_d3d_output_size(640, 480, &ow, &oh);
        CHECK(ow == 1440 && oh == 1080, "--resolution 1920x1080: the 4:3 part, 1440x1080 (%dx%d)", ow, oh);
        hle_d3d_res_w = 1024;
        hle_d3d_res_h = 1280;
        hle_d3d_output_size(640, 480, &ow, &oh);
        CHECK(ow == 1024 && oh == 768, "--resolution 1024x1280: 1024x768 (%dx%d)", ow, oh);
        hle_d3d_res_w = hle_d3d_res_h = 0;
        video_fit(4, 3, 1920, 1080, r);
        CHECK(r[0] == 240 && r[1] == 0 && r[2] == 1440 && r[3] == 1080, "4:3 in 1920x1080: pillarbox %d,%d %dx%d", r[0], r[1], r[2], r[3]);
        video_fit(16, 9, 1280, 1024, r);
        CHECK(r[0] == 0 && r[1] == 152 && r[2] == 1280 && r[3] == 720, "16:9 in 1280x1024: letterbox %d,%d %dx%d", r[0], r[1], r[2], r[3]);
        video_fit(640, 480, 0, 0, r);
        CHECK(r[2] == 0 && r[3] == 0, "empty window: empty rectangle");
    }

    /* Present pacing: at most one swapped frame waits for the host loop (hle_d3d_service), so a
     * slow host adds at most one frame of input latency; a Swap that finds the slot taken waits
     * for the host and, when the host is stalled, drops the waiting frame after a bounded wait. */
    hle_d3d_renderer = "none";
    uint32_t presented0, dropped0, presented, dropped;
    for (int i = 0; i < 10; i++) hle_d3d_service();   /* present what the earlier swaps left */
    hle_d3d_present_stats(&presented0, &dropped0);
    call("D3DDevice_Swap", 1, A(0));
    for (int i = 0; i < 10; i++) hle_d3d_service();
    call("D3DDevice_Swap", 1, A(0));
    for (int i = 0; i < 10; i++) hle_d3d_service();
    hle_d3d_present_stats(&presented, &dropped);
    CHECK(presented - presented0 == 2 && dropped == dropped0, "a host that keeps up presents every swap (%u presented, %u dropped)",
          presented - presented0, dropped - dropped0);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 4; i++) call("D3DDevice_Swap", 1, A(0));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    for (int i = 0; i < 10; i++) hle_d3d_service();
    hle_d3d_present_stats(&presented0, &dropped0);
    CHECK(presented0 - presented == 1 && dropped0 - dropped == 3 && secs < 2.0,
          "four swaps with a stalled host -> one frame waits (%u presented, %u dropped, %.2f s)", presented0 - presented,
          dropped0 - dropped, secs);
    /* Hand-off: Swap returns once the host has taken the frame, so the title's next input read
     * is not older than the frame the host renders next. */
    pthread_t host;
    pthread_create(&host, NULL, late_host, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    call("D3DDevice_Swap", 1, A(0));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    pthread_join(host, NULL);
    secs = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    hle_d3d_present_stats(&presented, &dropped);
    CHECK(secs > 0.04 && secs < 0.1 && presented - presented0 == 1 && dropped == dropped0,
          "Swap returns when the host takes the frame (%.3f s, %u presented, %u dropped)", secs, presented - presented0, dropped - dropped0);
    CHECK(hle_d3d_frame_count() == 9, "swap count 9 (%u)", hle_d3d_frame_count());

    hle_d3d_summary();
    printf(failures ? "FAILED: %d\n" : "ALL OK\n", failures);
    return failures != 0;
}
