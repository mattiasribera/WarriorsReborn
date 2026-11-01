#define _GNU_SOURCE
#include "files.h"
#include "platform.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static char disc[1024], hdd[1024];
#define MAX_LINKS 64
static struct { char name[128], target[256]; } links[MAX_LINKS];
static int nlinks;

void files_add_symlink(const char *name, const char *target) {
    for (int i = 0; i < nlinks; i++)
        if (!strcasecmp(links[i].name, name)) { snprintf(links[i].target, sizeof links[i].target, "%s", target); return; }
    if (nlinks < MAX_LINKS) {
        snprintf(links[nlinks].name, sizeof links[0].name, "%s", name);
        snprintf(links[nlinks].target, sizeof links[0].target, "%s", target);
        nlinks++;
    }
}
void files_remove_symlink(const char *name) {
    for (int i = 0; i < nlinks; i++)
        if (!strcasecmp(links[i].name, name)) links[i] = links[--nlinks];
}
const char *files_symlink_target(const char *name) {
    for (int i = 0; i < nlinks; i++)
        if (!strcasecmp(links[i].name, name)) return links[i].target;
    return NULL;
}

void files_init(const char *disc_dir, const char *hdd_dir) {
    snprintf(disc, sizeof disc, "%s", disc_dir);
    snprintf(hdd, sizeof hdd, "%s", hdd_dir);
    plat_mkdir(hdd);
    files_add_symlink("\\??\\D:", "\\Device\\CdRom0");
}

/* Appends one component to host path, matching an existing entry case-insensitively. */
static int append_ci(char *path, size_t len, const char *comp) {
    char *end = path + strlen(path);
    DIR *d = opendir(path);
    const char *use = comp;
    struct dirent *e;
    if (d) {
        while ((e = readdir(d)))
            if (!strcasecmp(e->d_name, comp)) { use = e->d_name; break; }
    }
    int n = snprintf(end, len - (end - path), "/%s", use);
    if (d) closedir(d);
    return n < 0 || (size_t)n >= len - (end - path) ? -1 : 0;
}

int files_resolve(const char *xp, char *host, size_t hostlen, int *read_only) {
    char path[1024];
    snprintf(path, sizeof path, "%s", xp);
    /* Expand \??\X: style drive links (one level, then device mapping). */
    for (int pass = 0; pass < 4; pass++) {
        int best = -1;
        size_t bestlen = 0;
        for (int i = 0; i < nlinks; i++) {
            size_t l = strlen(links[i].name);
            if (l > bestlen && !strncasecmp(path, links[i].name, l) && (path[l] == '\\' || path[l] == 0)) { best = i; bestlen = l; }
        }
        if (best < 0) break;
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s%s", links[best].target, path + bestlen);
        snprintf(path, sizeof path, "%s", tmp);
    }
    const char *rest;
    *read_only = 0;
    if (!strncasecmp(path, "\\Device\\CdRom0", 14) && (path[14] == '\\' || !path[14])) {
        snprintf(host, hostlen, "%s", disc);
        rest = path + 14;
        *read_only = 1;
    } else if (!strncasecmp(path, "\\Device\\Harddisk0\\Partition", 27)) {
        int part = atoi(path + 27);
        rest = path + 27;
        while (*rest >= '0' && *rest <= '9') rest++;
        if (!*rest) {   /* raw volume/disk open: a sparse image file per partition */
            snprintf(host, hostlen, "%s/partition%d.img", hdd, part);
            struct stat st;
            if (stat(host, &st) != 0) {
                FILE *f = fopen(host, "wb");
                if (f) { fclose(f); if (truncate(host, part == 0 ? 0x80000 : 0x2EE00000)) return -1; }
            }
            return 0;
        }
        snprintf(host, hostlen, "%s/partition%d", hdd, part);
        plat_mkdir(host);
    } else {
        return -1;
    }
    char comp[256];
    while (*rest) {
        while (*rest == '\\') rest++;
        size_t n = strcspn(rest, "\\");
        if (!n) break;
        if (n >= sizeof comp) return -1;
        memcpy(comp, rest, n);
        comp[n] = 0;
        rest += n;
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) return -1;   /* no escape from the disc/hdd roots */
        if (append_ci(host, hostlen, comp)) return -1;
    }
    return 0;
}
