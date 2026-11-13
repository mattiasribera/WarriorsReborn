/* The PC's graphics card, for the recommended preset: probe_gpus() lists the adapters the system
 * reports (Vulkan through the loader on Windows and Linux; else DXGI on Windows and
 * /sys/class/drm on Linux), choose_gpu() keeps the one the game will use (the runtime prefers a
 * discrete GPU, see runtime/src/renderer/vk_renderer.cpp), classifies it by memory and gives it a
 * tier from the tier table (data/gpu_tiers.txt, embedded), and preset_for_tier() maps the tier to
 * a preset. refresh_gpu_detection() keeps the result in the profiles file and works it out again
 * only when the card, its driver or the table changes. Everything but the probes is pure, so
 * launcher_test feeds it fake adapter descriptions. */
#ifndef LAUNCHER_GPU_H
#define LAUNCHER_GPU_H
#include "i18n.h"
#include "profile.h"
#include <cstdint>
#include <string>
#include <vector>

namespace launcher {

/* What the API says the adapter is; Unknown when it does not say (DXGI), and the classifier
 * guesses from the vendor, the name and the memory. */
enum class GpuKind { Unknown, Integrated, Discrete, Software };

struct GpuAdapter {
    std::string name;            /* as the API reports it, e.g. "NVIDIA GeForce GTX 1050" */
    uint32_t vendor_id = 0;      /* PCI vendor: 0x10de NVIDIA, 0x1002 AMD, 0x8086 Intel */
    uint64_t vram = 0;           /* dedicated video memory in bytes, 0 = unknown */
    bool vram_at_least = false;  /* the API's reading tops out (32-bit DXGI): vram or more */
    GpuKind kind = GpuKind::Unknown;
    uint32_t device_id = 0;      /* PCI device ID, 0 = unknown */
    std::string driver;          /* decoded driver version ("566.36"), empty = unknown */
    uint64_t budget = 0;         /* the memory budget of the device-local heap (VK_EXT_memory_budget),
                                  * else its size; 0 = unknown */
    int bc = -1;                 /* BC (DXT) texture support: 1, 0, -1 = unknown */
};

/* Performance tiers. T0 -> Performance, T1 -> Balanced, T2 -> Quality, T3 -> Quality for now:
 * T3 is the hook for a future Ultra preset (preset_for_tier). */
enum class GpuTier { Unknown, T0, T1, T2, T3 };

struct GpuTierRule {
    enum Kind { Name, Id, Demote } kind = Name;
    uint32_t vendor = 0;         /* PCI vendor, 0 = any */
    GpuTier tier = GpuTier::Unknown;
    std::string pattern;         /* Name, Demote: lower case, # digit, ? any character, * any run */
    uint32_t lo = 0, hi = 0;     /* Id: the device ID range, inclusive */
};

/* data/gpu_tiers.txt: the format is described in the file. */
struct GpuTierTable {
    int version = 0;
    std::vector<GpuTierRule> rules;   /* in file order; the first Name/Id match wins */
};

/* integrated -> Laptop, low (discrete, < 4 GB) -> Balanced, mid (4-8 GB) and high (8 GB and up)
 * -> Gaming, unknown (nothing found, or a card whose memory is not known) -> Balanced. */
enum class GpuClass { Unknown, Integrated, Low, Mid, High };

struct GpuChoice {
    GpuClass cls = GpuClass::Unknown;
    GpuTier tier = GpuTier::Unknown;
    bool found = false;          /* adapter holds the classified card */
    GpuAdapter adapter;
};

GpuKind gpu_kind(const GpuAdapter &a);           /* the API's kind, else the guess */
GpuClass classify_gpu(const GpuAdapter &a);
/* The adapter the game will use: software renderers are left out, then a discrete card before an
 * integrated one, then the most memory. Its tier comes from table. */
GpuChoice choose_gpu(const std::vector<GpuAdapter> &adapters);
GpuChoice choose_gpu(const std::vector<GpuAdapter> &adapters, const GpuTierTable &table);
Preset preset_for_gpu(GpuClass cls);             /* the memory-only rule of earlier launchers */
const char *gpu_class_key(GpuClass cls);         /* unknown|integrated|low|mid|high */

/* The tier table: parse_gpu_tiers fails on a malformed line (err names it) or a missing
 * version; builtin_gpu_tiers is the embedded data/gpu_tiers.txt (empty, version 0, if it ever
 * failed to parse, so only the memory rule is left). */
bool parse_gpu_tiers(const std::string &text, GpuTierTable &out, std::string *err);
const GpuTierTable &builtin_gpu_tiers();
bool gpu_name_matches(const std::string &pattern, const std::string &lower_name);   /* anywhere in the name */
/* Integrated -> T0, software or nothing -> Unknown; else the first Name rule matching the display
 * name or Id rule matching the device ID, one lower per matching Demote rule kind (once, not below
 * T1), else the memory class (low T1, mid T2, high T3). Then at most T1 under 3.5 GB and T0
 * without BC textures. */
GpuTier gpu_tier(const GpuTierTable &table, const GpuAdapter &a);
Preset preset_for_tier(GpuTier tier);
Preset preset_for_adapter(const GpuAdapter &a, GpuTier tier); /* Iris Xe for Intel Iris Xe / Arc integrated, else preset_for_tier */
const char *gpu_tier_key(GpuTier tier);          /* unknown|t0|t1|t2|t3 */
bool gpu_tier_from_key(const std::string &key, GpuTier &out);

/* VkPhysicalDeviceProperties::driverVersion as the vendor's tools show it: NVIDIA 10/8/8/6 bits
 * ("566.36"), Intel on Windows 18.14 bits ("101.5762"), else VK_MAKE_VERSION ("24.2.8"); empty
 * for 0. */
std::string decode_driver_version(uint32_t vendor, uint32_t raw, bool windows);
/* "vendor:device:driver" in hex/text, e.g. "10de:1f08:566.36". */
std::string gpu_identity(const GpuAdapter &a);

/* Stores now's card in stored when it differs from the stored one (identity) or the stored one was
 * worked out with another table version; true when it changed. A probe that found nothing keeps
 * the stored card. */
bool refresh_gpu_detection(GpuDetection &stored, const GpuChoice &now, const GpuTierTable &table);
/* The stored recommendation; Balanced when nothing was detected. */
Preset gpu_detected_preset(const GpuDetection &d);
/* "Detectamos: NVIDIA GeForce RTX 2060 (6 GB) — recomendado: Calidad", in the launcher's language. */
std::string gpu_detected_line(const GpuDetection &d);

/* The name without (R), (TM) and a trailing driver code name such as "(RADV NAVI23)". */
std::string gpu_display_name(const std::string &name);
/* "2 GB", "512 MB", "4+ GB" (vram_at_least); empty when unknown. */
std::string gpu_vram_text(const GpuAdapter &a);
/* Why the first run picked preset, in the launcher's language (i18n.h), e.g. "We picked
 * Balanced for your GeForce GTX 1050 (2 GB)." */
std::string gpu_reason(const GpuChoice &choice, Preset preset);

/* Every adapter the system reports; empty when none can be read. Can take a moment (a Vulkan
 * instance), so it runs on a worker (gui.h app_gpu_start). vulkan (when given) is set to whether
 * the Vulkan loader listed a usable device: false means no Vulkan driver for this 32-bit process,
 * which the game's renderer needs (the adapters then come from DXGI or /sys/class/drm). */
std::vector<GpuAdapter> probe_gpus(bool *vulkan = nullptr);
/* probe_gpus' result as the launcher's GPU job returns it (gui.h app_gpu_start, on a worker). */
struct GpuProbe {
    std::vector<GpuAdapter> adapters;
    bool vulkan = false;
};
/* The Home warning's Vulkan flag once the probe is in: a value set before (a capture's pin, 0 or 1)
 * is kept, else 1 or 0 from the probe. Until the probe is in the flag stays -1 (no warning). */
int vulkan_driver_after_probe(int current, const GpuProbe &probe);

/* The Home page warning about frames the game cannot draw: NoVulkanDriver when the probe found no
 * 32-bit Vulkan driver (vulkan_driver 0; 1 found, -1 not probed), whatever the renderer; else
 * RendererNone when the profile's renderer is "none" outside the advanced options (with them shown
 * it is a developer's choice); else None. Launching is never blocked. */
enum class RendererWarning { None, NoVulkanDriver, RendererNone };
RendererWarning renderer_warning(int vulkan_driver, const std::string &renderer, bool advanced);
S renderer_warning_text(RendererWarning w);   /* the warning's string id (S::Count for None) */

} // namespace launcher
#endif
