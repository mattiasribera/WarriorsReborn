/* A mod's own assets (src/modassets.h "own assets", docs/MODDING.md "Your own assets"): .glb meshes
 * through cgltf, PNG / DDS textures, WAV / OGG sounds through stb_vorbis, the own/ folder's part of
 * the cache key, and the game rig's joint names. Plain C, no runtime state, no game data: the
 * launcher builds it too. */
#include "modassets.h"
#include "meshpack.h"
#include "texpack.h"
#include "xtex.h"
#include "../third_party/cgltf/cgltf.h"
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* stb_image (src/texpack_stb.c builds it, PNG only) and stb_vorbis (src/modassets_libs.c). */
unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp, int req_comp);
void stbi_image_free(void *retval_from_stbi_load);
const char *stbi_failure_reason(void);
int stb_vorbis_decode_memory(const unsigned char *mem, int len, int *channels, int *sample_rate, short **output);

const char *const ma_joints[MA_JOINTS] = {
    "bip_sw_ground", "bip_sw", "bip_sw_pelvis", "bip_sw_spine", "bip_sw_spine1", "bip_sw_neck",
    "bip_sw_head", "bip_brow_left", "bip_jaw", "bip_lip_lower", "bip_mouth_lft", "bip_mouth_lft1",
    "bip_mouth_rt", "bip_mouth_rgt1", "bip_eye_lid", "bip_lip_upper",
    "bip_sw_l_clavicle", "bip_sw_l_upperarm", "bip_sw_l_forearm", "bip_sw_l_hand",
    "bip_sw_l_finger1", "bip_sw_l_finger2",
    "bip_sw_r_clavicle", "bip_sw_r_upperarm", "bip_sw_r_forearm", "bip_sw_r_hand",
    "bip_sw_r_finger1", "bip_sw_r_finger2",
    "bip_sw_l_thigh", "bip_sw_l_calf", "bip_sw_l_foot", "bip_sw_r_thigh", "bip_sw_r_calf", "bip_sw_r_foot",
};

int ma_joint_index(const char *name) {
    for (int j = 0; name && j < MA_JOINTS; j++)
        if (!strcmp(ma_joints[j], name)) return j;
    return -1;
}

static int edit_distance(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 63) la = 63;
    if (lb > 63) lb = 63;
    int d[64], prev;
    for (int j = 0; j <= lb; j++) d[j] = j;
    for (int i = 1; i <= la; i++) {
        prev = d[0];
        d[0] = i;
        for (int j = 1; j <= lb; j++) {
            int t = d[j];
            int sub = prev + (tolower((unsigned char)a[i - 1]) != tolower((unsigned char)b[j - 1]));
            int del = d[j] + 1, ins = d[j - 1] + 1;
            d[j] = sub < del ? (sub < ins ? sub : ins) : (del < ins ? del : ins);
            prev = t;
        }
    }
    return d[lb];
}

const char *ma_joint_closest(const char *name) {
    int best = 2, bd = 1 << 30;
    for (int j = 2; j < MA_JOINTS; j++) {
        int d = edit_distance(name ? name : "", ma_joints[j]);
        if (d < bd) bd = d, best = j;
    }
    return ma_joints[best];
}

static int oerr(char *err, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int oerr(char *err, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (err && cap) vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    return -1;
}

int ma_own_path_check(const char *rel, char *err, size_t cap) {
    if (!rel || strncmp(rel, "own/", 4) || !rel[4]) return oerr(err, cap, "%s: a path outside the mod's own folder (own/NAME)", rel ? rel : "?");
    int slashes = 0;
    for (const char *p = rel + 4; *p; p++) {
        if (*p == '/') {
            if (p[1] == '/' || !p[1] || p == rel + 4) return oerr(err, cap, "%s: a path outside the mod's own folder (own/NAME)", rel);
            slashes++;
        } else if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') {
            return oerr(err, cap, "%s: a path outside the mod's own folder (names are a-z A-Z 0-9 - _ .)", rel);
        }
    }
    if (strstr(rel, "..") || slashes > 1 || strlen(rel) > 120) return oerr(err, cap, "%s: a path outside the mod's own folder (own/NAME)", rel);
    return 0;
}

