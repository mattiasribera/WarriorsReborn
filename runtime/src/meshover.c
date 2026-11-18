/* Mesh overrides of the Lua mods: see meshover.h. */
#include "meshover.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEMO_SIZE 4096
#define MAX_GROUPS 32

static struct {
    MeshOverride *o;
    int n;
    struct {
        char name[96];
        int on;
        int has_marker;    /* an instance group: its "<marker>.worn" file */
        uint64_t marker;
        int placed;        /* a placed group: its "placed" file */
        int np;            /* points (meshover_point_set) */
        struct {
            char key[MESHOVER_KEY];
            float p[3];
        } pt[MESHOVER_POINTS];
    } g[MAX_GROUPS];
    int ng;
    struct {
        uint64_t key, hash;
    } memo[MEMO_SIZE];
} mo;

static int hex16(const char *s, uint64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return -1;
        v = v << 4 | (uint64_t)d;
    }
    *out = v;
    return 0;
}

uint64_t meshover_draw_hash(uint32_t prim, const uint16_t *idx, uint32_t ni, uint32_t lo, uint32_t hi, const uint8_t *s0,
                            uint32_t stride0, const uint8_t *s1, uint32_t stride1) {
    uint64_t h = meshpack_hash(idx, ni, lo, hi, s0, stride0, s1, stride1);
    return prim == 5 ? h : h ^ (uint64_t)prim * 0x9e3779b97f4a7c15ull;
}

static int suffix(const char *s, int *hide) {
    if (!strcmp(s, ".wmesh")) return *hide = 0, 0;
    if (!strcmp(s, ".hide")) return *hide = 1, 0;
    return -1;
}

int meshover_parse_name(const char *name, uint64_t *mesh, uint64_t *tex, int *has_tex, int *hide) {
    if (hex16(name, mesh)) return -1;
    *has_tex = 0;
    *tex = 0;
    if (!suffix(name + 16, hide)) return 0;
    if (name[16] != '-' || hex16(name + 17, tex) || suffix(name + 33, hide)) return -1;
    *has_tex = 1;
    return 0;
}

static int matches(const MeshOverride *o, uint64_t mesh, uint64_t tex, int has_tex) {
    return o->mesh.hash == mesh && o->has_tex == has_tex && (!has_tex || o->tex == tex);
}

/* Loading: the override of exactly this name in this group (duplicates are skipped). */
static const MeshOverride *find_exact(uint64_t mesh, uint64_t tex, int has_tex, int group) {
    for (int i = 0; i < mo.n; i++)
        if (matches(&mo.o[i], mesh, tex, has_tex) && mo.o[i].group == group) return &mo.o[i];
    return NULL;
}

/* Drawing: an enabled group's override of this name first (groups in off count as off for this
 * draw), else the always-on one. */
static const MeshOverride *find_active(uint64_t mesh, uint64_t tex, int has_tex, uint32_t off) {
    const MeshOverride *always = NULL;
    for (int i = 0; i < mo.n; i++) {
        const MeshOverride *o = &mo.o[i];
        if (!matches(o, mesh, tex, has_tex)) continue;
        if (!o->group) {
            if (!always) always = o;
        } else if (mo.g[o->group - 1].on && !(off >> (o->group - 1) & 1)) {
            return o;
        }
    }
    return always;
}

static int valid_group_name(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 40) return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) return 0;
    return 1;
}

static int group_index(const char *name) {
    for (int i = 0; i < mo.ng; i++)
        if (!strcmp(mo.g[i].name, name)) return i;
    return -1;
}

static uint8_t *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    uint8_t *buf = NULL;
    long n = -1;
    if (f && !fseek(f, 0, SEEK_END) && (n = ftell(f)) > 0 && !fseek(f, 0, SEEK_SET) && (buf = malloc((size_t)n)) &&
        fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    if (f) fclose(f);
    *len = buf ? (size_t)n : 0;
    return buf;
}

/* A "<marker>.worn" file of a group folder: the group is an instance group; a file "placed": a
 * placed group (meshover.h). */
