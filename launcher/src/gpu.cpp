#include "gpu.h"
#include "i18n.h"
#include "platform.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#else
#include <dlfcn.h>
#endif

namespace launcher {

static const uint32_t VENDOR_NVIDIA = 0x10de, VENDOR_AMD = 0x1002, VENDOR_INTEL = 0x8086, VENDOR_MICROSOFT = 0x1414;
static const uint64_t GIB = 1024ull * 1024 * 1024, MIB = 1024ull * 1024;

static std::string lower(const std::string &s) {
    std::string r = s;
    for (char &c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

static bool has(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

/* ---- Classifier ---- */

GpuKind gpu_kind(const GpuAdapter &a) {
    if (a.kind != GpuKind::Unknown) return a.kind;
    std::string n = lower(a.name);
    if (a.vendor_id == VENDOR_MICROSOFT || has(n, "basic render") || has(n, "llvmpipe") || has(n, "swiftshader"))
        return GpuKind::Software;
    switch (a.vendor_id) {
    case VENDOR_NVIDIA: return GpuKind::Discrete;   /* no integrated NVIDIA graphics on PCs */
    /* APUs are "AMD Radeon(TM) Graphics", "Radeon Vega 8 Graphics", "Radeon 780M Graphics";
     * their memory is a carve-out of the system's, up to 8 GB, so it cannot tell them apart.
     * The cards are "Radeon RX 6600", "Radeon RX 580 Series", "Radeon HD 7850". */
    case VENDOR_AMD: return has(n, "graphics") ? GpuKind::Integrated : GpuKind::Discrete;
    /* Intel's integrated graphics report at most a few hundred MB of their own (even the
     * integrated "Arc Graphics"); the Arc cards have 4 GB and more. */
    case VENDOR_INTEL: return a.vram >= 2 * GIB ? GpuKind::Discrete : GpuKind::Integrated;
    default: return GpuKind::Unknown;
    }
}

GpuClass classify_gpu(const GpuAdapter &a) {
    switch (gpu_kind(a)) {
    case GpuKind::Integrated: return GpuClass::Integrated;
    case GpuKind::Discrete:
        if (a.vram == 0) return GpuClass::Unknown;
        if (a.vram < 4 * GIB) return GpuClass::Low;
        if (a.vram < 8 * GIB) return GpuClass::Mid;
        return GpuClass::High;
    default: return GpuClass::Unknown;
    }
}

GpuChoice choose_gpu(const std::vector<GpuAdapter> &adapters) { return choose_gpu(adapters, builtin_gpu_tiers()); }

GpuChoice choose_gpu(const std::vector<GpuAdapter> &adapters, const GpuTierTable &table) {
    GpuChoice c;
    int best_rank = -1;
    for (const GpuAdapter &a : adapters) {
        GpuKind k = gpu_kind(a);
        if (k == GpuKind::Software) continue;
        int rank = k == GpuKind::Discrete ? 2 : 1;
        /* the first of equal rank, but the discrete card with the most memory */
        if (rank > best_rank || (rank == 2 && a.vram > c.adapter.vram)) {
            best_rank = rank;
            c.adapter = a;
            c.found = true;
        }
    }
    if (c.found) c.cls = classify_gpu(c.adapter), c.tier = gpu_tier(table, c.adapter);
    return c;
}

Preset preset_for_gpu(GpuClass cls) {
    switch (cls) {
    case GpuClass::Integrated: return Preset::Laptop;
    case GpuClass::Mid:
    case GpuClass::High: return Preset::Gaming;
    default: return Preset::Balanced; /* low, and unknown: safe on most cards and still looks good */
    }
}

const char *gpu_class_key(GpuClass cls) {
    switch (cls) {
    case GpuClass::Integrated: return "integrated";
    case GpuClass::Low: return "low";
    case GpuClass::Mid: return "mid";
    case GpuClass::High: return "high";
    default: return "unknown";
    }
}

/* ---- Tier table ---- */

extern const unsigned char gpu_tiers_txt[];   /* data/gpu_tiers.txt (tools/embed_files.py) */
extern const unsigned int gpu_tiers_txt_size;

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

/* The first word of s (moved past it and the blanks after it). */
static std::string word(std::string &s) {
    size_t end = s.find_first_of(" \t");
    std::string w = s.substr(0, end);
    s = end == std::string::npos ? "" : trim(s.substr(end));
    return w;
}

static bool vendor_from_word(const std::string &w, uint32_t &out) {
    if (w == "nvidia") return out = VENDOR_NVIDIA, true;
    if (w == "amd") return out = VENDOR_AMD, true;
    if (w == "intel") return out = VENDOR_INTEL, true;
    if (w == "any") return out = 0, true;
    return false;
}

static bool hex_id(const std::string &w, uint32_t &out) {
    char *end = nullptr;
    unsigned long v = strtoul(w.c_str(), &end, 16);
    if (w.empty() || *end || v > 0xFFFF) return false;
    out = (uint32_t)v;
    return true;
}

bool parse_gpu_tiers(const std::string &text, GpuTierTable &out, std::string *err) {
    GpuTierTable t;
    size_t pos = 0;
    int line_no = 0;
    auto fail = [&](const char *what) {
        if (err) *err = "line " + std::to_string(line_no) + ": " + what;
        return false;
    };
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = trim(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = nl == std::string::npos ? text.size() : nl + 1;
        line_no++;
        if (line.empty() || line[0] == '#') continue;   /* comments are whole lines: # is a pattern digit */
        std::string rest = line, first = lower(word(rest));
        if (first == "version") {
            char *end = nullptr;
            long v = strtol(rest.c_str(), &end, 10);
            if (rest.empty() || *end || v <= 0) return fail("version must be a positive number");
            t.version = (int)v;
            continue;
        }
        GpuTierRule r;
        if (first == "demote") r.kind = GpuTierRule::Demote;
        else if (first.size() == 2 && first[0] == 't' && first[1] >= '0' && first[1] <= '3')
            r.tier = (GpuTier)((int)GpuTier::T0 + (first[1] - '0'));
        else return fail("expected T0-T3, demote or version");
        if (!vendor_from_word(lower(word(rest)), r.vendor)) return fail("vendor must be nvidia, amd, intel or any");
        std::string how = lower(word(rest));
        if (how == "name") {
            r.pattern = lower(rest);
            if (r.pattern.empty()) return fail("empty name pattern");
            if (r.kind != GpuTierRule::Demote) r.kind = GpuTierRule::Name;
        } else if (how == "id" && r.kind != GpuTierRule::Demote) {
            size_t dash = rest.find('-');
            if (dash == std::string::npos || !hex_id(trim(rest.substr(0, dash)), r.lo) || !hex_id(trim(rest.substr(dash + 1)), r.hi) || r.lo > r.hi)
                return fail("id needs a range such as 0x1e00-0x1f7f");
            r.kind = GpuTierRule::Id;
        } else return fail(r.kind == GpuTierRule::Demote ? "demote needs name" : "expected name or id");
        t.rules.push_back(r);
    }
    if (t.version <= 0) {
        if (err) *err = "missing version line";
        return false;
    }
    out = t;
    return true;
}

const GpuTierTable &builtin_gpu_tiers() {
    static GpuTierTable table;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        GpuTierTable t;
        if (parse_gpu_tiers(std::string((const char *)gpu_tiers_txt, gpu_tiers_txt_size), t, nullptr)) table = t;
    }
    return table;
}

static bool match_here(const char *p, const char *s) {
    if (!*p) return true;   /* the pattern may end anywhere in the name */
    if (*p == '*') {
        for (;; s++) {
            if (match_here(p + 1, s)) return true;
            if (!*s) return false;
        }
    }
    if (!*s) return false;
    bool one = *p == '#' ? std::isdigit((unsigned char)*s) != 0 : *p == '?' || *p == *s;
    return one && match_here(p + 1, s + 1);
}

bool gpu_name_matches(const std::string &pattern, const std::string &lower_name) {
    if (pattern.empty()) return false;
    for (size_t i = 0; i <= lower_name.size(); i++)
        if (match_here(pattern.c_str(), lower_name.c_str() + i)) return true;
    return false;
}

static GpuTier tier_below(GpuTier t) { return t > GpuTier::T1 ? (GpuTier)((int)t - 1) : t; }

GpuTier gpu_tier(const GpuTierTable &table, const GpuAdapter &a) {
    GpuKind kind = gpu_kind(a);
    if (kind == GpuKind::Software) return GpuTier::Unknown;
    if (kind == GpuKind::Integrated) return GpuTier::T0;
    std::string name = lower(gpu_display_name(a.name));
    GpuTier tier = GpuTier::Unknown;
    bool matched = false;
    for (const GpuTierRule &r : table.rules) {
        if (r.kind == GpuTierRule::Demote || (r.vendor && r.vendor != a.vendor_id)) continue;
        if (r.kind == GpuTierRule::Name ? gpu_name_matches(r.pattern, name)
                                        : a.device_id != 0 && a.device_id >= r.lo && a.device_id <= r.hi) {
            tier = r.tier, matched = true;
            break;
        }
    }
    if (matched) {
        for (const GpuTierRule &r : table.rules)
            if (r.kind == GpuTierRule::Demote && (!r.vendor || r.vendor == a.vendor_id) && gpu_name_matches(r.pattern, name)) {
                tier = tier_below(tier);
                break;
            }
    } else {
        switch (classify_gpu(a)) {   /* the memory rule */
        case GpuClass::Low: tier = GpuTier::T1; break;
        case GpuClass::Mid: tier = GpuTier::T2; break;
        case GpuClass::High: tier = GpuTier::T3; break;
        default: return GpuTier::Unknown;
        }
    }
    if (a.vram && !a.vram_at_least && a.vram < 7 * GIB / 2 && tier > GpuTier::T1) tier = GpuTier::T1;
    if (a.bc == 0) tier = GpuTier::T0;   /* every texture would be decoded to RGBA8 first */
    return tier;
}

Preset preset_for_tier(GpuTier tier) {
    switch (tier) {
    case GpuTier::T0: return Preset::Laptop;
    case GpuTier::T2: return Preset::Gaming;
    /* Ultra hook: when an Ultra preset exists, T3 maps to it; until then T3 gets Quality. */
    case GpuTier::T3: return Preset::Gaming;
    default: return Preset::Balanced;   /* T1, and unknown: safe on most cards */
    }
}

static const char *const TIER_KEYS[] = {"unknown", "t0", "t1", "t2", "t3"};

const char *gpu_tier_key(GpuTier tier) { return TIER_KEYS[(int)tier]; }

bool gpu_tier_from_key(const std::string &key, GpuTier &out) {
    for (int i = 0; i < 5; i++)
        if (key == TIER_KEYS[i]) return out = (GpuTier)i, true;
    return false;
}

std::string decode_driver_version(uint32_t vendor, uint32_t v, bool windows) {
    if (!v) return "";
    char buf[32];
    if (vendor == VENDOR_NVIDIA) snprintf(buf, sizeof buf, "%u.%02u", v >> 22, (v >> 14) & 0xFF);
    else if (vendor == VENDOR_INTEL && windows) snprintf(buf, sizeof buf, "%u.%u", v >> 14, v & 0x3FFF);
    else snprintf(buf, sizeof buf, "%u.%u.%u", v >> 22, (v >> 12) & 0x3FF, v & 0xFFF);
    return buf;
}

std::string gpu_identity(const GpuAdapter &a) {
    char buf[32];
    snprintf(buf, sizeof buf, "%04x:%04x:", a.vendor_id, a.device_id);
    return buf + a.driver;
}

bool refresh_gpu_detection(GpuDetection &stored, const GpuChoice &now, const GpuTierTable &table) {
    if (!now.found) return false;
    std::string id = gpu_identity(now.adapter);
    if (stored.id == id && stored.table == table.version) return false;
    const GpuAdapter &a = now.adapter;
    GpuDetection d;
    d.id = id;
    d.name = gpu_display_name(a.name);
    if (d.name.empty()) d.name = "GPU";
    d.driver = a.driver;
    GpuTier tier = gpu_tier(table, a);
    d.tier = gpu_tier_key(tier);
    d.recommended = preset_key(preset_for_tier(tier));
    d.vram_mb = (int)(a.vram / MIB);
    d.budget_mb = (int)(a.budget / MIB);
    d.bc = a.bc;
    d.table = table.version;
    stored = d;
    return true;
}

Preset gpu_detected_preset(const GpuDetection &d) {
    Preset p = d.id.empty() ? Preset::Balanced : preset_from_key(d.recommended);
    return p == Preset::Custom ? Preset::Balanced : p;
}

std::string gpu_display_name(const std::string &name) {
    std::string s = name;
    for (const char *mark : {"(R)", "(r)", "(TM)", "(tm)"})
        for (size_t at; (at = s.find(mark)) != std::string::npos;) s.erase(at, strlen(mark));
    /* a trailing driver code name: "(RADV NAVI23)", "(KBL GT2)", "(LLVM 15.0.7, 256 bits)" */
    size_t open = s.rfind(" (");
    if (!s.empty() && s.back() == ')' && open != std::string::npos && open > 0) s.erase(open);
    std::string out;
    for (char c : s)
        if (!(c == ' ' && (out.empty() || out.back() == ' '))) out += c;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string gpu_vram_text(const GpuAdapter &a) {
    if (a.vram == 0) return "";
    char buf[32];
    if (a.vram < GIB && !a.vram_at_least) snprintf(buf, sizeof buf, "%u MB", (unsigned)((a.vram + MIB / 2) / MIB));
    else snprintf(buf, sizeof buf, "%u%s GB", (unsigned)((a.vram + GIB / 2) / GIB), a.vram_at_least ? "+" : "");
    return buf;
}

static S preset_title(Preset preset) {
    switch (preset) {
    case Preset::Laptop: return S::preset_laptop;
    case Preset::Gaming: return S::preset_gaming;
    case Preset::Custom: return S::preset_custom;
    default: return S::preset_balanced;
    }
}

std::string gpu_reason(const GpuChoice &choice, Preset preset) {
    const char *title = tr(preset_title(preset));
    if (!choice.found) return trf(S::gpu_picked_unknown, title);
    std::string name = gpu_display_name(choice.adapter.name), vram = gpu_vram_text(choice.adapter);
    if (name.empty()) name = "GPU";
    if (choice.cls == GpuClass::Integrated) return trf(S::gpu_picked_integrated, title, name.c_str());
    if (vram.empty()) return trf(S::gpu_picked_no_memory, title, name.c_str());
    return trf(S::gpu_picked, title, name.c_str(), vram.c_str());
}

std::string gpu_detected_line(const GpuDetection &d) {
    const char *title = tr(preset_title(gpu_detected_preset(d)));
    if (d.id.empty()) return trf(S::gpu_detected_none, title);
    std::string card = d.name;
    GpuAdapter mem;
    mem.vram = (uint64_t)d.vram_mb * MIB;
    if (std::string v = gpu_vram_text(mem); !v.empty()) card += " (" + v + ")";
    return trf(S::gpu_detected, card.c_str(), title);
}

/* ---- Probes ---- */

/* Vulkan, the game's own API: the loader is opened at run time (vulkan-1.dll, libvulkan.so.1), so
 * the launcher links and starts without it. Just enough of vulkan_core.h: the structs keep their C
 * layout on each target (the compiler lays them out as it does the real header's). */
namespace vk {
#if defined(_WIN32) && !defined(_WIN64)
#define VKAPI __stdcall
#else
#define VKAPI
#endif
typedef struct Instance_T *Instance;
typedef struct PhysicalDevice_T *PhysicalDevice;
struct ApplicationInfo {
    int sType;
    const void *pNext;
    const char *pApplicationName;
    uint32_t applicationVersion;
    const char *pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
};
struct InstanceCreateInfo {
    int sType;
    const void *pNext;
    uint32_t flags;
    const ApplicationInfo *pApplicationInfo;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
};
struct PropertiesHead { /* VkPhysicalDeviceProperties up to deviceName; limits follow */
    uint32_t apiVersion, driverVersion, vendorID, deviceID;
    int deviceType;
    char deviceName[256];
};
struct MemoryHeap { uint64_t size; uint32_t flags; };
struct MemoryProperties {
    uint32_t memoryTypeCount;
    struct { uint32_t propertyFlags, heapIndex; } memoryTypes[32];
    uint32_t memoryHeapCount;
    MemoryHeap memoryHeaps[16];
};
struct MemoryProperties2 { int sType; void *pNext; MemoryProperties memoryProperties; };
struct MemoryBudgetProperties { int sType; void *pNext; uint64_t heapBudget[16], heapUsage[16]; };
struct ExtensionProperties { char extensionName[256]; uint32_t specVersion; };
enum {
    STRUCTURE_TYPE_APPLICATION_INFO = 0, STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
    STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 = 1000059006,
    STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT = 1000237000,
    MEMORY_HEAP_DEVICE_LOCAL_BIT = 1,
    FEATURE_TEXTURE_COMPRESSION_BC = 22,   /* VkPhysicalDeviceFeatures::textureCompressionBC, the 23rd VkBool32 */
};
enum { TYPE_OTHER, TYPE_INTEGRATED_GPU, TYPE_DISCRETE_GPU, TYPE_VIRTUAL_GPU, TYPE_CPU };
static const uint32_t API_1_0 = 1u << 22, API_1_1 = (1u << 22) | (1u << 12);
typedef void (VKAPI *Void)();
typedef Void (VKAPI *GetInstanceProcAddr)(Instance, const char *);
typedef int (VKAPI *EnumerateInstanceVersion)(uint32_t *);
typedef int (VKAPI *CreateInstance)(const InstanceCreateInfo *, const void *, Instance *);
typedef void (VKAPI *DestroyInstance)(Instance, const void *);
typedef int (VKAPI *EnumeratePhysicalDevices)(Instance, uint32_t *, PhysicalDevice *);
typedef void (VKAPI *GetProperties)(PhysicalDevice, void *);
typedef void (VKAPI *GetMemoryProperties)(PhysicalDevice, MemoryProperties *);
typedef void (VKAPI *GetMemoryProperties2)(PhysicalDevice, MemoryProperties2 *);
typedef void (VKAPI *GetFeatures)(PhysicalDevice, uint32_t *);
typedef int (VKAPI *EnumerateDeviceExtensions)(PhysicalDevice, const char *, uint32_t *, ExtensionProperties *);
} // namespace vk

#ifdef _WIN32
static void *open_vulkan() { return (void *)LoadLibraryW(L"vulkan-1.dll"); }
static vk::GetInstanceProcAddr vulkan_entry(void *lib) {
    return (vk::GetInstanceProcAddr)(void *)GetProcAddress((HMODULE)lib, "vkGetInstanceProcAddr");
}
static void close_vulkan(void *lib) { FreeLibrary((HMODULE)lib); }
static const bool WINDOWS = true;
#else
static void *open_vulkan() { return dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL); }
static vk::GetInstanceProcAddr vulkan_entry(void *lib) { return (vk::GetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr"); }
static void close_vulkan(void *lib) { dlclose(lib); }
static const bool WINDOWS = false;
#endif

static bool has_device_extension(vk::EnumerateDeviceExtensions enumerate, vk::PhysicalDevice dev, const char *name) {
    uint32_t n = 0;
    if (!enumerate || enumerate(dev, nullptr, &n, nullptr) < 0 || !n) return false;
    std::vector<vk::ExtensionProperties> ext(n);
    if (enumerate(dev, nullptr, &n, ext.data()) < 0) return false;
    for (uint32_t i = 0; i < n && i < ext.size(); i++)
        if (strncmp(ext[i].extensionName, name, sizeof ext[i].extensionName) == 0) return true;
    return false;
}

static void probe_vulkan(std::vector<GpuAdapter> &out) {
    void *lib = open_vulkan();
    if (!lib) return;
    vk::GetInstanceProcAddr gipa = vulkan_entry(lib);
    if (!gipa) {
        close_vulkan(lib);
        return;
    }
#define VK_GLOBAL(type, name) ((vk::type)gipa(nullptr, name))
#define VK_FN(type, name) ((vk::type)gipa(inst, name))
    vk::EnumerateInstanceVersion version = VK_GLOBAL(EnumerateInstanceVersion, "vkEnumerateInstanceVersion");
    vk::CreateInstance create = VK_GLOBAL(CreateInstance, "vkCreateInstance");
    uint32_t loader_api = vk::API_1_0;
    if (version && version(&loader_api) != 0) loader_api = vk::API_1_0;
    /* 1.1 when the loader has it, for vkGetPhysicalDeviceMemoryProperties2 (memory budget); a 1.0
     * loader rejects a 1.1 application. */
    uint32_t api = loader_api >= vk::API_1_1 ? vk::API_1_1 : vk::API_1_0;
    vk::Instance inst = nullptr;
    vk::ApplicationInfo app = {vk::STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "warriors-launcher", 1, nullptr, 0, api};
    vk::InstanceCreateInfo ci = {vk::STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, 0, nullptr};
    if (create && create(&ci, nullptr, &inst) == 0 && inst) {
        vk::DestroyInstance destroy = VK_FN(DestroyInstance, "vkDestroyInstance");
        vk::EnumeratePhysicalDevices enumerate = VK_FN(EnumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
        vk::GetProperties props = VK_FN(GetProperties, "vkGetPhysicalDeviceProperties");
        vk::GetMemoryProperties mem = VK_FN(GetMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
        vk::GetMemoryProperties2 mem2 = api >= vk::API_1_1 ? VK_FN(GetMemoryProperties2, "vkGetPhysicalDeviceMemoryProperties2") : nullptr;
        vk::GetFeatures features = VK_FN(GetFeatures, "vkGetPhysicalDeviceFeatures");
        vk::EnumerateDeviceExtensions extensions = VK_FN(EnumerateDeviceExtensions, "vkEnumerateDeviceExtensionProperties");
        vk::PhysicalDevice devs[16];
        uint32_t n = 16;
        if (destroy && enumerate && props && mem && enumerate(inst, &n, devs) >= 0) /* VK_SUCCESS or VK_INCOMPLETE */
            for (uint32_t i = 0; i < n && i < 16; i++) {
                /* VkPhysicalDeviceProperties is under 1 KB; the buffer leaves room for any ABI */
                alignas(8) static unsigned char buf[4096];
                memset(buf, 0, sizeof buf);
                props(devs[i], buf);
                const vk::PropertiesHead *h = (const vk::PropertiesHead *)buf;
                GpuAdapter a;
                a.name.assign(h->deviceName, strnlen(h->deviceName, sizeof h->deviceName));
                a.vendor_id = h->vendorID;
                a.device_id = h->deviceID;
                a.driver = decode_driver_version(h->vendorID, h->driverVersion, WINDOWS);
                a.kind = h->deviceType == vk::TYPE_INTEGRATED_GPU ? GpuKind::Integrated
                         : h->deviceType == vk::TYPE_DISCRETE_GPU ? GpuKind::Discrete
                         : h->deviceType == vk::TYPE_CPU          ? GpuKind::Software
                                                                  : GpuKind::Unknown;
                /* The heaps, with their budget (VK_EXT_memory_budget) when the device has it. */
                vk::MemoryBudgetProperties budget;
                memset(&budget, 0, sizeof budget);
                budget.sType = vk::STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
                vk::MemoryProperties2 m2;
                memset(&m2, 0, sizeof m2);
                m2.sType = vk::STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
                bool with_budget = mem2 && h->apiVersion >= vk::API_1_1 && has_device_extension(extensions, devs[i], "VK_EXT_memory_budget");
                if (with_budget) {
                    m2.pNext = &budget;
                    mem2(devs[i], &m2);
                } else mem(devs[i], &m2.memoryProperties);
                const vk::MemoryProperties &m = m2.memoryProperties;
                for (uint32_t j = 0; j < m.memoryHeapCount && j < 16; j++) /* the largest device-local heap */
                    if ((m.memoryHeaps[j].flags & vk::MEMORY_HEAP_DEVICE_LOCAL_BIT) && m.memoryHeaps[j].size > a.vram) {
                        a.vram = m.memoryHeaps[j].size;
                        a.budget = with_budget && budget.heapBudget[j] ? budget.heapBudget[j] : a.vram;
                    }
                if (features) {
                    uint32_t f[128]; /* VkPhysicalDeviceFeatures: 55 VkBool32 */
                    memset(f, 0, sizeof f);
                    features(devs[i], f);
                    a.bc = f[vk::FEATURE_TEXTURE_COMPRESSION_BC] ? 1 : 0;
                }
                out.push_back(a);
            }
        if (destroy) destroy(inst, nullptr);
    }
#undef VK_GLOBAL
#undef VK_FN
    close_vulkan(lib);
}

#ifdef _WIN32

/* DXGI 1.1 (Windows 7 and later), loaded at run time so the launcher links as before. */
static const GUID IID_FACTORY1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
static const GUID IID_ADAPTER3 = {0x645967a4, 0x1392, 0x4310, {0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd}};

static std::string utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

static void probe_dxgi(std::vector<GpuAdapter> &out) {
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) return;
    typedef HRESULT(WINAPI * CreateFactory1)(REFIID, void **);
    CreateFactory1 create = (CreateFactory1)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1");
    IDXGIFactory1 *factory = nullptr;
    if (create && SUCCEEDED(create(IID_FACTORY1, (void **)&factory)) && factory) {
        IDXGIAdapter1 *adapter = nullptr;
        for (UINT i = 0; i < 16 && factory->EnumAdapters1(i, &adapter) == S_OK; i++) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(adapter->GetDesc1(&d))) {
                GpuAdapter a;
                a.name = utf8(d.Description);
                a.vendor_id = d.VendorId;
                a.device_id = d.DeviceId;
                a.vram = d.DedicatedVideoMemory;
                if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) a.kind = GpuKind::Software;
                /* A 32-bit process reads DedicatedVideoMemory as a SIZE_T that tops out just under
                 * 4 GB: that card has 4 GB or more. */
                if (sizeof(SIZE_T) == 4 && d.DedicatedVideoMemory >= 0xFFF00000u) a.vram = 4 * GIB, a.vram_at_least = true;
                /* Windows 10's 64-bit local memory budget, a little under the card's memory: it
                 * only raises the reading of a card already taken as discrete (an integrated GPU's
                 * local budget is system memory). */
                IDXGIAdapter3 *a3 = nullptr;
                if (sizeof(SIZE_T) == 4 && gpu_kind(a) == GpuKind::Discrete &&
                    SUCCEEDED(adapter->QueryInterface(IID_ADAPTER3, (void **)&a3)) && a3) {
                    DXGI_QUERY_VIDEO_MEMORY_INFO info;
                    if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) && info.Budget > a.vram)
                        a.vram = info.Budget, a.vram_at_least = true;
                    a3->Release();
                }
                out.push_back(a);
            }
            adapter->Release();
            adapter = nullptr;
        }
        factory->Release();
    }
    FreeLibrary(dxgi);
}

std::vector<GpuAdapter> probe_gpus() {
    std::vector<GpuAdapter> out;
    probe_vulkan(out);
    if (!choose_gpu(out).found) { /* no 32-bit Vulkan driver: what DXGI says */
        out.clear();
        probe_dxgi(out);
    }
    return out;
}

#else

/* No Vulkan loader (or no 32-bit driver): the PCI vendor of each DRM card, and the memory amdgpu
 * reports. No name and no kind, so the classifier guesses (the tier table goes by device ID). */
static void probe_drm(std::vector<GpuAdapter> &out) {
    std::vector<DirEntry> cards;
    if (!list_dir("/sys/class/drm", cards)) return;
    for (const DirEntry &e : cards) {
        if (e.name.compare(0, 4, "card") != 0 || e.name.find('-') != std::string::npos) continue;
        std::string dev = "/sys/class/drm/" + e.name + "/device/", text;
        if (!read_file(dev + "vendor", text, 64)) continue;
        GpuAdapter a;
        a.vendor_id = (uint32_t)strtoul(text.c_str(), nullptr, 16);
        a.name = a.vendor_id == VENDOR_NVIDIA ? "NVIDIA" : a.vendor_id == VENDOR_AMD ? "AMD" : a.vendor_id == VENDOR_INTEL ? "Intel" : "";
        if (read_file(dev + "device", text, 64)) a.device_id = (uint32_t)strtoul(text.c_str(), nullptr, 16);
        if (read_file(dev + "mem_info_vram_total", text, 64)) a.vram = strtoull(text.c_str(), nullptr, 10);
        out.push_back(a);
    }
}

std::vector<GpuAdapter> probe_gpus() {
    std::vector<GpuAdapter> out;
    probe_vulkan(out);
    if (!choose_gpu(out).found) {
        out.clear();
        probe_drm(out);
    }
    return out;
}

#endif

} // namespace launcher
