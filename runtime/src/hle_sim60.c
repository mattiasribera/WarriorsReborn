/* --sim60: 60 Hz world (see hle_sim60.h). */
#include "hle_sim60.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Little-endian bytes of a 32-bit value (an operand address or a float's bits). */
#define B4(x) (uint8_t)((x) & 0xff), (uint8_t)(((x) >> 8) & 0xff), (uint8_t)(((x) >> 16) & 0xff), (uint8_t)(((x) >> 24) & 0xff)
/* An SSE load/multiply with an absolute operand (4 opcode/modrm bytes + address): from -> to. */
#define OPERAND(name, va, op0, op1, op2, op3, from, to) \
    {name, va, 8, {op0, op1, op2, op3, B4(from)}, {op0, op1, op2, op3, B4(to)}}
#define MOVSS_XMM0(name, va, from, to) OPERAND(name, va, 0xf3, 0x0f, 0x10, 0x05, from, to)
#define MULSS_XMM0(name, va, from, to) OPERAND(name, va, 0xf3, 0x0f, 0x59, 0x05, from, to)
#define NOP6 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00
/* jmp rel32 from the instruction at va (5 bytes) to target. */
#define JMP(va, target) 0xe9, B4((uint32_t)(target) - ((uint32_t)(va) + 5u))
/* test byte [0x5825fc],1: the low bit of the step counter that FUN_0016d8d0 increments once per
 * step (every step is a world update with --sim60), so code behind it runs 30 times a second. */
#define TEST_STEP_ODD 0xf6, 0x05, 0xfc, 0x25, 0x58, 0x00, 0x01

/* Read-only float constants in .rdata (title), and one we add. */
#define K_1_30 0x00410440u    /* 1/30: the world time step */
#define K_1_60 0x00410448u    /* 1/60 */
#define K_1_120 0x004306d4u   /* 1/120 */
/* 1/240 has no copy in the title: it goes into the 20 unused bytes between the end of .rdata
 * (0x44c06c) and .data (0x44c080), which no code references (checked as zero before writing). */
#define K_1_240 0x0044c070u
/* Half the litter's per-update gravity term (0.16333 / 2), in the same unused bytes. */
#define K_LITTER_G 0x0044c074u
#define F_LITTER_G 0x3da740dbu
/* Code for counters that no operand can halve: the unused tail of the image-header page
 * (SizeOfHeaders 0xbd4, the page is mapped and zero up to 0x11000; checked zero before writing). */
#define CAVE_DRAIN 0x00010c00u
#define CAVE_STRUGGLE 0x00010c20u
#define CAVE_WORKOUT 0x00010c40u
#define CAVE_FAD 0x00010c80u      /* FollowAndDefend period */
#define CAVE_FFORM 0x00010ca0u    /* FollowFormation period */
#define CAVE_FOBJ1 0x00010cc0u    /* FollowObject period, constructor 0x1f0930 */
#define CAVE_FOBJ2 0x00010ce0u    /* FollowObject period, constructor 0x1f5b70 */
#define CAVE_ROOTROT 0x00010d00u  /* animation root rotation: half angle per update */
#define K_QUAT_ID 0x00010d40u     /* (0, 0, 0, 1): the identity quaternion, 16 bytes */
#define CAVE_TURN 0x00010d50u     /* turn-rate cap wrapper: half the cap per update */
#define CAVE_HELD 0x00010d70u     /* held-button test: a poll counter (144 bytes, four entries) */
#define HELD_COUNTS 0x00010e00u   /* 256 one-byte poll counters (pad record offset + button bit) */
#define CAVE_TAP 0x00010f00u      /* tap test: reads the counted run on a release (100 bytes) */
#define CAVE_HITLIFE 0x00010f70u  /* weapon hit spheres: lifetime stored doubled (19 bytes) */
#define CAVE_RRSEL 0x00010f90u    /* root rotation: the 30 Hz sample on odd steps (74 bytes) */
/* Per-character root rotation cache (64 x 32 bytes: quaternion, step). Past the end of the image
 * (SizeOfImage ends at 0x5d41e0); map_image rounds the mapping up to 0x5d5000, the title never
 * references 0x5d41e0-0x5d4fff, so the page tail is zero and unused. */
#define RR_CACHE 0x005d4200u
#define CAVE_ANIMSNAP 0x005d4a00u /* after RR_CACHE: anim pose sample snap (99 bytes) */
#define CAVE_KEYSEG 0x005d4a80u   /* after it: key interval of a half-frame sample (93 bytes) */
/* call rel32 from the instruction at va (5 bytes) to target. */
#define CALL(va, target) 0xe8, B4((uint32_t)(target) - ((uint32_t)(va) + 5u))
/* mov r32,N (opcode b8+r, imm32) used as a divisor of the brain age: N -> 2N. */
#define AGE_DIV(name, va, op, n) {name, va, 5, {op, B4(n)}, {op, B4(2u * (n))}}
#define RANDOM_RANGE 0x0023e930u  /* thiscall random integer in [a, b] (RNG object 0x5a6f14) */
#define K_1_2 0x00410174u     /* 0.5 */
#define K_1_4 0x0041b1c0u     /* 0.25 */
#define K_1_8 0x0041bfacu     /* 0.125 */
#define K_1 0x0040ffc8u       /* 1.0 */
#define K_8 0x004106e0u       /* 8.0 */
#define K_16 0x0041ac0cu      /* 16.0 */
#define K_30 0x00410408u      /* 30.0 */
#define K_60 0x0041bb84u      /* 60.0 */
#define F_1_30 0x3d088889u
#define F_1_60 0x3c888889u
#define F_1_240 0x3b888889u

/* Title patches. .text VA = file offset + 0x10000; .rdata VA 0x40fb20 = file offset 0x402000;
 * .data VA 0x44c080 = file offset 0x43f000. */