/* ---- the cache key's own/ part ---- */

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

static int cmp_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* The regular files of dir (and of its subfolders when depth is 0) as "prefix/name" strings. */
static int list_own(const char *dir, const char *prefix, int depth, char **names, int *n, char *err, size_t cap) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int rc = 0;
    while (!rc && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[1400], rel[300];
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= sizeof path ||
            (size_t)snprintf(rel, sizeof rel, "%s%s", prefix, e->d_name) >= sizeof rel)
            continue;
        struct stat st;
        if (stat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth == 0) {
                char sub[300];
                snprintf(sub, sizeof sub, "%s/", rel);
                rc = list_own(path, sub, 1, names, n, err, cap);
            }
            continue;
        }
        if (*n == MA_OWN_FILES_MAX) rc = oerr(err, cap, "own/: more than %d files", MA_OWN_FILES_MAX);
        else if (!(names[(*n)++] = strdup(rel))) rc = oerr(err, cap, "out of memory");
    }
    closedir(d);
    return rc;
}

int ma_own_key(const char *mod_dir, uint64_t *key, char *err, size_t cap) {
    char own[1200];
    snprintf(own, sizeof own, "%s/own", mod_dir ? mod_dir : "");
    struct stat st;
    if (!mod_dir || stat(own, &st) || !S_ISDIR(st.st_mode)) return 0;
    char *names[MA_OWN_FILES_MAX];
    int n = 0, rc = list_own(own, "", 0, names, &n, err, cap);
    if (!rc) qsort(names, (size_t)n, sizeof *names, cmp_name);
    uint64_t h = fnv(*key, "own", 4), total = 0;
    for (int i = 0; i < n && !rc; i++) {
        char path[1600];
        snprintf(path, sizeof path, "%s/%s", own, names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) {
            rc = oerr(err, cap, "own/%s: cannot read it", names[i]);
            break;
        }
        h = fnv(h, names[i], strlen(names[i]) + 1);
        uint8_t buf[65536];
        size_t r;
        while ((r = fread(buf, 1, sizeof buf, f)) > 0) {
            h = fnv(h, buf, r);
            total += r;
        }
        fclose(f);
        if (total > MA_OWN_BYTES_MAX) rc = oerr(err, cap, "own/: more than %u MiB", MA_OWN_BYTES_MAX >> 20);
    }
    for (int i = 0; i < n; i++) free(names[i]);
    if (!rc) *key = h;
    return rc;
}

/* ---- .glb meshes ---- */

void ma_own_mesh_free(MaOwnMesh *m) {
    free(m->pos), free(m->nrm), free(m->uv), free(m->slot), free(m->w), free(m->idx), free(m->col);
    memset(m, 0, sizeof *m);
    m->image = -1;
}

static void to_game(const float *in, float *out) {   /* glTF Y-up -> game Z-up */
    const float x = in[0], y = in[1], z = in[2];
    out[0] = x, out[1] = -z, out[2] = y;
}

static void mat_point(const float *m, const float *p, float w, float *out) {   /* column-major glTF matrix */
    for (int k = 0; k < 3; k++) out[k] = m[k] * p[0] + m[4 + k] * p[1] + m[8 + k] * p[2] + m[12 + k] * w;
}

static int starts_collision(const char *s) {
    if (!s) return 0;
    const char *c = "collision";
    for (; *c; c++, s++)
        if (tolower((unsigned char)*s) != *c) return 0;
    return 1;
}

static int is_identity(const float *m) {
    for (int i = 0; i < 16; i++)
        if (fabsf(m[i] - (i % 5 == 0 ? 1.0f : 0.0f)) > 1e-5f) return 0;
    return 1;
}

static const char *result_name(cgltf_result r) {
    switch (r) {
    case cgltf_result_data_too_short: return "the file is truncated";
    case cgltf_result_unknown_format: return "not a glTF binary (.glb) file";
    case cgltf_result_invalid_json: return "its JSON is invalid";
    case cgltf_result_invalid_gltf: return "not valid glTF 2.0";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "glTF 1.0 (export glTF 2.0)";
    default: return "it cannot be read";
    }
}

