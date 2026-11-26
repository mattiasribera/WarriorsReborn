/* The in-game settings' apply queue: see settings_apply.h. */
#include "settings_apply.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define QUEUE_MAX 32
#define VALUE_MAX 256

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static const SaLive *table;
static int ntable;
static struct {
    const SaLive *s;
    char value[VALUE_MAX];
} queue[QUEUE_MAX];
static int nqueue;
/* Keys a setter changed in this run: going back to the start value is then live too (a row whose
 * own class is not a, such as the temporal upscaling mode, is live only while it changes live). */
static unsigned char touched[256];

void sa_register(const SaLive *t, int n) {
    pthread_mutex_lock(&lock);
    table = t;
    ntable = n;
    nqueue = 0;
    memset(touched, 0, sizeof touched);
    pthread_mutex_unlock(&lock);
}

static const SaLive *setter(const char *key) {
    for (int i = 0; i < ntable; i++)
        if (!strcmp(table[i].key, key)) return &table[i];
    return NULL;
}

/* Caller holds the lock. */
static const SaLive *live_setter(const SmValues *start, const char *key, const char *value) {
    const int i = sm_find(key);
    const SmSetting *s = sm_setting(i);
    const SaLive *l = s ? setter(key) : NULL;
    if (!l || !l->set || !sm_valid(s, value) || sm_change_class(start, i, value) != SM_LIVE) return NULL;
    const char *v0 = sm_get(start, key);
    const int changed = !v0 || strcmp(v0, value);
    if (s->cls != SM_LIVE && !changed && !(i < (int)sizeof touched && touched[i])) return NULL;
    if (l->can && !l->can(value)) return NULL;
    return l;
}

int sa_can_live(const SmValues *start, const char *key, const char *value) {
    pthread_mutex_lock(&lock);
    const int ok = live_setter(start, key, value) != NULL;
    pthread_mutex_unlock(&lock);
    return ok;
}

int sa_request(const SmValues *start, const char *key, const char *value) {
    pthread_mutex_lock(&lock);
    const SaLive *l = live_setter(start, key, value);
    int q = -1;
    for (int k = 0; l && k < nqueue && q < 0; k++)
        if (queue[k].s == l) q = k;
    if (l && q < 0 && nqueue < QUEUE_MAX) q = nqueue++;
    if (q >= 0) snprintf(queue[q].value, sizeof queue[q].value, "%s", value), queue[q].s = l;
    const int i = sm_find(key);
    if (q >= 0 && i >= 0 && i < (int)sizeof touched) touched[i] = 1;
    pthread_mutex_unlock(&lock);
    return q >= 0;
}

int sa_frame(void) {
    pthread_mutex_lock(&lock);
    const int n = nqueue;
    for (int k = 0; k < n; k++) {
        queue[k].s->set(queue[k].value);
        printf("[settings] live %s = %s (applied at the frame boundary)\n", queue[k].s->key, queue[k].value);
    }
    nqueue = 0;
    pthread_mutex_unlock(&lock);
    if (n) fflush(stdout);
    return n;
}

int sa_queued(void) {
    pthread_mutex_lock(&lock);
    const int n = nqueue;
    pthread_mutex_unlock(&lock);
    return n;
}

void sa_reset(void) { sa_register(NULL, 0); }