const Sim60Patch sim60_table[] = {
    /* Step loop FUN_002b2430: steps run in pairs (inner count 2) and only when two are due
     * (threshold = step << 1, computed with shld/shl at both checks). Count 1, threshold 1 step:
     * with the frame-locked guest clock one step per presented frame. */
    {"step count", 0x002b24a0u, 8, {0xc7, 0x44, 0x24, 0x14, 0x02, 0x00, 0x00, 0x00}, {0xc7, 0x44, 0x24, 0x14, 0x01, 0x00, 0x00, 0x00}},
    {"step due", 0x002b2476u, 6, {0x0f, 0xa4, 0xea, 0x01, 0xd1, 0xe5}, {NOP6}},
    {"step again", 0x002b2572u, 6, {0x0f, 0xa4, 0xea, 0x01, 0xd1, 0xe5}, {NOP6}},
    /* FUN_0016d8d0: ++[0x5825fc]; test al,1; jne skip -> the world update runs on every step. */
    {"world gate", 0x0016d8d9u, 13, {0xa8, 0x01, 0xa3, 0xfc, 0x25, 0x58, 0x00, 0x0f, 0x85, 0x03, 0x08, 0x00, 0x00},
     {0xa8, 0x01, 0xa3, 0xfc, 0x25, 0x58, 0x00, NOP6}},
    /* AI round-robin FUN_001afab0(counter): human i thinks (FUN_001aa8a0) when i % 5 ==
     * (counter >> 1) % 5, once per 5 world updates (6 times a second). With an update per step
     * the slot would last two updates: counter % 10 == i % 5 keeps one think per 1/6 s. */
    {"ai think", 0x001afacau, 13, {0x8b, 0x44, 0x24, 0x04, 0xd1, 0xe8, 0x33, 0xd2, 0xb9, 0x05, 0x00, 0x00, 0x00},
     {0x8b, 0x44, 0x24, 0x04, 0x89, 0xc0, 0x33, 0xd2, 0xb9, 0x0a, 0x00, 0x00, 0x00}},
    /* The game clock [0x4aca1c] in fixed-step mode (+0x4c = 1: menus and front end) adds one
     * frame per update, 1/fps s with fps = [0x458860] (30), plus [0x458878] = 1/30 s in ticks to
     * its second accumulator. At 60 presented frames that is twice real time: two steps (two world
     * updates, two pad polls) per frame, so a button press seen by the first poll is gone by the
     * second and menus miss it. 60 fps and 1/60 s: one step per frame (data and static init). */
    {"frontend fps", 0x00458860u, 4, {0x1e, 0, 0, 0}, {0x3c, 0, 0, 0}},
    {"frontend fps init", 0x0006c93au, 5, {0xba, 0x1e, 0x00, 0x00, 0x00}, {0xba, 0x3c, 0x00, 0x00, 0x00}},
    {"frontend frame", 0x00458878u, 4, {B4(0x0174fe1cu)}, {B4(0x00ba7f0eu)}},
    {"frontend frame init", 0x0006c9c3u, 10, {0xc7, 0x05, 0x78, 0x88, 0x45, 0x00, B4(0x0174fe1cu)},
     {0xc7, 0x05, 0x78, 0x88, 0x45, 0x00, B4(0x00ba7f0eu)}},
    /* World time step [0x45cc38]: initial value, then every writer (normal and slow motion). */
    {"dt initial", 0x0045cc38u, 4, {B4(F_1_30)}, {B4(F_1_60)}},
    {"1/240", K_1_240, 4, {0, 0, 0, 0}, {B4(F_1_240)}},
    MULSS_XMM0("dt scale", 0x0015aa26u, K_1_30, K_1_60),        /* dt = scale * 1/30 */
    MOVSS_XMM0("dt reset", 0x0015aa40u, K_1_30, K_1_60),        /* dt = 1/30 */
    MULSS_XMM0("dt human scale", 0x0015fe07u, K_1_30, K_1_60),  /* 0 < scale < 1: dt = scale/30 */
    MOVSS_XMM0("dt human reset", 0x0015fe2cu, K_1_30, K_1_60),
    MOVSS_XMM0("dt slow 1/2", 0x001b9025u, K_1_60, K_1_120),    /* slow motion: 1/60 -> 1/120 */
    MOVSS_XMM0("dt slow 1/4", 0x001b9069u, K_1_120, K_1_240),
    MOVSS_XMM0("dt slow end", 0x001b9088u, K_1_30, K_1_60),
    MOVSS_XMM0("dt slow end 2", 0x001b90b0u, K_1_30, K_1_60),
    MOVSS_XMM0("dt slow 1/4 b", 0x001b938cu, K_1_120, K_1_240),
    MOVSS_XMM0("dt slow end 3", 0x001b93acu, K_1_30, K_1_60),
    MULSS_XMM0("dt script scale", 0x00335ed4u, K_1_30, K_1_60), /* dt = [0x460e98] * 1/30 */
    MOVSS_XMM0("dt script reset", 0x00335f11u, K_1_30, K_1_60),
    MOVSS_XMM0("dt script reset 2", 0x00335f29u, K_1_30, K_1_60),
    /* World update, per human with a model fading out (human+0xb4): the alpha byte (+0x2b of
     * the colour at +0x28) drops by 10 per update while above 10. cmp cl,10; jbe; sub cl,10 -> 5. */
    {"human fade", 0x0016dc30u, 8, {0x80, 0xf9, 0x0a, 0x76, 0x1e, 0x80, 0xe9, 0x0a}, {0x80, 0xf9, 0x0a, 0x76, 0x1e, 0x80, 0xe9, 0x05}},
    /* End of the world update: the cameras step by max(dt, 1/30). */
    OPERAND("camera dt floor", 0x0016e0bfu, 0xf3, 0x0f, 0x10, 0x0d, K_1_30, K_1_60),
    /* Litter pool 1 (FUN_0008bc00, first call of the world update; 64 pieces of 0x70 bytes):
     * position += velocity (units/s) * 1/30 per update. Everything else is counted in updates and
     * doubles below, so the litter moves, falls, tumbles and respawns as at 30 Hz. The wind kick
     * FUN_0008b270 sets velocity = velocity * 0.7 + wind * 1/30 (a velocity, not a step) every
     * +0x68 = random(1, 30) updates: with the interval doubled it keeps the title's 1/30 (halving
     * it, with the damping per kick, gave half the 30 Hz drift speed). */
    MOVSS_XMM0("litter step", 0x0008c390u, K_1_30, K_1_60),
    {"litter probe", 0x0008bd6du, 4, {0xc6, 0x46, 0x6e, 0x14}, {0xc6, 0x46, 0x6e, 0x28}},  /* ground ray every 20 -> 40 */
    /* Settle time +0x66 = random(600, 900) updates on the ground, then the lifetime starts. */
    {"litter settle", 0x0008ae04u, 10, {0x68, B4(900u), 0x68, B4(600u)}, {0x68, B4(1800u), 0x68, B4(1200u)}},
    /* Lifetime +0x64 = 60 updates (settled, or far from the camera), then the piece respawns. */
    {"litter life", 0x0008be8fu, 6, {0x66, 0xc7, 0x46, 0x64, 0x3c, 0x00}, {0x66, 0xc7, 0x46, 0x64, 0x78, 0x00}},
    {"litter life far", 0x0008bf6bu, 6, {0x66, 0xc7, 0x46, 0x64, 0x3c, 0x00}, {0x66, 0xc7, 0x46, 0x64, 0x78, 0x00}},
    /* Wind kick interval +0x68 = random(1, 30) (push 30; push 1), at spawn and after each kick. */
    {"litter kick init", 0x0008adc8u, 4, {0x6a, 0x1e, 0x6a, 0x01}, {0x6a, 0x3c, 0x6a, 0x02}},
    {"litter kick", 0x0008bf80u, 4, {0x6a, 0x1e, 0x6a, 0x01}, {0x6a, 0x3c, 0x6a, 0x02}},
    /* Tumble: a kick in the air blends the rotation over +0x60 = +0x62 = random(30, 60) updates
     * (weight +0x62 / +0x60, +0x62 counted down per update). */
    {"litter tumble", 0x0008b79bu, 7, {0x6a, 0x3c, 0x89, 0x48, 0x0c, 0x6a, 0x1e}, {0x6a, 0x78, 0x89, 0x48, 0x0c, 0x6a, 0x3c}},
    /* Gravity: vz -= (0.5 - 0.5 |n.z|) * 0.32667 + 0.16333 per update (0x420778, 0x420774):
     * 4.9-9.8 units/s^2 at 30 Hz. Half per update: x 0.16333 (0x420774) + 0.081667 (added). */
    {"litter gravity", 0x0008bfd2u, 12, {0xd8, 0x0d, B4(0x00420778u), 0xd8, 0x05, B4(0x00420774u)},
     {0xd8, 0x0d, B4(0x00420774u), 0xd8, 0x05, B4(K_LITTER_G)}},
    {"litter gravity constant", K_LITTER_G, 4, {0, 0, 0, 0}, {B4(F_LITTER_G)}},
    /* Litter pool 2 (FUN_0008d0c0; 36 pieces of 0x5c bytes, rotation only): the kick interval +0x54
     * = random(20, 60) (spawn FUN_0008caa0, refresh), the sway blend +0x4c = +0x4e = random(30, 60)
     * (kick FUN_0008ce00) and the lifetime +0x50 = 60 updates. */
    {"litter2 kick init", 0x0008cd6fu, 4, {0x6a, 0x3c, 0x6a, 0x14}, {0x6a, 0x78, 0x6a, 0x28}},
    {"litter2 kick", 0x0008d1fau, 4, {0x6a, 0x3c, 0x6a, 0x14}, {0x6a, 0x78, 0x6a, 0x28}},
    {"litter2 sway", 0x0008d094u, 7, {0x6a, 0x3c, 0x89, 0x48, 0x0c, 0x6a, 0x1e}, {0x6a, 0x78, 0x89, 0x48, 0x0c, 0x6a, 0x3c}},
    {"litter2 life", 0x0008d1e7u, 6, {0x66, 0xc7, 0x46, 0x50, 0x3c, 0x00}, {0x66, 0xc7, 0x46, 0x50, 0x78, 0x00}},
    /* Airborne fail-safe (human update, reached through the human vtable): while the character's
     * state has 0x4000000 (airborne) +0x344 counts updates; at 60, when the mover (+0x170,
     * FUN_002503a0, once per update from FUN_00168780) has also failed to resolve its collision
     * for 60 consecutive moves (+0x68, reset by a free move), FUN_00142220 puts the character back
     * at its last safe position (+0x348). 2 s at 30 Hz, 1 s at 60 Hz: both limits 120. */
    {"airborne count", 0x0016b292u, 3, {0x83, 0xf8, 0x3c}, {0x83, 0xf8, 0x78}},
    {"airborne stuck count", 0x0016b2a7u, 4, {0x83, 0x78, 0x68, 0x3c}, {0x83, 0x78, 0x68, 0x78}},
    /* Button-tap run (HuSetButtonTapControl -> FUN_0015ae00 -> human state FUN_00169d00): the
     * mission 8 cart chase (also bound in missions 14 and 18). Per update, a tap (action 0x2d)
     * stores the updates since the last tap in +0x610, otherwise +0x614 counts up (capped at 10,
     * which also sets +0x610); the run speed is max * (1 - clamp((+0x610 - 2) / 8, 0, 1)), and a
     * slower target is approached by 1/30 of the difference per update. Taps T s apart give 30T - 1
     * at 30 Hz and 60T - 1 at 60 Hz: (60T - 1 - 5) / 16 = (30T - 1 - 2) / 8; cap 21 = 2 x 10 + 1
     * (idle stop after the same 0.37 s); 1/60 per update for the slow-down. 16.0 and 60.0 are
     * existing .rdata constants. */
    {"tap cap", 0x0016a0a7u, 5, {0xb8, 0x0a, 0x00, 0x00, 0x00}, {0xb8, 0x15, 0x00, 0x00, 0x00}},
    {"tap offset", 0x0016a0c5u, 3, {0x83, 0xea, 0x02}, {0x83, 0xea, 0x05}},
    OPERAND("tap range", 0x0016a0ccu, 0xf3, 0x0f, 0x5e, 0x05, K_8, K_16),
    OPERAND("tap slow-down", 0x0016a15cu, 0xf3, 0x0f, 0x5e, 0x15, K_30, K_60),
    /* Scene task FUN_002b0270 (in-engine cinematics, fixed scenes, scene animations): the scene
     * camera and objects follow keyframes at 30 per s, interpolated with the task's own elapsed
     * time, and the task calls the scene camera's update. It runs on the task wheel FUN_002b2fc0
     * (one slot per 1/60 s step) and reschedules itself every 2 steps (FUN_002b1080(2)): 30 Hz, in
     * step with the 30 Hz world. With the 60 Hz world the cinematic camera would move on every
     * second frame only, under characters animating at 60 Hz: run it every step. Its per-run
     * counters keep their duration: the start delay (+0xc0, frames) drops 0.5 per run (0.5 at
     * 0x410174), the skip lock-out (+0xc8 = 60 runs, set in FUN_002abe00) becomes 120 runs. */
    {"scene task rate", 0x002b0300u, 2, {0x6a, 0x02}, {0x6a, 0x01}},
    OPERAND("scene delay", 0x002b032au, 0xf3, 0x0f, 0x5c, 0x05, K_1, K_1_2),
    {"scene skip lock", 0x002abe68u, 10, {0xc7, 0x86, 0xc8, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00},
     {0xc7, 0x86, 0xc8, 0x00, 0x00, 0x00, 0x78, 0x00, 0x00, 0x00}},
    /* Health drain in the 0x4000 state (FUN_0014cee0: 3 s from a hazard zone, e.g. fire): FUN_0017a6f0
     * runs once per world update and takes 1 health per update (`dec word [edi+0x144]`), so at 60 Hz
     * the same 3 s took twice the health. The decrement runs on even step counts only. */
    {"drain gate", 0x0017a754u, 7, {0x66, 0xff, 0x8f, 0x44, 0x01, 0x00, 0x00}, {JMP(0x0017a754u, CAVE_DRAIN), 0x66, 0x90}},
    {"drain cave", CAVE_DRAIN, 21, {0},
     {TEST_STEP_ODD, 0x75, 0x07, 0x66, 0xff, 0x8f, 0x44, 0x01, 0x00, 0x00, JMP(CAVE_DRAIN + 16u, 0x0017a75bu)}},
    /* The hazard check before it: human update FUN_0016adb0 counts +0x660 down once per update
     * from [0x45cae4] = 30 (1 s), then FUN_00142150 (inside a hazard zone of the list at 0x4fe018)
     * starts the 0x4000 state. 60 updates keep the second. */
    {"hazard delay", 0x0045cae4u, 4, {0x1e, 0, 0, 0}, {0x3c, 0, 0, 0}},
    /* Struggle meter (CfgButtonMash(15, 250, 1000); FUN_0019d890, grapples): each press adds,
     * each update without one subtracts [0x45cb28] (15, set from Lua, so not patchable as data)
     * from the meter at +0x40. The cave subtracts it on even step counts and 0 on odd ones. */
    {"struggle gate", 0x0019d9aau, 7, {0x66, 0x8b, 0x0d, 0x28, 0xcb, 0x45, 0x00}, {JMP(0x0019d9aau, CAVE_STRUGGLE), 0x66, 0x90}},
    {"struggle cave", CAVE_STRUGGLE, 23, {0},
     {0x66, 0x8b, 0x0d, 0x28, 0xcb, 0x45, 0x00, TEST_STEP_ODD, 0x74, 0x02, 0x31, 0xc9,
      JMP(CAVE_STRUGGLE + 18u, 0x0019d9b1u)}},
    /* Gym workouts (CfgWorkoutParams; human state FUN_0017b020: chin-ups, push-ups, ...): a press
     * (action 0x12) sets the hold count +0x614 from a table; every update without one decrements it
     * and, while it is non-zero, adds the gain to the progress +0x610, else subtracts the decay
     * (both from Lua tables). At 60 Hz the decay per second doubled and the hold ran out in half the
     * time: "mash hard and barely move". Presses are still seen on every update; the countdown and
     * the gain/decay step run on even step counts only, the odd ones keep the progress. */
    {"workout gate", 0x0017b3f4u, 6, {0x8b, 0x86, 0x14, 0x06, 0x00, 0x00}, {JMP(0x0017b3f4u, CAVE_WORKOUT), 0x90}},
    {"workout cave", CAVE_WORKOUT, 36, {0},
     {TEST_STEP_ODD, 0x75, 0x0b, 0x8b, 0x86, 0x14, 0x06, 0x00, 0x00, JMP(CAVE_WORKOUT + 15u, 0x0017b3fau),
      0xc1, 0xe7, 0x02, 0xf3, 0x0f, 0x10, 0x86, 0x10, 0x06, 0x00, 0x00, JMP(CAVE_WORKOUT + 31u, 0x0017b481u)}},
    /* Blood gush effect attached for 30 updates (+0x31c, FUN_0016fea0; counted down in FUN_0016adb0). */
    {"blood gush", 0x00170016u, 10, {0xc7, 0x87, 0x1c, 0x03, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00},
     {0xc7, 0x87, 0x1c, 0x03, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00}},
    /* AI brain age: FUN_001afab0 runs the brain tick FUN_001a8870
     * for every brain on every world update; it counts the age [brain+0x28] and runs the goal's
     * update, and goals do their periodic work when age % N == 0 (pedestrians re-decide, cower,
     * scatter, cull and fade out, followers re-path, ...). At 60 Hz each fired twice per second:
     * NPCs "rushed" in scripted scenes. Every divisor is doubled, so the same decisions happen per
     * game second (a 2N period at 60 updates/s). */
    AGE_DIV("age ManWeaponPile 10", 0x001c0748u, 0xb9, 10),
    AGE_DIV("age ObjectThrower 15", 0x001c0ec5u, 0xb9, 15),
    AGE_DIV("age ObjectThrower 45", 0x001c0f06u, 0xbf, 45),
    AGE_DIV("age ObjectThrower 5", 0x001c101eu, 0xb9, 5),
    AGE_DIV("age AreaWalker 7", 0x001c3da6u, 0xb9, 7),
    AGE_DIV("age PedReaction 30", 0x001c61e1u, 0xb9, 30),
    AGE_DIV("age goal_1c6cb0 300", 0x001c6cc0u, 0xbe, 300),
    AGE_DIV("age Pedestrian 90", 0x001c70edu, 0xbd, 90),
    AGE_DIV("age Pedestrian 20", 0x001c7559u, 0xb9, 20),
    AGE_DIV("age Peddler 30", 0x001c832cu, 0xb9, 30),
    AGE_DIV("age Grabbed 30", 0x001cb841u, 0xb9, 30),
    AGE_DIV("age Mounted 30", 0x001cbac6u, 0xb9, 30),
    AGE_DIV("age HelpRespond 30", 0x001cbd7eu, 0xbf, 30),
    AGE_DIV("age StandGround 30", 0x001cda43u, 0xbd, 30),
    AGE_DIV("age goal_1ce1d0 20", 0x001ce235u, 0xb9, 20),
    AGE_DIV("age Spectate 30", 0x001d1660u, 0xb9, 30),
    AGE_DIV("age Spectate 5", 0x001d1873u, 0xb9, 5),
    AGE_DIV("age FightBackOff 45", 0x001d23cau, 0xb9, 45),
    AGE_DIV("age SaveHuman 5", 0x001d2b97u, 0xb9, 5),
    AGE_DIV("age SaveHuman 3", 0x001d2d16u, 0xb9, 3),
    AGE_DIV("age ObjectPile 10", 0x001d3d52u, 0xb9, 10),
    AGE_DIV("age ObjectPile 30", 0x001d3e7du, 0xbb, 30),
    AGE_DIV("age GrabTarget 30", 0x001d426bu, 0xb9, 30),
    AGE_DIV("age goal_1d5d20 20", 0x001d5d8eu, 0xbe, 20),
    AGE_DIV("age FollowAndAttack 20", 0x001d641au, 0xb9, 20),
    AGE_DIV("age goal_1d68e0 20", 0x001d694eu, 0xbe, 20),
    AGE_DIV("age goal_1dade0 40", 0x001dadefu, 0xbe, 40),
    AGE_DIV("age Patrol 90", 0x001dd9c1u, 0xb9, 90),
    AGE_DIV("age Patrol 192", 0x001ddbd7u, 0xbd, 192),
    AGE_DIV("age goal_1e5ac0 45", 0x001e5ac9u, 0xbe, 45),
    AGE_DIV("age AddressPerson 30", 0x001e6846u, 0xb9, 30),
    AGE_DIV("age goal_1e7300 10", 0x001e7332u, 0xb9, 10),
    AGE_DIV("age Steal 30", 0x001e7ce1u, 0xb9, 30),
    AGE_DIV("age Riot 60", 0x001e9cc3u, 0xb9, 60),
    AGE_DIV("age Riot 90", 0x001e9d5cu, 0xb9, 90),
    AGE_DIV("age Scatter 90", 0x001eaab5u, 0xbf, 90),
    AGE_DIV("age Cower 90", 0x001eb5bfu, 0xbf, 90),
    AGE_DIV("age Cower 45", 0x001eb668u, 0xb9, 45),
    AGE_DIV("age Cower 45", 0x001eb6aau, 0xb9, 45),
    AGE_DIV("age goal_1f26d0 30", 0x001f2b39u, 0xbd, 30),
    AGE_DIV("age MoveToUseFlag 15", 0x001f3248u, 0xbf, 15),
    AGE_DIV("age DestroyCar 30", 0x001f4a5eu, 0xb9, 30),
    AGE_DIV("age FollowPlayer 30", 0x001f53d1u, 0xb9, 30),
    AGE_DIV("age TrackHuman 30", 0x001f5943u, 0xb9, 30),
    AGE_DIV("age TrackHuman 40", 0x001f596du, 0xbb, 40),
    AGE_DIV("age FollowFormation 6", 0x001f6065u, 0xb9, 6),
    AGE_DIV("age goal_1f64c0 10", 0x001f659du, 0xb9, 10),
    AGE_DIV("age AvoidEnemies 30", 0x001f895bu, 0xb9, 30),
    AGE_DIV("age goal_1f8cc0 30", 0x001f8d68u, 0xb9, 30),
    AGE_DIV("age Confront 15", 0x001f8e40u, 0xb9, 15),
    AGE_DIV("age Shopkeeper 10", 0x001fb894u, 0xb9, 10),
    AGE_DIV("age StationaryThrower 5", 0x001fe701u, 0xbf, 5),
    AGE_DIV("age BigThrower 5", 0x002062ddu, 0xbf, 5),
    AGE_DIV("age StationaryShooterA 30", 0x00208dcfu, 0xb9, 30),
    AGE_DIV("age LutherShooter 30", 0x0020938au, 0xb9, 30),
    AGE_DIV("age LutherShooter 3", 0x00209406u, 0xb9, 3),
    AGE_DIV("age StationaryShooterB 30", 0x0020a3f3u, 0xb9, 30),
    AGE_DIV("age HideAndSeek 30", 0x0020bf08u, 0xbf, 30),
    AGE_DIV("age goal_1e5da0 30", 0x001e5dceu, 0xbe, 30),
    AGE_DIV("age brain health 90", 0x001a888bu, 0xbf, 90),
    /* The age == 30 test (FUN_001a0e20: one second after creation). */
    {"age 30 test", 0x001a0e25u, 4, {0x83, 0x7e, 0x28, 0x1e}, {0x83, 0x7e, 0x28, 0x3c}},
    /* Periods kept in the goal: FollowAndDefend +0x32 = random(45, 75) and FollowFormation +0x37 =
     * random(2, 7), stored doubled; FollowObject +0x2c comes from the caller (two constructors). */
    {"FollowAndDefend gate", 0x001d5932u, 8, {CALL(0x001d5932u, RANDOM_RANGE), 0x88, 0x46, 0x32},
     {JMP(0x001d5932u, CAVE_FAD), 0x90, 0x90, 0x90}},
    {"FollowAndDefend cave", CAVE_FAD, 15, {0},
     {CALL(CAVE_FAD, RANDOM_RANGE), 0x02, 0xc0, 0x88, 0x46, 0x32, JMP(CAVE_FAD + 10u, 0x001d593au)}},
    {"FollowFormation gate", 0x001f0b0bu, 8, {CALL(0x001f0b0bu, RANDOM_RANGE), 0x88, 0x46, 0x37},
     {JMP(0x001f0b0bu, CAVE_FFORM), 0x90, 0x90, 0x90}},
    {"FollowFormation cave", CAVE_FFORM, 15, {0},
     {CALL(CAVE_FFORM, RANDOM_RANGE), 0x02, 0xc0, 0x88, 0x46, 0x37, JMP(CAVE_FFORM + 10u, 0x001f0b13u)}},
    {"FollowObject gate 1", 0x001f096cu, 6, {0x89, 0x48, 0x2c, 0xc2, 0x10, 0x00}, {JMP(0x001f096cu, CAVE_FOBJ1), 0x90}},
    {"FollowObject cave 1", CAVE_FOBJ1, 8, {0}, {0x01, 0xc9, 0x89, 0x48, 0x2c, 0xc2, 0x10, 0x00}},
    {"FollowObject gate 2", 0x001f5bdau, 8, {0xf3, 0x0f, 0x11, 0x40, 0x28, 0x89, 0x50, 0x2c},
     {JMP(0x001f5bdau, CAVE_FOBJ2), 0x90, 0x90, 0x90}},
    {"FollowObject cave 2", CAVE_FOBJ2, 15, {0},
     {0xf3, 0x0f, 0x11, 0x40, 0x28, 0x01, 0xd2, 0x89, 0x50, 0x2c, JMP(CAVE_FOBJ2 + 10u, 0x001f5be2u)}},
    /* Animation root motion: the human update FUN_0016adb0 takes
     * the clip's root delta (character vtable +0x58 = FUN_001610a0, records at 0x571e80 + anim *
     * 0x464: a translation that is integrated with dt, and a rotation quaternion per 1/30 s) and
     * FUN_00164f50 multiplies the rotation into the character's facing once per world update. With
     * char+0x250 == -1 it scales the angle by dt * 30 first; otherwise (scripted clips: cutscene
     * walks, turns to marks) the quaternion is applied as it is, so at 60 Hz every scripted turn was
     * twice as far per second while the walk speed stayed right: the mission 7 opening Warriors
     * curved past their marks and an NPC walked through the player. The cave applies the square
     * root of the quaternion, (q + 1) / |q + 1| (half the angle about the same axis): two 60 Hz
     * updates turn exactly as far as one 30 Hz update, also in slow motion. The replaced bytes copy
     * the quaternion (+0x10 of the record) to the local at [esp+0x10]; no branch lands inside. */
    {"root rotation gate", 0x00165123u, 30,
     {0x83, 0xc6, 0x10, 0x8b, 0x16, 0x8b, 0x46, 0x04, 0x8b, 0x4e, 0x08, 0x89, 0x54, 0x24, 0x10,
      0x8b, 0x56, 0x0c, 0x89, 0x44, 0x24, 0x14, 0x89, 0x4c, 0x24, 0x18, 0x89, 0x54, 0x24, 0x1c},
     {JMP(0x00165123u, CAVE_ROOTROT), 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
      0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
    /* add esi,0x10; movups xmm0,[esi]; movups xmm1,[K_QUAT_ID]; addps xmm0,xmm1 (q + 1);
     * movaps xmm1,xmm0; mulps xmm1,xmm1; movhlps xmm2,xmm1; addps xmm1,xmm2; movaps xmm2,xmm1 */
    {"root rotation cave", CAVE_ROOTROT, 31, {0},
     {0x83, 0xc6, 0x10, 0x0f, 0x10, 0x06, 0x0f, 0x10, 0x0d, B4(K_QUAT_ID), 0x0f, 0x58, 0xc1, 0x0f, 0x28, 0xc8,
      0x0f, 0x59, 0xc9, 0x0f, 0x12, 0xd1, 0x0f, 0x58, 0xca, 0x0f, 0x28, 0xd1}},
    /* shufps xmm2,xmm2,0x55; addss xmm1,xmm2 (|q + 1|^2); sqrtss xmm1,xmm1; shufps xmm1,xmm1,0;
     * divps xmm0,xmm1; movups [esp+0x10],xmm0; jmp back after the replaced copy */
    {"root rotation cave 2", CAVE_ROOTROT + 31u, 29, {0},
     {0x0f, 0xc6, 0xd2, 0x55, 0xf3, 0x0f, 0x58, 0xca, 0xf3, 0x0f, 0x51, 0xc9, 0x0f, 0xc6, 0xc9, 0x00,
      0x0f, 0x5e, 0xc1, 0x0f, 0x11, 0x44, 0x24, 0x10, JMP(CAVE_ROOTROT + 55u, 0x00165141u)}},
    {"identity quaternion", K_QUAT_ID, 16, {0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, B4(0x3f800000u)}},
    /* Root rotation sampling: the root delta FUN_00164f50 gets
     * (a local copy of the clip record) holds the rotation of the 1/30 s key frame its sample time
     * falls in, so the two 60 Hz updates of one 30 Hz update usually read the same key and the
     * halves add up. Where a clip loops or switches between the two samples (the 32-degree last
     * key of a loop on the even step, the new loop's 0 on the odd one) only one half was applied,
     * and a zero-angle sample skips the rotation code altogether: Luther's fight in l84_c7 (scene
     * clips, char+0x250 = 2181) ended 25 degrees off after a few such boundaries, his root motion
     * walked sideways and the knife missed him. Even steps sample the same clip times as the 30 Hz
     * world (the step counter is even on its updates). Before the angle test (0x165097, no branch
     * lands inside the 9 bytes) a scripted clip's quaternion is stored per character slot
     * (char+0x7a & 63) with the step count on even steps, and on an odd step the stored one
     * replaces the sample when it is from the step before: the 60 Hz pair applies two halves of
     * the 30 Hz sample. char+0x250 == -1 clips (dt-scaled) are left as they are. */
    {"root rotation sample gate", 0x00165097u, 9, {0x8b, 0x56, 0x1c, 0x52, CALL(0x0016509bu, 0x00036970u)},
     {JMP(0x00165097u, CAVE_RRSEL), 0x90, 0x90, 0x90, 0x90}},
    /* cmp dword [ebx+0x250],-1; je tail; movsx eax,word [ebx+0x7a]; and eax,63; shl eax,5;
     * add eax,RR_CACHE; mov ecx,[0x5825fc]; test cl,1; jnz odd; movups xmm0,[esi+0x10] */
    {"root rotation sample", CAVE_RRSEL, 39, {0},
     {0x83, 0xbb, 0x50, 0x02, 0x00, 0x00, 0xff, 0x74, 0x33, 0x0f, 0xbf, 0x43, 0x7a, 0x83, 0xe0, 0x3f,
      0xc1, 0xe0, 0x05, 0x05, B4(RR_CACHE), 0x8b, 0x0d, 0xfc, 0x25, 0x58, 0x00, 0xf6, 0xc1, 0x01, 0x75,
      0x0c, 0x0f, 0x10, 0x46, 0x10}},
    /* movups [eax],xmm0; mov [eax+0x10],ecx; jmp tail; odd: dec ecx; cmp [eax+0x10],ecx; jne tail;
     * movups xmm0,[eax]; movups [esi+0x10],xmm0 */
    {"root rotation sample 2", CAVE_RRSEL + 39u, 21, {0},
     {0x0f, 0x11, 0x00, 0x89, 0x48, 0x10, 0xeb, 0x0d, 0x49, 0x39, 0x48, 0x10, 0x75, 0x07, 0x0f, 0x10, 0x00,
      0x0f, 0x11, 0x46, 0x10}},
    /* tail: the replaced mov edx,[esi+0x1c]; push edx; call 0x36970; jmp 0x1650a0 */
    {"root rotation sample 3", CAVE_RRSEL + 60u, 14, {0},
     {0x8b, 0x56, 0x1c, 0x52, CALL(CAVE_RRSEL + 64u, 0x00036970u), JMP(CAVE_RRSEL + 69u, 0x001650a0u)}},
    /* Turn-rate caps (sim60-polish T1): FUN_00149af0 (thiscall, no stack arguments, result in
     * st0) returns a character's maximum turn per world update in radians, from the table at
     * 0x45ca90 that Lua sets with CfgSetTurnRates (1.5 to 24 degrees per update: 45 to 720 degrees
     * per second at 30 Hz) or two constants. The human states add an acceleration of cap * f to
     * the turn speed +0x588 and clamp it to the cap once per world update, so at 60 Hz every turn
     * was twice as fast. The entry jumps to a wrapper that calls the body (with the replaced
     * prologue) and multiplies the result by 0.5 (0x410174): cap and acceleration per update
     * halve, the same degrees per second. The decay of +0x588 (x0.8 per update, Lua-set) is kept:
     * the steady turn speed per second is unchanged by it, only the ease-in of small corrections
     * settles in half the time. Other readers of the table (FUN_00149a10 from route planning,
     * 0x1527e0, 0x152930, 0x20fff1) keep the 30 Hz per-update values the planner was built on. */
    {"turn cap gate", 0x00149af0u, 6, {0x83, 0xec, 0x08, 0x56, 0x8b, 0xf1}, {JMP(0x00149af0u, CAVE_TURN), 0x90}},
    /* call body; fmul dword [0.5]; ret; body: sub esp,8; push esi; mov esi,ecx; jmp 0x149af6 */
    {"turn cap cave", CAVE_TURN, 23, {0},
     {CALL(CAVE_TURN, CAVE_TURN + 12u), 0xd8, 0x0d, B4(K_1_2), 0xc3,
      0x83, 0xec, 0x08, 0x56, 0x8b, 0xf1, JMP(CAVE_TURN + 18u, 0x00149af6u)}},
    /* Held-button test (sim60-polish T2): FUN_00069440(pad, mask) (cdecl, result in al) is true
     * once a button has been down for the last N = [0x458774] polls (CfgButtonHeldFrames(7)) of the
     * pad's 8-entry history (+0x1c, index +0x2c) and was up within it, i.e. exactly once per hold,
     * at the N-th poll. The pad is polled per world update, so at 60 Hz a hold was recognised after
     * 117 ms instead of 233 ms: Y and B presses of 7-13 frames gave the hold actions (0xb, 0xe) on
     * top of the press actions (measured). 2N polls do not fit the history, so the entry jumps to
     * a replacement that counts consecutive down polls in a byte per pad record and button bit
     * (HELD_COUNTS, zero page tail) and is true when the count reaches 2N: once per hold, after the
     * same 233 ms. Its only caller, the binding loop FUN_0006b460 (0x6b56f), tests each held
     * binding once per poll, before the tap bindings. On the poll that releases the button the
     * byte keeps the run length with bit 7 set (cleared on the next up poll, restarted by a press)
     * for the tap test below. */
    {"held gate", 0x00069440u, 6, {0x8b, 0x15, 0x74, 0x87, 0x45, 0x00}, {JMP(0x00069440u, CAVE_HELD), 0x90}},
    /* Tap test FUN_00069350(pad, mask) (cdecl, al; its only caller is the same loop, 0x6b7ac): true
     * on the poll that releases a button that was down for at most 6 polls (the history holds 8):
     * a B tap is the grab (action 0xd), B held for N polls the tackle (0xe). At 30 Hz every B press
     * is one of the two: a tap up to 200 ms, a hold from 233 ms. With the hold at 2N polls only, B
     * presses of 7-13 polls at 60 Hz (117-217 ms) gave neither: the grab "did nothing" (measured,
     * fix-weapon-heavy). The entry jumps to a replacement that, on a release, reads the run the held
     * counter kept and is true when it was shorter than 2N polls; without a counted run (a button
     * the held table does not test) it runs the title's own test. */
    {"tap gate", 0x00069350u, 7, {0x8b, 0x4c, 0x24, 0x04, 0x8b, 0x41, 0x2c}, {JMP(0x00069350u, CAVE_TAP), 0x90, 0x90}},
    /* held: ecx = pad; ebx = mask; edi = (pad - 0x4ac790 + bsf(mask)) & 0xff (counter index);
     * latest entry [ecx+0x1c+2*(idx&7)] down: previous entry down and no run-ended flag: count + 1
     * (capped at 127), else 1; al = count == 2 * [0x458774]. Latest up: previous down: flag the
     * count (bit 7), else clear it; al = 0. */
    {"held cave", CAVE_HELD, 40, {0},
     {0x8b, 0x4c, 0x24, 0x04, 0x53, 0x56, 0x57, 0x0f, 0xb7, 0x5c, 0x24, 0x14, 0x0f, 0xbc, 0xd3, 0x74,
      0x79, 0x8d, 0x7c, 0x11, 0x70, 0x81, 0xe7, 0xff, 0x00, 0x00, 0x00, 0x8b, 0x41, 0x2c, 0x89, 0xc2,
      0x83, 0xe2, 0x07, 0x0f, 0xb7, 0x74, 0x51, 0x1c}},
    {"held cave 2", CAVE_HELD + 40u, 40, {0},
     {0x21, 0xde, 0x39, 0xde, 0x75, 0x37, 0x48, 0x83, 0xe0, 0x07, 0x0f, 0xb7, 0x74, 0x41, 0x1c, 0x21,
      0xde, 0x0f, 0xb6, 0x87, 0x00, 0x0e, 0x01, 0x00, 0x39, 0xde, 0x75, 0x04, 0xa8, 0x80, 0x74, 0x02,
      0x31, 0xc0, 0x83, 0xf8, 0x7f, 0x83, 0xd0, 0x00}},
    {"held cave 3", CAVE_HELD + 80u, 40, {0},
     {0x88, 0x87, 0x00, 0x0e, 0x01, 0x00, 0x8b, 0x15, 0x74, 0x87, 0x45, 0x00, 0x01, 0xd2, 0x39, 0xd0,
      0x0f, 0x94, 0xc0, 0xeb, 0x27, 0x48, 0x83, 0xe0, 0x07, 0x0f, 0xb7, 0x74, 0x41, 0x1c, 0x0f, 0xb6,
      0x87, 0x00, 0x0e, 0x01, 0x00, 0x85, 0xde, 0x74}},
    {"held cave 4", CAVE_HELD + 120u, 24, {0},
     {0x0a, 0x0c, 0x80, 0x88, 0x87, 0x00, 0x0e, 0x01, 0x00, 0xeb, 0x07, 0xc6, 0x87, 0x00, 0x0e, 0x01,
      0x00, 0x00, 0x31, 0xc0, 0x5f, 0x5e, 0x5b, 0xc3}},
    /* tap: latest entry up and previous down for the mask, else 0; counter flagged with a run of
     * 1..127: al = run < 2 * [0x458774]; otherwise replay the replaced prologue and continue in the
     * title's test at 0x69357. */
    {"tap cave", CAVE_TAP, 40, {0},
     {0x8b, 0x4c, 0x24, 0x04, 0x8b, 0x41, 0x2c, 0x0f, 0xb7, 0x54, 0x41, 0x1c, 0x56, 0x57, 0x8b, 0x74,
      0x24, 0x10, 0x85, 0xd6, 0x75, 0x3b, 0x8d, 0x50, 0xff, 0x83, 0xe2, 0x07, 0x0f, 0xb7, 0x54, 0x51,
      0x1c, 0x85, 0xd6, 0x74, 0x2c, 0x0f, 0xbc, 0xd6}},
    {"tap cave 2", CAVE_TAP + 40u, 40, {0},
     {0x8d, 0x7c, 0x11, 0x70, 0x81, 0xe7, 0xff, 0x00, 0x00, 0x00, 0x0f, 0xb6, 0x97, 0x00, 0x0e, 0x01,
      0x00, 0x84, 0xd2, 0x79, 0x19, 0x83, 0xe2, 0x7f, 0x74, 0x14, 0xa1, 0x74, 0x87, 0x45, 0x00, 0x01,
      0xc0, 0x39, 0xc2, 0x0f, 0x92, 0xc0, 0x5f, 0x5e}},
    {"tap cave 3", CAVE_TAP + 80u, 20, {0},
     {0xc3, 0x31, 0xc0, 0x5f, 0x5e, 0xc3, 0x5f, 0x5e, 0x8b, 0x4c, 0x24, 0x04, 0x8b, 0x41, 0x2c, JMP(CAVE_TAP + 95u, 0x00069357u)}},
    /* Weapon hit spheres (fix-weapon-heavy): a held weapon's attack calls FUN_00162370(human, N),
     * which adds spheres along the weapon (FUN_0016ce20) to the attacker's collision body (human
     * vtable +0x74) and sets their lifetime body+0xb0 = N + 1; the collision update counts it down
     * once per world update and drops the spheres at 0. Hits are swept sphere tests between two
     * updates (FUN_00251130 from the human update), so only the part of the swing inside the
     * lifetime can hit: 6 updates = 200 ms at 30 Hz, but only 100 ms with --sim60, the first half
     * of the arc. A bat's heavy swing (move 0x24) then ended before reaching an enemy in front
     * (measured: hit spheres present at 7125-7135 at 30 Hz, 7123-7128 with --sim60, same path per
     * second) and downed enemies, low in the arc, were never hit. The store
     * `mov eax,[esp+0x3c]; inc eax; mov [edi+0xb0],eax` jumps to a cave that stores 2 x (N + 1). */
    {"hit life gate", 0x001623a8u, 11, {0x8b, 0x44, 0x24, 0x3c, 0x40, 0x89, 0x87, 0xb0, 0x00, 0x00, 0x00},
     {JMP(0x001623a8u, CAVE_HITLIFE), 0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
    /* mov eax,[esp+0x3c]; lea eax,[eax+eax+2]; mov [edi+0xb0],eax; jmp 0x1623b3 */
    {"hit life cave", CAVE_HITLIFE, 19, {0},
     {0x8b, 0x44, 0x24, 0x3c, 0x8d, 0x44, 0x00, 0x02, 0x89, 0x87, 0xb0, 0x00, 0x00, 0x00, JMP(CAVE_HITLIFE + 14u, 0x001623b3u)}},
    /* Stealth walk (fix-weapon-heavy): in stealth the state dispatcher (0x1801a1) keeps the walk
     * state (table 0x45d390 entry 4) only while the character's current speed is at least
     * FUN_0013fa40() x 0.25 (the walk speed, 1.73); else it falls back to stealth idle (0x182510 ->
     * 0x17f860), which resets the speed. The speed ramps up by 0.8 per update at 30 Hz but 0.4 at
     * 60 Hz (dt-scaled), so the first 60 Hz walk update was always below 0.43: walk one update,
     * idle ten, and the character stood still (measured 4.0 units in 2.3 s at 30 Hz, 0.09 with
     * --sim60). The threshold is 0.125 x the walk speed, which the first 60 Hz step reaches. */
    {"stealth walk threshold", 0x00180453u, 6, {0xd8, 0x0d, B4(K_1_4)}, {0xd8, 0x0d, B4(K_1_8)}},
    /* Climb pop: the pose sampler FUN_000180e0 samples a clip at
     * cursor time +0x18 x 30, linear between keys; the whole-frame counter +0x1c (rounded time x 30,
     * FUN_00015890) fires the clip's events (FUN_00013d70) when base +0x128 + counter reaches their
     * frame. The climb clips (gen_climb_wall_stand_p2 and three more, the only ones with an event of
     * type 0x40) jump the hips 1.35-2.55 m between key 0 and key 1 and cancel it with that event, an
     * actor offset teleport at frame 1. At 30 Hz the sample time is a whole frame; at 60 Hz the tick
     * between the keys drew the hips half way across the jump: the climber popped about a metre
     * (out of place, into a wall or another light) for one frame on every climb. The load of the
     * sample time jumps to a cave that keeps time x 30 except when the next whole frame carries a
     * 0x40 event: then it samples at the counter, the same key the teleport has (or has not yet)
     * been applied for. */
    {"anim snap gate", 0x00018104u, 16,
     {0xf3, 0x0f, 0x10, 0x45, 0x18, 0x8b, 0x55, 0x04, 0xf3, 0x0f, 0x59, 0x05, B4(K_30)},
     {JMP(0x00018104u, CAVE_ANIMSNAP), 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
    /* movss xmm0,[ebp+0x18]; mov edx,[ebp+4] (clip); mulss xmm0,[30.0]; push ebx/esi/edi/ecx;
     * movzx ebx,word [edx+0x1a] (events); test ebx,ebx; jz done; esi = [edx+0x1c] + word [edx+0x10]
     * + word [edx+0x12] + [edx+0x14] (event list); ecx = trunc(xmm0) + 1 + word [ebp+0x128] */
    {"anim snap cave", CAVE_ANIMSNAP, 40, {0},
     {0xf3, 0x0f, 0x10, 0x45, 0x18, 0x8b, 0x55, 0x04, 0xf3, 0x0f, 0x59, 0x05, B4(K_30), 0x53, 0x56, 0x57, 0x51,
      0x0f, 0xb7, 0x5a, 0x1a, 0x85, 0xdb, 0x74, 0x3e, 0x8b, 0x72, 0x1c, 0x0f, 0xb7, 0x7a, 0x10, 0x01, 0xfe,
      0x0f, 0xb7, 0x7a}},
    /* loop: cmp word [esi+2],0x40; jne next; movzx edi,word [esi]; cmp edi,ecx; jne next;
     * movzx edi,word [ebp+0x1c]; cvtsi2ss xmm0,edi (the counter); jmp done */
    {"anim snap cave 2", CAVE_ANIMSNAP + 40u, 40, {0},
     {0x12, 0x01, 0xfe, 0x03, 0x72, 0x14, 0xf3, 0x0f, 0x2c, 0xc8, 0x41, 0x0f, 0xb7, 0xbd, 0x28, 0x01, 0x00, 0x00,
      0x01, 0xf9, 0x66, 0x83, 0x7e, 0x02, 0x40, 0x75, 0x11, 0x0f, 0xb7, 0x3e, 0x39, 0xcf, 0x75, 0x0a, 0x0f, 0xb7,
      0x7d, 0x1c, 0xf3, 0x0f}},
    /* next: add esi,0x18; dec ebx; jnz loop; done: pop ecx/edi/esi/ebx; jmp 0x18114 */
    {"anim snap cave 3", CAVE_ANIMSNAP + 80u, 19, {0},
     {0x2a, 0xc7, 0xeb, 0x06, 0x83, 0xc6, 0x18, 0x4b, 0x75, 0xe2, 0x59, 0x5f, 0x5e, 0x5b,
      JMP(CAVE_ANIMSNAP + 94u, 0x00018114u)}},
    /* Ground pops: the sampler interpolates each track between its
     * cursor key and the next, rotations with FUN_00018070 and the two position tracks with
     * FUN_00015f90 (cdecl: key record, out, key frame, time x 30; fraction (time x 30 - frame) /
     * dt of record + 8). The cursors advance with the whole-frame counter, and that is rounded
     * (trunc(time x 30 + 0.5), FUN_00015890), so on a 60 Hz tick half way to a key the cursor
     * already sits on that key: the fraction is -0.5 / dt, the pose extrapolated backwards along
     * the next interval. Where the motion turns at that key (a body hitting the floor, a limb
     * bouncing, the end of a stomp) the extrapolation drew a pose no key holds for one frame;
     * a 30 Hz tick is a whole frame and always at or after its key. Both functions are called
     * only by the sampler; their entries call a check that, when the key frame lies after the
     * time and the key is not the first of its track (dt byte != 0), passes the previous record
     * and its frame (frame - dt): the interval the time falls in. Climb snaps sample at the
     * counter, never before the key, and are left alone. */
    {"key interval gate rotation", 0x00018070u, 8, {0x83, 0xec, 0x20, 0x0f, 0xb7, 0x4c, 0x24, 0x2c},
     {JMP(0x00018070u, CAVE_KEYSEG + 0x30u), 0x90, 0x90, 0x90}},
    {"key interval gate position", 0x00015f90u, 7, {0x83, 0xec, 0x20, 0x8b, 0x44, 0x24, 0x24},
     {JMP(0x00015f90u, CAVE_KEYSEG + 0x50u), 0x90, 0x90}},
    /* check (called at the entry: record [esp+8], frame [esp+0x10], time x 30 [esp+0x14]):
     * movzx ecx,word [esp+0x10]; mov eax,[esp+8]; movzx edx,byte [eax] (dt into this key);
     * test edx,edx; jz ret; push ecx; fild dword [esp]; pop ecx; fcomp dword [esp+0x14];
     * fnstsw ax; sahf; jbe ret (frame <= time, or unordered) */
    {"key interval check", CAVE_KEYSEG, 30, {0},
     {0x0f, 0xb7, 0x4c, 0x24, 0x10, 0x8b, 0x44, 0x24, 0x08, 0x0f, 0xb6, 0x10, 0x85, 0xd2, 0x74, 0x19,
      0x51, 0xdb, 0x04, 0x24, 0x59, 0xd8, 0x5c, 0x24, 0x14, 0xdf, 0xe0, 0x9e, 0x76, 0x0b}},
    /* sub dword [esp+8],8 (previous record); sub ecx,edx; mov [esp+0x10],ecx (its frame); ret */
    {"key interval check 2", CAVE_KEYSEG + 30u, 12, {0},
     {0x83, 0x6c, 0x24, 0x08, 0x08, 0x29, 0xd1, 0x89, 0x4c, 0x24, 0x10, 0xc3}},
    /* call check; the replaced sub esp,0x20; movzx ecx,word [esp+0x2c]; jmp 0x18078 */
    {"key interval rotation", CAVE_KEYSEG + 0x30u, 18, {0},
     {CALL(CAVE_KEYSEG + 0x30u, CAVE_KEYSEG), 0x83, 0xec, 0x20, 0x0f, 0xb7, 0x4c, 0x24, 0x2c,
      JMP(CAVE_KEYSEG + 0x3du, 0x00018078u)}},
    /* call check; the replaced sub esp,0x20; mov eax,[esp+0x24]; jmp 0x15f97 */
    {"key interval position", CAVE_KEYSEG + 0x50u, 17, {0},
     {CALL(CAVE_KEYSEG + 0x50u, CAVE_KEYSEG), 0x83, 0xec, 0x20, 0x8b, 0x44, 0x24, 0x24,
      JMP(CAVE_KEYSEG + 0x5cu, 0x00015f97u)}},
};
const int sim60_count = (int)(sizeof sim60_table / sizeof sim60_table[0]);

/* Appends "s" to the report, keeping it NUL-terminated and within cap. */
static void put(char *buf, size_t cap, size_t *len, const char *s) {
    if (*len + 1 >= cap) return;
    int n = snprintf(buf + *len, cap - *len, "%s", s);
    if (n > 0) *len += (size_t)n < cap - *len ? (size_t)n : cap - *len - 1;
}

static int patch_matches(const Sim60Patch *p, intptr_t bias) {
    if (!p->len || p->len > SIM60_BYTES_MAX) return 0;
    return !memcmp((const void *)(uintptr_t)(p->va + bias), p->orig, p->len);
}

int sim60_apply(const Sim60Patch *t, int n, int enabled, intptr_t bias, char *report, size_t cap) {
    size_t len = 0;
    if (!cap) return 0;
    report[0] = 0;
    if (!enabled) {
        put(report, cap, &len, "[sim60] off: the world updates 30 times a second (--sim60: experimental 60 Hz world)");
        return 0;
    }
    char list[16384] = "";
    size_t llen = 0;
    int bad = 0;
    for (int i = 0; i < n; i++) {
        if (patch_matches(&t[i], bias)) continue;
        char item[64];
        snprintf(item, sizeof item, "%s%s (%08x)", bad++ ? ", " : "", t[i].name, t[i].va);
        put(list, sizeof list, &llen, item);
    }
    if (bad) {
        put(report, cap, &len, "[sim60] applied: none; skipped: ");
        put(report, cap, &len, list);
        put(report, cap, &len, " (signature mismatch: nothing written, the world stays at 30 Hz)");
        return 0;
    }
    /* Diagnostic: XBRUN_SIM60_SKIP="prefix,prefix" leaves entries whose name starts with one of
     * them unwritten (bisecting a 60 Hz difference; caves of a skipped gate are then unused). */
    const char *skip = getenv("XBRUN_SIM60_SKIP");
    for (int i = 0; i < n; i++) {
        int skipped = 0;
        for (const char *q = skip; q && *q && !skipped;) {
            size_t k = strcspn(q, ",");
            skipped = k && !strncmp(t[i].name, q, k);
            q += k + (q[k] == ',');
        }
        if (skipped) {
            printf("[sim60] diagnostic: %s not applied (XBRUN_SIM60_SKIP)\n", t[i].name);
            continue;
        }
        memcpy((void *)(uintptr_t)(t[i].va + bias), t[i].repl, t[i].len);
        char item[64];
        snprintf(item, sizeof item, "%s%s (%08x)", i ? ", " : "", t[i].name, t[i].va);
        put(list, sizeof list, &llen, item);
    }
    /* A list longer than the report cuts the list (ending in "...") so the summary stays. */
    static const char head[] = "[sim60] applied: ", tail[] = "; skipped: none", more[] = " ...";
    put(report, cap, &len, head);
    size_t room = cap > len + sizeof tail ? cap - len - sizeof tail : 0;
    if (n && llen > room && room >= sizeof more) {
        list[room - (sizeof more - 1)] = 0;
        put(report, cap, &len, list);
        put(report, cap, &len, more);
    } else {
        put(report, cap, &len, n ? list : "none");
    }
    put(report, cap, &len, tail);
    return n;
}

int sim60_parse_arg(const char *arg, int *enabled) {
    if (!strcmp(arg, "--sim60")) { *enabled = 1; return 1; }
    if (!strcmp(arg, "--no-sim60")) { *enabled = 0; return 1; }
    return 0;
}

int hle_sim60_active;

int hle_sim60_install(int enabled) {
    char report[16384];
    int n = sim60_apply(sim60_table, sim60_count, enabled, 0, report, sizeof report);
    printf("%s\n", report);
    hle_sim60_active = n > 0;
    return hle_sim60_active;
}
