/* The startup line on what the Vulkan device supports (stage 0 of the Vulkan audit): printed
 * right after "[renderer] Vulkan on <device> ..." so every xbrun.log shows the device's Vulkan
 * version, the loader's, the device type and which newer features the device offers, before the
 * minimum Vulkan version is raised.
 *
 *   [renderer] Vulkan 1.3.289 (loader 1.4.315), device type discrete, features: timeline semaphores, ...
 *
 * Support, not enablement: nothing listed here is enabled by it. The device's apiVersion comes
 * from vkGetPhysicalDeviceProperties, the loader's from vkEnumerateInstanceVersion ("unknown"
 * without it: a Vulkan 1.0 loader), a feature from the device extension (or the core version
 * that promoted it) and, where vkGetPhysicalDeviceFeatures2KHR is available, its feature bit.
 * Without device extension enumeration the list is "unknown"; with nothing offered, "none".
 *
 * Kept out of renderer.h on purpose: that header is the renderer's wire API (the render64 recorder
 * and its generator read it). The formatter is header-only so tests/vk_caps_test.c checks it
 * without a GPU. */
#ifndef RENDERER_VK_CAPS_H
#define RENDERER_VK_CAPS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Feature bits of VkCapsReport.features, in the order the line lists them. */
enum {
    VK_CAP_TIMELINE_SEMAPHORE = 1u << 0,
    VK_CAP_SYNCHRONIZATION2 = 1u << 1,
    VK_CAP_DYNAMIC_RENDERING = 1u << 2,
    VK_CAP_GRAPHICS_PIPELINE_LIBRARY = 1u << 3,
    VK_CAP_EXTENDED_DYNAMIC_STATE2 = 1u << 4,
    VK_CAP_EXTENDED_DYNAMIC_STATE3 = 1u << 5,
    VK_CAP_FRAGMENT_SHADING_RATE = 1u << 6,
    VK_CAP_SHADER_FLOAT16 = 1u << 7,
    VK_CAP_SUBGROUP_SIZE_CONTROL = 1u << 8,
    VK_CAP_HOST_IMAGE_COPY = 1u << 9,
    VK_CAP_PRESENT_WAIT = 1u << 10,
    VK_CAP_MEMORY_BUDGET = 1u << 11,
    VK_CAP_RAY_QUERY = 1u << 12,
    VK_CAP_ACCELERATION_STRUCTURE = 1u << 13,
    VK_CAP_COUNT = 14
};

typedef struct VkCapsReport {
    uint32_t api;        /* the device's apiVersion (VK_MAKE_API_VERSION encoding) */
    uint32_t loader;     /* vkEnumerateInstanceVersion; 0: unknown */
    int type;            /* VkPhysicalDeviceType */
    int features_known;  /* 0: device extensions could not be enumerated ("unknown") */
    uint32_t features;   /* VK_CAP_* */
} VkCapsReport;

static inline const char *vk_caps_feature_name(int bit) {
    static const char *const names[VK_CAP_COUNT] = {
        "timeline semaphores", "synchronization2", "dynamic rendering", "graphics pipeline library",
        "extended dynamic state 2", "extended dynamic state 3", "fragment shading rate", "shader float16",
        "subgroup size control", "host image copy", "present wait", "memory budget", "ray query",
        "acceleration structure"};
    return bit >= 0 && bit < VK_CAP_COUNT ? names[bit] : "?";
}

static inline const char *vk_caps_type_name(int type) {
    switch (type) {   /* VkPhysicalDeviceType */
    case 1: return "integrated";
    case 2: return "discrete";
    case 3: return "virtual";
    case 4: return "cpu";
    default: return "other";
    }
}

/* "Vulkan <api> (loader <version|unknown>), device type <type>, features: <list|none|unknown>"
 * into out (truncated to cap, always terminated). Returns out. */
static inline char *vk_caps_format(char *out, size_t cap, const VkCapsReport *r) {
    if (!cap) return out;
    char loader[32] = "unknown";
    if (r->loader)
        snprintf(loader, sizeof loader, "%u.%u.%u", (unsigned)(r->loader >> 22 & 0x7f), (unsigned)(r->loader >> 12 & 0x3ff),
                 (unsigned)(r->loader & 0xfff));
    int n = snprintf(out, cap, "Vulkan %u.%u.%u (loader %s), device type %s, features: ", (unsigned)(r->api >> 22 & 0x7f),
                     (unsigned)(r->api >> 12 & 0x3ff), (unsigned)(r->api & 0xfff), loader, vk_caps_type_name(r->type));
    size_t len = n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
    int listed = 0;
    for (int b = 0; b < VK_CAP_COUNT && r->features_known; b++) {
        if (!(r->features >> b & 1)) continue;
        n = snprintf(out + len, cap - len, "%s%s", listed++ ? ", " : "", vk_caps_feature_name(b));
        len = n < 0 ? len : len + (size_t)n < cap ? len + (size_t)n : cap - 1;
    }
    if (!listed) snprintf(out + len, cap - len, "%s", r->features_known ? "none" : "unknown");
    return out;
}

/* After renderer_init: the line above for the device in use (without the "[renderer] " prefix);
 * "" before. */
const char *renderer_vk_caps(void);

#ifdef __cplusplus
}
#endif

#endif
