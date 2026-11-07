#include "vram.h"
#include "platform.h"
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace launcher {

namespace {

constexpr uint64_t MiB = 1ull << 20;
constexpr int BACK_W = 640, BACK_H = 480;   /* the title's back buffer */
constexpr uint32_t MAX_DIM = 16384;          /* the device's image size limit (RTX 2060; most cards) */

/* Calibrated on an RTX 2060 (driver 580): */
/* the title's offscreen surfaces (shadow maps, reflections) at render scale 1 (640x480 internal);
 * they grow with the square of the internal height over 480 times the shadow scale */
constexpr uint64_t SURFACES_AT_1X = 3 * MiB;
/* the game's own textures in the cache (DXT as BC blocks) */
constexpr uint64_t GAME_TEXTURES = 8 * MiB;
/* the GPU skinning buffers: one per frame in flight, grown in powers of two (8 MiB each with the
 * game's models, 32 MiB with the HD characters' denser meshes) */
constexpr uint64_t SKIN_SLOT = 8 * MiB, SKIN_SLOT_HD = 32 * MiB;
/* driver memory the runtime does not allocate itself (pipelines, descriptor pools, command
 * buffers, the context): nvidia-smi's per-process reading minus the runtime's [vram] total
 * (45 and 51 MiB in the two calibration runs) */
constexpr uint64_t DRIVER = 48 * MiB;
/* the share of a pack the cache ends up holding: the cache keeps every replacement it loaded
 * until the budget, and the in-game union of the texture dumps (7 areas)
 * is 674 MiB of texpack-v4-bc's 1874 MiB of BC payloads */
constexpr uint64_t PACK_SHARE_NUM = 9, PACK_SHARE_DEN = 25;
/* the runtime's 2x character skins (--sharp-faces) and 4x font atlases (sharp fonts) */
constexpr uint64_t SHARP_FACES = 8 * MiB, SHARP_FONTS = 4 * MiB;
constexpr int SWAPCHAIN_IMAGES = 3;

/* runtime/src/video.c video_fit: the largest sw:sh rectangle inside dw x dh. */
void fit(int sw, int sh, int dw, int dh, int &w, int &h) {
    if ((int64_t)dw * sh >= (int64_t)dh * sw) {
        h = dh;
        w = (int)(((int64_t)dh * sw * 2 + sh) / (2 * sh));
    } else {
        w = dw;
        h = (int)(((int64_t)dw * sh * 2 + sw) / (2 * sw));
    }
}

uint64_t px(uint64_t w, uint64_t h) { return w * h; }

/* Bytes of a full mip chain of a w x h image with bpp bytes per texel. */
uint64_t mip_chain(uint64_t w, uint64_t h, uint64_t bpp) {
    uint64_t n = 0;
    for (;;) {
        n += w * h * bpp;
        if (w == 1 && h == 1) return n;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
}

uint32_t be32(const std::string &s, size_t at) {
    return (uint32_t)(uint8_t)s[at] << 24 | (uint32_t)(uint8_t)s[at + 1] << 16 | (uint32_t)(uint8_t)s[at + 2] << 8 | (uint8_t)s[at + 3];
}

bool ends_with(const std::string &s, const char *suffix, std::string &stem) {
    size_t n = strlen(suffix);
    if (s.size() <= n) return false;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)s[s.size() - n + i]) != suffix[i]) return false;
    stem = s.substr(0, s.size() - n);
    return true;
}

} // namespace

bool parse_resolution(const std::string &s, int &w, int &h) {
    char *end = nullptr;
    long a = strtol(s.c_str(), &end, 10);
    if (!end || (*end != 'x' && *end != 'X')) return false;
    long b = strtol(end + 1, &end, 10);
    if (a <= 0 || b <= 0 || *end) return false;
    w = (int)a, h = (int)b;
    return true;
}

uint64_t VramEstimate::total() const {
    uint64_t t = 0;
    for (uint64_t b : bytes) t += b;
    return t;
}