static int add_marker(const char *dir, const char *name, int group) {
    uint64_t mesh;
    if (!strcmp(name, "placed")) {
        if (!group) printf("[meshover] %s/placed: belongs in a group folder; ignored\n", dir);
        else mo.g[group - 1].placed = 1;
        return 1;
    }
    if (hex16(name, &mesh) || strcmp(name + 16, ".worn")) return 0;
    if (!group) {
        printf("[meshover] %s/%s: a .worn file belongs in a group folder; ignored\n", dir, name);
        return 1;
    }
    if (mo.g[group - 1].has_marker && mo.g[group - 1].marker != mesh) {
        printf("[meshover] %s/%s: the group already has the marker %016llx; ignored\n", dir, name,
               (unsigned long long)mo.g[group - 1].marker);
        return 1;
    }
    mo.g[group - 1].has_marker = 1;
    mo.g[group - 1].marker = mesh;
    return 1;
}

/* The override files of one folder, tagged with group (0: always on). */
static int add_files(const char *dir, int mod, int group) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int added = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        uint64_t mesh, tex;
        int has_tex, hide;
        if (add_marker(dir, e->d_name, group)) continue;
        if (meshover_parse_name(e->d_name, &mesh, &tex, &has_tex, &hide) || find_exact(mesh, tex, has_tex, group)) continue;
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        MeshOverride *no = realloc(mo.o, (size_t)(mo.n + 1) * sizeof *no);
        if (!no) break;
        mo.o = no;
        if (hide) {   /* no mesh: the draw is skipped */
            MeshPackMesh none = {0};
            none.hash = mesh;
            mo.o[mo.n++] = (MeshOverride){none, tex, has_tex, NULL, mod, group, 1, 0};
            added++;
            continue;
        }
        size_t len;
        uint8_t *buf = read_all(path, &len);
        MeshPackMesh m;
        if (buf && !meshpack_parse_any(buf, len, mesh, &m)) {
            MeshOverride *o = &mo.o[mo.n++];
            *o = (MeshOverride){m, tex, has_tex, NULL, mod, group, 0, 0};
            size_t k = strlen(path) - 6;   /* ".wmesh" -> ".png" */
            memcpy(path + k, ".png", 5);
            FILE *f = fopen(path, "rb");
            if (f) {
                fclose(f);
                o->png = strdup(path);
            }
            added++;
        } else {
            printf("[meshover] skipped %s: not a valid mesh\n", path);
        }
        free(buf);
    }
    closedir(d);
    return added;
}

int meshover_add_dir(const char *dir, int mod, const char *id) {
    int added = add_files(dir, mod, 0);
    if (added < 0 || !id) return added;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char sub[4096], name[sizeof mo.g[0].name];
        if (!valid_group_name(e->d_name) || (size_t)snprintf(name, sizeof name, "%s/%s", id, e->d_name) >= sizeof name) continue;
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        DIR *t = opendir(sub);
        if (!t) continue;   /* a file, not a folder */
        closedir(t);
        int g = group_index(name);
        if (g < 0) {
            if (mo.ng == MAX_GROUPS) {
                printf("[meshover] %s: more than %d groups; skipped\n", sub, MAX_GROUPS);
                continue;
            }
            g = mo.ng++;
            snprintf(mo.g[g].name, sizeof mo.g[g].name, "%s", name);
            mo.g[g].on = 0;
            mo.g[g].has_marker = 0;
            mo.g[g].placed = 0;
            mo.g[g].np = 0;
        }
        int k = add_files(sub, mod, g + 1);
        if (k > 0) added += k;
    }
    if (d) closedir(d);
    return added;
}

int meshover_group_set(const char *name, int on) {
    int g = name ? group_index(name) : -1;
    if (g < 0) return -1;
    mo.g[g].on = on != 0;
    int n = 0;
    for (int i = 0; i < mo.n; i++) n += mo.o[i].group == g + 1;
    return n;
}

int meshover_group_on(const char *name) {
    int g = name ? group_index(name) : -1;
    return g < 0 ? -1 : mo.g[g].on;
}

int meshover_count(void) { return mo.n; }

int meshover_count_mod(int mod) {
    int n = 0;
    for (int i = 0; i < mo.n; i++) n += mo.o[i].mod == mod;
    return n;
}

int meshover_has_mesh(uint64_t mesh, int *needs_tex) {
    int any = 0;
    *needs_tex = 0;
    for (int i = 0; i < mo.n; i++)
        if (mo.o[i].mesh.hash == mesh) {
            any = 1;
            if (mo.o[i].has_tex) *needs_tex = 1;
        }
    return any;
}

