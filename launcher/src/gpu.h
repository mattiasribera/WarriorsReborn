/* The PC's graphics card, for the first-run preset: probe_gpus() lists the adapters the system
 * reports (Windows: DXGI; Linux: Vulkan through a dlopen of the loader, else /sys/class/drm),
 * choose_gpu() keeps the one the game will use (the runtime prefers a discrete GPU, see
 * runtime/src/renderer/vk_renderer.cpp) and classifies it, and preset_for_gpu() maps the class
 * to a preset. The classifier is pure, so launcher_test feeds it fake adapter descriptions. */
#ifndef LAUNCHER_GPU_H
#define LAUNCHER_GPU_H
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
};

/* integrated -> Laptop, low (discrete, < 4 GB) -> Balanced, mid (4-8 GB) and high (8 GB and up)
 * -> Gaming, unknown (nothing found, or a card whose memory is not known) -> Balanced. */
enum class GpuClass { Unknown, Integrated, Low, Mid, High };

struct GpuChoice {
    GpuClass cls = GpuClass::Unknown;
    bool found = false;          /* adapter holds the classified card */
    GpuAdapter adapter;
};

GpuKind gpu_kind(const GpuAdapter &a);           /* the API's kind, else the guess */
GpuClass classify_gpu(const GpuAdapter &a);
/* The adapter the game will use: software renderers are left out, then a discrete card before an
 * integrated one, then the most memory. */
GpuChoice choose_gpu(const std::vector<GpuAdapter> &adapters);
Preset preset_for_gpu(GpuClass cls);
const char *gpu_class_key(GpuClass cls);         /* unknown|integrated|low|mid|high */

/* The name without (R), (TM) and a trailing driver code name such as "(RADV NAVI23)". */
std::string gpu_display_name(const std::string &name);
/* "2 GB", "512 MB", "4+ GB" (vram_at_least); empty when unknown. */
std::string gpu_vram_text(const GpuAdapter &a);
/* Why the first run picked preset, in the launcher's language (i18n.h), e.g. "We picked
 * Balanced for your GeForce GTX 1050 (2 GB)." */
std::string gpu_reason(const GpuChoice &choice, Preset preset);

/* Every adapter the system reports; empty when none can be read. Can take a moment (a Vulkan
 * instance), so it runs only for a first-run profile and New. */
std::vector<GpuAdapter> probe_gpus();

} // namespace launcher
#endif