VramEstimate estimate_vram(const Profile &p, const VramEnv &env) {
    VramEstimate e;
    /* The output size (runtime/src/hle_d3d.c hle_d3d_output_size): the title's aspect fitted into
     * the resolution, or 640x480 x render scale. */
    const int aw = p.widescreen ? 16 : BACK_W, ah = p.widescreen ? 9 : BACK_H;
    int rw = 0, rh = 0, fw = 0, fh = 0;
    if (!parse_resolution(p.resolution, rw, rh)) {
        int n = p.render_scale > 0 ? p.render_scale : 1;
        rw = BACK_W * n * 4, rh = BACK_H * n;
        fit(aw, ah, rw, rh, fw, fh);
        rw = fw, rh = fh;
    } else {
        fit(aw, ah, rw, rh, fw, fh);
    }
    /* Supersampling, halved while the internal size exceeds the device limit (create_targets). */
    int ss = p.supersample >= 4 ? 4 : p.supersample >= 2 ? 2 : 1;
    while (ss > 1 && ((uint32_t)(fw * ss) > MAX_DIM || (uint32_t)(fh * ss) > MAX_DIM)) ss /= 2;
    const uint64_t ow = (uint64_t)fw * ss, oh = (uint64_t)fh * ss, P = ow * oh, F = px(fw, fh);
    const uint64_t samples = p.msaa >= 2 ? (uint64_t)p.msaa : 1;
    const bool ms = samples > 1;

    /* The frame's targets (create_targets): color and depth (4 bytes, depth at the sample count),
     * the MSAA color, the base image and the supersampling down chain. */
    uint64_t screen = 4 * P + 4 * P * samples;
    if (ms) screen += 4 * P * samples;
    if (ms || ow != BACK_W || oh != BACK_H) screen += 4 * px(BACK_W, BACK_H);
    for (uint64_t dw = ow / 2, dh = oh / 2; ss > 1 && dw >= (uint64_t)fw; dw /= 2, dh /= 2) screen += 4 * dw * dh;
    /* Post-processing targets, each as its *_create allocates it. */
    const bool glass = p.hud_style == "glass" || ((p.hud_style == "hd" || p.hud_style == "hd-glass") && p.hud_frost > 0);
    if (glass) screen += 2 * 4 * P;
    if (p.sharpen > 0) screen += 4 * F;
    if (p.bloom) {   /* RGBA16F from half the internal size, 4 levels (6 with the filmic look) */
        uint64_t w = ow / 2 > 1 ? ow / 2 : 1, h = oh / 2 > 1 ? oh / 2 : 1;
        int levels = 1, want = p.filmic ? 6 : 4;
        while (levels < want && (h >> levels) >= 2) levels++;
        if (levels < 2) levels = 2;
        for (int i = 0; i < levels; i++, w = w / 2 > 1 ? w / 2 : 1, h = h / 2 > 1 ? h / 2 : 1) screen += 8 * w * h;
    }
    if (p.filmic) screen += 4 * P + 32 * 32 * 32 * 4;
    const uint64_t half = px((ow + 2 * ss - 1) / (2 * ss), (oh + 2 * ss - 1) / (2 * ss));   /* half the output size */
    if (p.ssao) screen += 2 * 4 * half;          /* two RG16F images */
    if (p.lamp_lights) screen += 8 * half;       /* RGBA16F */
    if (p.rain_hd || p.lamp_lights || p.contact_shadows) screen += 4 * P;   /* the characters' depth (D24) */
    if (p.rain_hd) {
        screen += 4 * F;                         /* the splash mask (RG16F at the output size) */
        screen += 512 * 512 * 2;                 /* the cover map (D16) */
        if (p.rain_puddles) screen += 4 * half + 2 * 8 * half;   /* source + two RGBA16F */
    }
    if (p.contact_shadows) screen += half;       /* R8 */
    /* The swapchain: the window, the desktop in fullscreen. */
    uint64_t sw = (uint64_t)rw, sh = (uint64_t)rh;
    if (p.fullscreen && env.desktop_w > 0 && env.desktop_h > 0) sw = (uint64_t)env.desktop_w, sh = (uint64_t)env.desktop_h;
    screen += 4 * sw * sh * SWAPCHAIN_IMAGES;
    e.bytes[(int)VramCat::Screen] = screen;

    /* The title's surfaces (renderer_surface): w x h x (internal height / 480 x shadow scale). */
    const int scale = p.shadow_scale > 1 ? (p.shadow_scale < 4 ? p.shadow_scale : 4) : 1;
    const uint64_t f_num = oh * (uint64_t)scale;   /* f = f_num / 480 */
    e.bytes[(int)VramCat::Shadows] = SURFACES_AT_1X * f_num * f_num / ((uint64_t)BACK_H * BACK_H);

    /* The texture cache: the game's textures and the share of the packs a game loads, at most the
     * budget. */
    uint64_t tex = GAME_TEXTURES + (env.pack ? env.pack_bytes * PACK_SHARE_NUM / PACK_SHARE_DEN : 0) + (p.sharp_faces ? SHARP_FACES : 0);
    const uint64_t budget = env.pack ? VRAM_TEXTURE_BUDGET_PACK : VRAM_TEXTURE_BUDGET;
    e.bytes[(int)VramCat::Textures] = tex < budget ? tex : budget;

    /* GPU skinning: a buffer per frame in flight; the HD characters double it. */
    const uint64_t frames = p.frames_in_flight > 0 ? (uint64_t)p.frames_in_flight : 2;
    e.bytes[(int)VramCat::Characters] = env.hd_models > 0 ? frames * (SKIN_SLOT_HD - SKIN_SLOT) : 0;
    e.bytes[(int)VramCat::Other] = DRIVER + frames * SKIN_SLOT + (p.sharp_fonts ? SHARP_FONTS : 0);
    return e;
}

