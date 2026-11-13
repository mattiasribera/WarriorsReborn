/* Game-independent test of the launcher's "no 3D renderer" warnings: the Home page warning picked
 * from the Vulkan probe and the profile's renderer (renderer_warning, gpu.h), its strings in both
 * languages, the Vulkan flag taken from the GPU job only once it is done, and the bug report's Renderer and Vulkan error lines (report.h). No game data. */
#include "../src/async.h"
#include "../src/gpu.h"
#include "../src/i18n.h"
#include "../src/report.h"
#include <cstdio>
#include <atomic>
#include <cstring>

using namespace launcher;

static int failures;
#define CHECK(cond, ...) do { if (cond) printf("ok   "); else { printf("FAIL "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static bool has(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

static void test_warning_rule() {
    CHECK(renderer_warning(0, "vulkan", false) == RendererWarning::NoVulkanDriver, "warning: no 32-bit Vulkan driver");
    CHECK(renderer_warning(0, "vulkan", true) == RendererWarning::NoVulkanDriver, "warning: no driver, also in advanced mode");
    CHECK(renderer_warning(0, "none", false) == RendererWarning::NoVulkanDriver, "warning: the missing driver comes first");
    CHECK(renderer_warning(1, "vulkan", false) == RendererWarning::None, "warning: none with a driver and Vulkan");
    CHECK(renderer_warning(-1, "vulkan", false) == RendererWarning::None, "warning: none while the probe has not run");
    CHECK(renderer_warning(1, "none", false) == RendererWarning::RendererNone, "warning: renderer None outside advanced mode");
    CHECK(renderer_warning(-1, "none", false) == RendererWarning::RendererNone, "warning: renderer None before the probe");
    CHECK(renderer_warning(1, "none", true) == RendererWarning::None, "warning: renderer None is a developer choice in advanced mode");
}

static void test_warning_strings() {
    for (Lang l : {Lang::En, Lang::Es}) {
        const char *a = tr(renderer_warning_text(RendererWarning::NoVulkanDriver), l);
        const char *b = tr(renderer_warning_text(RendererWarning::RendererNone), l);
        CHECK(a && *a && b && *b && strcmp(a, b), "warning: two different texts in %s", lang_key(l));
        CHECK(!has(a, "GitHub") && !has(b, "GitHub"), "warning: no GitHub in %s", lang_key(l));
    }
    CHECK(has(tr(S::warn_no_vulkan, Lang::Es), "NVIDIA, AMD o Intel") && has(tr(S::warn_no_vulkan, Lang::Es), "negro"),
          "warning: the Spanish text names the vendors and the black screen");
}

/* Home takes the flag from the GPU job as gui.cpp does (app_gpu_ready: ready() never blocks), so
 * no warning shows while the worker probes and the missing driver shows once it is done. */
static void test_warning_after_job() {
    std::atomic<bool> release{false};
    Async<GpuProbe> job;
    job.start([&release](const std::atomic<bool> &cancel) {
        while (!release && !cancel) {}
        GpuProbe probe;
        probe.vulkan = false; /* no 32-bit Vulkan driver */
        return probe;
    });
    int driver = -1;
    auto home = [&]() {
        if (driver < 0 && job.ready()) driver = vulkan_driver_after_probe(driver, job.get());
        return renderer_warning(driver, "vulkan", false);
    };
    CHECK(home() == RendererWarning::None && driver == -1, "job: no warning while the probe runs");
    release = true;
    job.wait();
    CHECK(home() == RendererWarning::NoVulkanDriver && driver == 0, "job: the warning once the probe found no driver");

    GpuProbe found;
    found.vulkan = true;
    CHECK(vulkan_driver_after_probe(-1, found) == 1, "job: a driver found clears the flag");
    CHECK(vulkan_driver_after_probe(1, GpuProbe()) == 1, "job: a capture's pinned driver is kept");
    CHECK(vulkan_driver_after_probe(0, found) == 0, "job: a capture's pinned no-driver is kept");
}

