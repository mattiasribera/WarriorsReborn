/* The Display page's video-memory meter: how much device memory the chosen settings need, by
 * category, against what the graphics card has (gpu.h probe). estimate_vram is pure (settings and
 * pack sizes in, bytes per category out) and mirrors the runtime's allocations
 * (runtime/src/renderer/vk_renderer.cpp): the frame's targets at the internal size (resolution x
 * supersampling, MSAA samples), the post-processing targets, the title's offscreen surfaces (shadow
 * maps) scaled by the render scale and the shadow scale, the texture cache (the game's own
 * textures plus the share of the packs a game loads, capped by the cache budget) and the
 * swapchain. Its constants were
 * calibrated against the runtime's --stats [vram] line and nvidia-smi on an RTX 2060
 *. pack_device_bytes reads a pack folder from disk. */
#ifndef LAUNCHER_VRAM_H
#define LAUNCHER_VRAM_H
#include "profile.h"
#include <atomic>
#include <cstdint>
#include <string>

namespace launcher {

/* The tooltip's categories: Screen = resolution and effects (frame targets, post-processing,
 * swapchain), Shadows = the title's offscreen surfaces, Textures = the texture cache, Characters =
 * the HD character models, Other = driver and fixed runtime memory. */
enum class VramCat { Screen, Shadows, Textures, Characters, Other, Count };

/* What the estimate needs besides the profile. */
struct VramEnv {
    int desktop_w = 0, desktop_h = 0; /* the swapchain size in fullscreen; 0 = the output size */
    uint64_t pack_bytes = 0;          /* device bytes of the texture packs the profile loads (pack_device_bytes) */
    bool pack = false;                /* a pack is loaded: the runtime's 1 GiB cache budget, else 512 MiB */
    int hd_models = 0;                /* HD character meshes the launcher passes (--mesh-pack) */
};

struct VramEstimate {
    uint64_t bytes[(int)VramCat::Count] = {};
    uint64_t total() const;
    uint64_t operator[](VramCat c) const { return bytes[(int)c]; }
};

VramEstimate estimate_vram(const Profile &p, const VramEnv &env);

/* The runtime's texture cache budget (runtime/src/hle_d3d.c TEXPACK_BUDGET, vk_renderer.cpp
 * DEFAULT_TEXTURE_BYTES). */
constexpr uint64_t VRAM_TEXTURE_BUDGET_PACK = 1024ull << 20, VRAM_TEXTURE_BUDGET = 512ull << 20;

/* The device bytes of the replacements in a pack folder, as the runtime uploads them: a .dds (when
 * the device samples BC, bc != 0) is its payload, else a .png is width x height x 4 with its mip
 * chain (x 4/3). The size comes from the file header; 0 for a missing folder. A pack holds thousands
 * of files, so the launcher scans it on a worker (async.h): a set *cancel stops the scan (0). */
uint64_t pack_device_bytes(const std::string &dir, bool bc, const std::atomic<bool> *cancel = nullptr);

/* Ok up to 80% of the available memory, High above, Over above 100%; Ok when available is 0. */
enum class VramLevel { Ok, High, Over };
VramLevel vram_level(uint64_t used, uint64_t available);

/* "1920x1080" -> 1920, 1080; false (w, h unchanged) for anything else. */
bool parse_resolution(const std::string &s, int &w, int &h);

/* "1,5 GB" (decimal comma) or "1.5 GB", one decimal; GB = 2^30 bytes, as gpu_vram_text and the
 * cards' own "6 GB" count. */
std::string format_vram_gb(uint64_t bytes, bool comma);

} // namespace launcher
#endif