uint64_t pack_device_bytes(const std::string &dir, bool bc) {
    std::vector<DirEntry> list;
    if (!list_dir(dir, list)) return 0;
    /* One replacement per name: the .dds when the device samples BC, else the .png. */
    std::map<std::string, std::pair<const DirEntry *, const DirEntry *>> by_stem;   /* png, dds */
    for (const DirEntry &e : list) {
        if (e.dir) continue;
        std::string stem;
        if (ends_with(e.name, ".png", stem)) by_stem[stem].first = &e;
        else if (ends_with(e.name, ".dds", stem)) by_stem[stem].second = &e;
    }
    uint64_t total = 0;
    for (const auto &kv : by_stem) {
        const DirEntry *png = kv.second.first, *dds = kv.second.second;
        if (dds && bc) {
            std::string head;
            uint64_t hdr = 128;   /* magic + DDS_HEADER, + DDS_HEADER_DXT10 for a "DX10" FourCC */
            if (read_file(path_join(dir, dds->name), head, 128) && head.size() >= 88 && head.compare(84, 4, "DX10") == 0) hdr += 20;
            if (dds->size > hdr) total += dds->size - hdr;
        } else if (png) {
            std::string head;
            if (read_file(path_join(dir, png->name), head, 24) && head.size() >= 24 && head.compare(12, 4, "IHDR") == 0) {
                uint32_t w = be32(head, 16), h = be32(head, 20);
                if (w && h && w <= MAX_DIM && h <= MAX_DIM) total += mip_chain(w, h, 4);
            }
        }
    }
    return total;
}

VramLevel vram_level(uint64_t used, uint64_t available) {
    if (!available) return VramLevel::Ok;
    if (used > available) return VramLevel::Over;
    if (used * 5 > available * 4) return VramLevel::High;
    return VramLevel::Ok;
}

std::string format_vram_gb(uint64_t bytes, bool comma) {
    uint64_t tenths = (bytes * 10 + (1ull << 29)) >> 30;
    char buf[32];
    snprintf(buf, sizeof buf, "%llu%c%llu GB", (unsigned long long)(tenths / 10), comma ? ',' : '.', (unsigned long long)(tenths % 10));
    return buf;
}

} // namespace launcher