static ReportInfo base_info() {
    ReportInfo info;
    info.launcher_version = "v1";
    info.os = "Windows 10";
    info.command = "TheWarriors.exe";
    info.head.push_back("[start] xbrun v2");
    return info;
}

static void test_report_renderer() {
    std::vector<ScrubRule> none;
    ReportInfo ok = base_info();
    ok.head.push_back("[renderer] Vulkan on NVIDIA GeForce GTX 1050 (driver 566.36), 1920x1080 (back buffer 640x480), 1 samples");
    ok.head.push_back("[video] present: Vulkan mailbox (no readback), 2 frames in flight");
    std::string r = bug_report(ok, none);
    CHECK(has(r, "\nRenderer: Vulkan\n"), "report: Renderer: Vulkan");
    CHECK(!has(r, "Vulkan error:"), "report: no Vulkan error line without one in the log");

    ReportInfo rb = ok;
    rb.head.back() = "[video] present: readback through the sdl backend";
    CHECK(has(bug_report(rb, none), "\nRenderer: Vulkan (readback)\n"), "report: Renderer: Vulkan (readback)");
    ReportInfo lost = ok;
    lost.log.push_back("[video] present: readback through the sdl backend (swapchain lost: x)");
    CHECK(has(bug_report(lost, none), "\nRenderer: Vulkan (readback)\n"), "report: readback after a lost swapchain");

    ReportInfo cpu = base_info();
    cpu.head.push_back("[renderer] Vulkan unavailable (vkCreateInstance failed: -9); using the CPU path only");
    cpu.head.push_back("[renderer] ERROR: 3D frames will be black: vkCreateInstance failed: -9");
    cpu.log.push_back("[vk-error] vkQueueSubmit: VK_ERROR_DEVICE_LOST (frame 3)");
    cpu.log.push_back("[vk-error] vkQueueSubmit: VK_ERROR_DEVICE_LOST (frame 4)");
    r = bug_report(cpu, none);
    CHECK(has(r, "\nRenderer: CPU only (Vulkan failed to start: vkCreateInstance failed: -9)\n"), "report: Renderer: CPU only, with the reason");
    CHECK(has(r, "\nVulkan error: [vk-error] vkQueueSubmit: VK_ERROR_DEVICE_LOST (frame 3)\n"), "report: the first [vk-error] line");

    ReportInfo off = base_info();
    off.head.push_back("[renderer] none (CPU path only)");
    CHECK(has(bug_report(off, none), "\nRenderer: CPU only (renderer set to none)\n"), "report: Renderer: CPU only (set to none)");

    ReportInfo empty = base_info();
    CHECK(has(bug_report(empty, none), "\nRenderer: unknown (not in the log)\n"), "report: Renderer unknown without a log line");
    empty.renderer = "none";
    CHECK(has(bug_report(empty, none), "\nRenderer: CPU only (renderer set to none)\n"), "report: the profile's None without a log line");
    ReportInfo nodrv = base_info();
    nodrv.vulkan_driver = 0;
    CHECK(has(bug_report(nodrv, none), "\nRenderer: unknown (not in the log); the launcher found no 32-bit Vulkan driver\n"),
          "report: the launcher's probe is added");
    cpu.vulkan_driver = 0;
    CHECK(has(bug_report(cpu, none), "Renderer: CPU only (Vulkan failed to start: vkCreateInstance failed: -9); the launcher found no 32-bit Vulkan driver\n"),
          "report: the probe is added to the log's answer");

    /* the lines before stay where they were */
    r = bug_report(ok, none);
    CHECK(r.find("GPU: ") < r.find("Renderer: ") && r.find("Renderer: ") < r.find("Command: "), "report: Renderer between GPU and Command");
}

int main() {
    test_warning_rule();
    test_warning_strings();
    test_warning_after_job();
    test_report_renderer();
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("ALL OK\n");
    return 0;
}
