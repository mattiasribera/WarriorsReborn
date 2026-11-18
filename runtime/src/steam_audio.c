/* Steam Audio, loaded at run time (steam_audio.h). The library's public header (Apache 2.0, from
 * Steam Audio v4.8.1, third_party/steam-audio/include) supplies the types; every function is
 * looked up with plat_dlsym, so nothing links against the library. */
#include "steam_audio.h"
#include "platform.h"
#include "phonon.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The functions used, with their argument bytes: the decorated stdcall export names of a MinGW
 * build ("iplContextCreate@8"); the official Windows DLL exports undecorated names, tried first. */
#define SA_FUNCS(X) \
    X(iplContextCreate, 8) X(iplContextRelease, 4) X(iplHRTFCreate, 16) X(iplHRTFRelease, 4) \
    X(iplBinauralEffectCreate, 16) X(iplBinauralEffectRelease, 4) X(iplBinauralEffectReset, 4) X(iplBinauralEffectApply, 16) \
    X(iplSceneCreate, 12) X(iplSceneRelease, 4) X(iplSceneCommit, 4) X(iplStaticMeshCreate, 12) X(iplStaticMeshRelease, 4) \
    X(iplStaticMeshAdd, 8) X(iplSimulatorCreate, 12) X(iplSimulatorRelease, 4) X(iplSimulatorSetScene, 8) \
    X(iplSimulatorSetSharedInputs, 12) X(iplSimulatorCommit, 4) X(iplSimulatorRunDirect, 4) X(iplSimulatorRunReflections, 4) \
    X(iplSourceCreate, 12) X(iplSourceRelease, 4) X(iplSourceAdd, 8) X(iplSourceRemove, 8) X(iplSourceSetInputs, 12) X(iplSourceGetOutputs, 12) \
    X(iplProbeArrayCreate, 8) X(iplProbeArrayRelease, 4) X(iplProbeArrayGenerateProbes, 12) X(iplProbeBatchCreate, 8) X(iplProbeBatchRelease, 4) \
    X(iplProbeBatchAddProbeArray, 8) X(iplProbeBatchCommit, 4) X(iplProbeBatchGetNumProbes, 4) X(iplProbeBatchGetDataSize, 8) X(iplProbeBatchSave, 8) \
    X(iplProbeBatchLoad, 12) X(iplPathBakerBake, 16) X(iplSerializedObjectCreate, 12) X(iplSerializedObjectRelease, 4) \
    X(iplSerializedObjectGetSize, 4) X(iplSerializedObjectGetData, 4) X(iplSimulatorAddProbeBatch, 8) X(iplSimulatorRemoveProbeBatch, 8) X(iplSimulatorRunPathing, 4)

static struct {
#define X(f, n) __typeof__(&f) f;
    SA_FUNCS(X)
#undef X
} sa;
static void *lib;
static int opened, open_result = -1;
static IPLContext ctx;
static IPLHRTF hrtf;
static IPLAudioSettings audio_settings = {SA_RATE, SA_FRAME};
static char version[16];

static void *sym(const char *name, int bytes) {
    void *p = plat_dlsym(lib, name);
#ifdef _WIN32
    if (!p) {
        char dec[96];
        snprintf(dec, sizeof dec, "%s@%d", name, bytes);
        p = plat_dlsym(lib, dec);
    }
#else
    (void)bytes;
#endif
    return p;
}

static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

#ifdef _WIN32
#define SA_DEV_DIR "steam-audio\\windows-x86\\"
#else
#define SA_DEV_DIR "steam-audio/linux-x86/"
#endif

static void IPLCALL sa_log(IPLLogLevel level, const char *msg) {
    printf("[steam-audio] library %s: %s\n", level == IPL_LOGLEVEL_ERROR ? "error" : level == IPL_LOGLEVEL_WARNING ? "warning" : "info", msg);
}