/* Grows the visual arrays to hold `need` vertices. */
static int grow_v(MaOwnMesh *m, int need, int *cap) {
    if (need <= *cap) return 0;
    int c = *cap ? *cap : 256;
    while (c < need) c *= 2;
    void *a = realloc(m->pos, sizeof *m->pos * (size_t)c);
    if (!a) return -1;
    m->pos = a;
    if (!(a = realloc(m->nrm, sizeof *m->nrm * (size_t)c))) return -1;
    m->nrm = a;
    if (!(a = realloc(m->uv, sizeof *m->uv * (size_t)c))) return -1;
    m->uv = a;
    if (!(a = realloc(m->slot, sizeof *m->slot * (size_t)c))) return -1;
    m->slot = a;
    if (!(a = realloc(m->w, sizeof *m->w * (size_t)c))) return -1;
    m->w = a;
    *cap = c;
    return 0;
}

/* One primitive of a node: visual (appended to m) or collision. */
static int add_primitive(MaOwnMesh *m, const cgltf_data *d, const cgltf_node *node, const cgltf_primitive *pr, int collision, const float *world,
                         int *capv, int *capi, int *capc, char *err, size_t cap) {
    if (pr->type != cgltf_primitive_type_triangles)
        return oerr(err, cap, "mesh %s: only triangles (not points, lines or strips; Blender: Triangulate)", node->mesh->name ? node->mesh->name : "?");
    const cgltf_accessor *pos = NULL, *nrm = NULL, *uv = NULL, *jo[2] = {NULL, NULL}, *we[2] = {NULL, NULL};
    for (cgltf_size i = 0; i < pr->attributes_count; i++) {
        const cgltf_attribute *a = &pr->attributes[i];
        if (a->type == cgltf_attribute_type_position) pos = a->data;
        else if (a->type == cgltf_attribute_type_normal) nrm = a->data;
        else if (a->type == cgltf_attribute_type_texcoord && a->index == 0) uv = a->data;
        else if (a->type == cgltf_attribute_type_joints && a->index < 2) jo[a->index] = a->data;
        else if (a->type == cgltf_attribute_type_weights && a->index < 2) we[a->index] = a->data;
    }
    const char *mname = node->mesh->name ? node->mesh->name : node->name ? node->name : "?";
    if (!pos || pos->count == 0) return oerr(err, cap, "mesh %s: no vertex positions", mname);
    const cgltf_size nv = pos->count, ni = pr->indices ? pr->indices->count : nv;
    if (ni % 3) return oerr(err, cap, "mesh %s: %u indices (not whole triangles)", mname, (unsigned)ni);
    if (collision) {
        const int nt = (int)(ni / 3);
        if (m->ncol + nt > MA_COLLISION_TRIS_MAX)
            return oerr(err, cap, "collision: more than %d triangles (simplify the collision mesh)", MA_COLLISION_TRIS_MAX);
        if (m->ncol + nt > *capc) {
            int c = *capc ? *capc : 64;
            while (c < m->ncol + nt) c *= 2;
            void *a = realloc(m->col, sizeof *m->col * (size_t)c);
            if (!a) return oerr(err, cap, "out of memory");
            m->col = a, *capc = c;
        }
        for (int t = 0; t < nt; t++)
            for (int k = 0; k < 3; k++) {
                cgltf_size v = pr->indices ? cgltf_accessor_read_index(pr->indices, (cgltf_size)t * 3 + k) : (cgltf_size)t * 3 + k;
                float p[3], q[3];
                if (v >= nv || !cgltf_accessor_read_float(pos, v, p, 3)) return oerr(err, cap, "mesh %s: an index past its vertices", mname);
                mat_point(world, p, 1.0f, q);
                to_game(q, m->col[m->ncol + t][k]);
            }
        m->ncol += nt;
        return 0;
    }
    if (!nrm) return oerr(err, cap, "mesh %s: no normals (export with Normals on)", mname);
    if (!uv) return oerr(err, cap, "mesh %s: no UVs (unwrap it: the game draws every mesh with a texture)", mname);
    const int skinned = node->skin != NULL;
    if (skinned && !jo[0]) return oerr(err, cap, "mesh %s: skinned but without joint weights", mname);
    if (m->nv && skinned != m->skinned) return oerr(err, cap, "mesh %s: skinned and static meshes mixed in one .glb", mname);
    m->skinned = skinned;
    if ((cgltf_size)m->nv + nv > MESHPACK_MAX_VERTS)
        return oerr(err, cap, "%u vertices: too many (at most %d; every UV seam and hard edge doubles vertices)", (unsigned)((cgltf_size)m->nv + nv), MESHPACK_MAX_VERTS);
    if (grow_v(m, m->nv + (int)nv, capv)) return oerr(err, cap, "out of memory");
    float nmat[16];
    memcpy(nmat, world, sizeof nmat);
    nmat[12] = nmat[13] = nmat[14] = 0;
    const cgltf_skin *sk = node->skin;
    for (cgltf_size v = 0; v < nv; v++) {
        float p[3], n[3] = {0, 0, 1}, t[4] = {0, 0}, q[3];
        cgltf_accessor_read_float(pos, v, p, 3);
        cgltf_accessor_read_float(nrm, v, n, 3);
        cgltf_accessor_read_float(uv, v, t, 2);
        const int o = m->nv + (int)v;
        if (!skinned) {
            mat_point(world, p, 1.0f, q);
            to_game(q, m->pos[o]);
            mat_point(nmat, n, 0.0f, q);
        } else {
            to_game(p, m->pos[o]);
            memcpy(q, n, sizeof q);
        }
        float g[3];
        to_game(q, g);
        float len = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        if (len <= 0) len = 1;
        for (int k = 0; k < 3; k++) m->nrm[o][k] = g[k] / len;
        m->uv[o][0] = t[0], m->uv[o][1] = t[1];
        m->slot[o][0] = m->slot[o][1] = m->slot[o][2] = -1;
        m->w[o][0] = m->w[o][1] = m->w[o][2] = 0;
        if (!skinned) continue;
        float acc[MA_JOINTS] = {0};
        for (int set = 0; set < 2 && jo[set]; set++) {
            cgltf_uint js[4] = {0};
            float ws[4] = {0};
            if (!we[set] || !cgltf_accessor_read_uint(jo[set], v, js, 4) || !cgltf_accessor_read_float(we[set], v, ws, 4))
                return oerr(err, cap, "mesh %s: unreadable joint weights", mname);
            for (int k = 0; k < 4; k++) {
                if (ws[k] <= 0) continue;
                if (js[k] >= sk->joints_count) return oerr(err, cap, "mesh %s: a weight on joint %u of a %u-joint skin", mname, js[k], (unsigned)sk->joints_count);
                const char *jn = sk->joints[js[k]]->name ? sk->joints[js[k]]->name : "";
                int j = ma_joint_index(jn);
                if (j < 0) return oerr(err, cap, "unknown joint %s (closest: %s); the game's joints are named bip_*", jn, ma_joint_closest(jn));
                if (j < 2) return oerr(err, cap, "joint %s carries no vertices in the game (weight them to bip_sw_pelvis or below)", jn);
                acc[j] += ws[k];
            }
        }
        int top[3] = {-1, -1, -1}, have = 0;
        for (int k = 0; k < 3; k++) {
            int b = -1;
            for (int j = 2; j < MA_JOINTS; j++)
                if (acc[j] > 0 && j != top[0] && j != top[1] && (b < 0 || acc[j] > acc[b])) b = j;
            top[k] = b;
            if (b >= 0) have++;
        }
        int more = 0;
        for (int j = 2; j < MA_JOINTS; j++) more += acc[j] > 0;
        if (more > 3) m->trimmed++;
        if (!have) return oerr(err, cap, "mesh %s: vertex %u has no weight on any joint (Weight Paint it)", mname, (unsigned)v);
        float sum = 0;
        for (int k = 0; k < have; k++) sum += acc[top[k]];
        for (int k = 0; k < have; k++) m->slot[o][k] = (int8_t)(top[k] - 2), m->w[o][k] = acc[top[k]] / sum;
    }
    if (m->ni + (int)ni > *capi) {
        int c = *capi ? *capi : 768;
        while (c < m->ni + (int)ni) c *= 2;
        uint16_t *a = realloc(m->idx, sizeof *a * (size_t)c);
        if (!a) return oerr(err, cap, "out of memory");
        m->idx = a, *capi = c;
    }
    for (cgltf_size i = 0; i < ni; i++) {
        cgltf_size v = pr->indices ? cgltf_accessor_read_index(pr->indices, i) : i;
        if (v >= nv) return oerr(err, cap, "mesh %s: an index past its vertices", mname);
        m->idx[m->ni++] = (uint16_t)(m->nv + (int)v);
    }
    if (m->image < 0 && pr->material && pr->material->has_pbr_metallic_roughness) {
        const cgltf_texture *tx = pr->material->pbr_metallic_roughness.base_color_texture.texture;
        if (tx && tx->image) m->image = (int)(tx->image - d->images);
    }
    m->nv += (int)nv;
    return 0;
}