const MeshOverride *meshover_find_inst(uint64_t mesh, uint64_t tex, int tex_known, uint32_t off) {
    const MeshOverride *o = tex_known ? find_active(mesh, tex, 1, off) : NULL;
    return o ? o : find_active(mesh, 0, 0, off);
}

const MeshOverride *meshover_find(uint64_t mesh, uint64_t tex, int tex_known) { return meshover_find_inst(mesh, tex, tex_known, 0); }

uint32_t meshover_instance_mask(uint64_t mesh) {
    uint32_t m = 0;
    for (int i = 0; i < mo.n; i++) {
        const int g = mo.o[i].group - 1;
        if (g >= 0 && mo.o[i].mesh.hash == mesh && mo.g[g].on && mo.g[g].has_marker) m |= 1u << g;
    }
    return m;
}

uint32_t meshover_marker_mask(uint64_t mesh) {
    uint32_t m = 0;
    for (int g = 0; g < mo.ng; g++)
        if (mo.g[g].on && mo.g[g].has_marker && mo.g[g].marker == mesh) m |= 1u << g;
    return m;
}

int meshover_point_set(const char *group, const char *key, const float *p) {
    const int g = group ? group_index(group) : -1;
    if (g < 0 || !key) return -1;
    int i = 0;
    while (i < mo.g[g].np && strncmp(mo.g[g].pt[i].key, key, MESHOVER_KEY - 1)) i++;
    if (!p) {   /* remove: the last point takes its slot */
        if (i < mo.g[g].np) mo.g[g].pt[i] = mo.g[g].pt[--mo.g[g].np];
        return mo.g[g].np;
    }
    if (i == mo.g[g].np) {
        if (i == MESHOVER_POINTS) return -2;
        snprintf(mo.g[g].pt[i].key, MESHOVER_KEY, "%s", key);
        mo.g[g].np++;
    }
    memcpy(mo.g[g].pt[i].p, p, sizeof mo.g[g].pt[i].p);
    return mo.g[g].np;
}

void meshover_points_clear(void) {
    for (int g = 0; g < mo.ng; g++) mo.g[g].np = 0;
}

int meshover_point_count(int g) { return g >= 0 && g < mo.ng ? mo.g[g].np : 0; }

const float *meshover_point(int g, int i) { return g >= 0 && g < mo.ng && i >= 0 && i < mo.g[g].np ? mo.g[g].pt[i].p : NULL; }

int meshover_points_any(void) {
    for (int g = 0; g < mo.ng; g++)
        if (mo.g[g].np) return 1;
    return 0;
}

uint32_t meshover_placed_mask(uint64_t mesh) {
    uint32_t m = 0;
    for (int i = 0; i < mo.n; i++) {
        const int g = mo.o[i].group - 1;
        if (g >= 0 && mo.o[i].mesh.hash == mesh && mo.g[g].on && mo.g[g].placed) m |= 1u << g;
    }
    return m;
}

int meshover_first_use(const MeshOverride *o) {
    if (!o || o->used) return 0;
    mo.o[o - mo.o].used = 1;
    return 1;
}

int meshover_index(const MeshOverride *o) { return (int)(o - mo.o); }

const MeshOverride *meshover_get(int i) { return i >= 0 && i < mo.n ? &mo.o[i] : NULL; }

int meshover_memo_get(uint64_t key, uint64_t *hash) {
    uint32_t s = (uint32_t)(key ^ key >> 29) & (MEMO_SIZE - 1);
    for (int i = 0; i < 8; i++, s = (s + 1) & (MEMO_SIZE - 1))
        if (mo.memo[s].key == key) {
            *hash = mo.memo[s].hash;
            return 1;
        }
    return 0;
}

void meshover_memo_set(uint64_t key, uint64_t hash) {
    uint32_t first = (uint32_t)(key ^ key >> 29) & (MEMO_SIZE - 1), s = first;
    int i;
    for (i = 0; i < 8; i++, s = (s + 1) & (MEMO_SIZE - 1))
        if (!mo.memo[s].key || mo.memo[s].key == key) break;
    if (i == 8) s = first;
    mo.memo[s].key = key;
    mo.memo[s].hash = hash;
}

void meshover_close(void) {
    for (int i = 0; i < mo.n; i++) {
        meshpack_free(&mo.o[i].mesh);
        free(mo.o[i].png);
    }
    free(mo.o);
    memset(&mo, 0, sizeof mo);
}
