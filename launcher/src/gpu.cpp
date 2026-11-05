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

GpuChoice choose_gpu(const std::vector<GpuAdapter> &adapters) {
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
    if (c.found) c.cls = classify_gpu(c.adapter);
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

/* ---- Probes ---- */

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

std::vector<GpuAdapter> probe_gpus() {
    std::vector<GpuAdapter> out;
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) return out;
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
    return out;
}

#else

/* Just enough of vulkan_core.h (1.0) to list the devices: the structs keep their C layout on
 * this target. */
namespace vk {
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
struct MemoryProperties {
    uint32_t memoryTypeCount;
    struct { uint32_t propertyFlags, heapIndex; } memoryTypes[32];
    uint32_t memoryHeapCount;
    struct { uint64_t size; uint32_t flags; } memoryHeaps[16];
};
enum { STRUCTURE_TYPE_APPLICATION_INFO = 0, STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1, MEMORY_HEAP_DEVICE_LOCAL_BIT = 1 };
enum { TYPE_OTHER, TYPE_INTEGRATED_GPU, TYPE_DISCRETE_GPU, TYPE_VIRTUAL_GPU, TYPE_CPU };
typedef int (*CreateInstance)(const InstanceCreateInfo *, const void *, Instance *);
typedef void (*DestroyInstance)(Instance, const void *);
typedef int (*EnumeratePhysicalDevices)(Instance, uint32_t *, PhysicalDevice *);
typedef void (*GetProperties)(PhysicalDevice, void *);
typedef void (*GetMemoryProperties)(PhysicalDevice, MemoryProperties *);
} // namespace vk

static void probe_vulkan(std::vector<GpuAdapter> &out) {
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return;
    vk::CreateInstance create = (vk::CreateInstance)dlsym(lib, "vkCreateInstance");
    vk::DestroyInstance destroy = (vk::DestroyInstance)dlsym(lib, "vkDestroyInstance");
    vk::EnumeratePhysicalDevices enumerate = (vk::EnumeratePhysicalDevices)dlsym(lib, "vkEnumeratePhysicalDevices");
    vk::GetProperties props = (vk::GetProperties)dlsym(lib, "vkGetPhysicalDeviceProperties");
    vk::GetMemoryProperties mem = (vk::GetMemoryProperties)dlsym(lib, "vkGetPhysicalDeviceMemoryProperties");
    vk::Instance inst = nullptr;
    vk::ApplicationInfo app = {vk::STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "warriors-launcher", 1, nullptr, 0, 1u << 22};
    vk::InstanceCreateInfo ci = {vk::STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, 0, nullptr};
    if (create && destroy && enumerate && props && mem && create(&ci, nullptr, &inst) == 0 && inst) {
        vk::PhysicalDevice devs[16];
        uint32_t n = 16;
        int r = enumerate(inst, &n, devs);
        if (r >= 0) /* VK_SUCCESS or VK_INCOMPLETE */
            for (uint32_t i = 0; i < n && i < 16; i++) {
                /* VkPhysicalDeviceProperties is under 1 KB; the buffer leaves room for any ABI */
                alignas(8) static unsigned char buf[4096];
                memset(buf, 0, sizeof buf);
                props(devs[i], buf);
                const vk::PropertiesHead *h = (const vk::PropertiesHead *)buf;
                vk::MemoryProperties m;
                memset(&m, 0, sizeof m);
                mem(devs[i], &m);
                GpuAdapter a;
                a.name.assign(h->deviceName, strnlen(h->deviceName, sizeof h->deviceName));
                a.vendor_id = h->vendorID;
                a.kind = h->deviceType == vk::TYPE_INTEGRATED_GPU ? GpuKind::Integrated
                         : h->deviceType == vk::TYPE_DISCRETE_GPU ? GpuKind::Discrete
                         : h->deviceType == vk::TYPE_CPU          ? GpuKind::Software
                                                                  : GpuKind::Unknown;
                for (uint32_t j = 0; j < m.memoryHeapCount && j < 16; j++) /* the largest device-local heap */
                    if ((m.memoryHeaps[j].flags & vk::MEMORY_HEAP_DEVICE_LOCAL_BIT) && m.memoryHeaps[j].size > a.vram)
                        a.vram = m.memoryHeaps[j].size;
                out.push_back(a);
            }
        destroy(inst, nullptr);
    }
    dlclose(lib);
}

/* No Vulkan loader (or no 32-bit driver): the PCI vendor of each DRM card, and the memory amdgpu
 * reports. No name and no kind, so the classifier guesses. */
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