static int open_glb(const uint8_t *data, size_t n, cgltf_data **out, char *err, size_t cap) {
    if (n < 12 || memcmp(data, "glTF", 4)) return oerr(err, cap, "not a glTF binary (.glb) file (Blender: File > Export > glTF 2.0, Format glTF Binary)");
    cgltf_options opt;
    memset(&opt, 0, sizeof opt);
    opt.type = cgltf_file_type_glb;
    cgltf_data *d = NULL;
    cgltf_result r = cgltf_parse(&opt, data, n, &d);
    if (r != cgltf_result_success) return oerr(err, cap, "%s", result_name(r));
    for (cgltf_size i = 0; i < d->buffers_count; i++)
        if (d->buffers[i].uri && strncmp(d->buffers[i].uri, "data:", 5)) {
            cgltf_free(d);
            return oerr(err, cap, "its data is in another file (%.60s): export as glTF Binary (.glb)", d->buffers[i].uri);
        }
    r = cgltf_load_buffers(&opt, d, NULL);
    if (r == cgltf_result_success) r = cgltf_validate(d);
    if (r != cgltf_result_success) {
        cgltf_free(d);
        return oerr(err, cap, "%s", result_name(r));
    }
    *out = d;
    return 0;
}

int ma_glb_load(const uint8_t *data, size_t n, MaOwnMesh *m, char *err, size_t cap) {
    memset(m, 0, sizeof *m);
    m->image = -1;
    cgltf_data *d;
    if (open_glb(data, n, &d, err, cap)) return -1;
    const cgltf_node *const *nodes = d->scene ? (const cgltf_node *const *)d->scene->nodes : NULL;
    /* every node of the scene (its roots and their children), else every node */
    const cgltf_node **list = malloc(sizeof *list * (d->nodes_count + 1));
    if (!list) {
        cgltf_free(d);
        return oerr(err, cap, "out of memory");
    }
    size_t nl = 0;
    for (cgltf_size i = 0; i < d->nodes_count; i++) {
        const cgltf_node *k = &d->nodes[i];
        if (!k->mesh) continue;
        int in = !nodes;
        for (const cgltf_node *p = k; p && !in; p = p->parent)
            for (cgltf_size r = 0; r < d->scene->nodes_count && !in; r++) in = nodes[r] == p;
        if (in) list[nl++] = k;
    }
    int rc = 0, capv = 0, capi = 0, capc = 0;
    for (size_t i = 0; i < nl && !rc; i++) {
        const cgltf_node *k = list[i];
        float world[16];
        cgltf_node_transform_world(k, world);
        const int coll = starts_collision(k->name) || starts_collision(k->mesh->name);
        if (k->skin && !coll && !is_identity(world)) m->moved = 1;
        if (k->skin && k->skin->joints_count > MA_OWN_SKIN_JOINTS_MAX)
            rc = oerr(err, cap, "too many bones: the skin has %u joints (at most %d; the game's rig has %d, named bip_*)", (unsigned)k->skin->joints_count,
                      MA_OWN_SKIN_JOINTS_MAX, MA_JOINTS);
        for (cgltf_size p = 0; p < k->mesh->primitives_count && !rc; p++)
            rc = add_primitive(m, d, k, &k->mesh->primitives[p], coll, world, &capv, &capi, &capc, err, cap);
    }
    free(list);
    cgltf_free(d);
    if (!rc && !m->ni) rc = oerr(err, cap, "no mesh with triangles (only collision meshes or empty nodes)");
    if (!rc) {
        for (int k = 0; k < 3; k++) m->lo[k] = m->hi[k] = m->pos[0][k];
        for (int v = 1; v < m->nv; v++)
            for (int k = 0; k < 3; k++) {
                if (m->pos[v][k] < m->lo[k]) m->lo[k] = m->pos[v][k];
                if (m->pos[v][k] > m->hi[k]) m->hi[k] = m->pos[v][k];
            }
        double ext = 0;
        for (int k = 0; k < 3; k++) ext = fmax(ext, (double)m->hi[k] - m->lo[k]);
        if (ext > MA_OWN_EXTENT_MAX)
            rc = oerr(err, cap, "the mesh is %.1f m across: it looks exported in centimetres (Blender: Unit Scale 1.0, Apply > Scale; at most %.0f m)", ext,
                      MA_OWN_EXTENT_MAX);
    }
    if (rc) ma_own_mesh_free(m);
    return rc;
}

