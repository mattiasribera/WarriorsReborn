/* CPU interpreter of NV2A vertex program microcode (the output of XGAssembleShader that the
 * title passes to CreateVertexShader).
 *
 * Format (xemu hw/xbox/nv2a/pgraph/vsh.c, Cxbx-Reloaded XbVertexShader.cpp): a header dword
 * (type 0x78 in bits 0-7, version in bits 8-15, instruction count in bits 16-31), then four
 * dwords per instruction (dword 0 unused): one MAC operation (mov mul add mad dp3 dph dp4 dst min
 * max slt sge arl) and one ILU operation (mov rcp rcc rsq exp log lit) that read the same three
 * operand fields A, B (MAC) and C (ILU, and the MAC add/mad addend), write temporaries (a paired
 * ILU writes r1) and at most one output register; the last instruction has the FINAL bit.
 * Registers: v0-v15 inputs, r0-r11 temporaries (r12 reads oPos), c0-c191 constants (D3D c-96..c95;
 * index = D3D register + 96), a0.x. Constant writes (state/read-write programs) are ignored. */
#ifndef VSH_H
#define VSH_H
#include <stdint.h>

#define VSH_MAX_INSTRUCTIONS 136
#define VSH_NCONST 192

/* Output registers by their NV2A address. */
enum { VSH_OPOS = 0, VSH_OD0 = 3, VSH_OD1 = 4, VSH_OFOG = 5, VSH_OPTS = 6, VSH_OB0 = 7, VSH_OB1 = 8, VSH_OT0 = 9, VSH_NOUT = 13 };

typedef struct {
    uint8_t mux, reg, neg, swz[4];   /* mux: 1 temporary, 2 input, 3 constant */
} VshSrc;
typedef struct {
    uint8_t mac, ilu, a0x, mac_mask, ilu_mask, out_r, o_mask, o_reg, o_addr, o_ilu;
    uint8_t vreg;    /* input register of the operands that read v */
    uint16_t creg;   /* constant index (0..255) of the operands that read c */
    VshSrc a, b, c;
} VshIns;
typedef struct {
    uint32_t n;
    VshIns ins[VSH_MAX_INSTRUCTIONS];
} VshProgram;

/* Decodes a program from its header and microcode (at most max_dwords dwords readable).
 * Returns 0, or -1 for another program type, no instructions or a truncated buffer. Decoding
 * stops at the first FINAL instruction. */
int vsh_load(const uint32_t *function, uint32_t max_dwords, VshProgram *p);
/* Runs the program for one vertex: v = inputs v0-v15, c = the 192 constants. Outputs default to
 * (0, 0, 0, 1); the color outputs oD0, oD1, oB0 and oB1 are saturated to [0, 1]. */
void vsh_run(const VshProgram *p, const float v[16][4], const float c[VSH_NCONST][4], float o[VSH_NOUT][4]);
/* Batch form of vsh_run for n (1..VSH_BATCH) vertices, components in separate rows (structure of
 * arrays: v[reg][component][vertex]) so each instruction is decoded once per batch and its
 * arithmetic runs over contiguous lanes. Results are bit-identical to vsh_run per vertex. */
#define VSH_BATCH 32
void vsh_run_batch(const VshProgram *p, int n, const float v[16][4][VSH_BATCH], const float c[VSH_NCONST][4],
                   float o[VSH_NOUT][4][VSH_BATCH]);
/* The D3D library appends a screen-space transform to every program (oPos.xyz = oPos.xyz *
 * c[-38] / oPos.w + c[-37], with the viewport scale and offset its SetViewport writes there). This
 * writes scale (1, 1, 1, 1) and offset 0 into c, so oPos ends as (clip.xyz / w, w) ... */
void vsh_set_screen_constants(float c[VSH_NCONST][4]);
/* ... and this turns such an oPos back into clip space (xyz * w, w). */
void vsh_clip(const float opos[4], float clip[4]);
#endif
