/* The in-game settings' apply queue. A change made in the menu
 * (the guest's thread, at its pad poll) is either applied live or recorded for the next start:
 *   live (class a)   the model's class of the change (sm_change_class against the start values) is
 *                    SM_LIVE and a setter is registered for the key that can take this value now:
 *                    the value is queued and the setter runs once at the next host frame boundary
 *                    (sa_frame, on the host thread after a frame was presented); a newer value for
 *                    the same key replaces a queued one, so a held left / right applies once a frame.
 *   on restart       every other change (b / c / d: W3 makes b and c live later). The menu records it
 *                    with settings_sync_set either way, so the launcher saves it; "Reiniciar ahora"
 *                    restarts through settings_sync_restart.
 * Thread-safe (one mutex); game-independent: the setters are registered by the runtime glue
 * (src/hle_settings.c) and by the tests (tools/settings_apply_test.c). */
#ifndef SETTINGS_APPLY_H
#define SETTINGS_APPLY_H
#include "settings_model.h"

typedef struct SaLive {
    const char *key;                   /* the profile key */
    int (*can)(const char *value);     /* NULL: always; 0: this value cannot apply live in this run */
    void (*set)(const char *value);    /* runs on the host thread at a frame boundary */
} SaLive;

/* The setters (kept by pointer: a static table). */
void sa_register(const SaLive *table, int n);
/* 1 when key = value would apply live now (class a for start and a setter that can take it). */
int sa_can_live(const SmValues *start, const char *key, const char *value);
/* Queues key = value for the next frame boundary when sa_can_live: 1 queued, 0 not live (record it
 * for the restart). */
int sa_request(const SmValues *start, const char *key, const char *value);
/* The host frame boundary: runs every queued setter once (in request order) and logs one
 * "[settings] live KEY = VALUE" line each. Returns the number applied. */
int sa_frame(void);
int sa_queued(void);   /* setters waiting for the next frame boundary */
void sa_reset(void);   /* tests: no setters, empty queue */

#endif