uint8_t *ma_glb_image(const uint8_t *data, size_t n, size_t *len, char *err, size_t cap) {
    MaOwnMesh m;
    if (ma_glb_load(data, n, &m, err, cap)) return NULL;
    const int im = m.image;
    ma_own_mesh_free(&m);
    cgltf_data *d;
    if (im < 0) return oerr(err, cap, "no base colour texture in its material (or give the texture as its own own/NAME.png)"), NULL;
    if (open_glb(data, n, &d, err, cap)) return NULL;
    const cgltf_image *img = &d->images[im];
    uint8_t *out = NULL;
    if (!img->buffer_view) {
        oerr(err, cap, "its texture %.60s is another file: give it as own/NAME.png, or pack it into the .glb", img->uri ? img->uri : "?");
    } else {
        const uint8_t *p = cgltf_buffer_view_data(img->buffer_view);
        *len = img->buffer_view->size;
        if (!p || !(out = malloc(*len ? *len : 1))) oerr(err, cap, "its texture cannot be read");
        else memcpy(out, p, *len);
    }
    cgltf_free(d);
    return out;
}

/* ---- textures ---- */

int ma_image_load(const uint8_t *data, size_t n, int *w, int *h, uint8_t **rgba, char *err, size_t cap) {
    *rgba = NULL;
    if (n >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) {
        if (n >= 24) {
            const uint32_t pw = (uint32_t)data[16] << 24 | (uint32_t)data[17] << 16 | (uint32_t)data[18] << 8 | data[19];
            const uint32_t ph = (uint32_t)data[20] << 24 | (uint32_t)data[21] << 16 | (uint32_t)data[22] << 8 | data[23];
            if (pw > MODASSETS_TEX_MAX || ph > MODASSETS_TEX_MAX || !pw || !ph)
                return oerr(err, cap, "bad texture size %ux%u (1 to %d texels a side)", pw, ph, MODASSETS_TEX_MAX);
        }
        int comp = 0;
        uint8_t *p = stbi_load_from_memory(data, (int)n, w, h, &comp, 4);
        if (!p) return oerr(err, cap, "the PNG cannot be decoded (%s)", stbi_failure_reason() ? stbi_failure_reason() : "?");
        *rgba = malloc((size_t)*w * *h * 4);
        if (*rgba) memcpy(*rgba, p, (size_t)*w * *h * 4);
        stbi_image_free(p);
        return *rgba ? 0 : oerr(err, cap, "out of memory");
    }
    if (n >= 4 && !memcmp(data, "DDS ", 4)) {
        TexpackDds dd;
        const char *why = texpack_dds_parse(data, n, &dd);
        if (why) return oerr(err, cap, "the DDS cannot be used: %s", why);
        if (dd.w > MODASSETS_TEX_MAX || dd.h > MODASSETS_TEX_MAX) return oerr(err, cap, "bad texture size %ux%u (1 to %d texels a side)", dd.w, dd.h, MODASSETS_TEX_MAX);
        if (dd.format != TEXPACK_BC1 && dd.format != TEXPACK_BC3) return oerr(err, cap, "a BC7 DDS: use BC1 (DXT1), BC3 (DXT5) or a PNG");
        const uint32_t code = dd.format == TEXPACK_BC1 ? 0x0C : 0x0F, bs = dd.format == TEXPACK_BC1 ? 8 : 16;
        *w = (int)dd.w, *h = (int)dd.h;
        uint8_t *o = *rgba = malloc((size_t)dd.w * dd.h * 4);
        if (!o) return oerr(err, cap, "out of memory");
        const uint32_t bw = (dd.w + 3) / 4, bh = (dd.h + 3) / 4;
        const uint8_t *b = data + dd.offset;
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++, b += bs) {
                uint32_t t[16];
                xtex_dxt_block(code, b, t);
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        const uint32_t X = bx * 4 + (uint32_t)x, Y = by * 4 + (uint32_t)y;
                        if (X >= dd.w || Y >= dd.h) continue;
                        uint8_t *q = o + ((size_t)Y * dd.w + X) * 4;
                        const uint32_t c = t[y * 4 + x];
                        q[0] = (uint8_t)(c >> 16), q[1] = (uint8_t)(c >> 8), q[2] = (uint8_t)c, q[3] = (uint8_t)(c >> 24);
                    }
            }
        return 0;
    }
    return oerr(err, cap, "not a PNG or DDS image");
}

