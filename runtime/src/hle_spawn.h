/* Spawn scaling experiment (--spawn-scale N): the title's gang spawners keep a "max alive at
 * once" word per spawner (+0x68e in a 0x120-byte spawner record, 4 per gang) and the world
 * object at [0x5b9f78] keeps a global cap (+0x420) on the characters its spawners have alive
 * (+0x41e), and a cop cap (+0x31e) on the cops alive (+0x31c). Characters live in a static pool
 * of 60 slots (handles 0..0x3b of the table at 0x5a7570, 8 bytes each, first dword non-zero
 * when used); a full pool makes spawners skip.
 *
 * With a scale above 1, the stores that set a spawner's max alive (GangAddSpawner's store at
 * 0x81d5d, GangSetMaxConcurrent's store at 0x806d1) and SetSpawnMax / SetCopSpawnMax
 * (0x1b6110 / 0x1b6130) are patched to multiply the scripts' values, and so are the defaults of
 * both caps for levels that never call them (12 from the world object's constructor, 16 from the
 * reset the world runs on each level load). A
 * gang tracks at most 16 members (16 dword slots at gang+0x48), so a spawner's max is clamped
 * to 16; the global caps are clamped to the 60-slot pool. A value is never lowered by the scaling, and values <= 0
 * (0 or -1 = none / unlimited) are kept. Scale 1 patches nothing. */
#ifndef HLE_SPAWN_H
#define HLE_SPAWN_H
#include <stdint.h>

#define SPAWN_SCALE_MAX 8
#define SPAWN_SPAWNER_CAP 16           /* gang member slots (gang+0x48, 16 dwords) */
#define SPAWN_POOL_SLOTS 60            /* character handles 0..0x3b */

#define SPAWN_ADD_STORE 0x00081d5du    /* mov [edi+0x68e], cx in GangAddSpawner's core (0x81c80) */
#define SPAWN_SETMAX_STORE 0x000806d1u /* mov [ecx+esi+0x68e], dx in GangSetMaxConcurrent (0x806b0) */
#define SPAWN_SETSPAWNMAX 0x001b6110u  /* SetSpawnMax core: [0x5b9f78]+0x420 = word arg */
#define SPAWN_SETCOPMAX 0x001b6130u    /* SetCopSpawnMax core: [0x5b9f78]+0x31e = word arg */
#define SPAWN_DEFAULT_CAPS 0x00338876u /* world constructor (0x3386b0): mov eax, 12 -> +0x420 and +0x31e */
#define SPAWN_RESET_CAPS 0x00338c2eu   /* world level reset (0x338b30, called from 0x7bdbd): mov eax, 16 -> +0x420 and +0x31e */
#define SPAWN_WORLD_PTR 0x005b9f78u    /* the world object holding the counts and caps */
#define SPAWN_HANDLE_TABLE 0x005a7570u /* handle slots, 8 bytes each; 0..0x3b are characters */

/* value * scale, clamped to cap but never below value; value <= 0 is returned unchanged. */
int spawn_scale_value(int value, int scale, int cap);
/* "N" with 1 <= N <= SPAWN_SCALE_MAX -> N, anything else -> -1. */
int spawn_scale_parse(const char *s);

/* Patches the sites above for scale > 1 (each only when its bytes match); returns the number
 * of sites patched, 0 for scale 1. */
int hle_spawn_install(int scale);

typedef struct {
    int valid;          /* the world object exists */
    int live, cap;      /* spawner-created characters alive (+0x41e) and SetSpawnMax's cap (+0x420) */
    int cops, cop_cap;  /* +0x31c / +0x31e */
    int slots;          /* occupied character handle slots (of SPAWN_POOL_SLOTS) */
} SpawnPool;
void spawn_pool_read(SpawnPool *p);

extern int hle_spawn_report;   /* print a [pool] line per 60 presented frames */
void hle_spawn_frame(uint32_t swap);   /* host thread, once per presented frame */
#endif
