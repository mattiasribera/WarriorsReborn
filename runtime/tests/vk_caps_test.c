/* Game-independent test of the startup line on the Vulkan device's support (src/renderer/vk_caps.h):
 * the versions, the device type names, the feature list in its order, "none", "unknown" and
 * truncation. */
#include "../src/renderer/vk_caps.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

#define VER(a, b, c) ((uint32_t)(a) << 22 | (uint32_t)(b) << 12 | (uint32_t)(c))

int main(void) {
    char line[512];

    VkCapsReport r = {VER(1, 4, 315), VER(1, 4, 315), 1, 1, VK_CAP_TIMELINE_SEMAPHORE | VK_CAP_MEMORY_BUDGET | VK_CAP_HOST_IMAGE_COPY};
    vk_caps_format(line, sizeof line, &r);
    CHECK(!strcmp(line, "Vulkan 1.4.315 (loader 1.4.315), device type integrated, features: timeline semaphores, host image copy, memory budget"),
          "versions, integrated, features in their order: \"%s\"", line);

    uint32_t all = (1u << VK_CAP_COUNT) - 1;
    r = (VkCapsReport){VER(1, 3, 289), VER(1, 3, 290), 2, 1, all};
    vk_caps_format(line, sizeof line, &r);
    CHECK(!strcmp(line, "Vulkan 1.3.289 (loader 1.3.290), device type discrete, features: timeline semaphores, synchronization2, "
                        "dynamic rendering, graphics pipeline library, extended dynamic state 2, extended dynamic state 3, "
                        "fragment shading rate, shader float16, subgroup size control, host image copy, present wait, "
                        "memory budget, ray query, acceleration structure"),
          "every feature: \"%s\"", line);

    /* A Vulkan 1.0 device behind a 1.0 loader (no vkEnumerateInstanceVersion), nothing offered. */
    r = (VkCapsReport){VER(1, 0, 61), 0, 4, 1, 0};
    vk_caps_format(line, sizeof line, &r);
    CHECK(!strcmp(line, "Vulkan 1.0.61 (loader unknown), device type cpu, features: none"), "1.0, loader unknown, none: \"%s\"", line);

    r.features_known = 0;
    r.features = all;   /* not listed when the extensions could not be enumerated */
    vk_caps_format(line, sizeof line, &r);
    CHECK(!strcmp(line, "Vulkan 1.0.61 (loader unknown), device type cpu, features: unknown"), "features unknown: \"%s\"", line);

    const int types[] = {0, 1, 2, 3, 4, 5, -1};
    const char *names[] = {"other", "integrated", "discrete", "virtual", "cpu", "other", "other"};
    for (int i = 0; i < 7; i++) {
        r = (VkCapsReport){VER(1, 2, 0), VER(1, 2, 0), types[i], 1, 0};
        vk_caps_format(line, sizeof line, &r);
        char want[128];
        snprintf(want, sizeof want, "Vulkan 1.2.0 (loader 1.2.0), device type %s, features: none", names[i]);
        CHECK(!strcmp(line, want), "device type %d is %s", types[i], names[i]);
    }

    /* Truncation keeps the buffer terminated and a prefix of the full line. */
    r = (VkCapsReport){VER(1, 3, 289), VER(1, 3, 290), 2, 1, all};
    char full[512], small[40];
    vk_caps_format(full, sizeof full, &r);
    memset(small, 'x', sizeof small);
    vk_caps_format(small, sizeof small, &r);
    CHECK(strlen(small) == sizeof small - 1 && !strncmp(small, full, sizeof small - 1), "truncated to the buffer: \"%s\"", small);

    printf(failures ? "%d FAILED\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