/* ---- sounds ---- */

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int wav_decode(const uint8_t *d, size_t n, int16_t **pcm, int *ch, int *rate, int *frames, char *err, size_t cap) {
    if (n < 12 || memcmp(d + 8, "WAVE", 4)) return oerr(err, cap, "not a WAV file");
    const uint8_t *fmt = NULL, *data = NULL;
    uint32_t data_len = 0, fmt_len = 0;
    for (size_t o = 12; o + 8 <= n;) {
        const uint32_t len = le32(d + o + 4);
        if (len > n - o - 8) {
            if (!memcmp(d + o, "data", 4)) data = d + o + 8, data_len = (uint32_t)(n - o - 8);   /* a truncated last chunk */
            break;
        }
        if (!memcmp(d + o, "fmt ", 4)) fmt = d + o + 8, fmt_len = len;
        else if (!memcmp(d + o, "data", 4) && !data) data = d + o + 8, data_len = len;
        o += 8 + (size_t)len + (len & 1);
    }
    if (!fmt || fmt_len < 16 || !data) return oerr(err, cap, "a WAV without its fmt or data chunk");
    uint16_t tag = le16(fmt);
    const int c = le16(fmt + 2), bits = le16(fmt + 14);
    if (tag == 0xFFFE && fmt_len >= 26) tag = le16(fmt + 24);
    *rate = (int)le32(fmt + 4);
    const int is_float = tag == 3;
    if (!(tag == 1 || (is_float && bits == 32)) || !(bits == 8 || bits == 16 || bits == 24 || bits == 32))
        return oerr(err, cap, "a WAV in another encoding (tag %u, %d bits): use PCM or 32-bit float", tag, bits);
    if (c < 1 || c > 2) return oerr(err, cap, "a WAV with %d channels (mono or stereo)", c);
    const int bpf = c * bits / 8;
    *frames = (int)(data_len / (uint32_t)bpf);
    *ch = c;
    if (*rate < 8000 || *rate > 96000) return oerr(err, cap, "a sample rate of %d Hz (8000 to 96000)", *rate);
    if (*frames <= 0) return oerr(err, cap, "an empty sound");
    if ((double)*frames / *rate > MA_SOUND_SECONDS_MAX) return oerr(err, cap, "%.0f s long (at most %d s)", (double)*frames / *rate, MA_SOUND_SECONDS_MAX);
    int16_t *o = *pcm = malloc(sizeof *o * (size_t)*frames * c);
    if (!o) return oerr(err, cap, "out of memory");
    for (size_t i = 0; i < (size_t)*frames * c; i++) {
        const uint8_t *s = data + i * (size_t)(bits / 8);
        int32_t v;
        if (bits == 8) v = ((int32_t)s[0] - 128) << 8;
        else if (bits == 16) v = (int16_t)le16(s);
        else if (bits == 24) v = (int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24) >> 16;
        else if (!is_float) v = (int32_t)le32(s) >> 16;
        else {
            float f;
            uint32_t u = le32(s);
            memcpy(&f, &u, 4);
            f = f > 1 ? 1 : f < -1 ? -1 : f;
            v = (int32_t)lrintf(f * 32767.0f);
        }
        o[i] = (int16_t)v;
    }
    return 0;
}