static void unload(void) {
    if (hrtf && sa.iplHRTFRelease) sa.iplHRTFRelease(&hrtf);
    if (ctx && sa.iplContextRelease) sa.iplContextRelease(&ctx);
    hrtf = NULL;
    ctx = NULL;
    /* the library stays mapped: the CRT of a statically linked libc++ may still hold atexit hooks */
    lib = NULL;
    memset(&sa, 0, sizeof sa);
    version[0] = 0;
}

int sa_open(const char *path) {
    if (opened) return open_result;
    opened = 1;
    char buf[1024];
    const char *where = path;
    if (!where) {
        snprintf(buf, sizeof buf, "%s%s", plat_program_dir(), SA_LIB_NAME);
        if (!file_exists(buf)) snprintf(buf, sizeof buf, "%s%s%s", plat_program_dir(), SA_DEV_DIR, SA_LIB_NAME);
        where = buf;
    }
    /* checked first: a missing library is the normal case, not a load failure to report */
    if (!file_exists(where)) {
        printf("[steam-audio] %s not found: using the built-in 3D sound models\n", path ? path : SA_LIB_NAME);
        return -1;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    lib = plat_dlopen(where);
    if (!lib) {
        printf("[steam-audio] cannot load %s: %s: using the built-in 3D sound models\n", where, plat_dlerror());
        return -1;
    }
#define X(f, n) if (!(*(void **)&sa.f = sym(#f, n))) { printf("[steam-audio] %s: %s missing: using the built-in 3D sound models\n", where, #f); unload(); return -1; }
    SA_FUNCS(X)
#undef X
    IPLContextSettings cs = {0};
    cs.version = STEAMAUDIO_VERSION;
    cs.logCallback = sa_log;
    cs.simdLevel = IPL_SIMDLEVEL_SSE2;
    IPLerror e = sa.iplContextCreate(&cs, &ctx);
    if (e != IPL_STATUS_SUCCESS) {
        printf("[steam-audio] %s: context creation failed (error %d; library older than %d.%d.%d?): using the built-in 3D sound models\n",
               where, (int)e, STEAMAUDIO_VERSION_MAJOR, STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH);
        ctx = NULL;
        unload();
        return -1;
    }
    IPLHRTFSettings hs = {0};
    hs.type = IPL_HRTFTYPE_DEFAULT;
    hs.volume = 1.0f;
    hs.normType = IPL_HRTFNORMTYPE_NONE;
    e = sa.iplHRTFCreate(ctx, &audio_settings, &hs, &hrtf);
    if (e != IPL_STATUS_SUCCESS) {
        printf("[steam-audio] %s: HRTF creation failed (error %d): using the built-in 3D sound models\n", where, (int)e);
        hrtf = NULL;
        unload();
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    snprintf(version, sizeof version, "%d.%d.%d", STEAMAUDIO_VERSION_MAJOR, STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH);
    printf("[steam-audio] %s loaded (API %s), default HRTF ready in %.1f ms\n", where, version,
           (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    open_result = 0;
    return 0;
}

int sa_ok(void) { return open_result == 0; }
const char *sa_version(void) { return version; }

void sa_close(void) {
    unload();
    opened = 0;
    open_result = -1;
}

void sa_listener_dir(const float pos[3], const float front[3], const float top[3], const float src[3], float dir[3]) {
    const float d[3] = {src[0] - pos[0], src[1] - pos[1], src[2] - pos[2]};
    const float right[3] = {top[1] * front[2] - top[2] * front[1], top[2] * front[0] - top[0] * front[2], top[0] * front[1] - top[1] * front[0]};
    const float rl = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    const float ul = sqrtf(top[0] * top[0] + top[1] * top[1] + top[2] * top[2]);
    const float fl = sqrtf(front[0] * front[0] + front[1] * front[1] + front[2] * front[2]);
    dir[0] = 0, dir[1] = 0, dir[2] = -1;
    if (!(rl > 1e-6f) || !(ul > 1e-6f) || !(fl > 1e-6f)) return;
    const float x = (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / rl;
    const float y = (d[0] * top[0] + d[1] * top[1] + d[2] * top[2]) / ul;
    const float z = -(d[0] * front[0] + d[1] * front[1] + d[2] * front[2]) / fl;   /* ahead is -z */
    const float l = sqrtf(x * x + y * y + z * z);
    if (!(l > 1e-6f) || !isfinite(l)) return;
    dir[0] = x / l, dir[1] = y / l, dir[2] = z / l;
}

struct SaBinaural {
    IPLBinauralEffect fx;
};

SaBinaural *sa_binaural_new(void) {
    if (!sa_ok()) return NULL;
    SaBinaural *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    IPLBinauralEffectSettings s = {hrtf};
    if (sa.iplBinauralEffectCreate(ctx, &audio_settings, &s, &b->fx) != IPL_STATUS_SUCCESS) {
        free(b);
        return NULL;
    }
    return b;
}

void sa_binaural_free(SaBinaural *b) {
    if (!b) return;
    if (b->fx && sa.iplBinauralEffectRelease) sa.iplBinauralEffectRelease(&b->fx);
    free(b);
}

void sa_binaural_reset(SaBinaural *b) {
    if (b && b->fx) sa.iplBinauralEffectReset(b->fx);
}

void sa_binaural_apply(SaBinaural *b, const float dir[3], float blend, const float *in, float *out_l, float *out_r) {
    float *ip[1] = {(float *)in}, *op[2] = {out_l, out_r};
    IPLAudioBuffer ib = {1, SA_FRAME, ip}, ob = {2, SA_FRAME, op};
    IPLBinauralEffectParams p = {0};
    p.direction = (IPLVector3){dir[0], dir[1], dir[2]};
    p.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
    p.spatialBlend = blend < 0 ? 0 : blend > 1 ? 1 : blend;
    p.hrtf = hrtf;
    sa.iplBinauralEffectApply(b->fx, &p, &ib, &ob);
}

/* ---- geometry ---- */
_Static_assert(sizeof(SaMaterial) == sizeof(IPLMaterial), "SaMaterial mirrors IPLMaterial");

struct SaScene {
    IPLScene scene;
    IPLStaticMesh mesh;
};

SaScene *sa_scene_new(const float (*vert)[3], int nvert, const int (*tri)[3], const int *mat_index, int ntri, const SaMaterial *mats, int nmat) {
    if (!sa_ok() || nvert <= 0 || ntri <= 0 || nmat <= 0) return NULL;
    SaScene *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    IPLSceneSettings ss = {0};
    ss.type = IPL_SCENETYPE_DEFAULT;
    if (sa.iplSceneCreate(ctx, &ss, &s->scene) != IPL_STATUS_SUCCESS) { free(s); return NULL; }
    IPLStaticMeshSettings ms = {0};
    ms.numVertices = nvert;
    ms.numTriangles = ntri;
    ms.numMaterials = nmat;
    ms.vertices = (IPLVector3 *)vert;   /* 3 floats each, the same layout */
    ms.triangles = (IPLTriangle *)tri;
    ms.materialIndices = (IPLint32 *)mat_index;
    ms.materials = (IPLMaterial *)mats;
    if (sa.iplStaticMeshCreate(s->scene, &ms, &s->mesh) != IPL_STATUS_SUCCESS) {
        sa.iplSceneRelease(&s->scene);
        free(s);
        return NULL;
    }
    sa.iplStaticMeshAdd(s->mesh, s->scene);
    sa.iplSceneCommit(s->scene);
    return s;
}

void sa_scene_free(SaScene *s) {
    if (!s) return;
    if (sa.iplStaticMeshRelease && s->mesh) sa.iplStaticMeshRelease(&s->mesh);
    if (sa.iplSceneRelease && s->scene) sa.iplSceneRelease(&s->scene);
    free(s);
}

#define SA_OCCL_SAMPLES 16
#define SA_TRANS_RAYS 3
#define SA_REVERB_DURATION 2.0f
#define SA_VIS_SAMPLES 4   /* point samples per probe when a path is validated at run time */
struct SaSim {
    IPLSimulator sim;
    IPLSource *src;
    int nsrc, rays, pathing;
    IPLSource rev;
    SaScene *scene;
    SaProbes *probes;
};

SaSim *sa_sim_new(int max_sources, int reverb_rays, int pathing) {
    if (!sa_ok() || max_sources <= 0) return NULL;
    SaSim *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->src = calloc((size_t)max_sources, sizeof *m->src);
    m->rays = reverb_rays > 0 ? reverb_rays : 1024;
    IPLSimulationSettings ss = {0};
    m->pathing = pathing != 0;
    ss.flags = IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS | (m->pathing ? IPL_SIMULATIONFLAGS_PATHING : 0);
    ss.sceneType = IPL_SCENETYPE_DEFAULT;
    ss.reflectionType = IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
    ss.maxNumOcclusionSamples = SA_OCCL_SAMPLES;
    ss.maxNumRays = m->rays;
    ss.numDiffuseSamples = 32;
    ss.maxDuration = SA_REVERB_DURATION;
    ss.maxOrder = m->pathing ? 1 : 0;   /* pathing writes order-1 SH into buffers of this order */
    ss.maxNumSources = max_sources + 1;
    ss.numThreads = 1;
    ss.numVisSamples = m->pathing ? SA_VIS_SAMPLES : 0;
    ss.samplingRate = SA_RATE;
    ss.frameSize = SA_FRAME;
    if (!m->src || sa.iplSimulatorCreate(ctx, &ss, &m->sim) != IPL_STATUS_SUCCESS) { free(m->src); free(m); return NULL; }
    IPLSourceSettings d = {IPL_SIMULATIONFLAGS_DIRECT | (m->pathing ? IPL_SIMULATIONFLAGS_PATHING : 0)}, r = {IPL_SIMULATIONFLAGS_REFLECTIONS};
    for (; m->nsrc < max_sources; m->nsrc++) {
        if (sa.iplSourceCreate(m->sim, &d, &m->src[m->nsrc]) != IPL_STATUS_SUCCESS) break;
        sa.iplSourceAdd(m->src[m->nsrc], m->sim);
    }
    if (sa.iplSourceCreate(m->sim, &r, &m->rev) == IPL_STATUS_SUCCESS) sa.iplSourceAdd(m->rev, m->sim);
    else m->rev = NULL;
    sa.iplSimulatorCommit(m->sim);
    return m;
}

void sa_sim_free(SaSim *m) {
    if (!m) return;
    sa_sim_set_probes(m, NULL);
    for (int i = 0; i < m->nsrc; i++) sa.iplSourceRelease(&m->src[i]);
    if (m->rev) sa.iplSourceRelease(&m->rev);
    if (m->sim) sa.iplSimulatorRelease(&m->sim);
    free(m->src);
    free(m);
}

void sa_sim_set_scene(SaSim *m, SaScene *s) {
    if (!m) return;
    m->scene = s;
    sa.iplSimulatorSetScene(m->sim, s ? s->scene : NULL);
    /* a fresh reverb source: the simulator averages a still listener's reflections over runs and does
     * not always see a new scene object as a change, so the last scene's energy would linger */
    if (m->rev) {
        sa.iplSourceRemove(m->rev, m->sim);
        sa.iplSourceRelease(&m->rev);
        IPLSourceSettings r = {IPL_SIMULATIONFLAGS_REFLECTIONS};
        if (sa.iplSourceCreate(m->sim, &r, &m->rev) == IPL_STATUS_SUCCESS) sa.iplSourceAdd(m->rev, m->sim);
        else m->rev = NULL;
    }
    sa.iplSimulatorCommit(m->sim);
}

static IPLCoordinateSpace3 space_at(const float p[3]) {
    IPLCoordinateSpace3 c = {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}, {p[0], p[1], p[2]}};
    return c;
}

void sa_sim_direct(SaSim *m, const float lis[3], const float (*src)[3], int n, float radius, float *occl, float (*trans)[3]) {
    for (int i = 0; i < n; i++) occl[i] = 1, trans[i][0] = trans[i][1] = trans[i][2] = 1;
    if (!m || !m->scene || n <= 0) return;
    if (n > m->nsrc) n = m->nsrc;
    IPLSimulationSharedInputs sh = {0};
    sh.listener = space_at(lis);
    sa.iplSimulatorSetSharedInputs(m->sim, IPL_SIMULATIONFLAGS_DIRECT, &sh);
    for (int i = 0; i < m->nsrc; i++) {
        IPLSimulationInputs in = {0};
        in.flags = IPL_SIMULATIONFLAGS_DIRECT;
        in.directFlags = i < n ? IPL_DIRECTSIMULATIONFLAGS_OCCLUSION | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION : 0;
        in.source = space_at(i < n ? src[i] : lis);
        in.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;
        in.occlusionRadius = radius;
        in.numOcclusionSamples = SA_OCCL_SAMPLES;
        in.numTransmissionRays = SA_TRANS_RAYS;
        sa.iplSourceSetInputs(m->src[i], IPL_SIMULATIONFLAGS_DIRECT, &in);
    }
    sa.iplSimulatorRunDirect(m->sim);
    for (int i = 0; i < n; i++) {
        IPLSimulationOutputs out;
        memset(&out, 0, sizeof out);
        sa.iplSourceGetOutputs(m->src[i], IPL_SIMULATIONFLAGS_DIRECT, &out);
        occl[i] = out.direct.occlusion;
        for (int b = 0; b < 3; b++) trans[i][b] = out.direct.transmission[b];
    }
}

int sa_sim_reverb(SaSim *m, const float lis[3], const float ahead[3], const float up[3], int bounces, float rt60[3]) {
    if (!m || !m->scene || !m->rev) return -1;
    IPLSimulationSharedInputs sh = {0};
    sh.listener = space_at(lis);
    sh.listener.ahead = (IPLVector3){ahead[0], ahead[1], ahead[2]};
    sh.listener.up = (IPLVector3){up[0], up[1], up[2]};
    sh.listener.right = (IPLVector3){ahead[1] * up[2] - ahead[2] * up[1], ahead[2] * up[0] - ahead[0] * up[2], ahead[0] * up[1] - ahead[1] * up[0]};
    sh.numRays = m->rays;
    sh.numBounces = bounces;
    sh.duration = SA_REVERB_DURATION;
    sh.order = 0;
    sh.irradianceMinDistance = 1.0f;
    sa.iplSimulatorSetSharedInputs(m->sim, IPL_SIMULATIONFLAGS_REFLECTIONS, &sh);
    IPLSimulationInputs in = {0};
    in.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
    in.source = space_at(lis);
    in.reverbScale[0] = in.reverbScale[1] = in.reverbScale[2] = 1.0f;
    sa.iplSourceSetInputs(m->rev, IPL_SIMULATIONFLAGS_REFLECTIONS, &in);
    sa.iplSimulatorRunReflections(m->sim);
    IPLSimulationOutputs out;
    memset(&out, 0, sizeof out);
    sa.iplSourceGetOutputs(m->rev, IPL_SIMULATIONFLAGS_REFLECTIONS, &out);
    for (int b = 0; b < 3; b++) rt60[b] = out.reflections.reverbTimes[b];
    return 0;
}


/* ---- pathing ---- */
struct SaProbes {
    IPLProbeBatch batch;
};
static const IPLBakedDataIdentifier PATH_ID = {IPL_BAKEDDATATYPE_PATHING, IPL_BAKEDDATAVARIATION_DYNAMIC, {{0, 0, 0}, 0}};

static SaProbes *probes_wrap(IPLProbeBatch b) {
    SaProbes *p = calloc(1, sizeof *p);
    if (!p) { sa.iplProbeBatchRelease(&b); return NULL; }
    p->batch = b;
    return p;
}

SaProbes *sa_probes_new(SaScene *s, const float lo[3], const float hi[3], float spacing, float height) {
    if (!sa_ok() || !s || !(spacing > 0)) return NULL;
    IPLProbeArray arr = NULL;
    if (sa.iplProbeArrayCreate(ctx, &arr) != IPL_STATUS_SUCCESS) return NULL;
    IPLProbeGenerationParams g;
    memset(&g, 0, sizeof g);
    g.type = IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR;
    g.spacing = spacing;
    g.height = height;
    /* the unit cube (centred on the origin as the generator reads it) scaled and moved onto the box; row-major */
    for (int i = 0; i < 3; i++) {
        g.transform.elements[i][i] = hi[i] - lo[i];
        g.transform.elements[i][3] = 0.5f * (lo[i] + hi[i]);
    }
    g.transform.elements[3][3] = 1;
    sa.iplProbeArrayGenerateProbes(arr, s->scene, &g);
    IPLProbeBatch b = NULL;
    if (sa.iplProbeBatchCreate(ctx, &b) != IPL_STATUS_SUCCESS) { sa.iplProbeArrayRelease(&arr); return NULL; }
    sa.iplProbeBatchAddProbeArray(b, arr);
    sa.iplProbeArrayRelease(&arr);
    sa.iplProbeBatchCommit(b);
    if (sa.iplProbeBatchGetNumProbes(b) <= 0) { sa.iplProbeBatchRelease(&b); return NULL; }
    return probes_wrap(b);
}

int sa_probes_count(const SaProbes *p) { return p ? sa.iplProbeBatchGetNumProbes(p->batch) : 0; }

size_t sa_probes_data_size(const SaProbes *p) {
    IPLBakedDataIdentifier id = PATH_ID;
    return p ? (size_t)sa.iplProbeBatchGetDataSize(p->batch, &id) : 0;
}

static void IPLCALL bake_progress(IPLfloat32 f, void *u) { (void)f, (void)u; }

int sa_probes_bake(SaProbes *p, SaScene *s, const SaPathBake *b) {
    if (!sa_ok() || !p || !s || !b) return -1;
    IPLPathBakeParams bp;
    memset(&bp, 0, sizeof bp);
    bp.scene = s->scene;
    bp.probeBatch = p->batch;
    bp.identifier = PATH_ID;
    bp.numSamples = b->samples > 0 ? b->samples : 1;
    bp.radius = b->radius;
    bp.threshold = b->threshold;
    bp.visRange = b->vis_range;
    bp.pathRange = b->path_range;
    bp.numThreads = b->threads > 0 ? b->threads : 1;
    sa.iplPathBakerBake(ctx, &bp, bake_progress, NULL);
    sa.iplProbeBatchCommit(p->batch);
    return sa_probes_data_size(p) > 0 ? 0 : -1;   /* nothing baked (no probe sees another) */
}

size_t sa_probes_save(SaProbes *p, uint8_t **out) {
    *out = NULL;
    if (!sa_ok() || !p) return 0;
    IPLSerializedObjectSettings ss = {NULL, 0};
    IPLSerializedObject so = NULL;
    if (sa.iplSerializedObjectCreate(ctx, &ss, &so) != IPL_STATUS_SUCCESS) return 0;
    sa.iplProbeBatchSave(p->batch, so);
    const size_t n = (size_t)sa.iplSerializedObjectGetSize(so);
    if (n && (*out = malloc(n))) memcpy(*out, sa.iplSerializedObjectGetData(so), n);
    sa.iplSerializedObjectRelease(&so);
    return *out ? n : 0;
}

SaProbes *sa_probes_load(const uint8_t *d, size_t n) {
    if (!sa_ok() || !d || !n) return NULL;
    IPLSerializedObjectSettings ss = {(IPLbyte *)d, (IPLsize)n};
    IPLSerializedObject so = NULL;
    if (sa.iplSerializedObjectCreate(ctx, &ss, &so) != IPL_STATUS_SUCCESS) return NULL;
    IPLProbeBatch b = NULL;
    const IPLerror e = sa.iplProbeBatchLoad(ctx, so, &b);
    sa.iplSerializedObjectRelease(&so);
    if (e != IPL_STATUS_SUCCESS || !b) return NULL;
    sa.iplProbeBatchCommit(b);
    SaProbes *p = probes_wrap(b);
    if (p && (sa_probes_count(p) <= 0 || !sa_probes_data_size(p))) { sa_probes_free(p); return NULL; }
    return p;
}

void sa_probes_free(SaProbes *p) {
    if (!p) return;
    if (p->batch && sa.iplProbeBatchRelease) sa.iplProbeBatchRelease(&p->batch);
    free(p);
}

void sa_sim_set_probes(SaSim *m, SaProbes *p) {
    if (!m || !m->pathing || m->probes == p) return;
    if (m->probes) sa.iplSimulatorRemoveProbeBatch(m->sim, m->probes->batch);
    m->probes = p;
    if (p) sa.iplSimulatorAddProbeBatch(m->sim, p->batch);
    sa.iplSimulatorCommit(m->sim);
}

/* Order-1 SH of the arriving paths: the library projects each path's virtual source with real harmonics
 * (no Condon-Shortley phase: Y1-1 = k y', Y10 = k z', Y11 = k x') after mapping (x, y, z) to
 * (x', y', z') = (-z, -x, y) (core/src/core/sh.cpp), so the order-1 coefficients are -k x, k y, -k z
 * of the summed direction in Steam Audio's axes (checked: steam_audio_test, the L corridor). */
#define SA_SH_Y00 0.28209479f

void sa_sim_pathing(SaSim *m, const float lis[3], const float (*src)[3], int n, const SaPathQuery *q, SaPath *out) {
    for (int i = 0; i < n; i++) memset(&out[i], 0, sizeof out[i]);
    if (!m || !m->pathing || !m->scene || !m->probes || n <= 0) return;
    if (n > m->nsrc) n = m->nsrc;
    IPLSimulationSharedInputs sh = {0};
    sh.listener = space_at(lis);
    sa.iplSimulatorSetSharedInputs(m->sim, IPL_SIMULATIONFLAGS_PATHING, &sh);
    for (int i = 0; i < m->nsrc; i++) {
        IPLSimulationInputs in = {0};
        in.flags = i < n ? IPL_SIMULATIONFLAGS_PATHING : 0;
        in.source = space_at(i < n ? src[i] : lis);
        in.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_INVERSEDISTANCE;
        in.distanceAttenuationModel.minDistance = 1.0f;
        in.pathingProbes = m->probes->batch;
        in.visRadius = q->vis_radius;
        in.visThreshold = q->vis_threshold;
        in.visRange = q->vis_range;
        in.pathingOrder = 1;
        in.enableValidation = q->validate ? IPL_TRUE : IPL_FALSE;
        in.findAlternatePaths = q->validate ? IPL_TRUE : IPL_FALSE;
        sa.iplSourceSetInputs(m->src[i], IPL_SIMULATIONFLAGS_PATHING, &in);
    }
    sa.iplSimulatorRunPathing(m->sim);
    for (int i = 0; i < n; i++) {
        IPLSimulationOutputs o;
        memset(&o, 0, sizeof o);
        sa.iplSourceGetOutputs(m->src[i], IPL_SIMULATIONFLAGS_PATHING, &o);
        const float *c = o.pathing.shCoeffs;
        if (!c || !(c[0] > 0) || !isfinite(c[0])) continue;   /* no path: the source is cut off */
        const float l = sqrtf(c[1] * c[1] + c[2] * c[2] + c[3] * c[3]);
        out[i].level = c[0] / SA_SH_Y00;
        for (int b = 0; b < 3; b++) out[i].eq[b] = o.pathing.eqCoeffs[b];
        if (l > 1e-9f) out[i].dir[0] = -c[1] / l, out[i].dir[1] = c[2] / l, out[i].dir[2] = -c[3] / l;
        else out[i].dir[2] = -1;
    }
}