int ma_sound_decode(const uint8_t *data, size_t n, int16_t **pcm, int *channels, int *rate, int *frames, char *err, size_t cap) {
    *pcm = NULL;
    if (n >= 12 && !memcmp(data, "RIFF", 4)) return wav_decode(data, n, pcm, channels, rate, frames, err, cap);
    if (n >= 4 && !memcmp(data, "OggS", 4)) {
        if (n > (64u << 20)) return oerr(err, cap, "the OGG is too large");
        short *out = NULL;
        int c = 0, r = 0, f = stb_vorbis_decode_memory(data, (int)n, &c, &r, &out);
        if (f <= 0 || !out) {
            free(out);
            return oerr(err, cap, "the OGG cannot be decoded (Ogg Vorbis only, not Opus or FLAC)");
        }
        if (c < 1 || c > 2 || r < 8000 || r > 96000 || (double)f / r > MA_SOUND_SECONDS_MAX) {
            free(out);
            if (c < 1 || c > 2) return oerr(err, cap, "an OGG with %d channels (mono or stereo)", c);
            if (r < 8000 || r > 96000) return oerr(err, cap, "a sample rate of %d Hz (8000 to 96000)", r);
            return oerr(err, cap, "%.0f s long (at most %d s)", (double)f / r, MA_SOUND_SECONDS_MAX);
        }
        *pcm = out, *channels = c, *rate = r, *frames = f;
        return 0;
    }
    return oerr(err, cap, "not a WAV or OGG sound");
}

int ma_wav_write(const char *path, const int16_t *pcm, int channels, int rate, int frames) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const uint32_t bytes = (uint32_t)frames * (uint32_t)channels * 2;
    uint8_t h[44];
    const uint32_t v[] = {36 + bytes, 16, (uint32_t)rate, (uint32_t)rate * (uint32_t)channels * 2, bytes};
    memcpy(h, "RIFF", 4), memcpy(h + 8, "WAVEfmt ", 8), memcpy(h + 36, "data", 4);
    for (int k = 0; k < 4; k++) h[4 + k] = (uint8_t)(v[0] >> (8 * k)), h[16 + k] = (uint8_t)(v[1] >> (8 * k)), h[24 + k] = (uint8_t)(v[2] >> (8 * k)),
                                h[28 + k] = (uint8_t)(v[3] >> (8 * k)), h[40 + k] = (uint8_t)(v[4] >> (8 * k));
    h[20] = 1, h[21] = 0, h[22] = (uint8_t)channels, h[23] = 0, h[32] = (uint8_t)(channels * 2), h[33] = 0, h[34] = 16, h[35] = 0;
    int ok = fwrite(h, 1, 44, f) == 44;
    uint8_t buf[8192];
    size_t nb = 0;
    for (size_t i = 0; ok && i < (size_t)frames * channels; i++) {
        buf[nb++] = (uint8_t)pcm[i], buf[nb++] = (uint8_t)((uint16_t)pcm[i] >> 8);
        if (nb == sizeof buf) ok = fwrite(buf, 1, nb, f) == nb, nb = 0;
    }
    if (ok && nb) ok = fwrite(buf, 1, nb, f) == nb;
    if (fclose(f)) ok = 0;
    return ok ? 0 : -1;
}
