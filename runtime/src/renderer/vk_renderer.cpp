// Vulkan offscreen renderer behind src/renderer.h.
//
// The loader (libvulkan.so.1, vulkan-1.dll on Windows) is opened at run time (platform.h) and every entry point is resolved through
// vkGetInstanceProcAddr / vkGetDeviceProcAddr (VK_NO_PROTOTYPES), so the runtime starts without
// Vulkan installed and falls back to the CPU path. No surface or swapchain: the frame renders
// into a device-local B8G8R8A8 color image plus a depth image, and renderer_end copies the color
// image into a host-visible buffer that is read back into the caller's XRGB8888 frame.
//
// Output scaling and MSAA (RendererConfig): the target can be larger than the back buffer the
// caller records in; viewports and scissors are scaled, and the base image is then uploaded into a
// back-buffer-sized image and drawn as the frame's first operation, a full-screen quad sampled
// with linear filtering (clamped). With MSAA the pass renders into multisampled color and depth
// images and resolves into the single-sample color image that is read back. At the back-buffer
// size without MSAA the base image is copied straight into the target, as before.
//
// Supersampling (RendererConfig.supersample): s.ow x s.oh is the internal render size, N x the
// output size s.fw x s.fh. After the main pass the color image is box-filtered down by successive
// 2:1 linear blits into the downsample images (each blit samples exactly between four source
// texels: a 2x2 average); the last one is what the readback and the swapchain blit read.
//
// FSR 1 (RendererConfig.fsr_w / fsr_h): s.fw x s.fh (= s.ow x s.oh, no supersampling) is then the
// internal render size, below the displayed size s.dispw x s.disph. After the main pass the frame
// is upscaled to the displayed size by AMD FidelityFX EASU and sharpened by RCAS (two full-target
// passes, shaders/fsr_easu.frag and fsr_rcas.frag); the gamma ramp, the frame counter, the readback
// and the swapchain blit work on that displayed-size image. Every scene pass (glass, bloom, ssao,
// lamp lights, ...) runs at the internal size, as at a lower --resolution. Without FSR the
// displayed size is the output size.
//
// Direct present (RendererConfig.create_surface): the window's surface is created right after the
// instance (with the window system's extensions), the device is picked with a queue family that
// can present to it and VK_KHR_swapchain, and renderer_present blits the finished target into the
// acquired swapchain image (aspect-fitted, black borders) instead of reading it back. Any failure
// on the way leaves the renderer offscreen; the caller logs renderer_present_info().
//
// A frame is recorded as a list of operations (clears, draws) and executed in one command buffer
// at renderer_end / renderer_present. renderer_end waits for it with a fence (the readback).
// renderer_present does not with more than one frame in flight (RendererConfig.frames_in_flight):
// each frame slot has its own command buffer, fence, acquire semaphore, descriptor pool and
// geometry, program and base-image buffers, and renderer_begin waits for the slot's previous
// frame before reusing them, so recording frame N overlaps the GPU executing frame N-1. The
// targets, surfaces and glass images are shared: the render passes' external dependencies and
// the barriers order each frame after the previous one's use of them (one queue).
//
// Textures: one sampled image (with its mip levels) per cache entry, keyed by the caller. Uploads
// run synchronously in renderer_texture_create through a one-time command buffer and fence. BC1-3 and BC7 blocks are uploaded as is when the device samples them; otherwise the caller
// decodes DXT on the CPU. When the cache exceeds its limits it is dropped as a whole at the start
// of a frame.
//
// Pixel shading: one fragment shader evaluates the NV2A register combiners (RPixelShader, see
// shaders/geom.frag). A program's structure (its input/output words, final combiner inputs and
// texture modes: Shape) is compiled in through specialization constants; its constants (stage and
// final constants, fog color) are stored per frame in a storage buffer and selected per draw by a
// push constant. Each draw's descriptor set (four stage textures plus that buffer) is allocated
// per frame from the frame slot's pool (reset when the slot is reused), cached by the texture combination.
//
// Output state (blending, color mask, depth write/function, culling; RState in NV2A values) and the
// program's Shape select a graphics pipeline variant, created on first use and cached until
// shutdown. The blend constant color is dynamic state.
//
// Offscreen surfaces (renderer_surface): the title's own render targets and depth surfaces, as
// persistent images (B8G8R8A8 color, D16 or a 24-bit-capable depth format) kept in their sampled
// layout between passes. Operations recorded with renderer_set_target run before the main pass in
// one-sample passes (one render pass object per depth class, attachments loaded and stored,
// layout transitions by barriers; a missing color or depth attachment is a per-size dummy image),
// then the main pass samples them. A stage bound to a depth surface evaluates the NV2A shadow
// compare (RPixelShader.shadow_func) with 2x2 percentage-closer filtering in the combiner shader.
#define VK_NO_PROTOTYPES
#include "../renderer.h"
#include "../ssao.h"
#include "../lamp_lights.h"
#include "../volfog.h"
#include "vertex_sink.h"
#include "pipe_cache.h"
extern "C" {
#include "../fps_overlay.h"
#include "../gamma_ramp.h"
}
#include "../fsr.h"
#include "../taa.h"
#include <vulkan/vulkan.h>
#include "../platform.h"
#include "../diaglog.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
extern "C" __declspec(dllimport) void *__stdcall GetCurrentThread(void);
extern "C" __declspec(dllimport) int __stdcall SetThreadPriority(void *thread, int priority);
extern "C" __declspec(dllimport) unsigned long __stdcall GetActiveProcessorCount(unsigned short group);
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include "geom_frag.h"   // SPIR-V generated at build time by glslangValidator (--vn)
#include "geom_vert.h"
#include "glass_blur_frag.h"
#include "glass_comp_frag.h"
#include "glass_vert.h"
#include "cas_frag.h"
#include "fsr_easu_frag.h"
#include "fsr_rcas_frag.h"
#include "fps_overlay_frag.h"
#include "bloom_down_frag.h"
#include "bloom_up_frag.h"
#include "bloom_comp_frag.h"
#include "filmic_frag.h"
#include "filmic_ae_frag.h"
#include "exposure_frag.h"
#include "ae_meter_frag.h"
#include "ae_adapt_frag.h"
#include "dof_focus_frag.h"
#include "dof_focus_ms_frag.h"
#include "dof_down_frag.h"
#include "dof_down_ms_frag.h"
#include "dof_gather_frag.h"
#include "dof_comp_frag.h"
#include "dof_comp_ms_frag.h"
#include "dof_copy_frag.h"
#include "gamma_frag.h"
#include "rain_vert.h"
#include "rain_frag.h"
#include "sky_vert.h"
#include "sky_frag.h"
#include "skyline_frag.h"
#include "ssao_ao_frag.h"
#include "ssao_ao_ms_frag.h"
#include "ssao_blur_frag.h"
#include "ssao_comp_frag.h"
#include "ssao_comp_ms_frag.h"
#include "ssao_blur_mask_frag.h"
#include "ssao_comp_ms_mask_frag.h"
#include "gtao_prefilter_comp.h"
#include "gtao_prefilter_ms_comp.h"
#include "gtao_main_comp.h"
#include "gtao_denoise_comp.h"
#include "gtao_denoise_mask_comp.h"
#include "gtao_temporal_comp.h"
#include "wet_frag.h"
#include "wet_ms_frag.h"
#include "sssr_hiz_frag.h"
#include "sssr_hiz_ms_frag.h"
#include "sssr_hiz_down_frag.h"
#include "sssr_surf_vert.h"
#include "sssr_surf_frag.h"
#include "rain_cover_vert.h"
#include "world_shadow_cut_vert.h"
#include "world_shadow_cut_frag.h"
#include "world_shadow_gate_comp.h"
#include "rain_cover_frag.h"
#include "splash_cover_frag.h"
#include "splash_cover_ms_frag.h"
#include "wet_cover_frag.h"
#include "wet_cover_ms_frag.h"
#include "char_mask_vert.h"
#include "splash_frag.h"
#include "splash_ms_frag.h"
#include "lamp_frag.h"
#include "lamp_ms_frag.h"
#include "shafts_frag.h"
#include "shafts_ms_frag.h"
#include "volfog_polar_comp.h"
#include "volfog_inject_comp.h"
#include "volfog_integrate_comp.h"
#include "volfog_comp_frag.h"
#include "volfog_comp_ms_frag.h"
#include "contact_frag.h"
#include "contact_ms_frag.h"
#include "geom_soft_frag.h"
#include "geom_soft_ms_frag.h"
#include "geom_lit_vert.h"
#include "geom_lit_frag.h"
#include "geom_emit_frag.h"
#include "geom_lit_emit_frag.h"
#include "geom_lit_relief_frag.h"
#include "geom_lit_hair_frag.h"
#include "geom_lit_relief_hair_frag.h"
#include "geom_lit_skin_frag.h"
#include "geom_lit_relief_skin_frag.h"
#include "geom_lit_hair_skin_frag.h"
#include "geom_lit_relief_hair_skin_frag.h"
#include "geom_lit_mat_frag.h"
#include "geom_lit_relief_mat_frag.h"
#include "geom_lit_hair_mat_frag.h"
#include "geom_lit_relief_hair_mat_frag.h"
#include "vsh_skin_comp.h"
#include "skin_faces_comp.h"
#include "skin_normals_comp.h"
#include "taa_resolve_frag.h"
#include "taa_resolve_ms_frag.h"
#include "taa_out_frag.h"
#include "taa_up_frag.h"
#include "taa_up_ms_frag.h"
#include "taa_out_up_frag.h"
#include "taa_vel_vert.h"
#include "taa_vel_frag.h"
#include "fg_mv_comp.h"
#include "fg_mv_ms_comp.h"
#include "fg_splat_comp.h"
#include "fg_interp_frag.h"
#include "fg_flow_comp.h"
#include "fg_hud_comp.h"

#define VK_GLOBAL_FNS(X) X(vkCreateInstance)
#define VK_INSTANCE_FNS(X)                                                                         \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)            \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties)             \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceImageFormatProperties) X(vkGetPhysicalDeviceFeatures) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define VK_DEVICE_FNS(X)                                                                           \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateImage) X(vkDestroyImage)  \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory)    \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateRenderPass)      \
    X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateCommandPool)   \
    X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer)                    \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdUpdateBuffer) \
    X(vkCmdCopyImageToBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass)                        \
    X(vkCmdClearAttachments) X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence)                   \
    X(vkWaitForFences) X(vkResetFences) X(vkCreateShaderModule) X(vkDestroyShaderModule)            \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines)              \
    X(vkDestroyPipeline) X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer)    \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetBlendConstants) X(vkCmdDrawIndexed) X(vkCmdDraw) X(vkCmdCopyImage) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool)        \
    X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets)                 \
    X(vkUpdateDescriptorSets) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdBlitImage)          \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCreateSemaphore) X(vkDestroySemaphore)      \
    X(vkCreateComputePipelines) X(vkCmdDispatch) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) \
    X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) X(vkCmdCopyBuffer)                          \
    X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkGetPipelineCacheData)
// Window-system entry points: resolved when present, needed only for renderer_present.
#define VK_SURFACE_FNS(X)                                                                          \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)           \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)           \
    X(vkDestroySurfaceKHR) X(vkEnumerateDeviceExtensionProperties)
#define VK_SWAPCHAIN_FNS(X)                                                                        \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)

#define DECLARE(fn) static PFN_##fn fn;
DECLARE(vkGetInstanceProcAddr)
VK_GLOBAL_FNS(DECLARE)
VK_INSTANCE_FNS(DECLARE)
VK_DEVICE_FNS(DECLARE)
VK_SURFACE_FNS(DECLARE)
VK_SWAPCHAIN_FNS(DECLARE)

namespace {

// Device-local memory per category (renderer_vram, the --stats [vram] line): every allocation in a
// device-local heap is added to the category of the mem_tag in effect (MemTag), or, without one, to
// EFFECTS for attachments and OTHER for the rest; free_buffer / free_image subtract it again.
int mem_tag = -1;   // RendererVramCat, -1 = classify by usage
struct MemTag {
    int prev;
    explicit MemTag(int cat) : prev(mem_tag) { mem_tag = cat; }
    ~MemTag() { mem_tag = prev; }
};
uint64_t vram_now[RENDERER_VRAM_CATS], vram_peak[RENDERER_VRAM_CATS], vram_peak_total;

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void *map = nullptr;
    VkDeviceSize size = 0;
    VkDeviceSize vram = 0;   // device-local bytes counted in vram_now[cat]
    int cat = 0;
};


struct Image {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceSize vram = 0;   // as Buffer::vram
    int cat = 0;
};

enum OpKind { OP_CLEAR, OP_DRAW, OP_GLASS, OP_BLOOM, OP_RAIN, OP_SSAO, OP_SPLASH, OP_WET, OP_SOFT, OP_FILMIC, OP_SKY, OP_SKYLINE, OP_LAMP, OP_CONTACT, OP_SHAFT, OP_VOLFOG, OP_GRAB, OP_TAA, OP_FG, OP_FG_END };
struct Op {
    OpKind kind;
    uint32_t target[2];      // offscreen color and depth surface handles (both 0 = the frame target)
    uint32_t flags, color;   // clear
    bool mask;               // clear: alpha 0 (the bloom light mask, see record_frame)
    float z;
    RViewport vp;            // draw
    uint32_t draw_flags;
    uint32_t first_index, index_count;   // OP_RAIN / OP_SKY: first_index = index into Rain::list / Sky::list
    int32_t vertex_offset;
    uint32_t tex[RENDERER_STAGES];   // handles, 0 = white
    uint32_t push[4];        // alpha test function (D3DCMPFUNC, 0 = off), reference, program index, 0
    uint32_t pipe_key;       // pipeline variant (see pipeline_key)
    uint32_t shape;          // combiner program structure (index into State::shapes), | EMIT_SHAPE
    uint32_t blend_color;    // ARGB8888 constant color
    uint32_t emit_tex;       // EMIT_SHAPE: the emission mask (renderer_set_emission)
    float emit[4];           // EMIT_SHAPE: strength, bloom light mask weight, stage, 0 (push[3] stays the light mask)
    uint32_t relief_tex[2];  // renderer_set_relief: the normal and specular maps (RELIEF_SHAPE once lit)
    uint32_t hair_tex;       // renderer_set_hair: the hair mask (HAIR_SHAPE once lit as a character)
    uint32_t skin_tex;       // renderer_set_skin: the skin mask (SKIN_SHAPE once lit as a character)
    float relief[4];         // normal strength, specular strength, stage + 4 x flags, baked-light weight
    float eye_open;          // --eyes: renderer_set_eye_open (the material variants' push words 8..11: open, 0, 0, 0)
    RGlass glass;            // OP_GLASS
    RSsao ssao;              // OP_SSAO
    RSoft soft;              // OP_SOFT
    bool split_depth;        // draw: color and depth in two passes (see_through_split)
};

// Structure of a combiner program: the RPixelShader fields that select code, in the order of the
// fragment shader's specialization constants (constant_id 0-35, shaders/geom.frag). The constants
// (stage and final constants, fog color) are not part of it; they stay per-draw data.
const uint32_t SHAPE_WORDS = 37;   // the last: shadow compare word (see shadow_word)
struct Shape {
    uint32_t w[SHAPE_WORDS];
};

struct Texture {
    Image im;
    VkDeviceSize bytes = 0;
    bool see_through = false;   // may sample under half opaque (texture_see_through)
};
const uint32_t SEE_THROUGH_REF = 128;   // see_through_split: depth is written from this alpha up

// Offscreen surface (renderer_surface): handle = SURFACE_BIT | (index + 1).
const uint32_t SURFACE_BIT = 0x8000;
// Handles of the movie planes (renderer_movie_plane): above MAX_TEXTURES, without SURFACE_BIT.
const uint32_t MOVIE_TEXTURE = 0x7FF0;
const int MOVIE_PLANES = 3;
bool is_movie(uint32_t handle) { return handle >= MOVIE_TEXTURE && handle < MOVIE_TEXTURE + MOVIE_PLANES; }
// Handle of the previous frame (renderer_prev_frame): after the movie planes, without SURFACE_BIT.
const uint32_t PREV_TEXTURE = MOVIE_TEXTURE + MOVIE_PLANES;
const uint32_t MAX_SURFACES = 64;
struct Surface {
    Image im;
    uint32_t kind = RENDERER_SURFACE_COLOR;
    int gw = 0, gh = 0;   // guest size (viewports)
    int w = 0, h = 0;     // image size
    uint32_t twin = 0;    // a colour surface its own draws sample: the copy they read (self_sampling)
};
// Framebuffer of an offscreen pass: the color and depth surfaces, or per-size dummies for a
// missing one (kept in their attachment layout).
struct OffscreenFb {
    VkFramebuffer fb = VK_NULL_HANDLE;
    Image dummy_color, dummy_depth;
    uint32_t pass_class = 1;   // 1 = D16 depth, 2 = 24-bit depth
    int w = 0, h = 0, gw = 0, gh = 0;
};

// Texture upload batch: a level's first frame creates a few hundred
// textures. make_texture copies each into one persistent staging buffer and records its copy into
// ub.cmd; flush_uploads submits and waits for them once (before the next frame or one-time
// submission, before textures are dropped, at shutdown, or when the buffer is full), instead of a
// buffer allocation, a submit and a fence wait per texture. A texture larger than the buffer, or
// any texture after the buffer could not be allocated, takes the one-time path as before.
const VkDeviceSize UPLOAD_BATCH_BYTES = 32ull << 20;
struct UploadBatch {
    Buffer staging;   // allocated at the first batched upload
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDeviceSize used = 0;
    bool open = false, unavailable = false;
};
UploadBatch ub;

// Cache limits: dropped as a whole beyond these (descriptor pool size and device memory).
const uint32_t MAX_TEXTURES = 4096;
const VkDeviceSize DEFAULT_TEXTURE_BYTES = 512ull << 20;
VkDeviceSize texture_budget = DEFAULT_TEXTURE_BYTES;   // kept across renderer_shutdown
uint32_t texture_drops;   // texture cache drops in renderer_begin (renderer_texture_drop_count)
// A texture that could not be created (device memory, or address space for its upload buffer in
// this 32-bit process) drops the whole cache at the next frame, so the frees let later textures
// in instead of every new one failing and sampling white for the rest of the session.
bool texture_drop_requested;
uint32_t texture_failures;
// VK_EXT_memory_budget (renderer_memory_budget): enabled with the device when the instance has
// VK_KHR_get_physical_device_properties2 and the device offers it; else nothing is reported.
PFN_vkGetPhysicalDeviceMemoryProperties2KHR get_memprops2;
bool memory_budget_ext;
// VK_EXT_extended_dynamic_state: the geometry pipelines take cull mode
// and depth test, write and compare as dynamic state, so draws that differ only there share one
// pipeline (eds_key). Not offered (or RendererConfig.no_dynamic_state): one pipeline per state, as
// before.
PFN_vkGetPhysicalDeviceFeatures2KHR get_features2;
PFN_vkWaitForPresentKHR wait_for_present;   // --frame-gen: VK_KHR_present_wait (nullptr: not enabled)
PFN_vkCmdSetCullModeEXT cmd_set_cull_mode;
PFN_vkCmdSetDepthTestEnableEXT cmd_set_depth_test;
PFN_vkCmdSetDepthWriteEnableEXT cmd_set_depth_write;
PFN_vkCmdSetDepthCompareOpEXT cmd_set_depth_compare;
int budget_heap = -1;   // the largest device-local heap, where the textures live
// Descriptor sets per frame (one per distinct stage-texture combination); draws beyond are skipped.
const uint32_t MAX_FRAME_SETS = 4096;

// Glass operations (renderer_glass): two scratch copies of the target, the blur pass and its
// framebuffers, and the blur and composite pipelines (set 0: the sampled scratch image, push
// constants). The main pass variants that store and reload the target around the split are
// State::split_pass / resume_pass, shared with bloom.
struct Glass {
    bool tried = false, ok = false;
    Image a, b;
    VkRenderPass blur_pass = VK_NULL_HANDLE;
    VkFramebuffer fb_a = VK_NULL_HANDLE, fb_b = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set_a = VK_NULL_HANDLE, set_b = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, blur_fs = VK_NULL_HANDLE, comp_fs = VK_NULL_HANDLE;
    VkPipeline blur = VK_NULL_HANDLE, comp = VK_NULL_HANDLE;
    uint32_t executed = 0;   // in the last frame
};

// Sharpening (RendererConfig.sharpen): the output-size image the pass writes (what the readback
// and the swapchain blit read instead of the frame), its render pass and framebuffer, and the
// pipeline (set 0: the frame, sampled; push constant: the CAS peak).
struct Sharpen {
    bool ok = false;
    int amount = 0;
    float peak = 0;
    Image out;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
};

// FSR 1 (RendererConfig.fsr_w / fsr_h): the displayed-size EASU image and the RCAS image (what the
// gamma ramp, the readback and the swapchain blit read instead of the frame), one render pass for
// both (same format, every pixel written), their framebuffers and descriptor sets (EASU samples the
// frame, RCAS the EASU image), one pipeline layout (set 0: the source; push constants: EASU's 4 x
// uvec4 or RCAS's uvec4, src/fsr.h) and the two pipelines.
struct Fsr {
    bool req = false;   // requested: the targets are at the internal size
    bool ok = false;    // in use
    float sharpness = 0;
    uint32_t easu_con[16] = {}, rcas_con[4] = {};
    Image easu, out;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer easu_fb = VK_NULL_HANDLE, rcas_fb = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet easu_set = VK_NULL_HANDLE, rcas_set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, easu_fs = VK_NULL_HANDLE, rcas_fs = VK_NULL_HANDLE;
    VkPipeline easu_pipe = VK_NULL_HANDLE, rcas_pipe = VK_NULL_HANDLE;
};

// The title's gamma ramp (renderer_gamma_ramp, src/gamma_ramp.h): the output-size image the pass
// writes from the finished frame (the sharpened image, else the frame), which the readback and
// the swapchain blit read instead while a table is set; the table's uniform buffer (64 uvec4), the
// render pass and framebuffer, the pipeline (set 0: the frame, sampled, and the table), and the
// frame counter's framebuffer on the image (drawn after the ramp, on the read-back frame).
struct Gamma {
    bool ok = false;       // resources created (at the first table)
    bool failed = false;   // their creation failed: frames as rendered
    bool on = false;       // a table is set
    bool dirty = false;    // lut changed since the buffer was last written
    bool active = false;   // the frame being presented went through the pass
    uint32_t lut[256] = {};
    uint32_t executed = 0;
    Image out;
    Buffer ubo;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE, overlay_fb = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
};

// Frame counter overlay (RendererConfig.fps_overlay, renderer_fps_overlay_text): one render pass and
// pipeline per target format (the read-back frame, the swapchain format), the read-back frame's
// framebuffer, the swapchain images' views and framebuffers (made at the first frame after the
// swapchain was created; it gets COLOR_ATTACHMENT usage only with the overlay), and the text.
struct OverlayPass {
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
};
struct Overlay {
    bool ok = false;
    bool sc_color = false;   // the swapchain images can be drawn to
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    OverlayPass rb, sc;
    VkFramebuffer rb_fb = VK_NULL_HANDLE;
    std::vector<VkImageView> sc_views;
    std::vector<VkFramebuffer> sc_fbs;
    uint8_t glyph[FPS_MAX_CHARS] = {};
    int n = 0;
    uint32_t executed = 0;   // boxes drawn in the last frame
    // GPU % (RendererConfig.fps_overlay_gpu): two timestamps per frame slot around the frame's
    // commands, read when the slot is next recorded (its fence waited: never a wait on the GPU).
    VkQueryPool queries = VK_NULL_HANDLE;
    uint32_t ts_bits = 0;     // the queue's timestampValidBits (0: no GPU %)
    float ts_period = 1;      // ns per tick
    bool stamped[3] = {};     // per frame slot (MAX_FRAMES_IN_FLIGHT)
    double busy_ms = 0;       // completed frames' GPU time since renderer_fps_overlay_gpu
    int busy_frames = 0;
    float last_ms = -1;       // the latest completed frame's GPU time (RendererTiming.gpu_busy_ms)
};
// --stats (RendererConfig.pass_stats): GPU time per pass. A timestamp
// after each pass; the interval since the previous timestamp is the pass's. Passes that run inside
// the frame's render pass are separated by the scene marks before them (GP_SCENE: the title's
// draws, with every per-draw feature: world and character lighting, relief, hair, skin...).
enum GpPass : uint8_t {
    GP_SKIN_IN, GP_SKIN, GP_SKIN_NORMALS, GP_SKIN_OUT, GP_PREP, GP_TARGETS, GP_COVER, GP_WSHADOW, GP_CHARS, GP_SURF, GP_TAAVEL, GP_SCENE, GP_SOFT, GP_BLOOM, GP_SPLASH, GP_LAMP,
    GP_CONTACT, GP_AO, GP_COMPOSITE, GP_SHAFT, GP_FOG_WALLS, GP_FOG_INJECT, GP_FOG_INTEG, GP_FOG_COMP, GP_WET, GP_FILMIC, GP_GLASS,
    GP_GRAB, GP_TAA, GP_RAIN, GP_SKY, GP_PREV, GP_DOWNSAMPLE, GP_SHARPEN, GP_FSR, GP_GAMMA, GP_FG, GP_BLIT, GP_COUNT
};
static const char *const gp_name[GP_COUNT] = {
    "skinning inputs", "skinning programs", "skinning normals", "skinning copies", "light updates", "title targets (shadow maps)",
    "rain cover", "world shadows", "character mask",
    "sssr surfaces", "taa velocity", "scene draws", "soft particles", "bloom", "rain splashes", "lamp lights",
    "contact shadows", "ambient occlusion", "light/ao composite", "light shafts", "fog light walls", "fog inject",
    "fog integrate", "fog composite", "wet/damp ground", "filmic/exposure/dof", "hud glass", "history grab",
    "taa resolve", "rain", "sky", "frame history", "downsample", "sharpen", "fsr", "gamma", "frame generation", "present blit"};
const uint32_t GP_MAX_MARKS = 160;
struct GpuProf {
    VkQueryPool pool[3] = {};   // per frame slot (MAX_FRAMES_IN_FLIGHT)
    uint8_t label[3][GP_MAX_MARKS] = {};
    uint32_t n[3] = {};         // timestamps written in the slot's frame (the first: its start)
    uint64_t mask = ~0ull;
    float period = 1;           // ns per tick
    double sum[2][GP_COUNT] = {}, total[2] = {};   // per window (renderer_gpu_passes)
    uint32_t frames[2] = {};
};
// Push constants of shaders/fps_overlay.frag.
struct OverlayPush {
    int32_t box[4];
    int32_t scale, n;
    uint32_t text[FPS_MAX_CHARS / 4];   // 64 bytes in all (128 guaranteed)
};
static_assert(sizeof(OverlayPush) <= 128 && FPS_MAX_CHARS % 4 == 0, "fps overlay push constants within the guaranteed 128 bytes");

// Bloom (RendererConfig.bloom, renderer_bloom): the level images (level 0 at half the internal
// size, each next one half of it), one framebuffer each for the downsample pass (contents
// discarded) and the upsample pass (loaded, blended), descriptor sets sampling the scene and each
// level, and the downsample, upsample and composite pipelines (push constants: BloomPush).
const int MAX_BLOOM_LEVELS = 7;
const int BLOOM_LEVELS = 4;          // half the internal size down to 1/16 (bloom_create)
const int BLOOM_LEVELS_FILMIC = 6;   // with --filmic: down to 1/64, a wider glow
struct Bloom {
    bool ok = false;
    float strength = 0, threshold = 0;
    float hdr = 0;   // --filmic: the light mask's pseudo-HDR range (BloomPush.hdr)
    int levels = 0;
    int w[MAX_BLOOM_LEVELS] = {}, h[MAX_BLOOM_LEVELS] = {};
    Image lv[MAX_BLOOM_LEVELS];
    VkFramebuffer fb[MAX_BLOOM_LEVELS] = {};
    VkRenderPass down_pass = VK_NULL_HANDLE, up_pass = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet scene_set = VK_NULL_HANDLE, lv_set[MAX_BLOOM_LEVELS] = {};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, down_fs = VK_NULL_HANDLE, up_fs = VK_NULL_HANDLE, comp_fs = VK_NULL_HANDLE;
    VkPipeline down = VK_NULL_HANDLE, up = VK_NULL_HANDLE, comp = VK_NULL_HANDLE;
    bool marked = false;     // renderer_bloom called in the frame being recorded
    uint32_t executed = 0;   // in the last frame
};

// Filmic look (RendererConfig.filmic, renderer_filmic): the copy of the resolved scene the pass
// reads (src), the grade's 3D LUT, the descriptor sets (binding 0 src, 1 the LUT, 2 the
// halation glow: bloom level 1 in set_glow, src in set_plain for frames without a bloom split)
// and the pipeline (shaders/filmic.frag, in the frame's resumed main pass; push constants:
// FilmicPush).
// --auto-exposure (RendererConfig.auto_exposure) adds, at the same anchor: the meter grid (ae_meter.frag
// over src), two 1x1 adaptation state images used in turn (ae_adapt.frag writes state[ae_cur] from
// the other one; binding 3 of the filmic sets, one set per state image), a host-visible copy of
// each frame's state per frame slot (read once the slot's fence has been waited: --stats), and the
// AUTO_EXPOSURE variant of filmic.frag, or exposure.frag when the filmic look is off (exposure_only:
// a 2x2x2 LUT keeps the descriptor sets complete).
const int AE_GRID_W = 32, AE_GRID_H = 18;
const int AE_SLOTS = 3;   // frame slots (MAX_FRAMES_IN_FLIGHT)
struct Filmic {
    bool ok = false;
    RFilmic p{};   // lut / lut_size cleared after the upload
    int lut_size = 0;
    Image src, lut;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set_glow[2] = {}, set_plain[2] = {};   // [state image]; [0] only without auto exposure
    bool has_glow = false;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    bool marked = false;     // renderer_filmic called in the frame being recorded
    uint32_t frame = 0;      // grain seed
    uint32_t executed = 0;   // in the last frame
    // --auto-exposure
    bool ae = false, exposure_only = false;
    RAutoExposure ae_p{};
    Image meter, state[2];
    VkRenderPass meter_pass = VK_NULL_HANDLE, state_pass = VK_NULL_HANDLE;
    VkFramebuffer meter_fb = VK_NULL_HANDLE, state_fb[2] = {};
    VkDescriptorSetLayout ae_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool ae_pool = VK_NULL_HANDLE;
    VkDescriptorSet meter_set = VK_NULL_HANDLE, adapt_set[2] = {};
    VkPipelineLayout ae_layout = VK_NULL_HANDLE;
    VkShaderModule meter_fs = VK_NULL_HANDLE, adapt_fs = VK_NULL_HANDLE;
    VkPipeline meter_pipe = VK_NULL_HANDLE, adapt_pipe = VK_NULL_HANDLE;
    int ae_cur = 0;          // the state image the next adaptation writes
    double ae_t = -1;        // host time (ms) of the last adaptation, -1 before the first
    bool ae_snap = false;    // renderer_auto_exposure_snap: the next adaptation snaps (a camera cut)
    Buffer readback[AE_SLOTS];
    uint32_t rb_seq[AE_SLOTS] = {};   // the adaptation each slot's buffer holds (0: none)
    uint32_t ae_seq = 0, ae_read_seq = 0;
    float ae_last[4] = {};   // the state of the last completed frame read back
    bool ae_have = false;
    // --cutscene-dof without the look and the auto exposure: the anchor only copies the scene for
    // the depth of field (no filmic pipeline or LUT) and runs only in the frames that ask for it.
    bool dof_only = false;
    // The filmic sets reading the depth of field's result (Dof::out) instead of src, [state image].
    VkDescriptorSet set_glow_dof[2] = {}, set_plain_dof[2] = {};
};

// Temporal anti-aliasing (RendererConfig.taa, renderer_taa; src/taa.h): the two RGBA16F history
// images used in turn (the resolve writes hist[cur] from hist[1 - cur]) with their R16F view
// distances (dist: each pixel's nearest in its 3 x 3 neighbourhood, the disocclusion test), their
// pass (two colour attachments) and framebuffers, one set layout (0 the frame or a history, 1 depth,
// 2 the previous history, 3 its distances; resolve_set[i] writes hist[i], out_set[i] draws hist[i]
// into the frame), the resolve
// pipeline (one sample, its own pass) and the output pipeline (the frame's pass, at its sample
// count). The frame's parameters (renderer_taa) and the jitter of the draws before its op.
struct TaaPush {
    float rx[4], ry[4], rw[4];   // the reprojection map's rows
    float proj[4];               // depth a, depth b, blend (resolve) or sharpness (output), reset
    float size[4];               // target w, h, 1 / w, 1 / h
    float vp[4];                 // the 3D viewport's x, y (target pixels), 2 / w, 2 / h
    float obj[4];                // x: 1 = the velocity target holds this frame's character motion; y, z: the jitter (--taa-upscale)
    float up[4];                 // --taa-upscale: render width, height; render / output per axis
};
const float TAA_BLEND = 0.1f;   // the frame's share of the resolve at rest
struct Taa {
    bool ok = false;
    bool marked = false;     // renderer_taa called in the frame being recorded
    RTaa p{};
    float sharpness = 0;
    Image hist[2], dist[2];
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer fb[2] = {};
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet resolve_set[2] = {}, out_set[2] = {};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, resolve_fs = VK_NULL_HANDLE, out_fs = VK_NULL_HANDLE;
    VkPipeline resolve_pipe = VK_NULL_HANDLE, out_pipe = VK_NULL_HANDLE;
    int cur = 0;                // the history the next resolve writes
    uint64_t last_serial = 0;   // frame_serial of the last resolve (0: none)
    uint32_t frame = 0;         // resolves so far: the jitter phase
    uint32_t executed = 0;      // in the last frame
    float jit[2] = {};          // the frame's jitter (target pixels)
    bool jitter_now = false;    // exec_op, sky_exec, skyline_exec: the draw is offset by jit
    size_t jit_until = 0;       // chars_record: the ops before it (the TAA op) are jittered
    // Character motion (renderer_set_draw_identity, taa_pose, taa_vel_record): the constants of
    // this and last frame's identified skinned draws (192 registers each at the mapped offset),
    // identities seen this frame, the frame's draws with a previous pose (op, current and previous
    // output in the skinning device buffer) and the velocity pass: RGBA16F (previous NDC x, y, the
    // previous and the current view distance; 0 where no character), its own depth, pass and pipeline.
    uint64_t next_id = 0;
    std::unordered_map<uint64_t, uint32_t> ids_now, ids_prev, seen;
    std::vector<float> pose_now, pose_prev;
    struct VelDraw {
        uint32_t op, cur, prev;   // pending (taa_pose): its job and last frame's constants; after taa_poses: the outputs
    };
    std::vector<VelDraw> vel;
    bool vel_ok = false, vel_live = false;
    Image vel_img, vel_depth;
    VkRenderPass vel_pass = VK_NULL_HANDLE;
    VkFramebuffer vel_fb = VK_NULL_HANDLE;
    VkPipelineLayout vel_layout = VK_NULL_HANDLE;
    VkShaderModule vel_vs = VK_NULL_HANDLE, vel_fs = VK_NULL_HANDLE;
    VkPipeline vel_pipe = VK_NULL_HANDLE;
    uint32_t vel_draws = 0;     // in the last frame
    // Temporal upscaling (RendererConfig.taa_up_w): the render size of the scene
    // (the top-left rw x rh of the frame target in a frame with a TAA op), the jitter length, the
    // output-size depth the upscale resolve writes (R32F: attachment 2 of up_pass) and the output
    // pass writes back, the upscale resolve and output pipelines.
    bool up = false, scoped = false;   // scoped: record_frame is recording the ops before the TAA op at rw x rh
    int rw = 0, rh = 0;
    int mw = 0, mh = 0, ow = 0;    // --dynres: the largest render size (RendererConfig.taa_up_w x taa_up_h) and the output width
    uint64_t size_serial = 0;      // renderer_taa_render_size: the frame_serial of the first frame at a new size
    uint32_t phases = TAA_JITTER_PHASES;
    Image upz;
    VkRenderPass up_pass = VK_NULL_HANDLE;
    VkFramebuffer up_fb[2] = {};
    VkShaderModule up_fs = VK_NULL_HANDLE, up_out_fs = VK_NULL_HANDLE;
    VkPipeline up_pipe = VK_NULL_HANDLE, up_out_pipe = VK_NULL_HANDLE;
};

// Frame generation (RendererConfig.frame_gen, renderer_frame_gen): the frame
// target at the anchor (hl_int) and at the HUD's end (hud_b: renderer_frame_gen_hud_end or the frame's
// end), whose difference is the HUD; the outputs as shown (display size, a ring of two: this frame's
// and the previous one's), the motion fields (RGBA16F, display size, ring of two, GENERAL: written by
// fg_mv.comp, the HUD zeroed by fg_hud.comp, read by the splat and the interpolation), the splat's
// distance and index fields (R32_UINT, GENERAL), the generated frame (gen) and, with optical flow
// (RendererConfig.frame_gen 2), the flow per 16 x 16 block (RGBA16F, GENERAL).
struct FgPush {
    float rx[4], ry[4], rw[4];   // the reprojection map's rows
    float proj[4];               // depth a, depth b, valid, velocity target live
    float size[4];               // display w, h; depth texels per display pixel x, y
    float vp[4];                 // the 3D viewport in display pixels: x, y, w, h
    float vel[4];                // velocity texels per display pixel x, y; dilate the depth (1)
    float jit[4];                // x, y: the frame's jitter (render pixels: the velocity target's)
};
struct FrameGen {
    bool ok = false;
    bool marked = false;     // renderer_frame_gen called in the frame being recorded
    bool captured = false;   // the op ran in the frame being recorded (hl_int, mv[cur] written)
    bool hud_marked = false; // renderer_frame_gen_hud_end called in the frame being recorded
    bool hud_taken = false;  // hud_b holds the frame being recorded's target at the HUD's end
    int show = 0;            // renderer_frame_gen_present: 1 the generated frame, 2 the real one twice
    bool built = false;      // the last recorded frame built gen
    bool rb_valid = false;   // fg.rb holds the last renderer_end's generated frame
    bool of = false;         // optical flow (--frame-gen-of)
    RFrameGen p{};
    Image hl_int, hud_b, hl[2], mv[2], dist, index, gen, flow;
    Buffer rb;               // the generated frame's readback (renderer_end)
    VkDescriptorSetLayout mv_set_layout = VK_NULL_HANDLE, splat_set_layout = VK_NULL_HANDLE, interp_set_layout = VK_NULL_HANDLE,
                          flow_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout mv_layout = VK_NULL_HANDLE, splat_layout = VK_NULL_HANDLE, interp_layout = VK_NULL_HANDLE, flow_layout = VK_NULL_HANDLE,
                     hud_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet mv_set[2] = {}, splat_set[2] = {}, interp_set[2] = {}, flow_set[2] = {}, hud_set[2] = {};
    VkShaderModule mv_cs = VK_NULL_HANDLE, splat_cs = VK_NULL_HANDLE, vs = VK_NULL_HANDLE, interp_fs = VK_NULL_HANDLE, flow_cs = VK_NULL_HANDLE,
                   hud_cs = VK_NULL_HANDLE;
    VkPipeline mv_pipe = VK_NULL_HANDLE, splat_pipe = VK_NULL_HANDLE, interp_pipe = VK_NULL_HANDLE, flow_pipe = VK_NULL_HANDLE,
               hud_pipe = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    int cur = 0;                // the ring entry the next frame writes
    uint64_t last_serial = 0;   // frame_serial of the last frame that captured (0: none)
    uint32_t executed = 0;      // generated frames built by the last frame
    uint32_t presented = 0;     // generated frames presented since renderer_init
    // Pacing by present completion (VK_KHR_present_id + VK_KHR_present_wait):
    // the real frame of every frame carries a present id; before a frame takes its images the renderer
    // waits until the real frame before the last one is on screen, so at most one frame's pair waits
    // in the queue and the GPU never waits for an image the display still holds.
    bool pw = false;            // present waits in use
    uint64_t pw_id = 0;         // the last present id given (this swapchain)
    uint64_t pw_last = 0, pw_prev = 0;   // ids of the last two real frames presented
    int pw_timeouts = 0;
    double pw_t = 0;            // the last wait's return (now_ms) and whether it waited
    bool pw_waited = false, pw_new = false;
};
const float FG_COLOUR_LIMIT = 0.25f;   // the two frames disagree beyond this (max channel): the new frame's

// Depth of field (RendererConfig.dof, renderer_dof): run inside the filmic pass's split (filmic_exec,
// after the scene copy) in the frames renderer_dof asks for. The 1x1 focus state images used in
// turn (dof_focus.frag writes state[cur] from the other one; a host-visible copy per frame slot for
// --stats), the half-size prefilter (pre) and gather (bokeh) images, the internal-size result
// (out: the filmic pass reads it, or copy_pipe draws it into the frame without the look), their
// passes and framebuffers, one set layout (0 depth, 1 scene, 2 pre, 3 bokeh, 4 a state image:
// focus_set[i] reads state[1 - i], set[i] state[i], copy_set reads out as the scene; 5 the
// character mask, Chars::depth: chars_record draws it in the frames that run the DOF) and the
// pipelines (push constants: DofPush).
struct Dof {
    bool ok = false;
    RDof p{};
    int hw = 0, hh = 0;
    Image pre, bokeh, out, state[2];
    VkRenderPass half_pass = VK_NULL_HANDLE, state_pass = VK_NULL_HANDLE, out_pass = VK_NULL_HANDLE;
    VkFramebuffer pre_fb = VK_NULL_HANDLE, bokeh_fb = VK_NULL_HANDLE, out_fb = VK_NULL_HANDLE, state_fb[2] = {};
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet focus_set[2] = {}, set[2] = {}, copy_set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, focus_fs = VK_NULL_HANDLE, down_fs = VK_NULL_HANDLE, gather_fs = VK_NULL_HANDLE,
                   comp_fs = VK_NULL_HANDLE, copy_fs = VK_NULL_HANDLE;
    VkPipeline focus_pipe = VK_NULL_HANDLE, down_pipe = VK_NULL_HANDLE, gather_pipe = VK_NULL_HANDLE, comp_pipe = VK_NULL_HANDLE,
               copy_pipe = VK_NULL_HANDLE;
    int cur = 0;             // the state image the next focus pass writes
    bool want = false;       // renderer_dof called in the frame being recorded
    RDofFrame frame{};       // its parameters
    Buffer readback[AE_SLOTS];
    uint32_t rb_seq[AE_SLOTS] = {};
    uint32_t seq = 0, read_seq = 0;
    float last[4] = {};
    bool have = false;
    uint32_t executed = 0;   // in the last frame
};

// Procedural rain (RendererConfig.rain, renderer_rain): two pipelines in the main pass (full-target
// triangle at the slice's depth, depth test, no depth write; push constants: RainPush): pipe blends
// the flat colour and veil by alpha, lit brightens what is behind a streak (DST_COLOR / ONE); and
// the frame's rain operations.
struct Rain {
    bool ok = false;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE, lit = VK_NULL_HANDLE;
    std::vector<RRain> list;   // the frame being recorded
    uint32_t executed = 0;     // in the last frame
};

// Procedural sky and skyline (RendererConfig.sky, renderer_sky / renderer_skyline): the sky is a
// full-target pipeline in the main pass (no depth, writes every channel: the bloom light mask in
// alpha); the skyline overlay one RVertex pipeline (shaders/geom.vert + skyline.frag, depth test
// less or equal without writes): haze and darkening (ONE, SRC_ALPHA, rgb only). It adds no light:
// the world drawn later blends some surfaces over the sky stage (alpha-blended ground with depth
// writes), so anything bright added here would show through them. The frame's operations' parameters.
struct Sky {
    bool ok = false;
    VkPipelineLayout layout = VK_NULL_HANDLE, line_layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE, line_vs = VK_NULL_HANDLE, line_fs = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE, line = VK_NULL_HANDLE;
    std::vector<RSky> list;        // the frame being recorded
    std::vector<RSkyline> lines;
    uint32_t executed = 0, lines_executed = 0;   // in the last frame
};

// Ambient occlusion (RendererConfig.ssao, renderer_ssao): two R16G16 images (occlusion, view depth)
// at the working size (ssao_work_size: half the output size, so supersampling does not multiply
// the cost) the occlusion and blur passes ping-pong between, their render pass and framebuffers,
// the composite's render pass (the frame's color target alone, loaded and stored: the depth is
// sampled, not attached) and framebuffer, descriptor sets (binding 0: the depth target, binding 1:
// image a or b, binding 2 with the light mask: the single-sample color s.color) and the occlusion,
// blur and composite pipelines (push constants: SsaoPush; specialization constant 1: the scale).
struct Ssao {
    bool ok = false;
    float strength = 0, radius = 0;
    int hw = 0, hh = 0;   // the working size (ssao_work_size)
    int scale = 2;        // internal pixels per texel
    bool mask = false;    // the light mask in s.color's alpha fades the occlusion (s.color sampled)
    Image a, b;
    VkRenderPass pass = VK_NULL_HANDLE, comp_pass = VK_NULL_HANDLE;
    VkFramebuffer fb_a = VK_NULL_HANDLE, fb_b = VK_NULL_HANDLE, comp_fb = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set_a = VK_NULL_HANDLE, set_b = VK_NULL_HANDLE;   // depth + a, depth + b
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, ao_fs = VK_NULL_HANDLE, blur_fs = VK_NULL_HANDLE, comp_fs = VK_NULL_HANDLE;
    VkPipeline ao = VK_NULL_HANDLE, blur = VK_NULL_HANDLE, comp = VK_NULL_HANDLE;
    bool marked = false;     // renderer_ssao called in the frame being recorded
    uint32_t executed = 0;   // in the last frame
    // --ssao-mode gtao (RendererConfig.ssao_mode): XeGTAO's compute passes
    // replace the occlusion and blur passes. All in VK_IMAGE_LAYOUT_GENERAL: the view depth with
    // levels MIPs (R32F, a storage view per level), two RGBA16F working images the main and denoise
    // passes ping-pong between (the last denoise pass writes occlusion and view depth, which the
    // composite reads as it reads image a); one set layout for the passes (0: sampled input,
    // 1-5: storage outputs, 6: the light mask s.color or the temporal pass's history), composite
    // sets in set_layout per image. With TAA the temporal pass
    // accumulates the main pass's output in gh[] (ping-pong: one written per frame, the other the
    // previous frame's) and the one denoise pass reads it.
    bool gtao = false;
    int quality = 0, levels = 0;
    bool taa_frame = false;   // the frame being recorded has a TAA op (per-frame noise, temporal pass, one denoise pass)
    Image gz, gw[2], gh[2];
    int gh_cur = 0;               // gh[gh_cur] holds the last temporal pass's output
    uint64_t gh_serial = 0;       // frame_serial of that pass (0: none): usable by the next frame only
    VkDescriptorSet gset_tmp[2] = {}, gset_den_h[2] = {};   // tmp[i]: gw[0] + gh[1 - i] -> gh[i]; den_h[i]: gh[i] -> gw[1]
    VkPipeline gtmp = VK_NULL_HANDLE;
    VkImageView gz_mip[GTAO_MIP_LEVELS] = {};
    VkSampler gz_sampler = VK_NULL_HANDLE;   // point, MIPs
    VkDescriptorSetLayout gset_layout = VK_NULL_HANDLE;
    VkDescriptorPool gpool = VK_NULL_HANDLE;
    VkDescriptorSet gset_pre = VK_NULL_HANDLE, gset_main = VK_NULL_HANDLE, gset_den[2] = {}, gset_comp[2] = {};   // den[i]: gw[i] -> gw[1 - i]
    VkPipelineLayout glayout = VK_NULL_HANDLE;
    VkPipeline gpre = VK_NULL_HANDLE, gmain = VK_NULL_HANDLE, gden = VK_NULL_HANDLE;
};

// Lamp lights (RendererConfig.lamp, renderer_lamp_lights): an R16G16B16A16 image (light, view depth)
// at the ssao working size, its render pass and framebuffer, the composite's render pass (the
// frame's color target, loaded; compatible with the ssao composite's, so either can hold both) and
// framebuffer, one descriptor set (0: the depth target, 1: the image, 2: the character mask or, as a
// placeholder, the depth target, 3: the single-sample color s.color (light mask), 4: the uniform
// block of lights, updated in the command buffer) and the light and composite pipelines of
// shaders/lamp.frag (specialization constants MODE, SCALE, CHARS).
struct Lamp {
    bool ok = false;
    bool chars = false;   // the character mask is bound
    int hw = 0, hh = 0, scale = 2;
    Image img;
    Buffer ubo;
    VkRenderPass pass = VK_NULL_HANDLE, comp_pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE, comp_fb = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipeline light = VK_NULL_HANDLE, comp = VK_NULL_HANDLE;
    bool marked = false;     // renderer_lamp_lights called in the frame being recorded
    RLampLights p{};         // its argument
    uint32_t executed = 0;   // in the last frame
};

// Light shafts (RendererConfig.shafts, renderer_light_shafts): an R16G16B16A16 image (light, view
// depth) at the ssao working size (twice as coarse with shafts_cheap), its render pass and
// framebuffer, the composite's render pass (the frame's color target, loaded) and framebuffer, one
// descriptor set (0: the depth target, 1: the image, 2: the uniform block of lights, updated in the
// command buffer) and the scatter and composite pipelines of shaders/shafts.frag (specialization
// constants MODE, SCALE, STEPS).
struct Shaft {
    bool ok = false;
    int hw = 0, hh = 0, scale = 2, steps = 8;
    Image img;
    Buffer ubo;
    VkRenderPass pass = VK_NULL_HANDLE, comp_pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE, comp_fb = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipeline light = VK_NULL_HANDLE, comp = VK_NULL_HANDLE;
    bool marked = false;     // renderer_light_shafts called in the frame being recorded
    RLightShafts p{};        // its argument
    uint32_t executed = 0;   // in the last frame
};
// Volumetric fog (RendererConfig.volfog, renderer_volfog): three RGBA16F 3D storage images of the
// froxel grid (volfog_grid) kept in the general layout: the injection of this frame and of the
// previous one (vol[f & 1] written, the other read as history) and the integral; the uniform block
// (shaders/volfog_common.glsl, updated in the command buffer), the polar wall maps (a storage
// buffer, VOLFOG_POLAR_BINS x RENDERER_VOLFOG_MAX x 2 vec4); two descriptor sets (one per parity) of
// one layout shared by the three compute pipelines and the composite (its render pass: the
// frame's colour target, loaded).
struct Volfog {
    bool ok = false;
    VolfogGrid g{};
    Image vol[2], integ;
    Buffer ubo, polar;
    bool fresh = true;       // the images are still undefined (first frame)
    uint32_t parity = 0;
    VkRenderPass comp_pass = VK_NULL_HANDLE;
    VkFramebuffer comp_fb = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule polar_cs = VK_NULL_HANDLE, inject_cs = VK_NULL_HANDLE, integ_cs = VK_NULL_HANDLE, vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipeline polar_pipe = VK_NULL_HANDLE, inject_pipe = VK_NULL_HANDLE, integ_pipe = VK_NULL_HANDLE, comp = VK_NULL_HANDLE;
    bool marked = false;     // renderer_volfog called in the frame being recorded
    bool ran = false;        // the frame being recorded ran it
    bool prev_ran = false;   // the previous frame did (else the history is dropped)
    RVolfog p{};             // its argument
    uint32_t executed = 0;   // in the last frame
};

// Soft particles (RendererConfig.soft, renderer_soft): a resume-pass variant whose subpass keeps the
// depth attachment read-only (DEPTH_STENCIL_READ_ONLY_OPTIMAL; render pass compatibility ignores
// layouts, so every pipeline of the main pass runs in it), the soft geometry pipelines' set 1 (the
// depth target in that layout, nearest) and their layout (set 0 as the geometry pipelines', 32 bytes
// of push constants: the draw's 16, then depth_a, depth_b, 1 / distance). Pipeline key bit 30
// selects the soft fragment shader (shaders/geom.frag built with SOFT, MSAA for a multisampled
// target).
struct Soft {
    bool ok = false;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule fs = VK_NULL_HANDLE;
    bool marked = false;    // renderer_soft called in the frame being recorded
    bool live = false;      // record_frame: passes resumed from here on keep the depth read-only
    bool in_pass = false;   // record_frame: the open render pass instance is `pass`
    float push[4] = {};     // the frame's depth_a, depth_b, 1 / distance, 0
    uint32_t executed = 0;  // soft draws in the last frame
};
const uint32_t SOFT_KEY = 1u << 30;

// Character lighting (RendererConfig.char_light, renderer_char_lights, renderer_draw_lit): the
// lit geometry variant (shaders/geom.vert and geom.frag built with CHAR_LIT, pipeline key bit 31):
// a second vertex stream (binding 1, location 8: the view normals, parallel to the frame's
// vertices, from the frame's normals region of its geometry buffer), set 1 (the uniform block of
// lights, shaders/char_lit.glsl, updated in the command buffer before the frame's passes) and the
// geometry pipelines' push constants.
struct CharLit {
    bool ok = false;
    Buffer ubo;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    bool marked = false;     // renderer_char_lights called (with lights) in the frame being recorded
    RLampLights p{};         // its argument
    uint32_t executed = 0;   // lit draws in the last frame
    uint32_t world_executed = 0;   // of those, world-lit (renderer_draw_world_lit: op.push[3] = the light mask)
};
const uint32_t CHARLIT_KEY = 1u << 31;

// Lamp character shadows (RendererConfig.lamp_shadow, renderer_lamp_shadows): the frame's lights in
// a storage buffer bound at set 0, binding 5 of every geometry pipeline (shaders/geom.frag
// LampShadows; the frame's programs buffer stands in there when it is off), updated in the command
// buffer before the frame's passes (lshadow_update). Receivers read it with shadow word bit 14.
// The level's wall map (renderer_lamp_wall_map, WallMap) is bound at set 0, binding 8.
struct LampShadowGpu {   // shaders/geom.frag LampShadows (std430)
    float inv[4][4];     // rows: world = inv x stq
    float params[4];     // A, B, strength, lights
    float shape[4];      // height, length, start, dir_share
    float stq[RENDERER_LAMP_SHADOW_MAX][4];
    float pos[RENDERER_LAMP_SHADOW_MAX][4];   // world xyz, radius
    float wt[RENDERER_LAMP_SHADOW_MAX][4];    // weight, -, -, -
    float fade[4];       // the low-light fade: min angle, full angle (degrees; full <= min: none), low length
    float eye[4];        // the camera (world)
    float wall[4];       // the wall map: x0, y0, 1 / cell, 1 = loaded
    float wallz[4];      // z0, zstep, the receiver's own wall: no wall test nearer than this (world units)
};
struct LampShadow {
    bool ok = false;
    Buffer buf;
    bool marked = false;     // renderer_lamp_shadows called (with lights) in the frame being recorded
    RLampShadows p{};        // its argument
    bool loaded = false;     // the buffer holds lights (a frame without any clears them)
    uint32_t executed = 0;   // lights of the last frame
};
// The level's wall map (renderer_lamp_wall_map, light-walls.md): an
// R16G16_UINT image (a 1x1 empty one until a level's) at set 0, binding 8 of every geometry pipeline
// (white stands in without it) and binding 5 of the lamp pass. Kept with lamp shadows or light walls
// (ok); the 1x1 image alone exists for the lamp pass's binding otherwise.
struct WallMap {
    bool ok = false;     // maps are kept (RendererConfig.lamp_shadow or light_walls)
    Image img;
    bool real = false;   // a level's map is loaded (not the empty 1x1 one)
    int w = 0, h = 0;    // its size (renderer_lamp_wall_map_region)
    float box[8]{};      // x0, y0, 1 / cell, 1 = loaded; z0, zstep, the receiver's own wall (1.5 cells), -
};

// World shadows (RendererConfig.world_shadow, renderer_world_shadow): the D16 map (cfg.world_shadow
// texels a side), its depth-only render pass (cleared, left shader-readable), framebuffer and
// pipeline (shaders/rain_cover.vert: the three clip -> map rows as push constants, slope-scaled
// depth bias), and the receivers' parameters in a storage buffer; set 0 binds the map at binding 6
// and the buffer at binding 7 of every geometry pipeline (white and the frame's programs buffer
// stand in when off). Receivers read them with shadow word bit 15.
struct WorldShadowGpu {   // shaders/geom.frag WorldShadow (std430)
    float row[3][4];      // clip (x, y, w, 1) -> map NDC x, y, depth
    float wrow[3][4];     // clip -> world
    float vp[4];          // the frame's viewport: x0, y0, 1 / w, 1 / h (target pixels)
    float k[4];           // on, bias, darken, map size
    float eye[4];         // eye (world), lamps
    float fade[4];        // near, far, baked lo, baked hi
    float lampk[4];       // lamp keep
    float lamp[RENDERER_WORLD_SHADOW_LAMPS][4];   // world xyz, radius
    float lrow[3][4];     // world -> map NDC x, y, depth (the indoor gate)
    float gin[4];         // dt (s), snap, clearance, 0
    float gfwd[4];        // the camera's view direction along the ground (x, y)
    float gate[4];        // level, covered share: written by the gate (shaders/world_shadow_gate.comp) only
};
struct WorldShadow {
    bool ok = false;
    int size = 0;
    Image map;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    // Cut-out casters (alpha-tested or alpha-blended draws: fences, wire): their stage-0 alpha
    // decides which texels cast (shaders/world_shadow_cut.*; set 0 the draw's frame set).
    VkPipelineLayout cut_layout = VK_NULL_HANDLE;
    VkShaderModule cut_vs = VK_NULL_HANDLE, cut_fs = VK_NULL_HANDLE;
    VkPipeline cut_pipe = VK_NULL_HANDLE;
    // The indoor gate (shaders/world_shadow_gate.comp): the map and the buffer in its own set.
    VkDescriptorSetLayout gate_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool gate_pool = VK_NULL_HANDLE;
    VkDescriptorSet gate_set = VK_NULL_HANDLE;
    VkPipelineLayout gate_layout = VK_NULL_HANDLE;
    VkShaderModule gate_cs = VK_NULL_HANDLE;
    VkPipeline gate_pipe = VK_NULL_HANDLE;
    bool had_map = false;    // the previous frame drew a map (else the gate snaps)
    Buffer buf;
    bool marked = false;     // renderer_world_shadow called in the frame being recorded
    RWorldShadow p{};        // its argument
    uint32_t executed = 0;   // maps drawn in the last frame
    uint32_t cut = 0;        // cut-out casters in the last map
};

// Neon glow (renderer_set_emission): the emissive geometry variant (shaders/geom.frag built with
// EMIT), marked in the draw's shape word (the pipeline key's 32 bits are taken): set 1 = a frame set
// holding the mask in binding 0, push constants {alpha test, program, 0, strength, bloom, stage}.
// A masked lit draw (CHARLIT_KEY: world-lit or character) uses the lit emissive variant (geom.frag
// built with CHAR_LIT and EMIT): set 1 = the lights, set 2 = the mask, word 3 = the light mask.
struct Emit {
    VkPipelineLayout layout = VK_NULL_HANDLE, lit_layout = VK_NULL_HANDLE;
    VkShaderModule fs = VK_NULL_HANDLE, lit_fs = VK_NULL_HANDLE;
    uint32_t tex = 0, stage = 0;   // renderer_set_emission's current mask
    float strength = 0, bloom = 0;
    uint32_t executed = 0;         // emissive draws in the last frame
};
const uint32_t EMIT_SHAPE = 1u << 31;

// --normal-maps (renderer_set_relief): the lit relief variant (shaders/geom.frag built with CHAR_LIT
// and RELIEF), marked in the shape word like EMIT_SHAPE, for lit draws (CHARLIT_KEY) only and never
// with an emission mask (neon glow wins). It shares the lit emissive variant's pipeline layout: set
// 1 = the lights, set 2 = the normal map (binding 0) and the specular map (binding 1), push
// constants {alpha test, program, light mask, normal strength, specular strength, stage + 4 x
// flags, baked weight}; flag 4 (renderer_set_relief_dry) reads the specular map's B / A material,
// flag 8 (renderer_set_relief_height with renderer_set_parallax) marches the normal map's height,
// whose depth is the lights' block's opts.y; flag 16 (renderer_set_relief_glossy) only marks the draw
// for --reflections sssr's surface pass (geom.frag does not read it).
// Pipeline variants (geom_resources, the pipeline cache records): 0 plain, 1
// emissive, 2 relief.
struct Relief {
    VkShaderModule fs = VK_NULL_HANDLE;
    uint32_t tex[2] = {0, 0};      // renderer_set_relief's current maps
    float p[4] = {0, 0, 0, 0};
    bool dry = false;              // renderer_set_relief_dry
    bool glossy = false;           // renderer_set_relief_glossy (flag 16: the glass's surface pass)
    bool height = false;           // renderer_set_relief_height
    float parallax = 0;            // renderer_set_parallax
    uint32_t executed = 0;         // relief draws in the last frame
};
const uint32_t RELIEF_SHAPE = 1u << 30;

// --hair (RendererConfig.hair, renderer_set_hair): the lit hair variants (shaders/geom.frag built
// with CHAR_LIT and HAIR, or CHAR_LIT, RELIEF and HAIR for a draw with relief maps too), marked in
// the shape word like RELIEF_SHAPE, for character draws lit by renderer_draw_lit or
// renderer_draw_skinned only (not world-lit ones, never with an emission mask). They share the lit
// emissive layout: set 2 = the relief maps (bindings 0 and 1; 0 = white, unread without relief)
// and the hair mask (binding 2); push constants as the relief variant's (zero without relief); the
// strength and world up are the lights' block's `hair`.
struct Hair {
    VkShaderModule fs = VK_NULL_HANDLE, relief_fs = VK_NULL_HANDLE;
    uint32_t tex = 0;              // renderer_set_hair's current mask
    uint32_t executed = 0;         // hair draws in the last frame
};
const uint32_t HAIR_SHAPE = 1u << 29;
// Pipeline variants (geom_resources, the pipeline cache records): 0 plain, 1 emissive, 2 relief,
// 3 hair, 4 relief and hair.
const uint32_t VAR_EMIT = 1, VAR_RELIEF = 2, VAR_HAIR = 3, VAR_RELIEF_HAIR = 4;

// --skin (RendererConfig.skin, renderer_set_skin): the lit skin variants (shaders/geom.frag built
// with CHAR_LIT and SKIN, plus RELIEF and / or HAIR as the draw has them), marked in the shape word
// by SKIN_SHAPE, for the same character draws as the hair. Same layout as the hair variants; the
// skin mask is set 2's binding 3, the strength the lights' block's `skin`. (Skin below is the GPU
// skinning.)
struct SkinLit {
    VkShaderModule fs[4] = {};     // by (relief ? 1 : 0) + (hair ? 2 : 0)
    uint32_t tex = 0;              // renderer_set_skin's current mask
    uint32_t executed = 0;         // skin draws in the last frame
    VkShaderModule mat_fs[4] = {};  // --eyes, --cloth: the material variants, by the same index
    uint32_t mat_executed = 0;      // material draws in the last frame
    float eye_open = 1;             // --eyes: renderer_set_eye_open's current value
};
const uint32_t SKIN_SHAPE = 1u << 28;
// 5 skin, 6 relief and skin, 7 hair and skin, 8 relief, hair and skin
const uint32_t VAR_SKIN = 5, VAR_RELIEF_SKIN = 6, VAR_HAIR_SKIN = 7, VAR_RELIEF_HAIR_SKIN = 8;
// --eyes, --cloth (RendererConfig.eyes / .cloth): every skin draw takes a material variant instead
// (shaders/geom.frag built with SKIN and MAT, plus RELIEF and / or HAIR): its skin mask is the
// three masks stacked (hair.h: skin, eyes, cloth), the strengths the lights' block's skin.y, .z.
// 9 skin, 10 relief, 11 hair, 12 relief and hair, all with the materials
const uint32_t VAR_MAT = 9, VAR_MAT_LAST = 12;
static bool var_mat(uint32_t var) { return var >= VAR_MAT && var <= VAR_MAT_LAST; }
static bool var_skin(uint32_t var) { return (var >= VAR_SKIN && var <= VAR_RELIEF_HAIR_SKIN) || var_mat(var); }
static bool var_relief(uint32_t var) {
    return var == VAR_RELIEF || var == VAR_RELIEF_HAIR || var == VAR_RELIEF_SKIN || var == VAR_RELIEF_HAIR_SKIN || (var_mat(var) && ((var - VAR_MAT) & 1));
}
static bool var_hair(uint32_t var) {
    return var == VAR_HAIR || var == VAR_RELIEF_HAIR || var == VAR_HAIR_SKIN || var == VAR_RELIEF_HAIR_SKIN || (var_mat(var) && ((var - VAR_MAT) & 2));
}

// Rain splashes (RendererConfig.rain, renderer_rain_splash), two steps of shaders/ssao_comp.frag
// (specialization constant 0): the mask (mode 1) of the drops at the output size (the internal size
// divided by the supersampling factor: drops are no finer than an output pixel) in an R16G16 image,
// then its composite (mode 2) onto the frame's color target, lit and flat (two draws), in its own
// render pass or inside the ssao composite's (the same attachment; the depth is sampled, not
// attached). Descriptor sets: binding 0 the depth target, binding 1 the white texture (mask pass,
// not read) or the mask (composite, bilinear). Push constants SsaoPush.
struct Splash {
    bool ok = false;
    int mw = 0, mh = 0, scale = 1;   // mask size, internal pixels per mask texel
    Image mask;
    VkRenderPass pass = VK_NULL_HANDLE, mask_pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE, mask_fb = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set_mask = VK_NULL_HANDLE, set_comp = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipeline mask_pipe = VK_NULL_HANDLE, pipe = VK_NULL_HANDLE, flat = VK_NULL_HANDLE;   // mask; lit (DST_COLOR / ONE), flat (ONE / ONE)
    // With the rain cover map: the mask pipeline is the RAIN_COVER variant, set 1 the cover's set.
    VkPipelineLayout mask_layout = VK_NULL_HANDLE;
    VkShaderModule mask_fs = VK_NULL_HANDLE;
    bool marked = false;     // renderer_rain_splash called in the frame being recorded
    RRainSplash p{};         // its parameters
    uint32_t executed = 0;   // in the last frame
};

// Contact shadows (RendererConfig.contact, renderer_contact), two steps of shaders/contact.frag
// (specialization constant 0): the occlusion mask (mode 0) at half the output size (`scale` internal
// pixels per texel) in an R8 image, then its composite (mode 1) multiplied onto the frame's color
// target, in its own render pass or inside the ssao, lamp or splash composite's (the same
// attachment). Descriptor sets: binding 0 the depth target, binding 1 the white texture (mask
// pass, not read) or the mask (composite, bilinear), binding 2 the character mask.
struct Contact {
    bool ok = false;
    int mw = 0, mh = 0, scale = 2;
    Image mask;
    VkRenderPass pass = VK_NULL_HANDLE, mask_pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE, mask_fb = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set_mask = VK_NULL_HANDLE, set_comp = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipeline mask_pipe = VK_NULL_HANDLE, pipe = VK_NULL_HANDLE;
    bool marked = false;     // renderer_contact called in the frame being recorded
    bool pending = false;    // record_frame: the next split (lamp, ssao or splash) draws it
    RContact p{};            // its parameters
    uint32_t executed = 0;   // in the last frame
};

// Wet ground (RendererConfig.rain_wet, renderer_rain_wet), three steps of shaders/wet.frag
// (specialization constant 0) at a reduced size (rw x rh, `scale` internal pixels per texel: half
// the output size): the resolved colour is blitted into src; mode 0 traces the reflections over the
// depth into a, mode 1 blurs them (mostly vertically) into b, mode 2 composites b onto the frame's
// color target (blend ONE / ONE_MINUS_SRC_ALPHA: darkened by its alpha, the reflection added).
// Descriptor sets: binding 0 the depth target, binding 1 src, a or b, binding 2 the character mask,
// binding 3 the light streaks' uniform block (WetUbo; written only in frames with streaks or
// materials), binding 4 the material map (RG16_UINT, `mat`: a 1x1 empty one until
// renderer_material_map loads a level's).
#define WET_HIZ_LEVELS 7   // shaders/sssr.glsl SSSR_MAX_MIP + 1
// --reflections sssr: a camera that moved more than WET_SSSR_CUT world units or turned (the mean
// cosine of its right and back axes below WET_SSSR_TURN) in a frame starts the history anew.
#define WET_SSSR_CUT 2.0f
#define WET_SSSR_TURN 0.9f
struct Wet {
    bool ok = false;
    int rw = 0, rh = 0, scale = 2;
    Image src, a, b;
    Buffer ubo;
    Image mat;                 // the material map (binding 4)
    bool mat_real = false;     // a level's map is loaded (not the empty 1x1 one)
    float mat_box[8]{};        // its x0, y0, 1 / cell, 0, z0, zstep, w, h
    bool mat_ubo = false;      // the uniform block holds materials from an earlier frame
    bool neon_ubo = false;     // the uniform block holds neon reflections (--neon-reflections) from an earlier frame
    VkRenderPass pass = VK_NULL_HANDLE, half_pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE, fb_a = VK_NULL_HANDLE, fb_b = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set_src = VK_NULL_HANDLE, set_a = VK_NULL_HANDLE, set_b = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipeline trace = VK_NULL_HANDLE, blur = VK_NULL_HANDLE, comp = VK_NULL_HANDLE;
    // With the rain cover map: the trace pipeline is the RAIN_COVER variant, set 1 the cover's set.
    VkPipelineLayout trace_layout = VK_NULL_HANDLE;
    VkShaderModule trace_fs = VK_NULL_HANDLE;
    bool marked = false;     // renderer_rain_wet called in the frame being recorded
    RRainWet p{};            // its parameters
    uint32_t executed = 0;   // in the last frame
    // --reflections sssr (cfg.sssr, the march budget; 0 classic). Bindings 6 and 7
    // of every set: the min-depth hierarchy `hiz` (R32_SFLOAT, hiz_levels levels from the internal
    // size; classic: a 1x1 stand-in at both) and, in the temporal sets set_t[i] (reading a, writing
    // h[i]), the other history h[1 - i]; set_h[i] reads h[i] for the blur. The hierarchy's own pass
    // (hiz_pass), per-level views and framebuffers, its layout (binding 0: the depth for level 0, the
    // level above otherwise) and sets.
    int sssr = 0;
    Image hiz, h[2];
    int hiz_levels = 0, hiz_w[WET_HIZ_LEVELS]{}, hiz_h[WET_HIZ_LEVELS]{};
    VkImageView hiz_lv[WET_HIZ_LEVELS]{};
    VkFramebuffer hiz_fb[WET_HIZ_LEVELS]{}, fb_h[2]{};
    VkRenderPass hiz_pass = VK_NULL_HANDLE;
    VkDescriptorSetLayout hiz_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool hiz_pool = VK_NULL_HANDLE;
    VkDescriptorSet hiz_set[WET_HIZ_LEVELS]{}, set_t[2]{}, set_h[2]{};
    VkPipelineLayout hiz_layout = VK_NULL_HANDLE;
    VkShaderModule hiz_fs0 = VK_NULL_HANDLE, hiz_fs = VK_NULL_HANDLE;
    VkPipeline hiz_p0 = VK_NULL_HANDLE, hiz_p = VK_NULL_HANDLE, temporal = VK_NULL_HANDLE;
    int cur = 0;               // the history written this frame
    uint32_t frame = 0;        // SSSR frames so far (the noise's sequence)
    bool hist_ok = false;      // the other history holds the previous frame's result
    float prev_cam[3][4]{};    // that frame's camera axes with the eye in [3]
    float prev_p[2]{};         // its 1 / px, 1 / py
    // --reflections sssr's surface pass (cfg.sssr_glass; surf_record): the
    // frame's material draws (a specular map read with --dry-highlights' B shine and A gloss) again,
    // single-sample at the internal size, before the main pass: `surf` (RGBA8: the octahedral face
    // normal in view space, shine, gloss; 0 where none) and `surf_depth` (the character mask's format)
    // with their pass (both cleared), framebuffer, layout (set 0 the draws' set layout, the specular
    // map at binding 1; push constants 1 / px, 1 / py, the map's stage), shaders and pipeline.
    // Bindings 8 and 9 of the wet sets (the hierarchy's view stands in without it).
    Image surf, surf_depth;
    VkRenderPass surf_pass = VK_NULL_HANDLE;
    VkFramebuffer surf_fb = VK_NULL_HANDLE;
    VkPipelineLayout surf_layout = VK_NULL_HANDLE;
    VkShaderModule surf_vs = VK_NULL_HANDLE, surf_fs = VK_NULL_HANDLE;
    VkPipeline surf_pipe = VK_NULL_HANDLE;
    bool surf_live = false;      // the surfaces hold this frame's draws
    uint32_t surf_executed = 0;  // material draws in the last frame's surface pass
};

// Rain cover map (RendererConfig.rain_cover, renderer_rain_cover): the D16 map, its depth-only render
// pass (cleared, left shader-readable), framebuffer and pipeline (shaders/rain_cover.vert, push
// constants: the three clip -> map rows), and what the consumers bind (set layout: binding 0 the
// map, binding 1 the uniform buffer `ubo` with the view -> map rows and the test's constants,
// rewritten at the start of every frame; descriptor set `set`).
struct Cover {
    bool ok = false;
    Image map;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    Buffer ubo;
    bool marked = false;     // renderer_rain_cover called in the frame being recorded
    RRainCover p{};          // its parameters
    uint32_t executed = 0;   // maps drawn in the last frame
};

// The rain's character mask (renderer_rain_chars_on): a depth image at the internal size (`fmt`,
// single-sample, left shader-readable), its depth-only render pass (cleared), framebuffer and
// pipeline (shaders/char_mask.vert, the main pass's transform, dynamic viewport and scissor). The
// splash mask and the wet ground bind it at set 0, binding 2.
struct Chars {
    bool ok = false;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    Image depth;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    // --taa-upscale: the mask at the output size, unjittered, for the passes after the resolve (wet
    // ground, depth of field); `depth` then holds the scene's render-size mask for the passes before it.
    Image depth_up;
    VkFramebuffer fb_up = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    uint32_t executed = 0;   // character draws in the last frame's mask
};

// Frames in flight (RendererConfig.frames_in_flight): what a submitted frame reads until its fence
// signals. A slot is reused only after renderer_begin (or record_frame) has waited for that fence,
// so its buffers are rewritten, grown and its descriptor pool reset without waiting for the device.
const int MAX_FRAMES_IN_FLIGHT = 3;
static_assert(sizeof(Overlay::stamped) == MAX_FRAMES_IN_FLIGHT, "one overlay timestamp pair per frame slot");
struct FrameSlot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;                      // submitted, fence not waited yet
    VkSemaphore acquired = VK_NULL_HANDLE;     // swapchain image acquire (direct present)
    VkSemaphore acquired2 = VK_NULL_HANDLE;    // --frame-gen: the second image (the real frame after the generated one)
    VkDescriptorPool set_pool = VK_NULL_HANDLE;   // the frame's descriptor sets
    Buffer upload;    // base image upload (w * h * 4 bytes)
    Buffer geometry;  // the frame's vertices, then its indices (grown on demand)
    Buffer programs;  // the frame's RPixelShader records (grown on demand)
    Buffer movie;     // the frame's movie planes (renderer_movie_plane; grown on demand)
    // renderer_set_vertex_check: the frame's GPU-skinned draws, checked over the device's vertices
    // in geometry once the fence has been waited (wait_slot).
    std::vector<RVertexCheckDraw> vcheck;
    VkDeviceSize vcheck_index_offset = 0;
};

// --gpu-skinning (RendererConfig.gpu_skin, renderer_draw_skinned): the frame's vertex-program jobs
// and their inputs, run by a compute pre-pass at the start of the frame's commands that puts their
// vertices (and the lit draws' normals) into the frame's geometry buffer. Per frame slot: a
// host-visible staging buffer of [jobs | normal jobs | program words | constants | stream bytes |
// weld tables] (256-aligned sections), copied as one block into a device-local buffer that also
// holds [face normals | volume partial sums | vertices | normals]: the shaders only touch device
// memory (scattered reads and 4-byte writes over the bus to host-visible memory cost several ms),
// and the results reach the geometry buffer by copy, one region per draw. Also a descriptor pool
// for the frame's one set and two timestamps around the pre-pass.
struct SkinNormJob {   // shaders/vsh_skin_common.glsl NormJob
    uint32_t vfirst, nv, ifirst, ntri, group, start, corner, face, part, nparts;
    float px, py;
    uint32_t pad[4];
};
struct SkinSlot {
    Buffer buf;       // host-visible staging: sections 0-5
    Buffer dev;       // device-local: sections 0-5 (copied), then 6 faces, 7 sums, 8 vertices, 9 normals
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    bool stamped = false;   // its last frame wrote both timestamps
    VkDeviceSize off[10] = {}, len[10] = {}, staged = 0;
};
struct Skin {
    bool ok = false;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline skin = VK_NULL_HANDLE, faces = VK_NULL_HANDLE, normals = VK_NULL_HANDLE;
    SkinSlot slot[3];
    bool timestamps = false;
    std::vector<RVertexCheckDraw> vcheck;   // renderer_set_vertex_check: this frame's draws (renderer_draw_skinned)
    uint32_t draw_tag = 0;                  // renderer_set_draw_tag
    float ts_period = 1;   // ns per timestamp tick
    uint64_t ts_mask = ~0ull;
    // The frame being recorded.
    std::vector<RSkinJob> jobs;
    std::vector<SkinNormJob> njobs;
    std::vector<uint32_t> ins;      // 4 words per instruction
    std::vector<float> consts;      // 192 x 4 per job (one block for jobs passed the same constants)
    const float (*last_consts)[4] = nullptr;   // the constants of the last block, and its first register
    uint32_t last_consts_at = 0;
    std::vector<uint8_t> data;      // stream bytes, 4-aligned per job
    std::vector<uint32_t> adj;      // weld tables
    uint32_t nfaces = 0, nparts = 0, nverts = 0;   // device-local faces, sums, vertices of the frame
    std::vector<uint32_t> dst;      // per job: its first vertex in the frame (out_vertex: in the device buffer)
    std::vector<uint8_t> prev;      // per job: 1 = --taa's run with last frame's constants (not copied to the frame)
    size_t last_job = 0;            // the job of the last renderer_draw_skinned
    std::vector<uint32_t> ndst;     // per normal job: likewise
    std::unordered_map<const void *, uint32_t> prog_at;   // instruction words already copied this frame
    std::vector<VkBufferCopy> regions;
    VkDescriptorSet set = VK_NULL_HANDLE;
    // The last completed frame (renderer_skin_stats) and the debug readback (renderer_end).
    float gpu_ms = 0;
    uint32_t draws = 0, vertices = 0, rec_draws = 0, rec_vertices = 0;
    const RVertex *debug_verts = nullptr;
    const float *debug_normals = nullptr;
};

struct State {
    void *lib = nullptr;
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memprops{};
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfamily = 0;
    int w = 0, h = 0;      // back-buffer size: the caller's viewports and base image
    int ow = 0, oh = 0;    // internal render size: the output size times the supersampling factor
    int fw = 0, fh = 0;    // output size of the scene passes (the readback and present source without FSR)
    int dispw = 0, disph = 0;   // displayed size (renderer_output_size, the readback and present source): s.fw x s.fh, or FSR's output
    int ss = 1;            // supersampling factor in use (1, 2, 4)
    Image down[2];         // supersampling: the 2:1 downsample chain (the last one is s.fw x s.fh)
    VkSampler nearest_sampler = VK_NULL_HANDLE;   // the base quad when supersampled at the back-buffer size
    bool base_nearest = false;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    float aniso = 1.0f;
    float lod_bias = 0.0f;   // the game textures' sampler (renderer_texture_lod_bias)
    bool quad_base = false;   // the base image is drawn as a quad (scaled target or MSAA)
    bool base_pending = false;
    Image color, depth;
    Image msaa;               // multisampled color (samples > 1), resolved into color
    Image base;               // base image source of the quad (quad_base)
    VkSampler clamp_sampler = VK_NULL_HANDLE;
    // Direct present: window surface, swapchain and its images, one acquire semaphore per frame
    // slot (FrameSlot.acquired: the slot's fence is waited before its next acquire) and one
    // render-done semaphore per image.
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> sc_images;
    std::vector<VkSemaphore> sc_done;
    VkExtent2D sc_extent{};
    VkFormat sc_format = VK_FORMAT_UNDEFINED;
    VkPresentModeKHR sc_mode = VK_PRESENT_MODE_FIFO_KHR;
    bool sc_stale = false;   // recreate before the next acquire (resize, suboptimal)
    char present_info[96] = "";
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    bool depth_clamp = false;
    bool eds = false;   // VK_EXT_extended_dynamic_state in use (eds_key)
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    // Variants of pass that store everything and reload it, around a split of the frame's pass
    // (glass, bloom); created on first use (split_passes_ready).
    VkRenderPass split_pass = VK_NULL_HANDLE, resume_pass = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;   // being recorded: the current slot's, or once_cmd
    FrameSlot frames[MAX_FRAMES_IN_FLIGHT];
    int nframes = 1;                        // frame slots in use (frames in flight)
    int slot = 0;                           // the current frame's slot
    double wait_ms = 0;                     // this frame's wait for its slot's fence
    // One-time submissions (texture uploads, new surfaces), waited for at once.
    VkCommandBuffer once_cmd = VK_NULL_HANDLE;
    VkFence once_fence = VK_NULL_HANDLE;
    Buffer readback;  // frame readback (w * h * 4 bytes), host-cached when the device offers it
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipelineCache pcache = VK_NULL_HANDLE;   // every pipeline's (create_pipeline_cache); none: --no-pipeline-cache
    // By shape << 32 | pipeline_key; VK_NULL_HANDLE = failed.
    std::unordered_map<uint64_t, VkPipeline> pipelines;
    std::vector<Shape> shapes;                    // program structures seen so far (kept until shutdown)
    std::unordered_map<uint64_t, uint32_t> shape_by_hash;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    std::vector<RPixelShader> programs;          // index 0 = the default program
    std::unordered_map<uint64_t, uint32_t> program_by_hash;
    uint32_t cur_program = 0, cur_shape = 0;
    uint32_t skipped_sets = 0;
    VkSampler sampler = VK_NULL_HANDLE;
    bool bc = false;               // BC1-3 sampled images supported and enabled
    Texture white;                 // bound for untextured draws
    std::vector<Texture> textures; // handle = index + 1
    std::unordered_map<uint64_t, uint32_t> by_key;
    VkDeviceSize texture_bytes = 0;
    uint32_t cur_tex[RENDERER_STAGES] = {};
    // Movie planes (renderer_movie_plane): persistent L8 images outside the texture cache, rewritten
    // from the frame slot's movie buffer at the start of each frame that has new plane data.
    Image movie[MOVIE_PLANES];
    // renderer_prev_frame: the last frame's colour (s.color copied at the end of the main pass),
    // sampled through a view whose alpha reads 1. serial: the frame it holds (0 = none); asked: the
    // last frame that asked for it (a frame copies while it or the one before it asked).
    struct {
        Image im;
        uint64_t serial = 0, asked = 0;
        bool marked = false;   // renderer_prev_frame_mark in this frame (an OP_GRAB)
    } prev;
    uint64_t frame_serial = 0;   // renderer_begin calls
    uint32_t movie_w[MOVIE_PLANES] = {}, movie_h[MOVIE_PLANES] = {};
    VkDeviceSize movie_off[MOVIE_PLANES] = {};
    uint32_t movie_pending = 0;   // planes with data in cur().movie for the frame being recorded
    uint32_t movie_uploads = 0;
    uint32_t cur_alpha[2] = {};
    RState cur_state{};
    std::vector<Surface> surfaces;
    std::unordered_map<uint64_t, uint32_t> surface_by_key;
    std::unordered_map<uint64_t, OffscreenFb> offscreen_fbs;   // by color handle << 16 | depth handle
    VkRenderPass off_pass[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // pass classes 1 (D16) and 2 (24-bit)
    VkFormat depth24_format = VK_FORMAT_UNDEFINED;   // sampled + attachment, for D24 surfaces
    VkSampler border_sampler = VK_NULL_HANDLE;       // surfaces: linear, white border
    uint32_t max_dim = 4096;
    uint32_t cur_target[2] = {};
    RPixelShader cur_ps{};
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE + 32] = "";   // "<device> (driver X.Y.Z)"
    char error[256] = "";
    std::vector<Op> ops;   // current frame
    VertexSink verts;
    std::vector<uint32_t> indices;
    std::vector<float> normals;   // renderer_draw_lit: 3 per vertex, parallel to verts up to the last lit draw
    RendererTiming timing{};
    Glass glass;
    Sharpen sharp;
    Fsr fsr;
    Gamma gamma;
    Overlay ov;
    GpuProf gp;
    Bloom bloom;
    Filmic filmic;
    Dof dof;
    Taa taa;
    FrameGen fg;
    Rain rain;
    Sky sky;
    Ssao ssao;
    Splash splash;
    Wet wet;
    Cover cover;
    Chars chars;
    Soft soft;
    Lamp lamp;
    Shaft shaft;
    Volfog volfog;
    Contact contact;
    CharLit charlit;
    LampShadow lshadow;
    WallMap walls;
    WorldShadow wshadow;
    Emit emit;
    Relief relief;
    Hair hair;
    SkinLit skinlit;
    Skin skin;
    bool depth_sampled = false;   // the depth target is also sampled (ssao configured and supported)
};
State s;
RendererConfig cfg = {0, 0, 1, 0};   // renderer_configure; survives renderer_shutdown

FrameSlot &cur() { return s.frames[s.slot]; }
// RendererConfig.frames_in_flight: 0 = the default (2), clamped to 1..MAX_FRAMES_IN_FLIGHT.
int configured_frames() {
    int n = cfg.frames_in_flight ? cfg.frames_in_flight : 2;
    return n < 1 ? 1 : n > MAX_FRAMES_IN_FLIGHT ? MAX_FRAMES_IN_FLIGHT : n;
}

// Texture handle of the base image in a draw's stage list (never a cache handle: MAX_TEXTURES).
const uint32_t BASE_TEXTURE = 0xFFFF;

double now_ms() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

// [vk-error] (diaglog.h): every VkResult-returning call goes through VKC, so a non-success result
// the code below ignores or handles quietly (fail() only keeps a message) is logged, rate-limited.
const char *vk_result_name(VkResult r) {
    switch (r) {
#define N(x) case x: return #x;
    N(VK_SUCCESS) N(VK_NOT_READY) N(VK_TIMEOUT) N(VK_EVENT_SET) N(VK_EVENT_RESET) N(VK_INCOMPLETE)
    N(VK_ERROR_OUT_OF_HOST_MEMORY) N(VK_ERROR_OUT_OF_DEVICE_MEMORY) N(VK_ERROR_INITIALIZATION_FAILED)
    N(VK_ERROR_DEVICE_LOST) N(VK_ERROR_MEMORY_MAP_FAILED) N(VK_ERROR_LAYER_NOT_PRESENT) N(VK_ERROR_EXTENSION_NOT_PRESENT)
    N(VK_ERROR_FEATURE_NOT_PRESENT) N(VK_ERROR_INCOMPATIBLE_DRIVER) N(VK_ERROR_TOO_MANY_OBJECTS)
    N(VK_ERROR_FORMAT_NOT_SUPPORTED) N(VK_ERROR_FRAGMENTED_POOL) N(VK_ERROR_UNKNOWN) N(VK_ERROR_OUT_OF_POOL_MEMORY)
    N(VK_ERROR_INVALID_EXTERNAL_HANDLE) N(VK_ERROR_FRAGMENTATION) N(VK_PIPELINE_COMPILE_REQUIRED)
    N(VK_ERROR_SURFACE_LOST_KHR) N(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) N(VK_SUBOPTIMAL_KHR) N(VK_ERROR_OUT_OF_DATE_KHR)
    N(VK_ERROR_INCOMPATIBLE_DISPLAY_KHR) N(VK_ERROR_VALIDATION_FAILED_EXT)
#undef N
    default: return "VK_RESULT_UNKNOWN";
    }
}
VkResult vk_check(const char *call, VkResult r) {
    if (r != VK_SUCCESS)
        dlog_vk_error(call, (int)r, vk_result_name(r), r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR, r == VK_ERROR_DEVICE_LOST);
    return r;
}
#define VKC(fn, ...) vk_check(#fn, fn(__VA_ARGS__))

int fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s.error, sizeof s.error, fmt, ap);
    va_end(ap);
    return -1;
}

int find_memory(uint32_t type_bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < s.memprops.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) && (s.memprops.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

// Counts an allocation of memory type `type` in vram_now when its heap is device-local.
void vram_add(int type, VkDeviceSize bytes, bool attachment, VkDeviceSize *vram, int *cat) {
    *vram = 0;
    *cat = mem_tag >= 0 ? mem_tag : attachment ? RENDERER_VRAM_EFFECTS : RENDERER_VRAM_OTHER;
    if (type < 0 || !(s.memprops.memoryHeaps[s.memprops.memoryTypes[type].heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) return;
    *vram = bytes;
    vram_now[*cat] += bytes;
    if (vram_now[*cat] > vram_peak[*cat]) vram_peak[*cat] = vram_now[*cat];
    uint64_t total = 0;
    for (int c = 0; c < RENDERER_VRAM_CATS; c++) total += vram_now[c];
    if (total > vram_peak_total) vram_peak_total = total;
}
void vram_sub(VkDeviceSize vram, int cat) { vram_now[cat] -= vram < vram_now[cat] ? vram : vram_now[cat]; }

// Host-visible, coherent buffer, mapped. cached: prefer a host-cached memory type, for buffers the
// CPU reads (the frame readback): reading uncached, write-combined memory costs about 25 ms per
// 640x480 frame on an NVIDIA GPU. device: prefer video memory the
// CPU can write (a discrete GPU's BAR window, all of it with resizable BAR), for buffers the CPU
// only writes and the GPU reads many times (the frame's geometry: every pass that draws it fetches
// its vertices over PCIe from system memory otherwise); taken only
// when its heap is at least 4 times the size, and system memory when that allocation fails.
int make_buffer(Buffer *b, VkDeviceSize size, VkBufferUsageFlags usage, bool cached = false, bool device = false) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (VKC(vkCreateBuffer, s.dev, &bi, nullptr, &b->buf) != VK_SUCCESS) return fail("vkCreateBuffer failed");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(s.dev, b->buf, &req);
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    int type = -1;
    if (device) {
        type = find_memory(req.memoryTypeBits, host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type >= 0 && s.memprops.memoryHeaps[s.memprops.memoryTypes[type].heapIndex].size / 4 < req.size) type = -1;
        if (type >= 0) {
            ai.memoryTypeIndex = (uint32_t)type;
            if (vkAllocateMemory(s.dev, &ai, nullptr, &b->mem) != VK_SUCCESS) {   // the window is full: system memory
                b->mem = VK_NULL_HANDLE;
                type = -1;
            }
        }
    }
    if (!b->mem) {
        if (type < 0 && cached) type = find_memory(req.memoryTypeBits, host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (type < 0) type = find_memory(req.memoryTypeBits, host);
        if (type < 0) return fail("no host-visible coherent memory type");
        ai.memoryTypeIndex = (uint32_t)type;
        if (VKC(vkAllocateMemory, s.dev, &ai, nullptr, &b->mem) != VK_SUCCESS) return fail("vkAllocateMemory (buffer) failed");
    }
    vram_add(type, req.size, false, &b->vram, &b->cat);
    VKC(vkBindBufferMemory, s.dev, b->buf, b->mem, 0);
    if (VKC(vkMapMemory, s.dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) != VK_SUCCESS) return fail("vkMapMemory failed");
    b->size = size;
    return 0;
}

void free_buffer(Buffer *b) {
    vram_sub(b->vram, b->cat);
    if (b->map) vkUnmapMemory(s.dev, b->mem);
    if (b->buf) vkDestroyBuffer(s.dev, b->buf, nullptr);
    if (b->mem) vkFreeMemory(s.dev, b->mem, nullptr);
    *b = Buffer();
}

int make_image(Image *im, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t w, uint32_t h,
               uint32_t levels = 1, VkDeviceSize *bytes = nullptr, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, 1};
    ii.mipLevels = levels;
    ii.arrayLayers = 1;
    ii.samples = samples;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (VKC(vkCreateImage, s.dev, &ii, nullptr, &im->img) != VK_SUCCESS) return fail("vkCreateImage failed (format %d)", fmt);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(s.dev, im->img, &req);
    int type = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = find_memory(req.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)type;
    if (bytes) *bytes = req.size;
    if (type < 0 || VKC(vkAllocateMemory, s.dev, &ai, nullptr, &im->mem) != VK_SUCCESS) return fail("vkAllocateMemory (image) failed");
    vram_add(type, req.size,
             (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0, &im->vram, &im->cat);
    VKC(vkBindImageMemory, s.dev, im->img, im->mem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im->img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, levels, 0, 1};
    if (VKC(vkCreateImageView, s.dev, &vi, nullptr, &im->view) != VK_SUCCESS) return fail("vkCreateImageView failed");
    return 0;
}

void free_image(Image *im) {
    vram_sub(im->vram, im->cat);
    if (im->view) vkDestroyImageView(s.dev, im->view, nullptr);
    if (im->img) vkDestroyImage(s.dev, im->img, nullptr);
    if (im->mem) vkFreeMemory(s.dev, im->mem, nullptr);
    *im = Image();
}

int load_loader() {
    s.lib = plat_dlopen(PLAT_LIB_VULKAN);
    if (!s.lib) return fail("cannot load %s: %s", PLAT_LIB_VULKAN, plat_dlerror());
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)plat_dlsym(s.lib, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr) return fail("%s has no vkGetInstanceProcAddr", PLAT_LIB_VULKAN);
#define LOAD_GLOBAL(fn) if (!(fn = (PFN_##fn)vkGetInstanceProcAddr(VK_NULL_HANDLE, #fn))) return fail("missing " #fn);
    VK_GLOBAL_FNS(LOAD_GLOBAL)
    return 0;
}

int create_instance() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "xbrun";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    bool want_surface = cfg.create_surface && cfg.instance_ext_count;
    // VK_KHR_get_physical_device_properties2 when the loader has it: VK_EXT_memory_budget's query.
    std::vector<const char *> ext;
    if (want_surface) ext.assign(cfg.instance_ext, cfg.instance_ext + cfg.instance_ext_count);
    bool props2 = false;
    auto enum_inst = (PFN_vkEnumerateInstanceExtensionProperties)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    uint32_t ne = 0;
    if (enum_inst && enum_inst(nullptr, &ne, nullptr) == VK_SUCCESS) {
        std::vector<VkExtensionProperties> have(ne);
        if (enum_inst(nullptr, &ne, have.data()) == VK_SUCCESS)
            for (uint32_t i = 0; i < ne; i++) props2 |= !strcmp(have[i].extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    }
    if (props2) ext.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    ci.enabledExtensionCount = (uint32_t)ext.size();
    ci.ppEnabledExtensionNames = ext.data();
    VkResult r = VKC(vkCreateInstance, &ci, nullptr, &s.inst);
    if (r != VK_SUCCESS && want_surface) {   // the window system's extensions are missing: offscreen
        snprintf(s.present_info, sizeof s.present_info, "instance without the window extensions (%d)", r);
        want_surface = false;
        ext.erase(ext.begin(), ext.begin() + cfg.instance_ext_count);
        ci.enabledExtensionCount = (uint32_t)ext.size();
        ci.ppEnabledExtensionNames = ext.data();
        r = VKC(vkCreateInstance, &ci, nullptr, &s.inst);
    }
    if (r != VK_SUCCESS) return fail("vkCreateInstance failed (%d)", r);
    get_memprops2 = props2 ? (PFN_vkGetPhysicalDeviceMemoryProperties2KHR)vkGetInstanceProcAddr(s.inst, "vkGetPhysicalDeviceMemoryProperties2KHR") : nullptr;
    get_features2 = props2 ? (PFN_vkGetPhysicalDeviceFeatures2KHR)vkGetInstanceProcAddr(s.inst, "vkGetPhysicalDeviceFeatures2KHR") : nullptr;
    memory_budget_ext = false;
#define LOAD_INSTANCE(fn) if (!(fn = (PFN_##fn)vkGetInstanceProcAddr(s.inst, #fn))) return fail("missing " #fn);
    VK_INSTANCE_FNS(LOAD_INSTANCE)
#define LOAD_OPTIONAL(fn) fn = (PFN_##fn)vkGetInstanceProcAddr(s.inst, #fn);
    VK_SURFACE_FNS(LOAD_OPTIONAL)
    if (!want_surface) return 0;
#define HAVE(fn) &&fn
    uint64_t surface = 0;
    if (!(true VK_SURFACE_FNS(HAVE)))
        snprintf(s.present_info, sizeof s.present_info, "loader without the surface entry points");
    else if (cfg.create_surface(s.inst, &surface) || !surface)
        snprintf(s.present_info, sizeof s.present_info, "window surface creation failed");
    else
        memcpy(&s.surface, &surface, sizeof s.surface);   // a 64-bit non-dispatchable handle
    return 0;
}

// Releases the window surface (and swapchain); the renderer stays usable offscreen. Returns -1 with
// the error (s.error) kept as the reason.
// --fps-overlay: the swapchain images' views and framebuffers (remade at the next frame).
void overlay_sc_free() {
    Overlay &v = s.ov;
    for (VkFramebuffer fb : v.sc_fbs)
        if (fb) vkDestroyFramebuffer(s.dev, fb, nullptr);
    for (VkImageView iv : v.sc_views)
        if (iv) vkDestroyImageView(s.dev, iv, nullptr);
    v.sc_fbs.clear();
    v.sc_views.clear();
}

int drop_present() {
    if (s.dev) VKC(vkDeviceWaitIdle, s.dev);
    overlay_sc_free();
    for (VkSemaphore sem : s.sc_done) vkDestroySemaphore(s.dev, sem, nullptr);
    s.sc_done.clear();
    s.sc_images.clear();
    for (FrameSlot &f : s.frames) {
        if (f.acquired) vkDestroySemaphore(s.dev, f.acquired, nullptr);
        if (f.acquired2) vkDestroySemaphore(s.dev, f.acquired2, nullptr);
        f.acquired = f.acquired2 = VK_NULL_HANDLE;
    }
    if (s.swapchain) vkDestroySwapchainKHR(s.dev, s.swapchain, nullptr);
    s.swapchain = VK_NULL_HANDLE;
    if (s.surface) vkDestroySurfaceKHR(s.inst, s.surface, nullptr);
    s.surface = VK_NULL_HANDLE;
    snprintf(s.present_info, sizeof s.present_info, "%s", s.error);
    return -1;
}

const char *format_name(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB (gamma applied twice)";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB (gamma applied twice)";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM";
    default: return "other format";
    }
}

// (Re)creates the swapchain at the surface's current size. A zero size (minimized window) leaves
// no swapchain and retries at the next frame. Returns -1 on failure (s.error).
int create_swapchain() {
    s.sc_stale = false;
    VkSurfaceCapabilitiesKHR caps;
    if (VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, s.phys, s.surface, &caps) != VK_SUCCESS) return fail("cannot query the surface");
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {   // the swapchain decides: the window's drawable size
        int w = s.dispw, h = s.disph;
        if (cfg.drawable_size) cfg.drawable_size(&w, &h);
        ext.width = (uint32_t)(w > 0 ? w : 0);
        ext.height = (uint32_t)(h > 0 ? h : 0);
        if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
        if (ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    VKC(vkDeviceWaitIdle, s.dev);
    overlay_sc_free();   // views of the old images go before the old swapchain
    if (!ext.width || !ext.height) {
        s.sc_stale = true;
        if (s.swapchain) vkDestroySwapchainKHR(s.dev, s.swapchain, nullptr);
        s.swapchain = VK_NULL_HANDLE;
        return 0;
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return fail("swapchain images cannot be transfer targets");
    uint32_t n = 0;
    VKC(vkGetPhysicalDeviceSurfaceFormatsKHR, s.phys, s.surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    VKC(vkGetPhysicalDeviceSurfaceFormatsKHR, s.phys, s.surface, &n, formats.data());
    // UNORM first: the frame is already display-encoded, an sRGB image would encode it again.
    const VkFormat prefs[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                              VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB};
    const VkSurfaceFormatKHR *pick = nullptr;
    for (VkFormat want : prefs)
        for (const VkSurfaceFormatKHR &f : formats) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(s.phys, f.format, &fp);
            if (!pick && f.format == want && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) pick = &f;
        }
    if (!pick) return fail("no surface format the renderer can blit to (%u formats)", n);
    // Present mode: the preference when the surface has it, else FIFO (always supported).
    VKC(vkGetPhysicalDeviceSurfacePresentModesKHR, s.phys, s.surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    VKC(vkGetPhysicalDeviceSurfacePresentModesKHR, s.phys, s.surface, &n, modes.data());
    VkPresentModeKHR want = cfg.present_mode == RENDERER_PRESENT_FIFO        ? VK_PRESENT_MODE_FIFO_KHR
                            : cfg.present_mode == RENDERER_PRESENT_IMMEDIATE ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                                             : VK_PRESENT_MODE_MAILBOX_KHR;
    if (s.fg.ok) want = VK_PRESENT_MODE_FIFO_KHR;   // --frame-gen: FIFO keeps its two presents a refresh apart
    s.sc_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR m : modes)
        if (m == want) s.sc_mode = m;
    uint32_t count = caps.minImageCount + 1;
    if (s.fg.ok) count = caps.minImageCount + 2 < 4 ? 4 : caps.minImageCount + 2;   // two images held per frame
    // With present waits one more: the real frame on screen, the pair queued after it and the next
    // frame's pair, so the next frame's images are free when it takes them.
    if (s.fg.ok && s.fg.pw) count = caps.minImageCount + 3 < 5 ? 5 : caps.minImageCount + 3;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = s.surface;
    ci.minImageCount = count;
    ci.imageFormat = pick->format;
    ci.imageColorSpace = pick->colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    if (s.ov.ok) {   // --fps-overlay draws on the image after the blit
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(s.phys, pick->format, &fp);
        s.ov.sc_color = (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
                        (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
    }
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (s.ov.sc_color ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & ci.compositeAlpha))
        for (uint32_t b = 1; b; b <<= 1)
            if (caps.supportedCompositeAlpha & b) { ci.compositeAlpha = (VkCompositeAlphaFlagBitsKHR)b; break; }
    ci.presentMode = s.sc_mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = s.swapchain;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = VKC(vkCreateSwapchainKHR, s.dev, &ci, nullptr, &sc);
    if (s.swapchain) vkDestroySwapchainKHR(s.dev, s.swapchain, nullptr);
    s.swapchain = VK_NULL_HANDLE;
    if (r != VK_SUCCESS) return fail("vkCreateSwapchainKHR failed (%d)", r);
    s.swapchain = sc;
    s.sc_extent = ext;
    s.fg.pw_id = s.fg.pw_last = s.fg.pw_prev = 0;   // present ids count per swapchain
    dlog_crumb("swapchain %s %ux%u", ci.oldSwapchain ? "recreate" : "create", ext.width, ext.height);
    s.sc_format = pick->format;
    VKC(vkGetSwapchainImagesKHR, s.dev, sc, &n, nullptr);
    s.sc_images.resize(n);
    VKC(vkGetSwapchainImagesKHR, s.dev, sc, &n, s.sc_images.data());
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    while (s.sc_done.size() < n) {
        VkSemaphore sem;
        if (VKC(vkCreateSemaphore, s.dev, &sci, nullptr, &sem) != VK_SUCCESS) return fail("vkCreateSemaphore failed");
        s.sc_done.push_back(sem);
    }
    for (int i = 0; i < s.nframes; i++) {
        if (!s.frames[i].acquired && VKC(vkCreateSemaphore, s.dev, &sci, nullptr, &s.frames[i].acquired) != VK_SUCCESS)
            return fail("vkCreateSemaphore failed");
        if (s.fg.ok && !s.frames[i].acquired2 && VKC(vkCreateSemaphore, s.dev, &sci, nullptr, &s.frames[i].acquired2) != VK_SUCCESS)
            return fail("vkCreateSemaphore failed");
    }
    const char *mode = s.sc_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : s.sc_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo";
    snprintf(s.present_info, sizeof s.present_info, "swapchain %ux%u %s %s, %u images", ext.width, ext.height, format_name(s.sc_format), mode, n);
    return 0;
}

// Whether a device offers an extension.
bool has_device_ext(VkPhysicalDevice d, const char *name) {
    if (!vkEnumerateDeviceExtensionProperties) return false;
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(d, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> e(n);
    vkEnumerateDeviceExtensionProperties(d, nullptr, &n, e.data());
    for (const VkExtensionProperties &x : e)
        if (!strcmp(x.extensionName, name)) return true;
    return false;
}
bool has_swapchain_ext(VkPhysicalDevice d) { return has_device_ext(d, VK_KHR_SWAPCHAIN_EXTENSION_NAME); }

// First device with a graphics queue, discrete GPUs before others; with a window surface, one
// whose graphics queue can present to it (else the surface is released: offscreen).
int pick_device() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(s.inst, &n, nullptr);
    if (!n) return fail("no Vulkan physical device");
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(s.inst, &n, devs.data());
    for (int pass = 0; pass < 4 && !s.phys; pass++) {
        if (pass == 2 && s.surface) {
            fail("no device can present to the window");
            drop_present();
        }
        for (VkPhysicalDevice d : devs) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(d, &p);
            if (pass % 2 == 0 && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
            if (pass < 2 && (!s.surface || !has_swapchain_ext(d))) continue;
            uint32_t nq = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, nullptr);
            std::vector<VkQueueFamilyProperties> qs(nq);
            vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, qs.data());
            for (uint32_t q = 0; q < nq; q++) {
                if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                VkBool32 present = VK_TRUE;
                if (pass < 2 && (vkGetPhysicalDeviceSurfaceSupportKHR(d, q, s.surface, &present) != VK_SUCCESS || !present)) continue;
                s.phys = d;
                s.qfamily = q;
                // The driver version as the vendor numbers it: NVIDIA 10/8/8/6 bits, Intel on
                // Windows 18.14, everyone else (Mesa, AMD) the Vulkan 10.10.12 encoding.
                uint32_t v = p.driverVersion, a = v >> 22, b = (v >> 12) & 0x3ff, c = v & 0xfff;
                if (p.vendorID == 0x10de) b = (v >> 14) & 0xff, c = (v >> 6) & 0xff;
#ifdef _WIN32
                if (p.vendorID == 0x8086) a = v >> 14, b = v & 0x3fff, c = 0;
#endif
                snprintf(s.name, sizeof s.name, "%s (driver %u.%u.%u)", p.deviceName, a, b, c);
                break;
            }
            if (s.phys) break;
        }
    }
    if (!s.phys) return fail("no Vulkan device with a graphics queue");
    vkGetPhysicalDeviceMemoryProperties(s.phys, &s.memprops);
    return 0;
}

int create_device() {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = s.qfamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkPhysicalDeviceFeatures have{}, want{};
    vkGetPhysicalDeviceFeatures(s.phys, &have);
    want.depthClamp = have.depthClamp;
    s.depth_clamp = have.depthClamp;
    // BC1-3 (DXT1/3/5): the feature implies sampling support for all BC formats.
    want.textureCompressionBC = have.textureCompressionBC;
    s.bc = have.textureCompressionBC;
    // MSAA: the largest supported count up to the request; anisotropy up to the device limit.
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(s.phys, &props);
    VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
    s.max_dim = props.limits.maxImageDimension2D;
    s.samples = VK_SAMPLE_COUNT_1_BIT;
    for (uint32_t n = 2; n <= 64 && (int)n <= cfg.samples; n *= 2)
        if (counts & n) s.samples = (VkSampleCountFlagBits)n;
    if (cfg.aniso > 1 && have.samplerAnisotropy) {
        want.samplerAnisotropy = VK_TRUE;
        s.aniso = (float)cfg.aniso < props.limits.maxSamplerAnisotropy ? (float)cfg.aniso : props.limits.maxSamplerAnisotropy;
    }
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.pEnabledFeatures = &want;
    const char *dev_ext[5];
    uint32_t n_ext = 0;
    if (s.surface) dev_ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    memory_budget_ext = get_memprops2 && has_device_ext(s.phys, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (memory_budget_ext) dev_ext[n_ext++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    s.eds = false;
    if (!cfg.no_dynamic_state && get_features2 && has_device_ext(s.phys, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &eds;
        get_features2(s.phys, &f2);
        if (eds.extendedDynamicState) {
            eds.pNext = nullptr;
            di.pNext = &eds;
            dev_ext[n_ext++] = VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME;
            s.eds = true;
        }
    }
    // --frame-gen with a window: present ids and waits, to pace the two presents a frame by the display.
    VkPhysicalDevicePresentIdFeaturesKHR pid{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDevicePresentWaitFeaturesKHR pwf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    s.fg.pw = false;
    if (cfg.frame_gen && s.surface && get_features2 && has_device_ext(s.phys, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
        has_device_ext(s.phys, VK_KHR_PRESENT_WAIT_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &pid;
        pid.pNext = &pwf;
        get_features2(s.phys, &f2);
        if (pid.presentId && pwf.presentWait) {
            pwf.pNext = (void *)di.pNext;
            pid.pNext = &pwf;
            di.pNext = &pid;
            dev_ext[n_ext++] = VK_KHR_PRESENT_ID_EXTENSION_NAME;
            dev_ext[n_ext++] = VK_KHR_PRESENT_WAIT_EXTENSION_NAME;
            s.fg.pw = true;
        }
    }
    di.enabledExtensionCount = n_ext;
    di.ppEnabledExtensionNames = n_ext ? dev_ext : nullptr;
    budget_heap = -1;
    for (uint32_t h = 0; h < s.memprops.memoryHeapCount; h++)
        if ((s.memprops.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            (budget_heap < 0 || s.memprops.memoryHeaps[h].size > s.memprops.memoryHeaps[budget_heap].size))
            budget_heap = (int)h;
    VkResult r = VKC(vkCreateDevice, s.phys, &di, nullptr, &s.dev);
    if (r != VK_SUCCESS) return fail("vkCreateDevice failed (%d)", r);
#define LOAD_DEVICE(fn) if (!(fn = (PFN_##fn)vkGetDeviceProcAddr(s.dev, #fn))) return fail("missing " #fn);
    VK_DEVICE_FNS(LOAD_DEVICE)
#define LOAD_DEVICE_OPTIONAL(fn) fn = (PFN_##fn)vkGetDeviceProcAddr(s.dev, #fn);
    VK_SWAPCHAIN_FNS(LOAD_DEVICE_OPTIONAL)
    if (s.eds) {
        cmd_set_cull_mode = (PFN_vkCmdSetCullModeEXT)vkGetDeviceProcAddr(s.dev, "vkCmdSetCullModeEXT");
        cmd_set_depth_test = (PFN_vkCmdSetDepthTestEnableEXT)vkGetDeviceProcAddr(s.dev, "vkCmdSetDepthTestEnableEXT");
        cmd_set_depth_write = (PFN_vkCmdSetDepthWriteEnableEXT)vkGetDeviceProcAddr(s.dev, "vkCmdSetDepthWriteEnableEXT");
        cmd_set_depth_compare = (PFN_vkCmdSetDepthCompareOpEXT)vkGetDeviceProcAddr(s.dev, "vkCmdSetDepthCompareOpEXT");
        s.eds = cmd_set_cull_mode && cmd_set_depth_test && cmd_set_depth_write && cmd_set_depth_compare;
    }
    wait_for_present = s.fg.pw ? (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(s.dev, "vkWaitForPresentKHR") : nullptr;
    s.fg.pw = wait_for_present != nullptr;
    if (s.surface && !(true VK_SWAPCHAIN_FNS(HAVE))) {
        fail("device without the swapchain entry points");
        drop_present();
    }
    vkGetDeviceQueue(s.dev, s.qfamily, 0, &s.queue);
    return 0;
}

// The frame's render pass (create_targets) and the glass variants, which are compatible with it
// (same attachments and subpass): SPLIT_PASS starts like MAIN_PASS but stores color (the
// multisampled one too) and depth, RESUME_PASS reloads them and stores them again.
enum { MAIN_PASS, SPLIT_PASS, RESUME_PASS, SOFT_PASS };
int create_main_pass(VkRenderPass *out, int variant) {
    // Copy path: color loaded (base image or black, uploaded by a transfer) and stored for the
    // readback. Quad path: color cleared to black (the base quad covers it), with MSAA in the
    // multisampled image that is resolved into color (attachment 2). Depth: cleared to 1.0 at the
    // start of every frame.
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    VkAttachmentDescription att[3]{};
    att[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    att[0].samples = s.samples;
    att[0].loadOp = s.quad_base ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    att[0].storeOp = ms ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = s.quad_base ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[0].finalLayout = ms ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    att[1].format = s.depth_format;
    att[1].samples = s.samples;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att[2] = att[0];
    att[2].samples = VK_SAMPLE_COUNT_1_BIT;
    att[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[2].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (variant != MAIN_PASS) {   // glass split: everything stored; resume: reloaded where it was left
        att[0].storeOp = att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    if (variant == RESUME_PASS || variant == SOFT_PASS) {
        att[0].loadOp = att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[2].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    // Soft particles: the depth is tested and sampled, never written, in the subpass (the pass
    // moves it to the read-only layout and back).
    VkAttachmentReference dref{1, variant == SOFT_PASS ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference rref{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pResolveAttachments = ms ? &rref : nullptr;
    sub.pDepthStencilAttachment = &dref;
    // In: after earlier work on the targets, including the previous frame's (still in flight with
    // more than one frame in flight): its attachment writes, the copies and blits reading color and
    // the base image upload. Out: the final layout transition (and resolve) before the readback
    // copy, the swapchain blit, the glass copy and a later frame's passes.
    VkSubpassDependency dep[2]{};
    const VkPipelineStageFlags att_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags att_access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = att_stages | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    dep[0].dstStageMask = att_stages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].dstAccessMask = att_access | VK_ACCESS_SHADER_READ_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = att_stages;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].dstStageMask = att_stages | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].dstAccessMask = att_access | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = ms ? 3 : 2;
    rp.pAttachments = att;
    rp.dependencyCount = 2;
    rp.pDependencies = dep;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    if (VKC(vkCreateRenderPass, s.dev, &rp, nullptr, out) != VK_SUCCESS) return fail("vkCreateRenderPass failed");
    return 0;
}

// --taa-upscale (RendererConfig.taa_up_w): the render size asked for, when usable: TAA on, no FSR 1,
// at most the output size on both axes and below it on one.
static bool taa_up_wanted(int *rw, int *rh) {
    if (!cfg.taa || s.fsr.req || cfg.taa_up_w <= 0 || cfg.taa_up_h <= 0 || cfg.taa_up_w > s.fw || cfg.taa_up_h > s.fh ||
        (cfg.taa_up_w == s.fw && cfg.taa_up_h == s.fh))
        return false;
    if (rw) *rw = cfg.taa_up_w;
    if (rh) *rh = cfg.taa_up_h;
    return true;
}

int create_targets() {
    MemTag tag(RENDERER_VRAM_SCENE);
    const VkFormat depths[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM};
    for (VkFormat f : depths) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(s.phys, f, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) { s.depth_format = f; break; }
    }
    if (s.depth_format == VK_FORMAT_UNDEFINED) return fail("no depth attachment format");
    // Sampled 24-bit (or better) depth for the title's D24 surfaces; D16_UNORM (required for both
    // uses) otherwise.
    const VkFormat depths24[] = {VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM};
    for (VkFormat f : depths24) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(s.phys, f, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((fp.optimalTilingFeatures & need) == need) { s.depth24_format = f; break; }
    }
    // Supersampling: 1, 2 or 4, halved while the internal size exceeds the device limit.
    s.ss = s.fsr.req || taa_up_wanted(nullptr, nullptr) ? 1 : cfg.supersample >= 4 ? 4 : cfg.supersample >= 2 ? 2 : 1;   // FSR renders below the output size instead
    while (s.ss > 1 && ((uint32_t)(s.fw * s.ss) > s.max_dim || (uint32_t)(s.fh * s.ss) > s.max_dim)) s.ss /= 2;
    s.ow = s.fw * s.ss;
    s.oh = s.fh * s.ss;
    s.base_nearest = s.ss > 1 && s.fw == s.w && s.fh == s.h;
    for (int k = 0, dw = s.ow / 2, dh = s.oh / 2; dw >= s.fw && s.ss > 1; k++, dw /= 2, dh /= 2)
        if (make_image(&s.down[k], VK_FORMAT_B8G8R8A8_UNORM,   // sampled: make_image creates a view
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                           (cfg.fps_overlay ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0),   // the overlay draws on the last one
                       VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)dw, (uint32_t)dh))
            return -1;
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    s.quad_base = ms || s.ow != s.w || s.oh != s.h;
    s.depth_sampled = false;
    if (cfg.ssao || cfg.rain || cfg.damp || (cfg.sssr > 0 && cfg.sssr_glass) || cfg.soft || cfg.lamp || cfg.contact || cfg.shafts || cfg.volfog || cfg.dof || cfg.taa || cfg.frame_gen) {   // ssao, lamp lights, light shafts, volumetric fog, rain splashes, soft particles, the depth of field, TAA and frame generation read the depth target: the format sampled, at this sample count
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(s.phys, s.depth_format, &fp);
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(s.phys, &pp);
        s.depth_sampled = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
                          (!ms || (pp.limits.sampledImageDepthSampleCounts & s.samples));
    }
    const uint32_t ow = (uint32_t)s.ow, oh = (uint32_t)s.oh;
    if (make_image(&s.color, VK_FORMAT_B8G8R8A8_UNORM,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT,   // sharpening, bloom, ssao, lamp lights (light mask) and the gamma ramp (created later, at the title's first ramp) sample it
                   VK_IMAGE_ASPECT_COLOR_BIT, ow, oh) ||
        make_image(&s.depth, s.depth_format,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | (s.depth_sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0),   // ssao samples it
                   VK_IMAGE_ASPECT_DEPTH_BIT, ow, oh, 1, nullptr, s.samples) ||
        (ms && make_image(&s.msaa, VK_FORMAT_B8G8R8A8_UNORM,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (cfg.glass || cfg.bloom ? 0 : VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT),
                          VK_IMAGE_ASPECT_COLOR_BIT, ow, oh, 1, nullptr, s.samples)) ||
        (s.quad_base && make_image(&s.base, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)s.w, (uint32_t)s.h)))
        return -1;

    if (create_main_pass(&s.pass, MAIN_PASS)) return -1;
    const uint32_t attachments = ms ? 3 : 2;
    VkImageView views[3] = {ms ? s.msaa.view : s.color.view, s.depth.view, s.color.view};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = s.pass;
    fi.attachmentCount = attachments;
    fi.pAttachments = views;
    fi.width = ow;
    fi.height = oh;
    fi.layers = 1;
    if (VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &s.fb) != VK_SUCCESS) return fail("vkCreateFramebuffer failed");
    return 0;
}

// The command pool, one command buffer, fence and upload buffer per frame slot, and the one-time
// command buffer and fence.
int create_commands() {
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = s.qfamily;
    if (VKC(vkCreateCommandPool, s.dev, &pi, nullptr, &s.pool) != VK_SUCCESS) return fail("vkCreateCommandPool failed");
    s.nframes = configured_frames();
    s.slot = 0;
    VkCommandBuffer cmds[MAX_FRAMES_IN_FLIGHT + 2];
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = s.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = (uint32_t)s.nframes + 2;   // the frame slots, once_cmd, ub.cmd
    if (VKC(vkAllocateCommandBuffers, s.dev, &ai, cmds) != VK_SUCCESS) return fail("vkAllocateCommandBuffers failed");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (int i = 0; i < s.nframes; i++) {
        FrameSlot &f = s.frames[i];
        f.cmd = cmds[i];
        if (VKC(vkCreateFence, s.dev, &fci, nullptr, &f.fence) != VK_SUCCESS) return fail("vkCreateFence failed");
        if (make_buffer(&f.upload, (VkDeviceSize)s.w * s.h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return -1;
    }
    s.cmd = s.frames[0].cmd;
    s.once_cmd = cmds[s.nframes];
    if (VKC(vkCreateFence, s.dev, &fci, nullptr, &s.once_fence) != VK_SUCCESS) return fail("vkCreateFence failed");
    ub.cmd = cmds[s.nframes + 1];
    if (VKC(vkCreateFence, s.dev, &fci, nullptr, &ub.fence) != VK_SUCCESS) return fail("vkCreateFence failed");
    return make_buffer(&s.readback, (VkDeviceSize)s.dispw * s.disph * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
}

static RVertexCheck vertex_check;   // renderer_set_vertex_check
static bool vertex_check_frame = true;   // renderer_set_vertex_check_frame
// Waits until the current slot's previous frame has completed (no-op when it has been waited); then
// the vertex check of its GPU-skinned draws, while geometry still holds what the device wrote.
int wait_slot() {
    FrameSlot &f = cur();
    if (!f.pending) return 0;
    double t = now_ms();
    VkResult r = VKC(vkWaitForFences, s.dev, 1, &f.fence, VK_TRUE, 5000000000ull);
    s.wait_ms += now_ms() - t;
    if (r != VK_SUCCESS) return fail("frame wait failed (%d)", r);
    f.pending = false;
    if (vertex_check && !f.vcheck.empty() && f.geometry.map) {
        const uint32_t *idx = (const uint32_t *)((const uint8_t *)f.geometry.map + f.vcheck_index_offset);
        vertex_check((const RVertex *)f.geometry.map, idx, f.vcheck.data(), (uint32_t)f.vcheck.size());
    }
    f.vcheck.clear();
    return 0;
}

// Submits the current slot's command buffer (ended by the caller) with its fence; the slot is
// pending until wait_slot. The next frame records into the next slot.
int flush_uploads();
VkResult submit_slot(const VkSubmitInfo *si) {
    if (flush_uploads()) return VK_ERROR_DEVICE_LOST;   // the frame's new textures, in queue order before it
    FrameSlot &f = cur();
    VKC(vkResetFences, s.dev, 1, &f.fence);
    VkResult r = VKC(vkQueueSubmit, s.queue, 1, si, f.fence);
    f.pending = r == VK_SUCCESS;
    return r;
}
void next_slot() {
    s.slot = (s.slot + 1) % s.nframes;
    s.cmd = cur().cmd;
}

VkShaderModule shader(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VKC(vkCreateShaderModule, s.dev, &ci, nullptr, &m);
    return m;
}

// Sampler (D3D defaults the title relies on so far: wrap, bilinear with linear mips), the
// descriptor set layout (bindings 0-3: stage textures, binding 4: the combiner programs) and the
// per-frame pool.
int create_texture_state() {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = 16.0f;
    int up_w = 0;   // --taa-upscale: the scene's textures keep the output size's detail (FSR 2's log2(render / display))
    if (taa_up_wanted(&up_w, nullptr)) si.mipLodBias = taa_lod_bias(up_w, s.fw);
    s.lod_bias = si.mipLodBias;
    si.anisotropyEnable = s.aniso > 1.0f ? VK_TRUE : VK_FALSE;
    si.maxAnisotropy = s.aniso;
    if (VKC(vkCreateSampler, s.dev, &si, nullptr, &s.sampler) != VK_SUCCESS) return fail("vkCreateSampler failed");
    // The base image quad: clamped (no wrap-around at the edges), no mips or anisotropy.
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.anisotropyEnable = VK_FALSE;
    si.maxAnisotropy = 1.0f;
    si.maxLod = 0.0f;
    si.mipLodBias = 0.0f;
    if (VKC(vkCreateSampler, s.dev, &si, nullptr, &s.clamp_sampler) != VK_SUCCESS) return fail("vkCreateSampler failed");
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (VKC(vkCreateSampler, s.dev, &si, nullptr, &s.nearest_sampler) != VK_SUCCESS) return fail("vkCreateSampler failed");
    // Binding 5: the lamp shadows' lights (LampShadow; the programs buffer when they are off).
    // Bindings 6 and 7: the world shadow map and its parameters (WorldShadow; white and the
    // programs buffer when off). Binding 8: the lamp shadows' wall map (white when off).
    VkDescriptorSetLayoutBinding b[RENDERER_STAGES + 5];
    for (uint32_t i = 0; i <= RENDERER_STAGES + 1; i++)
        b[i] = {i, i < RENDERER_STAGES ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[RENDERER_STAGES + 2] = {RENDERER_STAGES + 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[RENDERER_STAGES + 3] = {RENDERER_STAGES + 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[RENDERER_STAGES + 4] = {RENDERER_STAGES + 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = RENDERER_STAGES + 5;
    li.pBindings = b;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &s.set_layout) != VK_SUCCESS) return fail("vkCreateDescriptorSetLayout failed");
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_FRAME_SETS * (RENDERER_STAGES + 2)},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * MAX_FRAME_SETS}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = MAX_FRAME_SETS;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    for (int i = 0; i < s.nframes; i++)
        if (VKC(vkCreateDescriptorPool, s.dev, &pi, nullptr, &s.frames[i].set_pool) != VK_SUCCESS) return fail("vkCreateDescriptorPool failed");
    return 0;
}

// Pipeline variant key: the Vulkan values of an output state, packed (bits 0-4 source factor,
// 5-9 destination factor, 10-12 blend op, 13 blend enable, 14-17 color write mask, 18 depth test,
// 19 depth write, 20-22 depth compare op, 23-24 cull mode, 25-26 pass class: 0 the frame target,
// 1/2 an offscreen pass with D16 / 24-bit depth, one sample; 27-29 bloom light mask role, see
// mask_role: added at execution to the frame target's draws before a renderer_bloom split).
VkBlendFactor blend_factor(uint32_t f, VkBlendFactor fallback) {
    switch (f) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 0x300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304: return VK_BLEND_FACTOR_ONE;    // DST_ALPHA: the target has no alpha (1)
    case 0x305: return VK_BLEND_FACTOR_ZERO;   // ONE_MINUS_DST_ALPHA
    case 0x306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x308: return VK_BLEND_FACTOR_ZERO;   // SRC_ALPHA_SATURATE = min(As, 1 - Ad) with Ad = 1
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return fallback;
    }
}
VkBlendOp blend_op(uint32_t eq) {
    switch (eq) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: case 0xF005: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;   // 0x8006 ADD, 0xF006 ADD_SIGNED
    }
}
uint32_t pipeline_key(const RState &st, uint32_t draw_flags) {
    uint32_t k = 0;
    if (st.blend_enable) {
        k |= blend_factor(st.blend_src, VK_BLEND_FACTOR_ONE) | blend_factor(st.blend_dst, VK_BLEND_FACTOR_ZERO) << 5 |
             blend_op(st.blend_eq) << 10 | 1u << 13;
    }
    uint32_t m = st.color_mask;   // NV2A B 1<<0, G 1<<8, R 1<<16 -> VK R 1, G 2, B 4 (alpha: offscreen color targets only, see renderer_draw)
    k |= ((m & 0x10000u ? VK_COLOR_COMPONENT_R_BIT : 0) | (m & 0x100u ? VK_COLOR_COMPONENT_G_BIT : 0) |
          (m & 1u ? VK_COLOR_COMPONENT_B_BIT : 0)) << 14;
    if (draw_flags & RENDERER_DEPTH_TEST) {
        uint32_t op = st.depth_func >= 0x200 && st.depth_func <= 0x207 ? st.depth_func - 0x200 : VK_COMPARE_OP_LESS_OR_EQUAL;
        k |= 1u << 18 | (st.depth_write ? 1u << 19 : 0) | op << 20;
    }
    // Screen winding (y down) in Vulkan's framebuffer: clockwise has negative area, so with a
    // counterclockwise front face D3DCULL_CW culls back faces and D3DCULL_CCW front faces.
    k |= (st.cull == 0x900 ? VK_CULL_MODE_BACK_BIT : st.cull == 0x901 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE) << 23;
    return k;
}

// Bloom light mask (renderer_bloom): what a draw on the frame target before the split does to the
// target's alpha channel, from its key's blend and depth state. Light sources of the title drawn
// additively (fire, neon, flares) mark the mask; the lamps' alpha-blended halo sprites do not: they
// already are the glow, and marking them bloomed every ceiling lamp into a large disk.
// So:
//   MASK_ADD   additive (ONE or SRC_ALPHA onto ONE, ADD): alpha + source alpha;
//   MASK_FADE  alpha blending (halo sprites, foliage, decals, fades): the mask fades with the
//              coverage (alpha x (1 - a_s));
//   MASK_CLEAR opaque: alpha = 0;
//   MASK_KEEP  no color written, or another blend (multiply, ...): alpha not written, as without
//              bloom.
enum { MASK_KEEP, MASK_CLEAR, MASK_FADE, MASK_ADD };
uint32_t mask_role(uint32_t key) {
    if (!(key >> 14 & 15)) return MASK_KEEP;
    if (!(key >> 13 & 1)) return MASK_CLEAR;
    const uint32_t src = key & 31, dst = key >> 5 & 31, op = key >> 10 & 7;
    if (dst == VK_BLEND_FACTOR_ONE && op == VK_BLEND_OP_ADD && (src == VK_BLEND_FACTOR_ONE || src == VK_BLEND_FACTOR_SRC_ALPHA))
        return MASK_ADD;
    if (dst == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA) return MASK_FADE;
    return MASK_KEEP;
}

// The geometry pipelines' lazily made resources for a key and its emissive flag (layouts, shader
// modules). Render thread only: the pre-warm thread (pipe_worker) only reads what this made.
bool geom_resources(uint32_t key, uint32_t var) {
    const bool hair = var_hair(var) && (key & CHARLIT_KEY) && !(key & SOFT_KEY);
    if (hair && !(cfg.hair > 0)) return false;   // no hair pipeline without --hair
    const bool skin = var_skin(var) && (key & CHARLIT_KEY) && !(key & SOFT_KEY);
    if (skin && !(var_mat(var) ? cfg.eyes > 0 || cfg.cloth > 0 : cfg.skin > 0)) return false;   // no skin pipeline without --skin (materials: --eyes, --cloth)
    const bool relief = var_relief(var) && (key & CHARLIT_KEY) && !(key & SOFT_KEY);
    const bool emit = var == VAR_EMIT || relief || hair || skin;   // the relief, hair and skin variants use the lit emissive layout
    if (!s.layout) {
        VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &s.set_layout;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &pc;
        if (VKC(vkCreatePipelineLayout, s.dev, &li, nullptr, &s.layout) != VK_SUCCESS) {
            fail("vkCreatePipelineLayout failed");
            return false;
        }
    }
    const uint32_t pass_class = key >> 25 & 3;
    if (pass_class && !s.off_pass[pass_class - 1]) {
        fail("no offscreen render pass");
        return false;
    }
    if (!s.vs) s.vs = shader(geom_vert, sizeof geom_vert);
    if (!s.fs) s.fs = shader(geom_frag, sizeof geom_frag);
    const bool soft = key & SOFT_KEY;
    const bool lit = (key & CHARLIT_KEY) && !soft;
    if (lit && !s.charlit.ok) {
        fail("character lighting pipeline without its resources");
        return false;
    }
    VkPipelineLayout &emit_layout = lit ? s.emit.lit_layout : s.emit.layout;
    if (emit && !emit_layout) {
        // set 1: the mask in binding 0; lit: set 1 the lights, set 2 the mask
        VkDescriptorSetLayout sets[3] = {s.set_layout, lit ? s.charlit.set_layout : s.set_layout, s.set_layout};
        // --eyes, --cloth: the material variants read one more vec4 (the eyes' openness)
        VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, lit && (cfg.eyes > 0 || cfg.cloth > 0) ? 48u : 32u};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = lit ? 3 : 2;
        li.pSetLayouts = sets;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &pc;
        if (VKC(vkCreatePipelineLayout, s.dev, &li, nullptr, &emit_layout) != VK_SUCCESS) {
            fail("vkCreatePipelineLayout (emit) failed");
            return false;
        }
    }
    if (emit && !lit && !s.emit.fs) s.emit.fs = shader(geom_emit_frag, sizeof geom_emit_frag);
    if (var == VAR_EMIT && lit && !s.emit.lit_fs) s.emit.lit_fs = shader(geom_lit_emit_frag, sizeof geom_lit_emit_frag);
    if (relief && !hair && !skin && !s.relief.fs) s.relief.fs = shader(geom_lit_relief_frag, sizeof geom_lit_relief_frag);
    if (hair && !skin && !relief && !s.hair.fs) s.hair.fs = shader(geom_lit_hair_frag, sizeof geom_lit_hair_frag);
    if (hair && !skin && relief && !s.hair.relief_fs) s.hair.relief_fs = shader(geom_lit_relief_hair_frag, sizeof geom_lit_relief_hair_frag);
    const int sv = (relief ? 1 : 0) + (hair ? 2 : 0);
    const bool mat = var_mat(var);
    VkShaderModule *skin_fs = mat ? s.skinlit.mat_fs : s.skinlit.fs;
    if (skin && !skin_fs[sv]) {
        static const uint32_t *const code[8] = {geom_lit_skin_frag, geom_lit_relief_skin_frag, geom_lit_hair_skin_frag, geom_lit_relief_hair_skin_frag,
                                                geom_lit_mat_frag,  geom_lit_relief_mat_frag,  geom_lit_hair_mat_frag,  geom_lit_relief_hair_mat_frag};
        static const size_t size[8] = {sizeof geom_lit_skin_frag, sizeof geom_lit_relief_skin_frag, sizeof geom_lit_hair_skin_frag, sizeof geom_lit_relief_hair_skin_frag,
                                       sizeof geom_lit_mat_frag,  sizeof geom_lit_relief_mat_frag,  sizeof geom_lit_hair_mat_frag,  sizeof geom_lit_relief_hair_mat_frag};
        skin_fs[sv] = shader(code[sv + (mat ? 4 : 0)], size[sv + (mat ? 4 : 0)]);
    }
    if (!s.vs || !s.fs || (soft && !s.soft.fs) || (var == VAR_EMIT && !(lit ? s.emit.lit_fs : s.emit.fs)) || (relief && !hair && !skin && !s.relief.fs) ||
        (hair && !skin && !(relief ? s.hair.relief_fs : s.hair.fs)) || (skin && !skin_fs[sv])) {
        fail("vkCreateShaderModule failed");
        return false;
    }
    return true;
}

// Geometry pipeline for a key, its emissive flag and its shape's words (resources from
// geom_resources): RVertex input (clip-space position, B8G8R8A8 diffuse and specular, four texture
// coordinates), triangle lists, dynamic viewport, scissor and blend constants, set 0 (stage
// textures, combiner programs) and the alpha test plus program index as push constants. bg: on the
// pre-warm thread (leaves s.error alone). Returns VK_NULL_HANDLE on failure.
VkPipeline make_geom_pipeline(uint32_t key, uint32_t var, const uint32_t *words, bool bg) {
    const uint32_t pass_class = key >> 25 & 3;
    const bool soft = key & SOFT_KEY;
    const bool lit = (key & CHARLIT_KEY) && !soft;
    const bool relief = var_relief(var) && lit;
    const bool hair = var_hair(var) && lit;
    const bool skin = var_skin(var) && lit;
    const bool emit = var == VAR_EMIT || relief || hair || skin;   // relief, hair, skin: the lit emissive layout
    const VkPipelineLayout emit_layout = lit ? s.emit.lit_layout : s.emit.layout;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = lit ? s.charlit.vs : s.vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = soft ? s.soft.fs : skin ? (var_mat(var) ? s.skinlit.mat_fs : s.skinlit.fs)[(relief ? 1 : 0) + (hair ? 2 : 0)] : hair ? (relief ? s.hair.relief_fs : s.hair.fs) : relief ? s.relief.fs : lit ? (emit ? s.emit.lit_fs : s.charlit.fs) : emit ? s.emit.fs : s.fs;
    stages[1].pName = "main";
    // The shape's words, then MASK_CLEAR (constant 37, bloom light mask: see below) and
    // SHADOW_RANGE (constant 38, a float; read only with the shadow word's bit 13); soft particles:
    // SOFT_RGB (constant 39: the fade scales the colour too unless the source factor is SRC_ALPHA).
    VkSpecializationMapEntry spec_map[SHAPE_WORDS + 3];
    for (uint32_t i = 0; i < SHAPE_WORDS + 3; i++) spec_map[i] = {i, i * 4, 4};
    uint32_t spec_data[SHAPE_WORDS + 3];
    memcpy(spec_data, words, sizeof(Shape));
    const uint32_t role = key >> 27 & 7;
    spec_data[SHAPE_WORDS] = role == MASK_CLEAR;
    const float range = cfg.shadow_range > 0 ? cfg.shadow_range : 0.0f;
    memcpy(&spec_data[SHAPE_WORDS + 1], &range, 4);
    spec_data[SHAPE_WORDS + 2] = (key & 31) != VK_BLEND_FACTOR_SRC_ALPHA;
    VkSpecializationInfo spec{soft ? SHAPE_WORDS + 3 : SHAPE_WORDS + 2, spec_map, soft ? sizeof spec_data : sizeof spec_data - 4, spec_data};
    stages[1].pSpecializationInfo = &spec;
    VkVertexInputBindingDescription bind[2] = {{0, sizeof(RVertex), VK_VERTEX_INPUT_RATE_VERTEX},
                                               {1, 3 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX}};
    VkVertexInputAttributeDescription attrs[5 + RENDERER_STAGES] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RVertex, x)},
        {1, 0, VK_FORMAT_B8G8R8A8_UNORM, offsetof(RVertex, color)},
        {2, 0, VK_FORMAT_B8G8R8A8_UNORM, offsetof(RVertex, specular)}};
    for (uint32_t i = 0; i < RENDERER_STAGES; i++)
        attrs[3 + i] = {3 + i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)(offsetof(RVertex, tex) + i * sizeof(float[4]))};
    attrs[3 + RENDERER_STAGES] = {3 + RENDERER_STAGES, 0, VK_FORMAT_R32_SFLOAT, offsetof(RVertex, fog)};
    attrs[4 + RENDERER_STAGES] = {4 + RENDERER_STAGES, 1, VK_FORMAT_R32G32B32_SFLOAT, 0};   // lit: the view normal
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = lit ? 2 : 1;
    vi.pVertexBindingDescriptions = bind;
    vi.vertexAttributeDescriptionCount = lit ? 5 + RENDERER_STAGES : 4 + RENDERER_STAGES;
    vi.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    // Depth clamp instead of near/far clipping, as the title's programs need it: their WVP constants
    // use a far plane (about 50) closer than the objects they draw.
    rs.depthClampEnable = s.depth_clamp ? VK_TRUE : VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = (key >> 23) & 3;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = pass_class ? VK_SAMPLE_COUNT_1_BIT : s.samples;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = key >> 18 & 1 ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = key >> 19 & 1 ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = (VkCompareOp)(key >> 20 & 7);
    VkPipelineColorBlendAttachmentState cb{};
    cb.blendEnable = key >> 13 & 1 ? VK_TRUE : VK_FALSE;
    cb.srcColorBlendFactor = cb.srcAlphaBlendFactor = (VkBlendFactor)(key & 31);
    cb.dstColorBlendFactor = cb.dstAlphaBlendFactor = (VkBlendFactor)(key >> 5 & 31);
    cb.colorBlendOp = cb.alphaBlendOp = (VkBlendOp)(key >> 10 & 7);
    cb.colorWriteMask = key >> 14 & 15;
    if (role) {   // bloom light mask: the color part is unchanged; opaque draws write alpha 0 (MASK_CLEAR)
        cb.srcAlphaBlendFactor = role == MASK_ADD ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        cb.dstAlphaBlendFactor = role == MASK_CLEAR ? VK_BLEND_FACTOR_ZERO
                                 : role == MASK_ADD ? VK_BLEND_FACTOR_ONE
                                                    : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.alphaBlendOp = VK_BLEND_OP_ADD;
        cb.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
    // s.eds: cull mode and depth state set per draw (exec_op); the key's bits for them are 0 here.
    VkDynamicState dyn[7] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                             VK_DYNAMIC_STATE_CULL_MODE_EXT, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT,
                             VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = s.eds ? 7 : 3;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cbs;
    pi.pDynamicState = &dy;
    pi.layout = soft ? s.soft.layout : emit ? emit_layout : lit ? s.charlit.layout : s.layout;
    pi.renderPass = pass_class ? s.off_pass[pass_class - 1] : s.pass;
    VkResult r = VKC(vkCreateGraphicsPipelines, s.dev, s.pcache, 1, &pi, nullptr, &pipe);
    if (r != VK_SUCCESS) {
        if (!bg) fail("vkCreateGraphicsPipelines failed (%d)", r);
        pipe = VK_NULL_HANDLE;
    }
    return pipe;
}

// With VK_EXT_extended_dynamic_state the pipeline of a key leaves out its depth test, write,
// compare and cull bits (set per draw by exec_op): one pipeline for draws that differ only there.
const uint32_t EDS_KEY_BITS = 0x7Fu << 18;
uint32_t eds_key(uint32_t key) { return s.eds ? key & ~EDS_KEY_BITS : key; }

// ---- Persistent pipeline cache, pre-warm and compile workers (RendererConfig.pipeline_cache_dir,
// pipeline_async, pipe_cache.h) ----
// One VkPipelineCache for every pipeline, created without EXTERNALLY_SYNCHRONIZED (the driver
// synchronizes it, so the workers and the render thread share it). The geometry pipelines a run
// uses are listed by key, variant and shape words (the shape index is per run); the next
// renderer_init builds them again on the compile workers (pc_worker_count threads, low priority).
// get_pipeline takes a pre-warmed one when it is ready. Otherwise, with pipeline_async, it queues
// the build for the workers ahead of the pre-warm (PcJobQueue) and waits for it at most what is
// left of the frame's PIPE_WAIT_MS; a draw whose pipeline is not ready is drawn with its base
// variant when that one exists, else skipped (exec_op), until a later frame finds it built. Without
// pipeline_async (and without workers) it is built on the render thread as before.
const uint32_t PC_STRIDE = SHAPE_WORDS + 2;   // key, variant (0 plain, VAR_EMIT .. VAR_MAT_LAST), the shape's words
// The meaning of a record (key bits, variant numbers, shape words). Bump it when one of them
// changes; a shader change alone keeps the list (its pipelines are simply built again).
const uint32_t PC_RECORD_VERSION = 1;
const uint32_t PC_SAVE_EVERY = 32;            // new pipelines between the periodic saves
const double PC_WINDOW_MS = 60000;            // pipeline_stats window
const double PIPE_WAIT_MS = 4;                // per frame, the render thread's wait for queued builds
struct PipeJob {
    uint32_t key, var;
    uint32_t words[SHAPE_WORDS];
};
struct PipeDone {
    VkPipeline pipe;
    double ms;   // the build, on its worker
    PipeJob job;
};
struct PipeWork {
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;        // the fields below marked (L)
    pthread_mutex_t save_lock = PTHREAD_MUTEX_INITIALIZER;   // one pipe_save at a time
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;          // (L) quit, save_req or a job queued
    pthread_cond_t done_cond{};                              // (L) a queued build finished (monotonic clock)
    bool done_cond_ok = false;
    std::vector<pthread_t> threads;                   // the compile workers
    bool running = false, started = false;
    bool quit = false, save_req = false;              // (L)
    std::string cache_path, keys_path;                // empty: no files (memory only)
    uint64_t build = 0;                               // the record layout's hash (pipe_build_hash)
    std::vector<uint32_t> recs;                       // (L) the key list: earlier runs', then this run's
    std::unordered_set<uint64_t> known;               // (L) hashes of recs
    std::unordered_set<uint64_t> claimed;             // (L) records get_pipeline has seen this run
    std::unordered_map<uint64_t, VkPipeline> ready;   // (L) pre-warmed, not taken yet
    std::vector<uint32_t> warm;                       // to pre-warm (fixed once the workers run)
    PcJobQueue queue;                                 // (L)
    std::unordered_map<uint64_t, PipeJob> jobs;       // (L) queued builds by s.pipelines id
    std::unordered_map<uint64_t, PipeDone> done;      // (L) finished builds not collected yet
    uint32_t added = 0;                               // (L) new records since the last save request
    uint32_t gen = 0, saved_gen = 0;                  // (L) pipelines built / when last saved
    uint32_t warm_built = 0, warm_skipped = 0;        // (L)
    double warm_t0 = 0;
    std::string report;                               // (L) pending log lines, '\n' separated
    // Render thread: queued builds not collected yet, totals and the current pipeline_stats window.
    std::unordered_map<uint64_t, uint32_t> pending;   // id -> the frame (frame_no) that queued it
    uint32_t frame_no = 0;
    double wait_left = PIPE_WAIT_MS;   // of this frame's wait for builds it queued
    uint32_t on_demand = 0, used = 0;
    double win_start = 0, win_ms = 0, win_max = 0, win_frame = 0, win_wait = 0;
    uint32_t win_n = 0, win_used = 0, win_idx = 0, win_skip = 0;
    double run_start = 0, run_ms = 0, run_max = 0, run_frame = 0, run_wait = 0;   // since renderer_init (pipe_totals)
    uint32_t run_skip = 0;   // frames that drew without a pipeline still building
    bool frame_skip = false;
    double frame_ms = 0;   // the render thread compiling or waiting for builds since the last pipe_frame (RendererTiming.pipeline_ms)
    double upload_ms = 0;  // renderer_texture_create since the last pipe_frame (RendererTiming.upload_ms)
    bool totals_done = false;
};
PipeWork pw;

// A [pipeline] log line for renderer_pipeline_report; pipe_line_locked: with pw.lock held.
void pipe_line_add(const char *fmt, va_list ap) {
    char line[512];
    vsnprintf(line, sizeof line, fmt, ap);
    if (pw.report.size() < 16384) pw.report += std::string(line) + "\n";
}
void pipe_line_locked(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pipe_line_add(fmt, ap);
    va_end(ap);
}
void pipe_line(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&pw.lock);
    pipe_line_add(fmt, ap);
    pthread_mutex_unlock(&pw.lock);
    va_end(ap);
}

// The record layout and meaning: a key list written for another one is dropped. Not the shaders:
// after an update that changed them the listed pipelines are built again from the new code (the
// driver's cache keeps only what still matches), so a new build pre-warms instead of stuttering.
uint64_t pipe_build_hash() {
    const uint32_t layout[3] = {PC_STRIDE, PC_RECORD_VERSION, (uint32_t)sizeof(Shape)};
    return pc_hash64(layout, sizeof layout);
}

// After create_device: the cache, from pipeline-cache.bin when it was made for this GPU and driver;
// the key list. Never fails (without a cache object every pipeline is built as before).
int create_pipeline_cache() {
    pw.win_start = pw.run_start = now_ms();
    pw.totals_done = false;
    if (cfg.no_pipeline_cache) return 0;
    std::vector<uint8_t> file;
    size_t off = 0, len = 0;
    const char *why = "none yet";
    if (cfg.pipeline_cache_dir && *cfg.pipeline_cache_dir) {
        pw.cache_path = std::string(cfg.pipeline_cache_dir) + "/pipeline-cache.bin";
        pw.keys_path = std::string(cfg.pipeline_cache_dir) + "/pipeline-keys.bin";
        pw.build = pipe_build_hash();
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(s.phys, &p);
        if (pc_read_file(pw.cache_path.c_str(), file)) {
            int r = pc_cache_check(file.data(), file.size(), p.vendorID, p.deviceID, p.pipelineCacheUUID, &off, &len);
            if (r != PC_OK) why = pc_cache_reason(r);
        }
        std::vector<uint8_t> keys;
        if (pc_read_file(pw.keys_path.c_str(), keys)) pc_keys_read(keys.data(), keys.size(), PC_STRIDE, pw.build, pw.recs);
        for (size_t i = 0; i < pw.recs.size(); i += PC_STRIDE) pw.known.insert(pc_hash64(&pw.recs[i], PC_STRIDE * 4));
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = len;
    ci.pInitialData = len ? file.data() + off : nullptr;
    VkResult r = VKC(vkCreatePipelineCache, s.dev, &ci, nullptr, &s.pcache);
    if (r != VK_SUCCESS && len) {   // the driver refused the data: start empty
        why = "refused by the driver";
        len = 0;
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        r = VKC(vkCreatePipelineCache, s.dev, &ci, nullptr, &s.pcache);
    }
    if (r != VK_SUCCESS) s.pcache = VK_NULL_HANDLE;
    if (!pw.cache_path.empty()) {
        char got[64];
        snprintf(got, sizeof got, "loaded %zu KB", len >> 10);
        pipe_line("[pipeline] cache %s: %s; %zu pipelines listed to pre-warm", pw.cache_path.c_str(), len ? got : why,
                  pw.recs.size() / PC_STRIDE);
    }
    return 0;
}

// Writes pipeline-cache.bin and pipeline-keys.bin if pipelines were built since the last write.
void pipe_save(bool final) {
    if (!pw.started || !s.pcache || pw.cache_path.empty()) return;
    pthread_mutex_lock(&pw.save_lock);
    pthread_mutex_lock(&pw.lock);
    const uint32_t gen = pw.gen;
    std::vector<uint32_t> recs = gen != pw.saved_gen ? pw.recs : std::vector<uint32_t>();
    pthread_mutex_unlock(&pw.lock);
    if (gen != pw.saved_gen) {
        std::vector<uint8_t> blob, file;
        VkResult r = VK_INCOMPLETE;
        for (int tries = 0; tries < 4 && r == VK_INCOMPLETE; tries++) {   // grown between the two calls
            size_t n = 0;
            if (vkGetPipelineCacheData(s.dev, s.pcache, &n, nullptr) != VK_SUCCESS) break;
            blob.resize(n);
            r = vkGetPipelineCacheData(s.dev, s.pcache, &n, blob.data());
            blob.resize(n);
        }
        bool ok = r == VK_SUCCESS && !blob.empty();
        if (ok) {
            pc_cache_wrap(blob.data(), blob.size(), file);
            ok = pc_write_atomic(pw.cache_path.c_str(), file.data(), file.size());
            pc_keys_write(recs, PC_STRIDE, pw.build, file);
            ok = pc_write_atomic(pw.keys_path.c_str(), file.data(), file.size()) && ok;
        }
        if (ok) pw.saved_gen = gen;
        if (final || cfg.pipeline_stats || !ok)
            pipe_line("[pipeline] %s %s (%zu KB, %zu pipelines listed)", ok ? "saved" : "could not save", pw.cache_path.c_str(), blob.size() >> 10,
                      recs.size() / PC_STRIDE);
    }
    pthread_mutex_unlock(&pw.save_lock);
}

void lower_thread_priority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), -2);   // THREAD_PRIORITY_LOWEST
#else
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
#endif
}

long cpu_count() {
#ifdef _WIN32
    return (long)GetActiveProcessorCount(0xffff);   // ALL_PROCESSOR_GROUPS
#else
    return sysconf(_SC_NPROCESSORS_ONLN);
#endif
}

// A pre-warm job (worker, pw.lock held; returns with it held): builds the listed pipeline unless
// get_pipeline needed it first.
void pipe_warm_one(size_t index) {
    const uint32_t *rec = &pw.warm[index * PC_STRIDE];
    const uint64_t h = pc_hash64(rec, PC_STRIDE * 4);
    if (pw.claimed.count(h)) {
        pw.warm_skipped++;
        return;
    }
    pthread_mutex_unlock(&pw.lock);
    VkPipeline p = make_geom_pipeline(rec[0], rec[1], rec + 2, true);
    pthread_mutex_lock(&pw.lock);
    pw.gen++;
    if (p && !pw.claimed.count(h)) {
        pw.ready[h] = p;
        pw.warm_built++;
        return;
    }
    if (p) {   // get_pipeline built it meanwhile
        pthread_mutex_unlock(&pw.lock);
        vkDestroyPipeline(s.dev, p, nullptr);
        pthread_mutex_lock(&pw.lock);
        pw.warm_skipped++;
    }
}

// A compile worker: the builds frames wait for, then the pre-warm list, and the files when asked
// (every PC_SAVE_EVERY new pipelines), until pipe_stop.
void *pipe_worker(void *) {
    lower_thread_priority();
    pthread_mutex_lock(&pw.lock);
    while (!pw.quit) {
        uint64_t id = 0;
        size_t index = 0;
        const int kind = pw.queue.take(&id, &index);
        if (kind == PcJobQueue::URGENT) {
            auto it = pw.jobs.find(id);
            if (it == pw.jobs.end()) continue;
            const PipeJob job = it->second;
            pw.jobs.erase(it);
            pthread_mutex_unlock(&pw.lock);
            const double t0 = now_ms();
            VkPipeline p = make_geom_pipeline(job.key, job.var, job.words, true);
            const double ms = now_ms() - t0;
            pthread_mutex_lock(&pw.lock);
            pw.done[id] = {p, ms, job};
            pw.gen++;
            if (pw.done_cond_ok) pthread_cond_broadcast(&pw.done_cond);
            continue;
        }
        if (kind == PcJobQueue::WARM) {
            pipe_warm_one(index);
            if (pw.queue.warm_done())
                pipe_line_locked("[pipeline] pre-warm: %u of %zu pipelines built in %.2f s (%u needed before they were ready)", pw.warm_built,
                                 pw.warm.size() / PC_STRIDE, (now_ms() - pw.warm_t0) / 1e3, pw.warm_skipped);
            continue;
        }
        if (pw.save_req) {
            pw.save_req = false;
            pthread_mutex_unlock(&pw.lock);
            pipe_save(false);
            pthread_mutex_lock(&pw.lock);
            continue;
        }
        pthread_cond_wait(&pw.cond, &pw.lock);
    }
    pthread_mutex_unlock(&pw.lock);
    return nullptr;
}

// The variant a listed record has with this run's options: the skin variants' material form
// follows --eyes / --cloth (get_pipeline's choice), so a list written with other options still
// pre-warms the pipelines this run draws with.
uint32_t pipe_var_for_options(uint32_t var) {
    const bool mat = cfg.eyes > 0 || cfg.cloth > 0;
    if (mat && var >= VAR_SKIN && var <= VAR_RELIEF_HAIR_SKIN) return var + (VAR_MAT - VAR_SKIN);
    if (!mat && var >= VAR_MAT && var <= VAR_MAT_LAST) return var - (VAR_MAT - VAR_SKIN);
    return var;
}

// End of renderer_init: the listed pipelines whose resources exist go to the compile workers
// (geom_resources makes the lazily made ones here, on the render thread), which start here.
// Workers run with the cache files (pre-warm, saves) or with pipeline_async.
void pipe_warm_start() {
    const bool files = s.pcache && !pw.cache_path.empty();
    if (!files && !cfg.pipeline_async) return;
    if (files) {
        char error[sizeof s.error];
        memcpy(error, s.error, sizeof error);
        std::unordered_set<uint64_t> seen;
        for (size_t i = 0; i < pw.recs.size(); i += PC_STRIDE) {
            uint32_t rec[PC_STRIDE];
            memcpy(rec, &pw.recs[i], sizeof rec);
            if (rec[1] > VAR_MAT_LAST) continue;
            rec[0] = eds_key(rec[0]);   // a list written with or without dynamic state
            rec[1] = pipe_var_for_options(rec[1]);
            const uint64_t h = pc_hash64(rec, sizeof rec);
            if (pw.claimed.count(h) || !seen.insert(h).second || !geom_resources(rec[0], rec[1])) continue;
            pw.warm.insert(pw.warm.end(), rec, rec + PC_STRIDE);
        }
        memcpy(s.error, error, sizeof error);
    }
    if (!pw.done_cond_ok) pw.done_cond_ok = plat_cond_init_monotonic(&pw.done_cond) == 0;
    const uint32_t workers = pc_worker_count(cpu_count());
    pw.started = true;
    pw.quit = false;
    pw.warm_t0 = now_ms();
    pw.queue.reset(pw.warm.size() / PC_STRIDE, workers);
    for (uint32_t i = 0; i < workers; i++) {
        pthread_t t;
        if (pthread_create(&t, nullptr, pipe_worker, nullptr) == 0) pw.threads.push_back(t);
    }
    pw.running = !pw.threads.empty();
}

// With pipeline_stats, once at exit: the whole run's counts (the last window is usually partial).
void pipe_totals() {
    if (!cfg.pipeline_stats || pw.totals_done) return;
    pw.totals_done = true;
    pipe_line("[pipeline] run (%.0f s): %u compiled on demand (%.1f ms, longest %.1f ms), %u pre-warmed used, waited %.1f ms, %u frames "
              "drew without a building pipeline, worst renderer frame %.1f ms",
              (now_ms() - pw.run_start) / 1e3, pw.on_demand, pw.run_ms, pw.run_max, pw.used, pw.run_wait, pw.run_skip, pw.run_frame);
}

// renderer_shutdown: stops the workers, writes the files, frees what was never taken.
void pipe_stop() {
    pipe_totals();
    if (pw.running) {
        pthread_mutex_lock(&pw.lock);
        pw.quit = true;
        pthread_cond_broadcast(&pw.cond);
        pthread_mutex_unlock(&pw.lock);
        for (pthread_t t : pw.threads) pthread_join(t, nullptr);
        pw.running = false;
    }
    pw.threads.clear();
    pipe_save(true);
    for (auto &p : pw.ready) vkDestroyPipeline(s.dev, p.second, nullptr);
    for (auto &d : pw.done)
        if (d.second.pipe) vkDestroyPipeline(s.dev, d.second.pipe, nullptr);
    if (s.pcache) vkDestroyPipelineCache(s.dev, s.pcache, nullptr);
    s.pcache = VK_NULL_HANDLE;
    pthread_mutex_lock(&pw.lock);
    pw.recs.clear();
    pw.known.clear();
    pw.claimed.clear();
    pw.ready.clear();
    pw.jobs.clear();
    pw.done.clear();
    pw.queue.reset(0, 1);
    pw.added = pw.gen = pw.saved_gen = pw.warm_built = pw.warm_skipped = 0;
    pw.save_req = pw.quit = false;
    pthread_mutex_unlock(&pw.lock);
    pw.pending.clear();
    pw.warm.clear();
    pw.cache_path.clear();
    pw.keys_path.clear();
    pw.started = false;
    pw.on_demand = pw.used = pw.win_n = pw.win_used = pw.win_idx = pw.win_skip = pw.run_skip = 0;
    pw.win_ms = pw.win_max = pw.win_frame = pw.win_wait = 0;
    pw.run_ms = pw.run_max = pw.run_frame = pw.run_wait = 0;
    pw.wait_left = PIPE_WAIT_MS;
    pw.frame_skip = false;
}

// get_pipeline's miss: the pre-warmed pipeline if ready (VK_NULL_HANDLE: build it); lists the record.
VkPipeline pipe_take(uint32_t key, uint32_t var, const uint32_t *words) {
    if (pw.cache_path.empty()) return VK_NULL_HANDLE;
    uint32_t rec[PC_STRIDE];
    rec[0] = key;
    rec[1] = var;
    memcpy(rec + 2, words, sizeof(Shape));
    const uint64_t h = pc_hash64(rec, sizeof rec);
    VkPipeline p = VK_NULL_HANDLE;
    bool save = false;
    pthread_mutex_lock(&pw.lock);
    pw.claimed.insert(h);
    auto it = pw.ready.find(h);
    if (it != pw.ready.end()) {
        p = it->second;
        pw.ready.erase(it);
    }
    if (pw.known.insert(h).second) {
        pw.recs.insert(pw.recs.end(), rec, rec + PC_STRIDE);
        if (++pw.added >= PC_SAVE_EVERY) {
            pw.added = 0;
            pw.save_req = save = true;
        }
    }
    if (save) pthread_cond_signal(&pw.cond);
    pthread_mutex_unlock(&pw.lock);
    if (p) {
        pw.used++;
        pw.win_used++;
    }
    return p;
}

// End of renderer_end / renderer_present (t_start: its start): the pipeline_stats window.
void pipe_frame(double t_start) {
    const double t = now_ms();
    s.timing.pipeline_ms = (float)pw.frame_ms;
    s.timing.upload_ms = (float)pw.upload_ms;
    s.timing.gpu_busy_ms = s.ov.last_ms;
    pw.frame_ms = pw.upload_ms = 0;
    pw.wait_left = PIPE_WAIT_MS;
    pw.frame_no++;
    if (pw.frame_skip) {
        pw.win_skip++;
        pw.run_skip++;
        pw.frame_skip = false;
    }
    if (t - t_start > pw.win_frame) pw.win_frame = t - t_start;
    if (t - t_start > pw.run_frame) pw.run_frame = t - t_start;
    if (!cfg.pipeline_stats || t - pw.win_start < PC_WINDOW_MS) return;
    pthread_mutex_lock(&pw.lock);
    const uint32_t built = pw.warm_built;
    pthread_mutex_unlock(&pw.lock);
    char warm[64] = "";
    if (!pw.warm.empty()) snprintf(warm, sizeof warm, "; pre-warm %u of %zu built", built, pw.warm.size() / PC_STRIDE);
    pipe_line("[pipeline] %u-%u s: %u compiled on demand (%.1f ms, longest %.1f ms), %u pre-warmed used, waited %.1f ms, %u frames drew "
              "without a building pipeline, worst renderer frame %.1f ms%s",
              pw.win_idx * 60, pw.win_idx * 60 + 60, pw.win_n, pw.win_ms, pw.win_max, pw.win_used, pw.win_wait, pw.win_skip, pw.win_frame, warm);
    pw.win_idx++;
    pw.win_start = t;
    pw.win_n = pw.win_used = pw.win_skip = 0;
    pw.win_ms = pw.win_max = pw.win_frame = pw.win_wait = 0;
}

// A pipeline built on demand (render thread or worker, ms): the counts and the slow-build line.
void pipe_count_build(uint32_t key, uint32_t var, const uint32_t *words, double ms) {
    pw.on_demand++;
    pw.win_n++;
    pw.win_ms += ms;
    if (ms > pw.win_max) pw.win_max = ms;
    pw.run_ms += ms;
    if (ms > pw.run_max) pw.run_max = ms;
    if (cfg.pipeline_stats && ms >= 20)
        pipe_line("[pipeline] slow build: key %08x variant %u shape %08x %.1f ms", key, var, (uint32_t)pc_hash64(words, sizeof(Shape)), ms);
}

// A queued build of s.pipelines id `full` (render thread): the pipeline once its worker finished;
// VK_NULL_HANDLE while building. A build queued this frame is waited for at most what is left of
// the frame's PIPE_WAIT_MS (enough for a driver cache hit); one that already missed a frame is a
// real compile (0.1 to 2 s) and is not waited for again.
VkPipeline pipe_collect(uint64_t full) {
    const auto pend = pw.pending.find(full);
    const bool fresh = pend != pw.pending.end() && pend->second == pw.frame_no;
    pthread_mutex_lock(&pw.lock);
    auto it = pw.done.find(full);
    if (it == pw.done.end() && fresh && pw.wait_left > 0 && pw.done_cond_ok) {
        const double t0 = now_ms();
        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        const long ns = (long)(pw.wait_left * 1e6);
        deadline.tv_nsec += ns;
        while (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_nsec -= 1000000000L;
            deadline.tv_sec++;
        }
        while ((it = pw.done.find(full)) == pw.done.end())
            if (plat_cond_timedwait_mono(&pw.done_cond, &pw.lock, &deadline) != 0) break;
        it = pw.done.find(full);
        const double waited = now_ms() - t0;
        pw.wait_left -= waited;
        pw.frame_ms += waited;
        pw.win_wait += waited;
        pw.run_wait += waited;
    }
    if (it == pw.done.end()) {
        pthread_mutex_unlock(&pw.lock);
        pw.frame_skip = true;
        return VK_NULL_HANDLE;
    }
    const PipeDone d = it->second;
    pw.done.erase(it);
    pthread_mutex_unlock(&pw.lock);
    pw.pending.erase(full);
    pipe_count_build(d.job.key, d.job.var, d.job.words, d.ms);
    if (!d.pipe) fail("vkCreateGraphicsPipelines failed");
    s.pipelines[full] = d.pipe;
    return d.pipe;
}

// The geometry pipeline of a key and shape. defer (exec_op with pipeline_async): a pipeline not
// built yet is queued for the compile workers and VK_NULL_HANDLE returned until it is ready.
VkPipeline get_pipeline(uint32_t key, uint32_t shape, bool defer = false) {
    key = eds_key(key);
    const uint64_t full = (uint64_t)shape << 32 | key;
    auto it = s.pipelines.find(full);
    if (it != s.pipelines.end()) return it->second;
    if (defer && pw.pending.count(full)) return pipe_collect(full);
    const bool litk = (key & CHARLIT_KEY) && !(key & SOFT_KEY);
    const uint32_t var = (shape & EMIT_SHAPE) && !(key & SOFT_KEY) ? VAR_EMIT
                         : litk && (shape & SKIN_SHAPE)   ? (cfg.eyes > 0 || cfg.cloth > 0 ? VAR_MAT : VAR_SKIN) + ((shape & RELIEF_SHAPE) ? 1 : 0) + ((shape & HAIR_SHAPE) ? 2 : 0)
                         : litk && (shape & HAIR_SHAPE)   ? ((shape & RELIEF_SHAPE) ? VAR_RELIEF_HAIR : VAR_HAIR)
                         : litk && (shape & RELIEF_SHAPE) ? VAR_RELIEF
                                                          : 0;
    const uint32_t *words = s.shapes[shape & ~(EMIT_SHAPE | RELIEF_SHAPE | HAIR_SHAPE | SKIN_SHAPE)].w;
    VkPipeline pipe = pipe_take(key, var, words);
    if (!pipe && defer && pw.running) {
        if (!geom_resources(key, var)) {   // made here, before a worker reads them
            s.pipelines[full] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        PipeJob job;
        job.key = key;
        job.var = var;
        memcpy(job.words, words, sizeof job.words);
        pthread_mutex_lock(&pw.lock);
        pw.jobs[full] = job;
        pw.queue.urgent.push_back(full);
        pthread_cond_signal(&pw.cond);
        pthread_mutex_unlock(&pw.lock);
        pw.pending[full] = pw.frame_no;
        return pipe_collect(full);
    }
    if (!pipe) {
        const double t0 = now_ms();
        pipe = geom_resources(key, var) ? make_geom_pipeline(key, var, words, false) : VK_NULL_HANDLE;
        const double ms = now_ms() - t0;
        pipe_count_build(key, var, words, ms);
        pw.frame_ms += ms;
        pthread_mutex_lock(&pw.lock);
        pw.gen++;
        pthread_mutex_unlock(&pw.lock);
    }
    s.pipelines[full] = pipe;
    return pipe;
}

// exec_op with pipeline_async: whether the pipeline of key and shape is built (no build queued).
bool pipe_built(uint32_t key, uint32_t shape) {
    auto it = s.pipelines.find((uint64_t)shape << 32 | eds_key(key));
    return it != s.pipelines.end() && it->second != VK_NULL_HANDLE;
}

// The default pipelines (no blending; depth test with writes or none), created at init so a
// broken device fails there instead of at the first draw.
uint32_t shape_index(const RPixelShader &ps, uint32_t shadow);
int create_default_pipelines() {
    RState d;
    renderer_state_default(&d);
    RPixelShader def;
    renderer_default_pixel_shader(&def);
    uint32_t shape = shape_index(def, 0);
    return get_pipeline(pipeline_key(d, 0), shape) && get_pipeline(pipeline_key(d, RENDERER_DEPTH_TEST), shape) ? 0 : -1;
}

// Uploads this frame's vertices and indices into its slot's host-visible buffer (grown on demand:
// only the slot's previous frame used the old one, and its fence has been waited).
// The lit draws' normals (renderer_draw_lit) follow the indices, from normal_offset.
static VkDeviceSize normal_offset;
int upload_geometry(VkDeviceSize *index_offset) {
    Buffer &g = cur().geometry;
    VkDeviceSize vbytes = s.verts.size() * sizeof(RVertex), ibytes = s.indices.size() * 4, nbytes = s.normals.size() * sizeof(float);
    *index_offset = (vbytes + 255) & ~(VkDeviceSize)255;
    normal_offset = (*index_offset + ibytes + 255) & ~(VkDeviceSize)255;
    VkDeviceSize need = nbytes ? normal_offset + nbytes : *index_offset + ibytes;
    if (need > g.size) {
        s.verts.leave_map();   // direct vertices in the old buffer: copied into the new one below
        free_buffer(&g);
        VkDeviceSize size = 1 << 20;
        // Direct vertices: headroom, so a slightly larger frame does not read the buffer back.
        while (size < (cfg.direct_geometry ? need + need / 2 : need)) size *= 2;
        if (make_buffer(&g, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false,
                        true))
            return -1;
    }
    if (!s.verts.direct()) memcpy(g.map, s.verts.data(), vbytes);   // direct: already there
    memcpy((uint8_t *)g.map + *index_offset, s.indices.data(), ibytes);
    if (nbytes) memcpy((uint8_t *)g.map + normal_offset, s.normals.data(), nbytes);
    return 0;
}

void barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access, VkAccessFlags dst_access,
             VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage, uint32_t levels = 1,
             VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, levels, 0, 1};
    vkCmdPipelineBarrier(s.cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// XRGB8888 color dword -> normalized B8G8R8A8 clear value (alpha 1).
VkClearColorValue clear_value(uint32_t c) {
    VkClearColorValue v{};
    v.float32[0] = ((c >> 16) & 0xFF) / 255.0f;
    v.float32[1] = ((c >> 8) & 0xFF) / 255.0f;
    v.float32[2] = (c & 0xFF) / 255.0f;
    v.float32[3] = 1.0f;
    return v;
}

// One-time submission (new surface image, a texture the upload batch cannot take): begin_once
// makes the one-time command buffer the recording one (barrier() records into s.cmd) and begins
// it; submit_once flushes the upload batch, runs it, waits for it and restores the frame's command
// buffer. The work touches only new images and frames submitted later see it through its final
// barrier (queue submission order).
void begin_once(VkCommandBuffer *saved) {
    *saved = s.cmd;
    s.cmd = s.once_cmd;
    VKC(vkResetCommandBuffer, s.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer, s.cmd, &bi);
}
int submit_once(VkCommandBuffer saved) {
    if (flush_uploads()) return -1;   // in queue order before any other one-time work
    VKC(vkEndCommandBuffer, s.cmd);
    const VkCommandBuffer once = s.cmd;
    s.cmd = saved;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &once;
    VKC(vkResetFences, s.dev, 1, &s.once_fence);
    VkResult r = VKC(vkQueueSubmit, s.queue, 1, &si, s.once_fence);
    if (r == VK_SUCCESS) r = VKC(vkWaitForFences, s.dev, 1, &s.once_fence, VK_TRUE, 5000000000ull);
    return r == VK_SUCCESS ? 0 : fail("submit/wait failed (%d)", r);
}

// Submits the batched texture uploads (ub) and waits for them; a no-op without any. Counted as
// texture creation time (RendererTiming.upload_ms, the [hitch] line's tex-upload).
int flush_uploads() {
    if (!ub.open) return 0;
    ub.open = false;
    ub.used = 0;
    const double t0 = now_ms();
    VKC(vkEndCommandBuffer, ub.cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &ub.cmd;
    VKC(vkResetFences, s.dev, 1, &ub.fence);
    VkResult r = VKC(vkQueueSubmit, s.queue, 1, &si, ub.fence);
    if (r == VK_SUCCESS) r = VKC(vkWaitForFences, s.dev, 1, &ub.fence, VK_TRUE, 5000000000ull);
    pw.upload_ms += now_ms() - t0;
    return r == VK_SUCCESS ? 0 : fail("texture upload submit/wait failed (%d)", r);
}

VkFormat texture_format(uint32_t f) {
    switch (f) {
    case RENDERER_TEX_BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
    case RENDERER_TEX_BC1: return s.bc ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : VK_FORMAT_UNDEFINED;
    case RENDERER_TEX_BC2: return s.bc ? VK_FORMAT_BC2_UNORM_BLOCK : VK_FORMAT_UNDEFINED;
    case RENDERER_TEX_BC3: return s.bc ? VK_FORMAT_BC3_UNORM_BLOCK : VK_FORMAT_UNDEFINED;
    case RENDERER_TEX_BC7: return s.bc ? VK_FORMAT_BC7_UNORM_BLOCK : VK_FORMAT_UNDEFINED;
    case RENDERER_TEX_L8: return VK_FORMAT_R8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

// Bytes of one mip level as the caller packs it.
VkDeviceSize level_size(uint32_t f, uint32_t w, uint32_t h) {
    if (f == RENDERER_TEX_BGRA8) return (VkDeviceSize)w * h * 4;
    if (f == RENDERER_TEX_L8) return (VkDeviceSize)w * h;
    return (VkDeviceSize)((w + 3) / 4) * ((h + 3) / 4) * (f == RENDERER_TEX_BC1 ? 8 : 16);
}

// Replaces the view of an R8 image with one sampled as (L, L, L, 1), like the CPU decode of L8.
int l8_view(Image *im, uint32_t levels) {
    vkDestroyImageView(s.dev, im->view, nullptr);
    im->view = VK_NULL_HANDLE;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im->img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8_UNORM;
    vi.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE};
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    if (VKC(vkCreateImageView, s.dev, &vi, nullptr, &im->view) != VK_SUCCESS) return fail("vkCreateImageView (L8) failed");
    return 0;
}

// Creates the image and uploads every level: through the upload batch (ub, flushed later), or a
// temporary staging buffer and a one-time submit when the batch cannot take it.
int make_texture(Texture *t, uint32_t f, uint32_t w, uint32_t h, uint32_t levels, const void *data) {
    VkFormat fmt = texture_format(f);
    if (fmt == VK_FORMAT_UNDEFINED || !w || !h || !levels) return fail("unsupported texture format %u (%ux%u, %u levels)", f, w, h, levels);
    MemTag tag(RENDERER_VRAM_TEXTURES);
    VkDeviceSize total = 0;
    for (uint32_t l = 0; l < levels; l++) total += level_size(f, w >> l ? w >> l : 1, h >> l ? h >> l : 1);
    if (make_image(&t->im, fmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, w, h, levels,
                   &t->bytes))
        return -1;
    if (f == RENDERER_TEX_L8 && l8_view(&t->im, levels)) return -1;
    bool batch = total <= UPLOAD_BATCH_BYTES && !ub.unavailable;
    if (batch && !ub.staging.buf) {
        MemTag other(RENDERER_VRAM_OTHER);   // staging, not a texture
        if (make_buffer(&ub.staging, UPLOAD_BATCH_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
            free_buffer(&ub.staging);
            ub.unavailable = true;
            batch = false;
        }
    }
    if (batch && ub.used + total > UPLOAD_BATCH_BYTES && flush_uploads()) return -1;
    Buffer up;
    VkDeviceSize off = 0;
    if (batch) {
        off = ub.used;
        ub.used = (ub.used + total + 15) & ~(VkDeviceSize)15;   // BC blocks and texels stay aligned
        memcpy((uint8_t *)ub.staging.map + off, data, total);
    } else {
        if (make_buffer(&up, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return -1;
        memcpy(up.map, data, total);
    }
    std::vector<VkBufferImageCopy> regions(levels);
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        regions[l] = VkBufferImageCopy{};
        regions[l].bufferOffset = off;
        regions[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
        regions[l].imageExtent = {lw, lh, 1};
        off += level_size(f, lw, lh);
    }
    VkCommandBuffer saved = s.cmd;
    if (batch) {   // barrier() records into s.cmd
        if (!ub.open) {
            VKC(vkResetCommandBuffer, ub.cmd, 0);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VKC(vkBeginCommandBuffer, ub.cmd, &bi);
            ub.open = true;
        }
        s.cmd = ub.cmd;
    } else {
        begin_once(&saved);
    }
    barrier(t->im.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, levels);
    vkCmdCopyBufferToImage(s.cmd, batch ? ub.staging.buf : up.buf, t->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, regions.data());
    barrier(t->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, levels);
    if (batch) {
        s.cmd = saved;
        return 0;
    }
    int r = submit_once(saved);
    free_buffer(&up);
    return r ? -1 : 0;
}

// Drops every cached texture (and the white one, which is recreated).
void drop_textures() {
    flush_uploads();   // no pending copy into an image freed below
    VKC(vkDeviceWaitIdle, s.dev);
    for (Texture &t : s.textures) free_image(&t.im);
    s.textures.clear();
    s.by_key.clear();
    s.texture_bytes = 0;
    free_image(&s.white.im);
}

// The frame at the output size before sharpening: the color target, or the last downsample image
// when supersampled.
Image &frame_image() { return s.ss > 1 ? s.down[s.ss == 4 ? 1 : 0] : s.color; }

// The finished frame before the gamma ramp, at the displayed size: the FSR (RCAS) image, else the
// sharpened image, else the frame.
Image &pre_gamma_image() { return s.fsr.ok ? s.fsr.out : s.sharp.ok ? s.sharp.out : frame_image(); }

// The finished frame (TRANSFER_SRC_OPTIMAL after record_frame): the gamma-ramped image when the
// frame went through the ramp, else the FSR image, else the sharpened image, else the frame.
VkImage output_image() { return s.gamma.active ? s.gamma.out.img : pre_gamma_image().img; }

// Blits the finished target (TRANSFER_SRC_OPTIMAL) into a swapchain image: black borders, the
// largest rectangle of the target's aspect, linear filtering; the image ends ready to present.
bool record_overlay_sc(uint32_t index);
void record_blit(VkImage dst, uint32_t index, VkImage src = VK_NULL_HANDLE) {   // src: --frame-gen's generated frame
    barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue black{};
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(s.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const int64_t dw = s.sc_extent.width, dh = s.sc_extent.height;
    int64_t w = dw, h = dh;
    const int64_t fw = s.dispw, fh = s.disph;
    if (dw * fh >= dh * fw) w = (dh * fw * 2 + fh) / (2 * fh);   // pillarbox
    else h = (dw * fh * 2 + fw) / (2 * fw);                      // letterbox
    VkImageBlit b{};
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {s.dispw, s.disph, 1};
    b.dstSubresource = b.srcSubresource;
    b.dstOffsets[0] = {(int32_t)((dw - w) / 2), (int32_t)((dh - h) / 2), 0};
    b.dstOffsets[1] = {(int32_t)((dw - w) / 2 + w), (int32_t)((dh - h) / 2 + h), 1};
    vkCmdBlitImage(s.cmd, src ? src : output_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                   VK_FILTER_LINEAR);
    if (s.ov.ok && record_overlay_sc(index)) return;   // the overlay pass ends the image in PRESENT_SRC
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

uint64_t program_hash(const RPixelShader &ps) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t *b = (const uint8_t *)&ps;
    for (size_t i = 0; i < sizeof ps; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
// Index of a program's structure (added when new); shadow = shadow_word of the bound stages.
uint32_t shape_index(const RPixelShader &ps, uint32_t shadow) {
    Shape sh;
    uint32_t *w = sh.w;
    *w++ = ps.combiners;
    for (int i = 0; i < 8; i++) *w++ = ps.rgb_in[i];
    for (int i = 0; i < 8; i++) *w++ = ps.alpha_in[i];
    for (int i = 0; i < 8; i++) *w++ = ps.rgb_out[i];
    for (int i = 0; i < 8; i++) *w++ = ps.alpha_out[i];
    *w++ = ps.final_abcd;
    *w++ = ps.final_efg;
    *w++ = ps.tex_modes;
    *w++ = shadow;
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < SHAPE_WORDS; i++) h = (h ^ sh.w[i]) * 1099511628211ull;
    for (auto it = s.shape_by_hash.find(h); it != s.shape_by_hash.end(); it = s.shape_by_hash.find(++h))
        if (!memcmp(&s.shapes[it->second], &sh, sizeof sh)) return it->second;
    s.shapes.push_back(sh);
    return s.shape_by_hash[h] = (uint32_t)s.shapes.size() - 1;
}
// Index of a program in this frame's list (added when new).
uint32_t program_index(const RPixelShader &ps) {
    uint64_t h = program_hash(ps);
    auto it = s.program_by_hash.find(h);
    if (it != s.program_by_hash.end() && !memcmp(&s.programs[it->second], &ps, sizeof ps)) return it->second;
    s.programs.push_back(ps);
    uint32_t i = (uint32_t)s.programs.size() - 1;
    if (it == s.program_by_hash.end()) s.program_by_hash[h] = i;
    return i;
}

// Uploads this frame's programs into its slot's storage buffer (grown on demand, like the geometry).
int upload_programs() {
    Buffer &p = cur().programs;
    VkDeviceSize need = s.programs.size() * sizeof(RPixelShader);
    if (need > p.size) {
        free_buffer(&p);
        VkDeviceSize size = 64 * 1024;
        while (size < need) size *= 2;
        if (make_buffer(&p, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return -1;
    }
    memcpy(p.map, s.programs.data(), need);
    return 0;
}

// Descriptor set of a stage-texture combination for this frame (VK_NULL_HANDLE when the pool is
// exhausted).
VkDescriptorSet frame_set(const uint32_t tex[RENDERER_STAGES], std::unordered_map<uint64_t, VkDescriptorSet> &sets) {
    uint64_t key = 0;
    for (int i = 0; i < RENDERER_STAGES; i++) key |= (uint64_t)(tex[i] & 0xFFFF) << (16 * i);
    auto it = sets.find(key);
    if (it != sets.end()) return it->second;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = cur().set_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &s.set_layout;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, &set) != VK_SUCCESS) return sets[key] = VK_NULL_HANDLE;
    VkDescriptorImageInfo ii[RENDERER_STAGES];
    VkWriteDescriptorSet w[RENDERER_STAGES + 5];
    for (int i = 0; i < RENDERER_STAGES; i++) {
        const bool surf = tex[i] != BASE_TEXTURE && (tex[i] & SURFACE_BIT);
        const Texture &t = tex[i] && tex[i] != BASE_TEXTURE && !surf && !is_movie(tex[i]) && tex[i] != PREV_TEXTURE ? s.textures[tex[i] - 1] : s.white;
        ii[i] = {s.sampler, t.im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        if (is_movie(tex[i])) ii[i] = {s.clamp_sampler, s.movie[tex[i] - MOVIE_TEXTURE].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        if (tex[i] == PREV_TEXTURE) ii[i] = {s.clamp_sampler, s.prev.im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        if (tex[i] == BASE_TEXTURE) ii[i] = {s.base_nearest ? s.nearest_sampler : s.clamp_sampler, s.base.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        if (surf) {
            const Surface &sf = s.surfaces[(tex[i] & ~SURFACE_BIT) - 1];
            // Depth surfaces read the border outside (lit); color surfaces clamp to the edge, as the
            // title's blur chain asks (D3DTADDRESS_CLAMP), so its passes do not pull a border in.
            ii[i] = {sf.kind == RENDERER_SURFACE_COLOR ? s.clamp_sampler : s.border_sampler, sf.im.view,
                     sf.kind == RENDERER_SURFACE_COLOR ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        }
        w[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &ii[i];
    }
    VkDescriptorBufferInfo bi{cur().programs.buf, 0, VK_WHOLE_SIZE};
    w[RENDERER_STAGES] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[RENDERER_STAGES].dstSet = set;
    w[RENDERER_STAGES].dstBinding = RENDERER_STAGES;
    w[RENDERER_STAGES].descriptorCount = 1;
    w[RENDERER_STAGES].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[RENDERER_STAGES].pBufferInfo = &bi;
    VkDescriptorBufferInfo li{s.lshadow.ok ? s.lshadow.buf.buf : cur().programs.buf, 0, VK_WHOLE_SIZE};
    w[RENDERER_STAGES + 1] = w[RENDERER_STAGES];
    w[RENDERER_STAGES + 1].dstBinding = RENDERER_STAGES + 1;
    w[RENDERER_STAGES + 1].pBufferInfo = &li;
    const bool ws = s.wshadow.ok;
    VkDescriptorImageInfo wi = ws ? VkDescriptorImageInfo{s.nearest_sampler, s.wshadow.map.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}
                                  : VkDescriptorImageInfo{s.sampler, s.white.im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w[RENDERER_STAGES + 2] = w[0];
    w[RENDERER_STAGES + 2].dstBinding = RENDERER_STAGES + 2;
    w[RENDERER_STAGES + 2].pImageInfo = &wi;
    VkDescriptorBufferInfo wb{ws ? s.wshadow.buf.buf : cur().programs.buf, 0, VK_WHOLE_SIZE};
    w[RENDERER_STAGES + 3] = w[RENDERER_STAGES];
    w[RENDERER_STAGES + 3].dstBinding = RENDERER_STAGES + 3;
    w[RENDERER_STAGES + 3].pBufferInfo = &wb;
    VkDescriptorImageInfo lw = s.walls.img.view ? VkDescriptorImageInfo{s.nearest_sampler, s.walls.img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                                            : VkDescriptorImageInfo{s.sampler, s.white.im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w[RENDERER_STAGES + 4] = w[0];
    w[RENDERER_STAGES + 4].dstBinding = RENDERER_STAGES + 4;
    w[RENDERER_STAGES + 4].pImageInfo = &lw;
    vkUpdateDescriptorSets(s.dev, RENDERER_STAGES + 5, w, 0, nullptr);
    return sets[key] = set;
}

int make_white() {
    const uint32_t white = 0xFFFFFFFFu;
    s.white = Texture();
    return make_texture(&s.white, RENDERER_TEX_BGRA8, 1, 1, 1, &white);
}


VkFormat surface_format(uint32_t kind) {
    return kind == RENDERER_SURFACE_COLOR ? VK_FORMAT_B8G8R8A8_UNORM : kind == RENDERER_SURFACE_DEPTH24 ? s.depth24_format : VK_FORMAT_D16_UNORM;
}
const Surface *surface_of(uint32_t handle) {
    uint32_t i = handle & ~SURFACE_BIT;
    return (handle & SURFACE_BIT) && i >= 1 && i <= s.surfaces.size() ? &s.surfaces[i - 1] : nullptr;
}

// The offscreen render passes (one per depth class): color B8G8R8A8 and depth, one sample,
// attachments loaded and stored in their attachment layouts (the caller transitions surfaces).
int create_offscreen_passes() {
    const VkFormat depth[2] = {VK_FORMAT_D16_UNORM, s.depth24_format};
    for (int c = 0; c < 2; c++) {
        VkAttachmentDescription att[2]{};
        att[0].format = VK_FORMAT_B8G8R8A8_UNORM;
        att[1].format = depth[c];
        for (int i = 0; i < 2; i++) {
            att[i].samples = VK_SAMPLE_COUNT_1_BIT;
            att[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            att[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            att[i].initialLayout = att[i].finalLayout =
                i ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &cref;
        sub.pDepthStencilAttachment = &dref;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 2;
        rp.pAttachments = att;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        if (VKC(vkCreateRenderPass, s.dev, &rp, nullptr, &s.off_pass[c]) != VK_SUCCESS) return fail("vkCreateRenderPass (offscreen) failed");
    }
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    if (VKC(vkCreateSampler, s.dev, &si, nullptr, &s.border_sampler) != VK_SUCCESS) return fail("vkCreateSampler (border) failed");
    return 0;
}

// Creates an image for a surface or a dummy attachment, cleared (color black, depth 1.0) and left in
// `layout`, through a synchronous one-time submit.
int make_surface_image(Image *im, VkFormat fmt, bool depth, int w, int h, VkImageLayout layout, bool sampled) {
    const VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (sampled) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (make_image(im, fmt, usage, aspect, (uint32_t)w, (uint32_t)h)) return -1;
    VkCommandBuffer saved;
    begin_once(&saved);
    barrier(im->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1, aspect);
    const VkImageSubresourceRange range{aspect, 0, 1, 0, 1};
    if (depth) {
        const VkClearDepthStencilValue one{1.0f, 0};
        vkCmdClearDepthStencilImage(s.cmd, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &range);
    } else {
        const VkClearColorValue black{};
        vkCmdClearColorImage(s.cmd, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    }
    barrier(im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT | (depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                               : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT),
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 1, aspect);
    return submit_once(saved);
}

// Framebuffer of an offscreen target pair (created on first use); nullptr on failure.
OffscreenFb *offscreen_fb(uint32_t color, uint32_t depth) {
    const uint64_t key = (uint64_t)color << 16 | depth;
    auto it = s.offscreen_fbs.find(key);
    if (it != s.offscreen_fbs.end()) return it->second.fb ? &it->second : nullptr;
    OffscreenFb &f = s.offscreen_fbs[key];
    const Surface *c = surface_of(color), *d = surface_of(depth);
    const Surface *any = c ? c : d;
    if (!any) return nullptr;
    f.w = any->w;
    f.h = any->h;
    f.gw = any->gw;
    f.gh = any->gh;
    f.pass_class = d && d->kind == RENDERER_SURFACE_DEPTH24 ? 2 : 1;
    const VkFormat dfmt = f.pass_class == 2 ? s.depth24_format : VK_FORMAT_D16_UNORM;
    MemTag tag(RENDERER_VRAM_SURFACES);
    if ((!c && make_surface_image(&f.dummy_color, VK_FORMAT_B8G8R8A8_UNORM, false, f.w, f.h, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)) ||
        (!d && make_surface_image(&f.dummy_depth, dfmt, true, f.w, f.h, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, false)))
        return nullptr;
    VkImageView views[2] = {c ? c->im.view : f.dummy_color.view, d ? d->im.view : f.dummy_depth.view};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = s.off_pass[f.pass_class - 1];
    fi.attachmentCount = 2;
    fi.pAttachments = views;
    fi.width = (uint32_t)f.w;
    fi.height = (uint32_t)f.h;
    fi.layers = 1;
    if (VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &f.fb) != VK_SUCCESS) {
        fail("vkCreateFramebuffer (offscreen) failed");
        f.fb = VK_NULL_HANDLE;
        return nullptr;
    }
    return &f;
}

void free_surfaces() {
    for (auto &f : s.offscreen_fbs) {
        if (f.second.fb) vkDestroyFramebuffer(s.dev, f.second.fb, nullptr);
        free_image(&f.second.dummy_color);
        free_image(&f.second.dummy_depth);
    }
    s.offscreen_fbs.clear();
    for (Surface &sf : s.surfaces) free_image(&sf.im);
    s.surfaces.clear();
    s.surface_by_key.clear();
    for (VkRenderPass &p : s.off_pass)
        if (p) vkDestroyRenderPass(s.dev, p, nullptr), p = VK_NULL_HANDLE;
    if (s.border_sampler) vkDestroySampler(s.dev, s.border_sampler, nullptr);
    s.border_sampler = VK_NULL_HANDLE;
}

// Shadow compare word of a draw (specialization constant 36, shaders/geom.frag): bits 0-2 the
// compare function, 4-7 the stages bound to a depth surface, 8-11 those of them with 24-bit depth,
// 12 soft filtering (RendererConfig.shadow_soft), 13 shadow range (RendererConfig.shadow_range), 14
// lamp shadows (RendererConfig.lamp_shadow, created), 15 world shadows (RendererConfig.world_shadow,
// created; frame-target draws only);
// 0 when no stage samples a depth surface (the function does not split pipelines then).
uint32_t shadow_word(const uint32_t tex[RENDERER_STAGES], uint32_t func) {
    uint32_t w = 0;
    for (int i = 0; i < RENDERER_STAGES; i++) {
        const Surface *sf = tex[i] != BASE_TEXTURE ? surface_of(tex[i]) : nullptr;
        if (!sf || sf->kind == RENDERER_SURFACE_COLOR) continue;
        w |= 1u << (4 + i);
        if (sf->kind == RENDERER_SURFACE_DEPTH24) w |= 1u << (8 + i);
    }
    return w ? w | (func & 7u) | (cfg.shadow_soft ? 1u << 12 : 0u) | (cfg.shadow_range > 0 ? 1u << 13 : 0u) |
                   (s.lshadow.ok ? 1u << 14 : 0u) | (s.wshadow.ok && !s.cur_target[0] && !s.cur_target[1] ? 1u << 15 : 0u)
             : 0;
}
}  // namespace

static void glass_free();
static void sharpen_init();
static void overlay_init();
static void overlay_free();
static void gp_init();
static void gp_free();
static void record_overlay_rb();
static void sharpen_free();
static void fsr_init();
static void fsr_free();
static void record_fsr();
static void gamma_free();
static void record_gamma();
static void bloom_init();
static void bloom_free();
static void filmic_init();
static void filmic_free();
static void taa_init();
static VkImageView chars_post_view();
static void taa_free();
static void fg_init();
static void fg_free();
static void taa_poses();
static void ae_collect();
static void dof_collect();
static void rain_init();
static void rain_free();
static void sky_init();
static void sky_free();
static void ssao_init();
static void ssao_free();
static void splash_init();
static void splash_free();
static void lamp_init();
static void lamp_free();
static void shaft_init();
static void volfog_init();
static void volfog_free();
static void shaft_free();
static void contact_init();
static void contact_free();
static void wet_init();
static void wet_free();
static void cover_init();
static void cover_free();
static void chars_init();
static void chars_free();
static void soft_init();
static void soft_free();
static void charlit_init();
static void charlit_free();
static void charlit_update();
static void lshadow_init();
static void walls_init();
static void walls_free();
static void lshadow_free();
static void wshadow_init();
static void wshadow_free();
static void wshadow_record(VkDeviceSize index_offset, float sx, float sy, std::unordered_map<uint64_t, VkDescriptorSet> &sets);
static void lshadow_update();
static void skin_init();
static void skin_free();
static int skin_upload(VkDeviceSize index_offset);
static void skin_record();

extern "C" {

const char *renderer_error(void) { return s.error; }
const char *renderer_device_name(void) { return s.name; }

void renderer_configure(const RendererConfig *c) { cfg = *c; }
void renderer_output_size(int *w, int *h) {
    *w = s.dispw;
    *h = s.disph;
}
int renderer_fsr(int *render_w, int *render_h) {
    if (render_w) *render_w = s.fw;
    if (render_h) *render_h = s.fh;
    return s.dev && s.fsr.ok ? 1 : 0;
}
int renderer_supersample(void) { return s.dev ? s.ss : 1; }
int renderer_sharpen(void) { return s.dev && s.sharp.ok ? s.sharp.amount : 0; }
int renderer_fps_overlay_on(void) { return s.dev && s.ov.ok; }
void renderer_fps_overlay_text(const char *text) { s.ov.n = text ? fps_overlay_glyphs(text, s.ov.glyph, FPS_MAX_CHARS) : 0; }
int renderer_fps_overlay_gpu(double *busy_ms, int *frames) {
    *busy_ms = s.ov.busy_ms;
    *frames = s.ov.busy_frames;
    s.ov.busy_ms = 0;
    s.ov.busy_frames = 0;
    return s.dev && s.ov.queries ? 1 : 0;
}
int renderer_samples(void) { return (int)s.samples; }
int renderer_frames_in_flight(void) { return s.dev ? s.nframes : configured_frames(); }
float renderer_anisotropy(void) { return s.aniso; }
float renderer_texture_lod_bias(void) { return s.lod_bias; }

int renderer_init(int w, int h) {
    if (s.dev) return 0;
    if (w <= 0 || h <= 0) return fail("bad target size %dx%d", w, h);
    s.w = w;
    s.h = h;
    s.fw = s.ow = cfg.out_w > 0 ? cfg.out_w : w;   // s.ow / s.oh: times the supersampling factor in create_targets
    s.fh = s.oh = cfg.out_h > 0 ? cfg.out_h : h;
    s.dispw = s.fw;
    s.disph = s.fh;
    // FSR: the scene at the internal size, upscaled to the output size (fsr_init). An internal
    // size not below the output on either axis leaves it off.
    s.fsr = Fsr();
    if (cfg.fsr_w > 0 && cfg.fsr_h > 0 && cfg.fsr_w <= s.fw && cfg.fsr_h <= s.fh && (cfg.fsr_w < s.fw || cfg.fsr_h < s.fh)) {
        s.fsr.req = true;
        s.fw = s.ow = cfg.fsr_w;
        s.fh = s.oh = cfg.fsr_h;
    }
    if (load_loader() || create_instance() || pick_device() || create_device() || create_pipeline_cache() || create_targets() || create_commands() ||
        create_texture_state() || create_offscreen_passes() || create_default_pipelines() || make_white()) {
        char why[sizeof s.error];
        snprintf(why, sizeof why, "%s", s.error);
        renderer_shutdown();
        snprintf(s.error, sizeof s.error, "%s", why);
        return -1;
    }
    sharpen_init();   // optional: without it the frame is presented as is
    fsr_init();       // likewise the upscale: without it the frame is presented at the internal size
    gp_init();
    overlay_init();   // likewise the frame counter (after sharpen_init: it draws on the sharpened image)
    bloom_init();     // optional too: without it renderer_bloom is ignored
    chars_init();     // the character mask (before the depth of field, splash and wet ground that read it)
    filmic_init();    // optional too: renderer_filmic (after bloom: it reads a bloom level)
    taa_init();       // and renderer_taa
    fg_init();        // and renderer_frame_gen (after taa_init: it reads the velocity target)
    cover_init();     // likewise renderer_rain_cover (before the rain, splash and wet ground that read it)
    rain_init();      // likewise renderer_rain
    sky_init();       // and renderer_sky / renderer_skyline
    ssao_init();      // and renderer_ssao
    splash_init();    // and renderer_rain_splash
    walls_init();     // the wall map (before wet_init, lshadow_init and lamp_init, whose sets bind it)
    wet_init();       // and renderer_rain_wet
    soft_init();      // and renderer_soft
    charlit_init();   // and renderer_char_lights, renderer_draw_lit
    lshadow_init();   // and renderer_lamp_shadows
    wshadow_init();   // and renderer_world_shadow
    skin_init();      // and renderer_draw_skinned
    lamp_init();      // and renderer_lamp_lights (after chars_init: it reads the character mask)
    contact_init();   // and renderer_contact (likewise)
    shaft_init();     // and renderer_light_shafts
    volfog_init();    // and renderer_volfog
    pipe_warm_start();   // after every *_init: the pre-warm thread only reads their resources
    if (s.surface && create_swapchain()) drop_present();
    return 0;
}

void renderer_shutdown(void) {
    if (s.surface) drop_present();
    if (s.dev) {
        flush_uploads();
        VKC(vkDeviceWaitIdle, s.dev);
        pipe_stop();   // first: the pre-warm thread reads the resources freed below
        for (FrameSlot &f : s.frames) {
            free_buffer(&f.upload);
            free_buffer(&f.geometry);
            free_buffer(&f.programs);
            free_buffer(&f.movie);
            if (f.set_pool) vkDestroyDescriptorPool(s.dev, f.set_pool, nullptr);
            if (f.fence) vkDestroyFence(s.dev, f.fence, nullptr);
        }
        if (s.once_fence) vkDestroyFence(s.dev, s.once_fence, nullptr);
        if (ub.fence) vkDestroyFence(s.dev, ub.fence, nullptr);
        free_buffer(&ub.staging);
        ub = UploadBatch();
        free_buffer(&s.readback);
        for (int i = 0; i < MOVIE_PLANES; i++) free_image(&s.movie[i]);
        drop_textures();
        free_surfaces();
        glass_free();
        sharpen_free();
        fsr_free();
        gamma_free();
        overlay_free();
        gp_free();
        filmic_free();
        taa_free();
        fg_free();
        bloom_free();
        rain_free();
        sky_free();
        ssao_free();
        splash_free();
        wet_free();
        cover_free();
        chars_free();
        soft_free();
        charlit_free();
        lshadow_free();
        walls_free();
        wshadow_free();
        skin_free();
        lamp_free();
        shaft_free();
        volfog_free();
        contact_free();
        if (s.split_pass) vkDestroyRenderPass(s.dev, s.split_pass, nullptr);
        if (s.resume_pass) vkDestroyRenderPass(s.dev, s.resume_pass, nullptr);
        for (auto &p : s.pipelines)
            if (p.second) vkDestroyPipeline(s.dev, p.second, nullptr);
        if (s.vs) vkDestroyShaderModule(s.dev, s.vs, nullptr);
        if (s.fs) vkDestroyShaderModule(s.dev, s.fs, nullptr);
        if (s.layout) vkDestroyPipelineLayout(s.dev, s.layout, nullptr);
        if (s.emit.fs) vkDestroyShaderModule(s.dev, s.emit.fs, nullptr);
        if (s.emit.lit_fs) vkDestroyShaderModule(s.dev, s.emit.lit_fs, nullptr);
        if (s.relief.fs) vkDestroyShaderModule(s.dev, s.relief.fs, nullptr);
        if (s.hair.fs) vkDestroyShaderModule(s.dev, s.hair.fs, nullptr);
        if (s.hair.relief_fs) vkDestroyShaderModule(s.dev, s.hair.relief_fs, nullptr);
        for (VkShaderModule m : s.skinlit.fs)
            if (m) vkDestroyShaderModule(s.dev, m, nullptr);
        for (VkShaderModule m : s.skinlit.mat_fs)
            if (m) vkDestroyShaderModule(s.dev, m, nullptr);
        if (s.emit.layout) vkDestroyPipelineLayout(s.dev, s.emit.layout, nullptr);
        if (s.emit.lit_layout) vkDestroyPipelineLayout(s.dev, s.emit.lit_layout, nullptr);
        if (s.set_layout) vkDestroyDescriptorSetLayout(s.dev, s.set_layout, nullptr);
        if (s.sampler) vkDestroySampler(s.dev, s.sampler, nullptr);
        if (s.clamp_sampler) vkDestroySampler(s.dev, s.clamp_sampler, nullptr);
        if (s.nearest_sampler) vkDestroySampler(s.dev, s.nearest_sampler, nullptr);
        if (s.pool) vkDestroyCommandPool(s.dev, s.pool, nullptr);
        if (s.fb) vkDestroyFramebuffer(s.dev, s.fb, nullptr);
        if (s.pass) vkDestroyRenderPass(s.dev, s.pass, nullptr);
        free_image(&s.color);
        free_image(&s.depth);
        free_image(&s.msaa);
        free_image(&s.base);
        free_image(&s.prev.im);
        free_image(&s.down[0]);
        free_image(&s.down[1]);
        vkDestroyDevice(s.dev, nullptr);
    }
    if (s.inst) vkDestroyInstance(s.inst, nullptr);
    // The loader stays loaded: unloading drivers at runtime is not worth the risk.
    void *lib = s.lib;
    s = State();
    s.lib = lib;
}

void renderer_begin(const uint32_t *base, int pitch_bytes) {
    s.frame_serial++;
    s.prev.marked = false;
    s.ops.clear();
    s.verts.reset(nullptr, 0);
    s.indices.clear();
    s.normals.clear();
    s.skin.jobs.clear();
    s.skin.prev.clear();
    {   // --taa: this frame's poses become the previous ones
        Taa &t = s.taa;
        t.ids_prev.swap(t.ids_now);
        t.ids_now.clear();
        t.pose_prev.swap(t.pose_now);
        t.pose_now.clear();
        t.seen.clear();
        t.vel.clear();
        t.next_id = 0;
    }
    s.skin.njobs.clear();
    s.skin.ins.clear();
    s.skin.consts.clear();
    s.skin.last_consts = nullptr;
    s.skin.data.clear();
    s.skin.adj.clear();
    s.skin.prog_at.clear();
    s.skin.nfaces = s.skin.nparts = s.skin.nverts = 0;
    s.skin.dst.clear();
    s.skin.ndst.clear();
    s.skin.debug_verts = nullptr;
    s.skin.debug_normals = nullptr;
    s.skin.vcheck.clear();
    s.charlit.marked = false;
    s.lshadow.marked = false;
    s.wshadow.marked = false;
    s.bloom.marked = false;
    s.filmic.marked = false;
    s.taa.marked = false;
    s.fg.marked = s.fg.hud_marked = false;
    s.dof.want = false;
    s.ssao.marked = false;
    s.soft.marked = false;
    s.lamp.marked = false;
    s.shaft.marked = false;
    s.volfog.marked = false;
    s.volfog.prev_ran = s.volfog.ran;
    s.volfog.ran = false;
    s.contact.marked = false;
    s.splash.marked = false;
    s.wet.marked = false;
    s.cover.marked = false;
    s.rain.list.clear();
    s.sky.list.clear();
    s.sky.lines.clear();
    for (uint32_t &t : s.cur_tex) t = 0;
    s.cur_alpha[0] = s.cur_alpha[1] = 0;
    renderer_state_default(&s.cur_state);
    s.cur_target[0] = s.cur_target[1] = 0;
    s.programs.clear();
    s.program_by_hash.clear();
    RPixelShader def;
    renderer_default_pixel_shader(&def);
    s.cur_ps = def;
    s.cur_program = program_index(def);
    s.cur_shape = shape_index(def, 0);
    if (!s.dev) return;
    // The slot's previous frame (frames_in_flight frames ago) must be done before its upload
    // buffer is rewritten below; a failed wait is reported by renderer_end / renderer_present.
    s.wait_ms = 0;
    if (!wait_slot() && cfg.direct_geometry) s.verts.reset(cur().geometry.map, cur().geometry.size);   // VertexSink
    if (s.textures.size() >= MAX_TEXTURES || s.texture_bytes >= texture_budget || texture_drop_requested) {
        texture_drop_requested = false;
        texture_drops++;
        drop_textures();
        make_white();
    }
    s.base_pending = false;
    if (s.quad_base && !base) return;   // the pass clears to black
    uint32_t *up = (uint32_t *)cur().upload.map;
    for (int y = 0; y < s.h; y++) {
        if (base) memcpy(up + (size_t)y * s.w, (const uint8_t *)base + (size_t)y * pitch_bytes, (size_t)s.w * 4);
        else memset(up + (size_t)y * s.w, 0, (size_t)s.w * 4);
    }
    if (base && s.bloom.ok && !s.quad_base)   // copied into the target: an empty light mask (alpha 0)
        for (size_t i = 0, n = (size_t)s.w * s.h; i < n; i++) up[i] &= 0x00FFFFFFu;
    if (!s.quad_base) return;
    // The base image as the first draw: a full-screen quad, stage 0 = the base image, the default
    // program (t0 x white), no depth test or blending.
    s.base_pending = true;
    RVertex q[4];
    const float xy[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
    for (int i = 0; i < 4; i++) {
        q[i] = RVertex{};
        q[i].x = xy[i][0];
        q[i].y = xy[i][1];
        q[i].w = 1;
        q[i].color = 0xFFFFFFFFu;
        q[i].tex[0][0] = xy[i][0] > 0 ? 1.0f : 0.0f;
        q[i].tex[0][1] = xy[i][1] > 0 ? 0.0f : 1.0f;
        q[i].tex[0][3] = 1;
    }
    const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
    const RViewport vp = {0, 0, (float)s.w, (float)s.h, 0, 1};
    s.cur_tex[0] = BASE_TEXTURE;
    renderer_draw(&vp, 0, q, 4, qi, 6);
    s.cur_tex[0] = 0;
}

void renderer_clear(uint32_t flags, uint32_t color, float z) {
    Op op{};
    op.kind = OP_CLEAR;
    op.target[0] = s.cur_target[0];
    op.target[1] = s.cur_target[1];
    op.flags = flags;
    op.color = color;
    op.z = z;
    s.ops.push_back(op);
}

// --normal-maps: a draw that has just become lit (CHARLIT_KEY) with relief maps recorded takes the
// relief variant (never over an emissive draw, which records no maps).
static void relief_mark(Op &op) {
    if ((op.relief_tex[0] | op.relief_tex[1]) && !(op.shape & EMIT_SHAPE) && !(op.pipe_key & SOFT_KEY)) op.shape |= RELIEF_SHAPE;
    // --hair: likewise (a world-lit draw drops it again: renderer_draw_world_lit)
    if (op.hair_tex && cfg.hair > 0 && !(op.shape & EMIT_SHAPE) && !(op.pipe_key & SOFT_KEY)) op.shape |= HAIR_SHAPE;
    // --skin: likewise
    if (op.skin_tex && (cfg.skin > 0 || cfg.eyes > 0 || cfg.cloth > 0) && !(op.shape & EMIT_SHAPE) && !(op.pipe_key & SOFT_KEY)) op.shape |= SKIN_SHAPE;
}

// See-through texels of an alpha-blended draw that writes depth. The
// title draws much of its world, chain-link fences and decals included, blended (SRC_ALPHA /
// ONE_MINUS_SRC_ALPHA, ADD) with depth writes and an alpha test that keeps nearly every texel
// (GREATER 1). From afar a fence's mip levels average its holes into an even haze, which passes that
// test and wrote depth over the whole fence: a decal on the wall behind it drawn later failed the
// depth test there (half a graffiti). Such a draw on the frame target, with a texture that may
// sample under half opaque, is split in exec_op: its depth alone where the texel is at least half
// opaque (GREATEREQUAL SEE_THROUGH_REF), then its color as recorded, depth tested against it without
// depth writes. Opaque textures (BC1, or every texel at least half opaque) keep the single draw.
static bool see_through_split(const Op &op) {
    const uint32_t k = op.pipe_key;
    if (op.target[0] || op.target[1] || (k & SOFT_KEY)) return false;
    if (!(k >> 18 & 1) || !(k >> 19 & 1) || !(k >> 13 & 1) || !(k >> 14 & 15)) return false;   // depth test + write, blending, color
    if ((k & 31) != VK_BLEND_FACTOR_SRC_ALPHA || (k >> 5 & 31) != VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA || (k >> 10 & 7) != VK_BLEND_OP_ADD)
        return false;
    const uint32_t func = op.push[0], ref = op.push[1];
    if (func && ((func != 5 && func != 7) || ref >= SEE_THROUGH_REF)) return false;   // off, GREATER or GREATEREQUAL below half
    for (int i = 0; i < RENDERER_STAGES; i++) {
        const uint32_t t = op.tex[i];
        if (t && !(t & SURFACE_BIT) && t <= s.textures.size() && s.textures[t - 1].see_through) return true;
    }
    return false;
}

// v NULL (renderer_draw_skinned): nv vertices are reserved for the compute pre-pass to fill (zero
// in the host copy; left as they are in the mapped buffer, VertexSink).
void renderer_draw(const RViewport *vp, uint32_t flags, const RVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni) {
    if (!nv || ni < 3) return;
    Op op{};
    op.kind = OP_DRAW;
    op.target[0] = s.cur_target[0];
    op.target[1] = s.cur_target[1];
    op.vp = *vp;
    op.draw_flags = flags;
    op.first_index = (uint32_t)s.indices.size();
    op.index_count = ni - ni % 3;
    op.vertex_offset = (int32_t)s.verts.size();
    for (int i = 0; i < RENDERER_STAGES; i++) op.tex[i] = s.cur_tex[i];
    op.push[0] = s.cur_alpha[0];
    op.push[1] = s.cur_alpha[1];
    op.push[2] = s.cur_program;
    op.pipe_key = pipeline_key(s.cur_state, flags);
    // A soft particle: after renderer_soft, on the frame target, depth tested and not written
    // (record_frame drops the key bit again when the frame's soft particles do not run).
    if ((flags & RENDERER_SOFT) && s.soft.marked && !op.target[0] && !op.target[1] && (flags & RENDERER_DEPTH_TEST) && !s.cur_state.depth_write)
        op.pipe_key |= SOFT_KEY;
    if (op.target[0] || op.target[1]) {   // offscreen: the pass class of its depth surface
        const Surface *d = surface_of(op.target[1]);
        op.pipe_key |= (d && d->kind == RENDERER_SURFACE_DEPTH24 ? 2u : 1u) << 25;
        // A title color surface keeps the alpha the title writes (NV2A mask A 1<<24), as its
        // A8R8G8B8 memory does: the blur chain (0xbc1c0) samples it with an alpha test GREATER 1,
        // so an alpha left at 0 would drop every pass and the composite (the frame target never
        // stores alpha; its channel is the bloom mask).
        if (op.target[0] && (s.cur_state.color_mask & 0x01000000u)) op.pipe_key |= (uint32_t)VK_COLOR_COMPONENT_A_BIT << 14;
    }
    const uint32_t shadow = shadow_word(s.cur_tex, s.cur_ps.shadow_func);
    op.shape = shadow ? shape_index(s.cur_ps, shadow) : s.cur_shape;
    op.blend_color = s.cur_state.blend_color;
    if (s.emit.tex && !op.target[0] && !op.target[1] && !(op.pipe_key & SOFT_KEY)) {   // neon glow: frame target only
        op.shape |= EMIT_SHAPE;
        op.emit_tex = s.emit.tex;
        op.emit[0] = s.emit.strength;
        op.emit[1] = s.emit.bloom;
        op.emit[2] = (float)s.emit.stage;
    } else if (s.relief.tex[0] | s.relief.tex[1]) {   // --normal-maps: taken up when the draw is lit (relief_mark)
        op.relief_tex[0] = s.relief.tex[0];
        op.relief_tex[1] = s.relief.tex[1];
        memcpy(op.relief, s.relief.p, sizeof op.relief);
    }
    if (!(op.shape & EMIT_SHAPE)) op.hair_tex = s.hair.tex;   // --hair: taken up when the draw is lit as a character (relief_mark)
    if (!(op.shape & EMIT_SHAPE)) op.skin_tex = s.skinlit.tex;   // --skin: likewise
    op.eye_open = s.skinlit.eye_open;
    op.split_depth = see_through_split(op);
    s.verts.add(v, nv);
    s.indices.insert(s.indices.end(), idx, idx + op.index_count);
    s.ops.push_back(op);
}

void renderer_draw_lit(const RViewport *vp, uint32_t flags, const RVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni, const float *normals) {
    if (!s.charlit.ok || !s.charlit.marked || !normals || s.cur_target[0] || s.cur_target[1] || !nv || ni < 3) {
        renderer_draw(vp, flags, v, nv, idx, ni);
        return;
    }
    s.normals.resize(s.verts.size() * 3, 0.0f);   // parallel to the vertices: padding for the plain draws before
    s.normals.insert(s.normals.end(), normals, normals + (size_t)nv * 3);
    const size_t at = s.ops.size();
    renderer_draw(vp, flags, v, nv, idx, ni);
    if (s.ops.size() > at && !(s.ops.back().pipe_key & SOFT_KEY)) {
        s.ops.back().pipe_key |= CHARLIT_KEY;
        relief_mark(s.ops.back());
    }
}

void renderer_draw_world_lit(const RViewport *vp, uint32_t flags, const RVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni,
                             const float *normals, uint32_t lights) {
    const size_t at = s.ops.size();
    renderer_draw_lit(vp, flags, v, nv, idx, ni, lights ? normals : nullptr);
    if (lights && s.ops.size() > at && (s.ops.back().pipe_key & CHARLIT_KEY)) {
        s.ops.back().push[3] = lights;
        s.ops.back().shape &= ~(HAIR_SHAPE | SKIN_SHAPE);   // hair and skin are for characters
    }
}
uint32_t renderer_world_lights_count(void) { return s.charlit.world_executed; }

void renderer_set_emission(uint32_t mask, uint32_t stage, float strength, float bloom) {
    s.emit.tex = stage < RENDERER_STAGES ? mask : 0;
    s.emit.stage = stage;
    s.emit.strength = strength;
    s.emit.bloom = bloom;
}
uint32_t renderer_emission_count(void) { return s.emit.executed; }

void renderer_set_relief(uint32_t normal, uint32_t specular, uint32_t stage, float strength, float spec_strength, float baked) {
    const bool on = stage < RENDERER_STAGES && (normal || specular);
    s.relief.tex[0] = on ? normal : 0;
    s.relief.tex[1] = on ? specular : 0;
    s.relief.p[0] = strength;
    s.relief.p[1] = spec_strength;
    s.relief.p[2] = (float)(stage + 4u * ((normal ? 1u : 0u) | (specular ? 2u : 0u) | (specular && s.relief.dry ? 4u : 0u) |
                                          (normal && s.relief.height && s.relief.parallax > 0 ? 8u : 0u) |
                                          (specular && s.relief.dry && s.relief.glossy ? 16u : 0u)));
    s.relief.p[3] = baked;
}
void renderer_set_relief_dry(int on) { s.relief.dry = on != 0; }
void renderer_set_relief_glossy(int on) { s.relief.glossy = on != 0; }
void renderer_set_parallax(float depth) { s.relief.parallax = depth > 0 ? depth : 0; }
void renderer_set_relief_height(int on) { s.relief.height = on != 0; }
uint32_t renderer_relief_count(void) { return s.relief.executed; }
void renderer_set_hair(uint32_t mask) { s.hair.tex = cfg.hair > 0 ? mask : 0; }
uint32_t renderer_hair_count(void) { return s.hair.executed; }
void renderer_set_skin(uint32_t mask) { s.skinlit.tex = cfg.skin > 0 || cfg.eyes > 0 || cfg.cloth > 0 ? mask : 0; }
void renderer_set_eye_open(float open) { s.skinlit.eye_open = open > 0 ? (open < 1 ? open : 1) : 0; }
uint32_t renderer_skin_count(void) { return s.skinlit.executed; }
uint32_t renderer_mat_count(void) { return s.skinlit.mat_executed; }

void renderer_char_lights(const RLampLights *p) {
    if (!s.charlit.ok || !p || !p->n || s.charlit.marked) return;
    s.charlit.marked = true;
    s.charlit.p = *p;
}
int renderer_char_lights_on(void) { return s.dev && s.charlit.ok; }

void renderer_lamp_shadows(const RLampShadows *p) {
    if (!s.lshadow.ok || !p || !p->n || s.lshadow.marked) return;
    s.lshadow.marked = true;
    s.lshadow.p = *p;
}
int renderer_lamp_shadows_on(void) { return s.dev && s.lshadow.ok; }
static bool wet_material_image(Image *im, int w, int h, const uint16_t *data);   // an R16G16_UINT image
static void lamp_bind_walls();
static void wet_bind_walls();
static void volfog_bind_walls();
void renderer_lamp_wall_map(const RLampWallMap *m) {
    WallMap &c = s.walls;
    if (!s.dev || !c.ok) return;
    const bool real = m && m->texels && m->w > 0 && m->h > 0 && m->cell > 0 && m->zstep > 0 && m->w <= 8192 && m->h <= 8192;
    if (!real && !c.real) return;
    VKC(vkDeviceWaitIdle, s.dev);   // no frame in flight reads the old map (the frames' sets are rebuilt each frame)
    Image im;
    if (!wet_material_image(&im, real ? m->w : 1, real ? m->h : 1, real ? m->texels : nullptr)) {
        free_image(&im);
        if (!wet_material_image(&im, 1, 1, nullptr)) {   // keep a valid binding
            free_image(&im);
            return;
        }
        printf("[renderer] wall map %dx%d: upload failed, no walls\n", real ? m->w : 1, real ? m->h : 1);
        m = nullptr;
    }
    free_image(&c.img);
    c.img = im;
    c.real = real && m;
    c.w = c.real ? m->w : 0;
    c.h = c.real ? m->h : 0;
    memset(c.box, 0, sizeof c.box);
    if (c.real) {
        // The receiver's own wall (a door or wall it lies on) covers its cell and may reach into the
        // next one: no wall test within 1.5 cells of the receiver.
        const float box[8] = {m->x0, m->y0, 1.0f / m->cell, 1, m->z0, m->zstep, 1.5f * m->cell, 0};
        memcpy(c.box, box, sizeof box);
    }
    lamp_bind_walls();   // the lamp pass's set holds the image (written once)
    wet_bind_walls();    // and the wet ground's (occluded streaks)
    volfog_bind_walls(); // and the volumetric fog's (polar wall maps)
}
int renderer_lamp_wall_map_on(void) { return s.dev && s.walls.ok && s.walls.real; }
int renderer_lamp_wall_map_region(const RLampWallMap *m, int x0, int y0, int x1, int y1) {
    WallMap &c = s.walls;
    if (!s.dev || !c.ok || !c.real || !m || !m->texels || m->w != c.w || m->h != c.h) return -1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c.w) x1 = c.w;
    if (y1 > c.h) y1 = c.h;
    if (x1 <= x0 || y1 <= y0) return 0;
    const uint32_t rw = (uint32_t)(x1 - x0), rh = (uint32_t)(y1 - y0);
    Buffer staging;
    if (make_buffer(&staging, (VkDeviceSize)rw * rh * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
        free_buffer(&staging);
        return -1;
    }
    for (uint32_t y = 0; y < rh; y++)
        memcpy((uint8_t *)staging.map + (size_t)y * rw * 4, m->texels + 2 * ((size_t)(y0 + y) * m->w + x0), (size_t)rw * 4);
    // The barrier's first scope holds every earlier submission on the queue: frames still reading
    // the map finish before the copy (no device-wide wait).
    VkCommandBuffer saved;
    begin_once(&saved);
    barrier(c.img.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {x0, y0, 0};
    region.imageExtent = {rw, rh, 1};
    vkCmdCopyBufferToImage(s.cmd, staging.buf, c.img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(c.img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    const int r = submit_once(saved);
    free_buffer(&staging);
    return r ? -1 : 0;
}
void renderer_world_shadow(const RWorldShadow *p) {
    if (!s.wshadow.ok || !p || s.wshadow.marked || s.cur_target[0] || s.cur_target[1]) return;
    s.wshadow.marked = true;
    s.wshadow.p = *p;
}
int renderer_world_shadow_on(void) { return s.dev && s.wshadow.ok; }
uint32_t renderer_world_shadow_count(void) { return s.wshadow.executed; }
float renderer_world_shadow_gate(float *share) {
    float g[4] = {0, 0, 0, 0};
    if (s.wshadow.ok && s.wshadow.buf.map) memcpy(g, (const char *)s.wshadow.buf.map + offsetof(WorldShadowGpu, gate), sizeof g);
    if (share) *share = g[1];
    return g[0];
}
uint32_t renderer_lamp_shadows_count(void) { return s.lshadow.executed; }
uint32_t renderer_char_lights_count(void) { return s.charlit.executed; }

void renderer_glass(const RGlass *g) {
    if (s.cur_target[0] || s.cur_target[1] || !(g->x1 > g->x0) || !(g->y1 > g->y0)) return;
    Op op{};
    op.kind = OP_GLASS;
    op.glass = *g;
    s.ops.push_back(op);
}
uint32_t renderer_glass_count(void) { return s.glass.executed; }

void renderer_bloom(void) {
    if (!s.bloom.ok || s.bloom.marked || s.cur_target[0] || s.cur_target[1]) return;
    s.bloom.marked = true;
    Op op{};
    op.kind = OP_BLOOM;
    s.ops.push_back(op);
}
void renderer_rain(const RRain *r) {
    if (!s.rain.ok || !r || !r->slices || s.cur_target[0] || s.cur_target[1]) return;
    Op op{};
    op.kind = OP_RAIN;
    op.first_index = (uint32_t)s.rain.list.size();
    s.rain.list.push_back(*r);
    if (s.rain.list.back().slices > RENDERER_RAIN_SLICES) s.rain.list.back().slices = RENDERER_RAIN_SLICES;
    s.ops.push_back(op);
}
void renderer_sky(const RSky *p) {
    if (!s.sky.ok || !p || s.cur_target[0] || s.cur_target[1]) return;
    Op op{};
    op.kind = OP_SKY;
    op.first_index = (uint32_t)s.sky.list.size();
    s.sky.list.push_back(*p);
    s.ops.push_back(op);
}
int renderer_sky_on(void) { return s.dev && s.sky.ok; }
uint32_t renderer_sky_count(void) { return s.sky.executed; }
void renderer_skyline(const RViewport *vp, const RVertex *v, uint32_t nv, const uint32_t *idx, uint32_t ni, const RSkyline *p) {
    if (!s.sky.ok || !p || !vp || !nv || ni < 3 || s.cur_target[0] || s.cur_target[1]) return;
    Op op{};
    op.kind = OP_SKYLINE;
    op.vp = *vp;
    op.first_index = (uint32_t)s.indices.size();
    op.index_count = ni - ni % 3;
    op.vertex_offset = (int32_t)s.verts.size();
    op.push[0] = (uint32_t)s.sky.lines.size();
    s.sky.lines.push_back(*p);
    s.verts.add(v, nv);
    s.indices.insert(s.indices.end(), idx, idx + op.index_count);
    s.ops.push_back(op);
}
uint32_t renderer_skyline_count(void) { return s.sky.lines_executed; }
void renderer_ssao(const RSsao *p) {
    if (!s.ssao.ok || !p || s.ssao.marked || s.cur_target[0] || s.cur_target[1]) return;
    s.ssao.marked = true;
    Op op{};
    op.kind = OP_SSAO;
    op.ssao = *p;
    s.ops.push_back(op);
}
int renderer_ssao_on(void) { return s.dev && s.ssao.ok; }
int renderer_ssao_gtao_on(void) { return s.dev && s.ssao.ok && s.ssao.gtao; }
void renderer_lamp_lights(const RLampLights *p) {
    if (!s.lamp.ok || !p || !p->n || s.lamp.marked || s.cur_target[0] || s.cur_target[1]) return;
    s.lamp.marked = true;
    s.lamp.p = *p;
    if (s.lamp.p.n > RENDERER_LAMP_MAX) s.lamp.p.n = RENDERER_LAMP_MAX;
    Op op{};
    op.kind = OP_LAMP;
    s.ops.push_back(op);
}
int renderer_lamp_lights_on(void) { return s.dev && s.lamp.ok; }
void renderer_light_shafts(const RLightShafts *p) {
    if (!s.shaft.ok || !p || !p->n || s.shaft.marked || s.cur_target[0] || s.cur_target[1]) return;
    s.shaft.marked = true;
    s.shaft.p = *p;
    if (s.shaft.p.n > RENDERER_SHAFT_MAX) s.shaft.p.n = RENDERER_SHAFT_MAX;
    Op op{};
    op.kind = OP_SHAFT;
    s.ops.push_back(op);
}
int renderer_light_shafts_on(void) { return s.dev && s.shaft.ok; }
uint32_t renderer_light_shafts_count(void) { return s.shaft.executed; }
void renderer_volfog(const RVolfog *p) {
    if (!s.volfog.ok || !p || !p->n || s.volfog.marked || s.cur_target[0] || s.cur_target[1]) return;
    s.volfog.marked = true;
    s.volfog.p = *p;
    if (s.volfog.p.n > RENDERER_VOLFOG_MAX) s.volfog.p.n = RENDERER_VOLFOG_MAX;
    Op op{};
    op.kind = OP_VOLFOG;
    s.ops.push_back(op);
}
int renderer_volfog_on(void) { return s.dev && s.volfog.ok; }
uint32_t renderer_volfog_count(void) { return s.volfog.executed; }
void renderer_contact(const RContact *p) {
    if (!s.contact.ok || !p || s.contact.marked || s.cur_target[0] || s.cur_target[1]) return;
    s.contact.marked = true;
    s.contact.p = *p;
    Op op{};
    op.kind = OP_CONTACT;
    s.ops.push_back(op);
}
int renderer_contact_on(void) { return s.dev && s.contact.ok; }
uint32_t renderer_contact_count(void) { return s.contact.executed; }
uint32_t renderer_lamp_lights_count(void) { return s.lamp.executed; }
void renderer_soft(const RSoft *p) {
    if (!s.soft.ok || !p || !(p->distance > 0) || s.soft.marked || s.cur_target[0] || s.cur_target[1]) return;
    s.soft.marked = true;
    Op op{};
    op.kind = OP_SOFT;
    op.soft = *p;
    s.ops.push_back(op);
}
int renderer_soft_on(void) { return s.dev && s.soft.ok; }
uint32_t renderer_soft_count(void) { return s.soft.executed; }
void renderer_rain_splash(const RRainSplash *p) {
    if (!s.splash.ok || !p || s.splash.marked || s.cur_target[0] || s.cur_target[1]) return;
    s.splash.marked = true;
    s.splash.p = *p;
    Op op{};
    op.kind = OP_SPLASH;
    s.ops.push_back(op);
}
int renderer_rain_splash_on(void) { return s.dev && s.splash.ok; }
uint32_t renderer_rain_splash_count(void) { return s.splash.executed; }
void renderer_rain_wet(const RRainWet *p) {
    if (!s.wet.ok || !p || s.wet.marked || s.cur_target[0] || s.cur_target[1]) return;
    if (p->damp > 0 && !p->dry && !(p->materials > 0 && s.wet.mat_real)) return;   // damp streets: nothing without the material map
    if (p->dry && !(s.wet.surf_pipe && p->glass > 0)) return;   // --reflections sssr's glass alone: nothing without its surface pass
    s.wet.marked = true;
    s.wet.p = *p;
    Op op{};
    op.kind = OP_WET;
    s.ops.push_back(op);
}
int renderer_rain_wet_on(void) { return s.dev && s.wet.ok; }
static void wet_material_bind();
void renderer_material_map(const RMaterialMap *m) {
    Wet &a = s.wet;
    if (!s.dev || !a.ok) return;
    const bool real = m && m->texels && m->w > 0 && m->h > 0 && m->cell > 0 && m->w <= 8192 && m->h <= 8192;
    if (!real && !a.mat_real) return;
    VKC(vkDeviceWaitIdle, s.dev);   // no frame in flight reads the old map
    Image im;
    if (!wet_material_image(&im, real ? m->w : 1, real ? m->h : 1, real ? m->texels : nullptr)) {
        free_image(&im);
        if (!wet_material_image(&im, 1, 1, nullptr)) {   // keep a valid binding
            free_image(&im);
            return;
        }
        printf("[renderer] material map %dx%d: upload failed, materials off\n", real ? m->w : 1, real ? m->h : 1);
        m = nullptr;
    }
    free_image(&a.mat);
    a.mat = im;
    a.mat_real = real && m;
    memset(a.mat_box, 0, sizeof a.mat_box);
    if (a.mat_real) {
        const float box[8] = {m->x0, m->y0, 1.0f / m->cell, 0, m->z0, m->zstep, (float)m->w, (float)m->h};
        memcpy(a.mat_box, box, sizeof box);
    }
    wet_material_bind();
    wet_bind_walls();   // binding 5 may stand in with the material map
}
int renderer_wet_sssr_on(void) { return s.dev && s.wet.ok && s.wet.sssr > 0; }
int renderer_wet_glass_on(void) { return renderer_wet_sssr_on() && s.wet.surf_pipe; }
uint32_t renderer_wet_glass_count(void) { return s.wet.surf_executed; }
int renderer_wet_hiz_read(int level, float *out, int max_texels, int *w, int *h) {
    Wet &a = s.wet;
    if (!renderer_wet_sssr_on() || level < 0 || level >= a.hiz_levels || !out) return -1;
    const int lw = a.hiz_w[level], lh = a.hiz_h[level];
    if (lw * lh > max_texels) return -1;
    VKC(vkDeviceWaitIdle, s.dev);
    Buffer buf;
    if (make_buffer(&buf, (VkDeviceSize)lw * lh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        free_buffer(&buf);
        return -1;
    }
    VkCommandBuffer saved;
    begin_once(&saved);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = a.hiz.img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, 1, 0, 1};
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, 0, 1};
    region.imageExtent = {(uint32_t)lw, (uint32_t)lh, 1};
    vkCmdCopyImageToBuffer(s.cmd, a.hiz.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf.buf, 1, &region);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    const int r = submit_once(saved);
    if (!r) memcpy(out, buf.map, (size_t)lw * lh * 4);
    free_buffer(&buf);
    *w = lw;
    *h = lh;
    return r ? -1 : 0;
}
int renderer_material_map_on(void) { return s.dev && s.wet.ok && s.wet.mat_real; }
uint32_t renderer_rain_wet_count(void) { return s.wet.executed; }
uint32_t renderer_ssao_count(void) { return s.ssao.executed; }
int renderer_rain_on(void) { return s.dev && s.rain.ok; }
void renderer_rain_cover(const RRainCover *c) {
    if (!s.cover.ok || !c || s.cover.marked || s.cur_target[0] || s.cur_target[1]) return;
    s.cover.marked = true;
    s.cover.p = *c;
}
int renderer_rain_cover_on(void) { return s.dev && s.cover.ok; }
uint32_t renderer_rain_cover_count(void) { return s.cover.executed; }
int renderer_rain_chars_on(void) { return s.dev && s.chars.ok; }
uint32_t renderer_rain_chars_count(void) { return s.chars.executed; }
uint32_t renderer_rain_count(void) { return s.rain.executed; }
int renderer_bloom_on(void) { return s.dev && s.bloom.ok; }
uint32_t renderer_bloom_count(void) { return s.bloom.executed; }
void renderer_filmic(void) {
    if (!s.filmic.ok || s.filmic.marked || s.cur_target[0] || s.cur_target[1] || (s.filmic.dof_only && !s.dof.want)) return;
    s.filmic.marked = true;
    Op op{};
    op.kind = OP_FILMIC;
    s.ops.push_back(op);
}
int renderer_filmic_on(void) { return s.dev && s.filmic.ok; }
int renderer_auto_exposure_on(void) { return s.dev && s.filmic.ok && s.filmic.ae; }
int renderer_auto_exposure_state(float out[4]) {
    if (!renderer_auto_exposure_on()) return 0;
    ae_collect();
    if (!s.filmic.ae_have) return 0;
    memcpy(out, s.filmic.ae_last, sizeof s.filmic.ae_last);
    return 1;
}
void renderer_auto_exposure_snap(void) {
    if (renderer_auto_exposure_on()) s.filmic.ae_snap = true;
}
uint32_t renderer_filmic_count(void) { return s.filmic.executed; }
void renderer_taa(const RTaa *p) {
    if (!s.taa.ok || s.taa.marked || s.cur_target[0] || s.cur_target[1] || !p) return;
    s.taa.marked = true;
    s.taa.p = *p;
    Op op{};
    op.kind = OP_TAA;
    s.ops.push_back(op);
}
int renderer_taa_on(void) { return s.dev && s.taa.ok; }
int renderer_taa_upscale(int *render_w, int *render_h) {
    if (!s.dev || !s.taa.ok || !s.taa.up) return 0;
    if (render_w) *render_w = s.taa.rw;
    if (render_h) *render_h = s.taa.rh;
    return 1;
}
void renderer_taa_render_size(int w, int h) {
    Taa &t = s.taa;
    if (!s.dev || !t.ok || !t.up) return;
    w = w < 1 ? 1 : w > t.mw ? t.mw : w;
    h = h < 1 ? 1 : h > t.mh ? t.mh : h;
    if (w == t.rw && h == t.rh) return;
    t.rw = w;
    t.rh = h;
    t.phases = taa_jitter_phases(w, t.ow);
    t.size_serial = s.frame_serial + 1;   // renderer_begin counts the next frame
}
void renderer_taa_sharpness(float v) {
    if (!s.dev || !s.taa.ok) return;
    s.taa.sharpness = !(v > 0) ? 0 : v > TAA_SHARPNESS_MAX ? TAA_SHARPNESS_MAX : v;
}
uint32_t renderer_taa_count(void) { return s.taa.executed; }
uint32_t renderer_taa_object_draws(void) { return s.taa.vel_live ? s.taa.vel_draws : 0; }
void renderer_frame_gen(const RFrameGen *p) {
    if (!s.fg.ok || s.fg.marked || s.cur_target[0] || s.cur_target[1] || !p) return;
    s.fg.marked = true;
    s.fg.p = *p;
    Op op{};
    op.kind = OP_FG;
    s.ops.push_back(op);
}
void renderer_frame_gen_hud_end(void) {
    if (!s.fg.ok || !s.fg.marked || s.fg.hud_marked || s.cur_target[0] || s.cur_target[1]) return;
    s.fg.hud_marked = true;
    Op op{};
    op.kind = OP_FG_END;
    s.ops.push_back(op);
}
int renderer_frame_gen_on(void) { return s.dev && s.fg.ok; }
uint32_t renderer_frame_gen_count(void) { return s.fg.executed; }
int renderer_frame_gen_readback(uint32_t *out, int pitch_bytes) {
    if (!s.dev || !s.fg.ok || !s.fg.rb_valid || !out) return 0;
    const uint32_t *px = (const uint32_t *)s.fg.rb.map;
    for (int y = 0; y < s.disph; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)out + (size_t)y * pitch_bytes);
        for (int x = 0; x < s.dispw; x++) row[x] = px[(size_t)y * s.dispw + x] & 0x00FFFFFFu;
    }
    return 1;
}
void renderer_frame_gen_present(int on) { s.fg.show = on == 2 ? 2 : on != 0; }
int renderer_frame_gen_present_wait(void) { return s.fg.ok && s.fg.pw; }
int renderer_frame_gen_pacing(double *t_ms, int *waited) {
    if (!s.fg.pw_new) return 0;
    s.fg.pw_new = false;
    *t_ms = s.fg.pw_t;
    *waited = s.fg.pw_waited;
    return 1;
}
uint32_t renderer_frame_gen_presented(void) { return s.fg.presented; }
void renderer_taa_jitter(float out[2]) {
    out[0] = out[1] = 0;
    if (renderer_taa_on()) taa_jitter_n(s.taa.frame, s.taa.phases, out);
}
void renderer_dof(const RDofFrame *p) {
    if (!s.dof.ok || s.filmic.marked || s.cur_target[0] || s.cur_target[1] || !p || !(p->weight > 0)) return;
    s.dof.want = true;
    s.dof.frame = *p;
}
int renderer_dof_on(void) { return s.dev && s.dof.ok; }
uint32_t renderer_dof_count(void) { return s.dof.executed; }
int renderer_dof_state(float out[4]) {
    if (!renderer_dof_on()) return 0;
    dof_collect();
    if (!s.dof.have) return 0;
    memcpy(out, s.dof.last, sizeof s.dof.last);
    return 1;
}

int renderer_has_bc(void) { return s.bc; }
uint32_t renderer_texture_count(void) { return (uint32_t)s.textures.size(); }

void renderer_set_texture_budget(uint64_t bytes) { texture_budget = bytes ? bytes : DEFAULT_TEXTURE_BYTES; }

uint32_t renderer_texture_find(uint64_t key) {
    auto it = s.by_key.find(key);
    return it == s.by_key.end() ? 0 : it->second;
}

// Whether a texture may sample under half opaque (alpha < SEE_THROUGH_REF), from its level 0:
// BGRA8 any texel, BC2 any explicit alpha, BC3 a block whose alpha can reach below it (the lower
// endpoint, or the 6-value mode with its explicit 0), BC7 always (texture packs store only textures
// with transparent texels as BC7). BC1 has 1-bit alpha, whose mip levels stay 0 or 1 as the title's
// do; L8 has none. Mip levels average a level's alpha, so they never go below level 0's minimum.
static bool texture_see_through(uint32_t format, uint32_t w, uint32_t h, const void *data) {
    if (!data) return false;
    const size_t blocks = (size_t)((w + 3) / 4) * ((h + 3) / 4);
    const uint8_t *b = (const uint8_t *)data;
    switch (format) {
    case RENDERER_TEX_BGRA8:
        for (size_t i = 0, n = (size_t)w * h; i < n; i++)
            if ((((const uint32_t *)data)[i] >> 24) < SEE_THROUGH_REF) return true;
        return false;
    case RENDERER_TEX_BC2:
        for (size_t i = 0; i < blocks; i++, b += 16)
            for (int k = 0; k < 8; k++)
                if ((b[k] & 15) * 17 < SEE_THROUGH_REF || (b[k] >> 4) * 17 < SEE_THROUGH_REF) return true;
        return false;
    case RENDERER_TEX_BC3:
        for (size_t i = 0; i < blocks; i++, b += 16)
            if (b[0] <= b[1] || b[1] < SEE_THROUGH_REF) return true;
        return false;
    case RENDERER_TEX_BC7: return true;
    default: return false;
    }
}

uint32_t renderer_texture_create(uint64_t key, uint32_t format, uint32_t w, uint32_t h, uint32_t levels, const void *data) {
    if (!s.dev || s.textures.size() >= MAX_TEXTURES) return 0;
    if (uint32_t found = renderer_texture_find(key)) return found;
    Texture t;
    const double t0 = now_ms();
    const int failed = make_texture(&t, format, w, h, levels, data);
    pw.upload_ms += now_ms() - t0;
    if (failed) {
        free_image(&t.im);
        if (!texture_failures++ || texture_failures % 100 == 0)
            printf("[renderer] cannot create a %ux%u texture (%u failures, %u textures, %llu MB): dropping the texture cache\n", w,
                   h, texture_failures, (uint32_t)s.textures.size(), (unsigned long long)(s.texture_bytes >> 20));
        texture_drop_requested = true;
        return 0;
    }
    t.see_through = texture_see_through(format, w, h, data);
    s.textures.push_back(t);
    s.texture_bytes += t.bytes;
    uint32_t handle = (uint32_t)s.textures.size();
    s.by_key[key] = handle;
    return handle;
}

uint32_t renderer_movie_plane(int plane, uint32_t w, uint32_t h, const void *data) {
    if (!s.dev || plane < 0 || plane >= MOVIE_PLANES || !w || !h || w > s.max_dim || h > s.max_dim || !data) return 0;
    if (s.movie_w[plane] != w || s.movie_h[plane] != h) {   // a new movie size: the image is (re)made once
        if (s.movie[plane].img) {
            VKC(vkDeviceWaitIdle, s.dev);
            free_image(&s.movie[plane]);
        }
        s.movie_w[plane] = s.movie_h[plane] = 0;
        if (make_image(&s.movie[plane], VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, w, h) ||
            l8_view(&s.movie[plane], 1)) {
            free_image(&s.movie[plane]);
            return 0;
        }
        s.movie_w[plane] = w;
        s.movie_h[plane] = h;
    }
    // Plane offsets in the slot buffer: 4-byte aligned (vkCmdCopyBufferToImage), in plane order.
    VkDeviceSize off = 0, need = 0;
    for (int i = 0; i < MOVIE_PLANES; i++) {
        if (i == plane) off = need;
        need += ((VkDeviceSize)s.movie_w[i] * s.movie_h[i] + 3) & ~(VkDeviceSize)3;
    }
    FrameSlot &f = cur();   // renderer_begin waited for this slot's previous frame
    if (f.movie.size < need) {
        free_buffer(&f.movie);
        if (make_buffer(&f.movie, need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return 0;
    }
    if (s.movie_pending && s.movie_off[plane] != off) s.movie_pending = 0;   // sizes changed within the frame
    memcpy((uint8_t *)f.movie.map + off, data, (size_t)w * h);
    s.movie_off[plane] = off;
    s.movie_pending |= 1u << plane;
    s.movie_uploads++;
    return MOVIE_TEXTURE + (uint32_t)plane;
}
uint32_t renderer_movie_upload_count(void) { return s.movie_uploads; }
uint32_t renderer_texture_drop_count(void) { return texture_drops; }
int renderer_memory_budget(uint64_t *budget, uint64_t *usage, int *heap) {
    if (!s.dev || !memory_budget_ext || !get_memprops2 || budget_heap < 0) return 0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT mb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    mp.pNext = &mb;
    get_memprops2(s.phys, &mp);
    if (!mb.heapBudget[budget_heap]) return 0;
    if (budget) *budget = mb.heapBudget[budget_heap];
    if (usage) *usage = mb.heapUsage[budget_heap];
    if (heap) *heap = budget_heap;
    return 1;
}
uint64_t renderer_texture_bytes(void) { return s.texture_bytes; }
uint64_t renderer_vram(uint64_t now[RENDERER_VRAM_CATS], uint64_t peak[RENDERER_VRAM_CATS]) {
    for (int c = 0; c < RENDERER_VRAM_CATS; c++) {
        if (now) now[c] = vram_now[c];
        if (peak) peak[c] = vram_peak[c];
    }
    return vram_peak_total;
}
uint64_t renderer_swapchain_bytes(void) {
    return s.swapchain ? (uint64_t)s.sc_extent.width * s.sc_extent.height * 4 * s.sc_images.size() : 0;
}

void renderer_state_default(RState *st) {
    *st = RState{};
    st->blend_src = 1;   // ONE
    st->blend_dst = 0;   // ZERO
    st->blend_eq = 0x8006;
    st->color_mask = 0x01010101u;
    st->depth_write = 1;
    st->depth_func = 0x203;   // LEQUAL
}
void renderer_set_state(const RState *st) { s.cur_state = *st; }
uint32_t renderer_pipeline_count(void) {
    uint32_t n = 0;
    for (auto &p : s.pipelines) n += p.second != VK_NULL_HANDLE;
    return n;
}
void renderer_pipeline_cache_save(void) {
    if (!s.dev) return;
    pipe_totals();
    pipe_save(true);
}
int renderer_pipeline_report(char *line, int size) {
    pthread_mutex_lock(&pw.lock);
    const size_t nl = pw.report.find('\n');
    if (nl != std::string::npos && size > 0) {
        snprintf(line, (size_t)size, "%.*s", (int)nl, pw.report.c_str());
        pw.report.erase(0, nl + 1);
    }
    pthread_mutex_unlock(&pw.lock);
    return nl != std::string::npos && size > 0;
}
void renderer_pipeline_counts(uint32_t *on_demand, uint32_t *prewarmed) {
    if (on_demand) *on_demand = pw.on_demand;
    if (prewarmed) *prewarmed = pw.used;
}

void renderer_set_texture(uint32_t handle) { renderer_set_stage_texture(0, handle); }
void renderer_set_stage_texture(uint32_t stage, uint32_t handle) {
    if (stage < RENDERER_STAGES)
        s.cur_tex[stage] = handle <= s.textures.size() || surface_of(handle) || (is_movie(handle) && s.movie[handle - MOVIE_TEXTURE].view) || (handle == PREV_TEXTURE && s.prev.im.view) ? handle : 0;
}

uint32_t renderer_surface(uint64_t key, uint32_t kind, int w, int h) {
    const bool guest_size = (kind & RENDERER_SURFACE_GUEST_SIZE) != 0;
    kind &= ~RENDERER_SURFACE_GUEST_SIZE;
    if (!s.dev || w <= 0 || h <= 0 || kind > RENDERER_SURFACE_DEPTH24) return 0;
    const uint64_t k = key ^ ((uint64_t)kind << 62) ^ ((uint64_t)guest_size << 61) ^ ((uint64_t)(uint32_t)w << 40) ^ ((uint64_t)(uint32_t)h << 24);
    auto it = s.surface_by_key.find(k);
    if (it != s.surface_by_key.end()) return it->second;
    if (s.surfaces.size() >= MAX_SURFACES) {
        fail("too many offscreen surfaces");
        return 0;
    }
    // Render scale (the internal height over the back-buffer height, on both axes) times
    // RendererConfig.shadow_scale; 1 for a guest-size surface.
    const int scale = cfg.shadow_scale > 1 ? (cfg.shadow_scale < 4 ? cfg.shadow_scale : 4) : 1;
    const double f = guest_size ? 1.0 : (double)s.oh / s.h * scale;
    Surface sf;
    sf.kind = kind;
    sf.gw = w;
    sf.gh = h;
    sf.w = (int)(w * f + 0.5);
    sf.h = (int)(h * f + 0.5);
    if (sf.w > (int)s.max_dim) sf.w = (int)s.max_dim;
    if (sf.h > (int)s.max_dim) sf.h = (int)s.max_dim;
    const bool depth = kind != RENDERER_SURFACE_COLOR;
    MemTag tag(RENDERER_VRAM_SURFACES);
    if (make_surface_image(&sf.im, surface_format(kind), depth, sf.w, sf.h,
                           depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true)) {
        free_image(&sf.im);
        return 0;
    }
    s.surfaces.push_back(sf);
    return s.surface_by_key[k] = SURFACE_BIT | (uint32_t)s.surfaces.size();
}
void renderer_surface_size(uint32_t surface, int *w, int *h) {
    const Surface *sf = surface_of(surface);
    *w = sf ? sf->w : 0;
    *h = sf ? sf->h : 0;
}
static VkRenderPass resume_pass();
static bool split_passes_ready();
// The history image (s.color's format and size), cleared to black, in SHADER_READ_ONLY_OPTIMAL; its
// view reads alpha as 1 (the title's front buffer is shown opaque). Never a render target.
static bool prev_create() {
    MemTag tag(RENDERER_VRAM_EFFECTS);
    if (make_surface_image(&s.prev.im, VK_FORMAT_B8G8R8A8_UNORM, false, s.ow, s.oh, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true)) {
        free_image(&s.prev.im);
        return false;
    }
    vkDestroyImageView(s.dev, s.prev.im.view, nullptr);
    s.prev.im.view = VK_NULL_HANDLE;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = s.prev.im.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_B8G8R8A8_UNORM;
    vi.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_ONE};
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (VKC(vkCreateImageView, s.dev, &vi, nullptr, &s.prev.im.view) != VK_SUCCESS) {
        free_image(&s.prev.im);
        return false;
    }
    return true;
}
void renderer_prev_frame_mark(void) {
    if (!s.dev || s.prev.marked || s.cur_target[0] || s.cur_target[1]) return;
    if ((!s.prev.im.img && !prev_create()) || !split_passes_ready()) return;
    s.prev.marked = true;
    s.prev.asked = s.frame_serial;
    Op op{};
    op.kind = OP_GRAB;
    s.ops.push_back(op);
}
uint32_t renderer_prev_frame(void) {
    if (!s.dev || !s.frame_serial) return 0;
    if (!s.prev.im.img && !prev_create()) return 0;
    s.prev.asked = s.frame_serial;
    return s.prev.serial && s.prev.serial + 1 == s.frame_serial ? PREV_TEXTURE : 0;
}
// At the end of the main pass (s.color in TRANSFER_SRC_OPTIMAL, resolved): the frame into the
// history while this frame or the one before it asked for it.
static void prev_copy() {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};   // the pass's colour writes (and resolve) -> the copy
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    barrier(s.prev.im.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy cp{};
    cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.extent = {(uint32_t)s.ow, (uint32_t)s.oh, 1};
    vkCmdCopyImage(s.cmd, s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.prev.im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    barrier(s.prev.im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    s.prev.serial = s.frame_serial;
}
static void record_prev_frame() {
    if (!s.prev.im.img || s.prev.asked + 1 < s.frame_serial || s.prev.serial == s.frame_serial) return;   // kept at a mark already
    prev_copy();
}
// OP_GRAB (renderer_prev_frame_mark) inside the frame's render pass (begun with s.split_pass or
// s.resume_pass): ends it (stored, MSAA resolved into s.color), copies s.color into the history
// and resumes the pass.
static void prev_grab_exec() {
    vkCmdEndRenderPass(s.cmd);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};   // depth writes -> the resumed pass (prev_copy orders the colour)
    mb.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
    prev_copy();
    barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = resume_pass();
    rb.framebuffer = s.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
}
uint32_t renderer_surface_count(void) { return (uint32_t)s.surfaces.size(); }
void renderer_set_target(uint32_t color, uint32_t depth) {
    const Surface *c = surface_of(color), *d = surface_of(depth);
    s.cur_target[0] = c && c->kind == RENDERER_SURFACE_COLOR ? color : 0;
    s.cur_target[1] = d && d->kind != RENDERER_SURFACE_COLOR ? depth : 0;
}
void renderer_default_pixel_shader(RPixelShader *ps) {
    *ps = RPixelShader{};
    ps->combiners = 1;
    ps->rgb_in[0] = 0x08040000u;    // A = t0, B = v0 (rgb, unsigned identity), C = D = 0
    ps->alpha_in[0] = 0x18141010u;  // A = t0.a, B = v0.a
    ps->rgb_out[0] = 0x000000C0u;   // AB -> r0
    ps->alpha_out[0] = 0x000000C0u;
    ps->tex_modes = 1;              // stage 0 project2d
    ps->fog = 0xFF000000u;
}
void renderer_set_pixel_shader(const RPixelShader *ps) {
    RPixelShader def;
    if (!ps) {
        renderer_default_pixel_shader(&def);
        ps = &def;
    }
    s.cur_ps = *ps;
    s.cur_program = program_index(*ps);
    s.cur_shape = shape_index(*ps, 0);
}
uint32_t renderer_pixel_shader_count(void) { return (uint32_t)s.programs.size(); }
void renderer_set_alpha_test(uint32_t func, uint32_t ref) {
    s.cur_alpha[0] = func <= 8 ? func : 0;
    s.cur_alpha[1] = ref & 0xFF;
}

// Draw and clear execution shared by the main and the offscreen passes: the bound pipeline,
// blend constants and descriptor set of the current render pass instance.
struct PassState {
    VkPipeline bound = VK_NULL_HANDLE;
    uint32_t bound_blend = 0;
    VkDescriptorSet bound_set = VK_NULL_HANDLE;
    VkPipelineLayout bound_layout = VK_NULL_HANDLE;   // of the bound descriptor sets
    VkDescriptorSet bound_emit = VK_NULL_HANDLE;      // set 1 of the emissive variant
    bool bound_buffers = false;
    uint32_t bound_dyn = 0;                           // s.eds: the key's EDS_KEY_BITS last set
};
// --taa: the frame's jitter (target pixels) as geom.vert reads it from a draw's firstInstance: the
// Vulkan NDC offset over a vw x vh viewport in units of 2^-20, x in the high and y in the low 16
// bits (two's complement; at least 1 so the shader sees a jittered draw).
static uint32_t taa_instance(float vw, float vh) {
    const float k = 1048576.0f * 2.0f;
    long jx = lroundf(s.taa.jit[0] * k / vw), jy = lroundf(s.taa.jit[1] * k / vh);
    jx = jx < -32768 ? -32768 : jx > 32767 ? 32767 : jx;
    jy = jy < -32768 ? -32768 : jy > 32767 ? 32767 : jy;
    const uint32_t u = (uint32_t)(uint16_t)(int16_t)jx << 16 | (uint16_t)(int16_t)jy;
    return u ? u : 1;
}
static void exec_op(const Op &op, PassState &ps, std::unordered_map<uint64_t, VkDescriptorSet> &sets, VkDeviceSize index_offset,
                    float sx, float sy, int tw, int th) {
    // see_through_split: the depth of the half-opaque texels first, then the color tested against
    // it without depth writes. The depth goes first so that the draw still hides its own far parts
    // behind its near solid ones: with the color first, a triangle drawn later and farther (the
    // back of a mesh, an arm behind a torso) blended over a nearer one drawn before it. The color pass's LESS becomes LESS_OR_EQUAL (GREATER:
    // GREATER_OR_EQUAL) so that it passes on the depth it just wrote; both passes use geom.vert,
    // whose gl_Position is invariant, so that depth is the same bit for bit.
    if (op.kind == OP_DRAW && op.split_depth) {
        Op d = op;
        d.split_depth = false;
        d.pipe_key &= ~(0x3FFFu | 15u << 14 | 7u << 27 | CHARLIT_KEY);   // no blending, color, light mask or lighting
        d.shape &= ~(EMIT_SHAPE | RELIEF_SHAPE | HAIR_SHAPE | SKIN_SHAPE);
        d.push[0] = 7;   // GREATEREQUAL
        d.push[1] = SEE_THROUGH_REF;
        d.push[3] = 0;
        exec_op(d, ps, sets, index_offset, sx, sy, tw, th);
        Op c = op;
        c.split_depth = false;
        c.pipe_key &= ~(1u << 19);
        const uint32_t cmp = c.pipe_key >> 20 & 7;
        if (cmp == VK_COMPARE_OP_LESS || cmp == VK_COMPARE_OP_GREATER) c.pipe_key = (c.pipe_key & ~(7u << 20)) | (cmp + 2) << 20;
        exec_op(c, ps, sets, index_offset, sx, sy, tw, th);
        return;
    }
    if (op.kind == OP_DRAW) {
        VkPipeline pipe = get_pipeline(op.pipe_key, op.shape, cfg.pipeline_async != 0);
        if (!pipe) {
            // pipeline_async, still building (or failed): the base variant (no emissive mask,
            // relief, hair or skin shading) when it is built, else the draw waits for a later frame.
            const uint32_t variants = EMIT_SHAPE | RELIEF_SHAPE | HAIR_SHAPE | SKIN_SHAPE;
            if (cfg.pipeline_async && (op.shape & variants) && pipe_built(op.pipe_key, op.shape & ~variants)) {
                Op b = op;
                b.shape &= ~variants;
                exec_op(b, ps, sets, index_offset, sx, sy, tw, th);
            }
            return;
        }
        if (!ps.bound_buffers) {
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(s.cmd, 0, 1, &cur().geometry.buf, &zero);
            if (!s.normals.empty()) vkCmdBindVertexBuffers(s.cmd, 1, 1, &cur().geometry.buf, &normal_offset);
            vkCmdBindIndexBuffer(s.cmd, cur().geometry.buf, index_offset, VK_INDEX_TYPE_UINT32);
            ps.bound_buffers = true;
        }
        if (ps.bound != pipe) vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        if (s.eds && (ps.bound != pipe || ps.bound_dyn != (op.pipe_key & EDS_KEY_BITS))) {   // the state left out of eds_key
            const uint32_t k = op.pipe_key;
            cmd_set_cull_mode(s.cmd, k >> 23 & 3);
            cmd_set_depth_test(s.cmd, k >> 18 & 1);
            cmd_set_depth_write(s.cmd, k >> 19 & 1);
            cmd_set_depth_compare(s.cmd, (VkCompareOp)(k >> 20 & 7));
            ps.bound_dyn = k & EDS_KEY_BITS;
        }
        if (ps.bound == VK_NULL_HANDLE || ps.bound_blend != op.blend_color) {
            const uint32_t bc = op.blend_color;
            const float k[4] = {(bc >> 16 & 0xFF) / 255.0f, (bc >> 8 & 0xFF) / 255.0f, (bc & 0xFF) / 255.0f, (bc >> 24) / 255.0f};
            vkCmdSetBlendConstants(s.cmd, k);
            ps.bound_blend = bc;
        }
        ps.bound = pipe;
        VkDescriptorSet set = frame_set(op.tex, sets);
        if (!set) {
            s.skipped_sets++;
            return;
        }
        // Soft particles use their own layout (set 1, longer push constants): switching layouts
        // rebinds the sets.
        const bool soft = op.pipe_key & SOFT_KEY;
        const bool lit = (op.pipe_key & CHARLIT_KEY) && !soft;
        const bool emit = (op.shape & EMIT_SHAPE) && !soft;
        const bool relief = (op.shape & RELIEF_SHAPE) && lit && !emit;
        const bool hair = (op.shape & HAIR_SHAPE) && lit && !emit;
        const bool skin = (op.shape & SKIN_SHAPE) && lit && !emit;
        const VkPipelineLayout layout = soft ? s.soft.layout : emit || relief || hair || skin ? (lit ? s.emit.lit_layout : s.emit.layout) : lit ? s.charlit.layout : s.layout;
        if (layout != ps.bound_layout) {
            ps.bound_set = ps.bound_emit = VK_NULL_HANDLE;
            ps.bound_layout = layout;
            if (soft) vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &s.soft.set, 0, nullptr);
            if (lit) vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &s.charlit.set, 0, nullptr);
        }
        if (set != ps.bound_set) vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        ps.bound_set = set;
        if (emit) {
            const uint32_t mt[RENDERER_STAGES] = {op.emit_tex};
            VkDescriptorSet es = frame_set(mt, sets);
            if (!es) {
                s.skipped_sets++;
                return;
            }
            if (es != ps.bound_emit) vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, lit ? 2 : 1, 1, &es, 0, nullptr);
            ps.bound_emit = es;
            uint32_t pc[8];
            memcpy(pc, op.push, 16);
            memcpy(pc + 4, op.emit, 16);
            vkCmdPushConstants(s.cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
        } else if (relief || hair || skin) {   // set 2: the normal and specular maps (0 = white, unread: the flags say which are bound), the hair and skin masks
            const uint32_t rt[RENDERER_STAGES] = {relief ? op.relief_tex[0] : 0u, relief ? op.relief_tex[1] : 0u, hair ? op.hair_tex : 0u, skin ? op.skin_tex : 0u};
            VkDescriptorSet rs = frame_set(rt, sets);
            if (!rs) {
                s.skipped_sets++;
                return;
            }
            if (rs != ps.bound_emit) vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, 1, &rs, 0, nullptr);
            ps.bound_emit = rs;
            uint32_t pc[12] = {};
            memcpy(pc, op.push, 16);
            if (relief) memcpy(pc + 4, op.relief, 16);
            const bool mat = skin && (cfg.eyes > 0 || cfg.cloth > 0);   // the material variants: the eyes' openness after the relief's words
            if (mat) memcpy(pc + 8, &op.eye_open, 4);
            vkCmdPushConstants(s.cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, mat ? 48 : 32, pc);
        } else if (soft) {
            uint32_t pc[8];
            memcpy(pc, op.push, 16);
            memcpy(pc + 4, s.soft.push, 16);
            vkCmdPushConstants(s.cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
        } else {
            vkCmdPushConstants(s.cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, op.push);
        }
        VkViewport v{op.vp.x * sx, op.vp.y * sy, op.vp.w * sx, op.vp.h * sy, op.vp.minz, op.vp.maxz};
        int x0 = (int)v.x, y0 = (int)v.y, x1 = (int)(v.x + v.width), y1 = (int)(v.y + v.height);
        x0 = x0 < 0 ? 0 : x0;
        y0 = y0 < 0 ? 0 : y0;
        x1 = x1 > tw ? tw : x1;
        y1 = y1 > th ? th : y1;
        if (x1 <= x0 || y1 <= y0 || v.width <= 0 || v.height <= 0) return;
        VkRect2D sc{{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
        vkCmdSetViewport(s.cmd, 0, 1, &v);
        vkCmdSetScissor(s.cmd, 0, 1, &sc);
        vkCmdDrawIndexed(s.cmd, op.index_count, 1, op.first_index, op.vertex_offset, s.taa.jitter_now ? taa_instance(v.width, v.height) : 0);
        if (soft) s.soft.executed++;
        if (lit) s.charlit.executed++;
        if (lit && op.push[3]) s.charlit.world_executed++;
        if (emit) s.emit.executed++;
        if (relief) s.relief.executed++;
        if (hair) s.hair.executed++;
        if (skin) s.skinlit.executed++;
        if (skin && (cfg.eyes > 0 || cfg.cloth > 0)) s.skinlit.mat_executed++;
        return;
    }
    VkClearAttachment ca[2]{};
    uint32_t n = 0;
    if (op.flags & RENDERER_CLEAR_COLOR) {
        ca[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca[n].colorAttachment = 0;
        ca[n].clearValue.color = clear_value(op.color);
        if (op.mask) ca[n].clearValue.color.float32[3] = 0.0f;
        n++;
    }
    if (op.flags & RENDERER_CLEAR_DEPTH) {
        ca[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        ca[n].clearValue.depthStencil = {op.z, 0};
        n++;
    }
    VkClearRect whole{{{0, 0}, {(uint32_t)tw, (uint32_t)th}}, 0, 1};
    if (n) vkCmdClearAttachments(s.cmd, n, ca, 1, &whole);
}

// Layout transitions of an offscreen pass's surfaces: sampled <-> attachment.
static void surface_barriers(const Op &op, bool to_attachment) {
    for (int k = 0; k < 2; k++) {
        const Surface *sf = surface_of(op.target[k]);
        if (!sf) continue;
        const bool depth = k == 1;
        const VkImageLayout sampled = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const VkImageLayout att = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        const VkAccessFlags att_access = depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                               : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        const VkPipelineStageFlags att_stage = depth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                                                     : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        const VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        if (to_attachment)
            barrier(sf->im.img, sampled, att, VK_ACCESS_SHADER_READ_BIT, att_access, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, att_stage, 1, aspect);
        else
            barrier(sf->im.img, att, sampled, att_access, VK_ACCESS_SHADER_READ_BIT, att_stage, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 1, aspect);
    }
}

// A draw into a colour surface that samples that surface (the title's blur mode, 0xbc1c0: half-texel
// shifts of its 512x256 target in place). A render pass cannot sample its own attachment, so such
// a draw reads a copy (the surface's twin) taken right before it.
static bool self_sampling(const Op &op) {
    if (op.kind != OP_DRAW || !op.target[0]) return false;
    for (int i = 0; i < RENDERER_STAGES; i++)
        if (op.tex[i] == op.target[0]) return true;
    return false;
}
// The twins the frame's self-sampling draws need, created before the command buffer is recorded
// (a one-time submission each). When one cannot be made, those draws read the surface as their
// pass began, as before.
static void twins_create() {
    for (const Op &op : s.ops) {
        if (!self_sampling(op)) continue;
        const uint32_t i = (op.target[0] & ~SURFACE_BIT) - 1;
        if (s.surfaces[i].twin || s.surfaces.size() >= MAX_SURFACES) continue;
        Surface t = s.surfaces[i];
        t.twin = 0;
        t.im = Image();
        MemTag tag(RENDERER_VRAM_SURFACES);
        if (make_surface_image(&t.im, surface_format(t.kind), false, t.w, t.h, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true)) {
            free_image(&t.im);
            continue;
        }
        s.surfaces.push_back(t);
        s.surfaces[i].twin = SURFACE_BIT | (uint32_t)s.surfaces.size();
    }
}
// Copies a colour surface (in SHADER_READ_ONLY_OPTIMAL, outside a render pass) into its twin.
static void twin_copy(const Surface &sf) {
    const Surface &t = s.surfaces[(sf.twin & ~SURFACE_BIT) - 1];
    barrier(sf.im.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(t.im.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy cp{};
    cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.extent = {(uint32_t)sf.w, (uint32_t)sf.h, 1};
    vkCmdCopyImage(s.cmd, sf.im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    barrier(t.im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    barrier(sf.im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

// The frame's offscreen operations, in recorded order: consecutive operations on the same target
// share one render pass instance (a self-sampling draw ends it, copies the target into its twin
// and starts a new one).
static void record_offscreen(std::unordered_map<uint64_t, VkDescriptorSet> &sets, VkDeviceSize index_offset) {
    const Op *open = nullptr;
    OffscreenFb *fb = nullptr;
    PassState ps;
    for (const Op &op0 : s.ops) {
        if ((!op0.target[0] && !op0.target[1]) || op0.kind == OP_GLASS) continue;
        const Surface *tsf = self_sampling(op0) ? surface_of(op0.target[0]) : nullptr;
        Op twin_op;
        const Op *opp = &op0;
        if (tsf && tsf->twin) {
            twin_op = op0;
            for (int i = 0; i < RENDERER_STAGES; i++)
                if (twin_op.tex[i] == op0.target[0]) twin_op.tex[i] = tsf->twin;
            opp = &twin_op;
        }
        const Op &op = *opp;
        const bool cut = opp == &twin_op;   // a new pass instance after the copy
        if (!cut && open && open->target[0] == op.target[0] && open->target[1] == op.target[1]) {
            if (fb) exec_op(op, ps, sets, index_offset, (float)fb->w / fb->gw, (float)fb->h / fb->gh, fb->w, fb->h);
            continue;
        }
        if (open && fb) {
            vkCmdEndRenderPass(s.cmd);
            surface_barriers(*open, false);
        }
        if (cut) twin_copy(*tsf);
        open = &op0;
        fb = offscreen_fb(op.target[0], op.target[1]);
        if (!fb) continue;
        surface_barriers(op, true);
        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rb.renderPass = s.off_pass[fb->pass_class - 1];
        rb.framebuffer = fb->fb;
        rb.renderArea = {{0, 0}, {(uint32_t)fb->w, (uint32_t)fb->h}};
        vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        ps = PassState();
        if (!op.target[1]) {   // the dummy depth starts every pass at 1.0
            VkClearAttachment ca{VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}};
            ca.clearValue.depthStencil = {1.0f, 0};
            VkClearRect whole{{{0, 0}, {(uint32_t)fb->w, (uint32_t)fb->h}}, 0, 1};
            vkCmdClearAttachments(s.cmd, 1, &ca, 1, &whole);
        }
        exec_op(op, ps, sets, index_offset, (float)fb->w / fb->gw, (float)fb->h / fb->gh, fb->w, fb->h);
    }
    if (open && fb) {
        vkCmdEndRenderPass(s.cmd);
        surface_barriers(*open, false);
    }
}

// ---- Glass operations (renderer_glass) ----

// Pipeline of the glass, sharpening and bloom passes: no vertex input (full-target triangle), no
// depth, dynamic viewport and scissor. blend: FS_REPLACE writes every channel; FS_COVER (the glass
// composite) blends by its coverage alpha, FS_ADD (bloom upsample) adds, FS_SCREEN (bloom
// composite) screens, FS_MULTIPLY (ssao composite) multiplies, FS_LIGHTEN (lit rain) adds src x
// dst and FS_PREMUL (wet ground) adds src over dst x (1 - src alpha), FS_VOLFOG (volumetric fog) src + dst x src alpha, all writing rgb only, like
// the geometry pipelines.
enum { FS_REPLACE, FS_COVER, FS_ADD, FS_SCREEN, FS_MULTIPLY, FS_LIGHTEN, FS_PREMUL, FS_VOLFOG };
static VkPipeline fullscreen_pipeline(VkShaderModule vs, VkPipelineLayout layout, VkShaderModule fs, VkRenderPass pass,
                                      VkSampleCountFlagBits samples, int blend, bool depth_test = false,
                                      const VkSpecializationInfo *spec = nullptr, uint32_t attachments = 1, bool depth_write = false) {
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs;
    st[1].pName = "main";
    st[1].pSpecializationInfo = spec;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = depth_test || depth_write ? VK_TRUE : VK_FALSE;   // rain slices: tested, never written
    ds.depthCompareOp = depth_write ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_LESS_OR_EQUAL;   // --taa-upscale's output pass: written everywhere
    ds.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;
    VkPipelineColorBlendAttachmentState cb{};
    cb.blendEnable = blend ? VK_TRUE : VK_FALSE;
    cb.srcColorBlendFactor = cb.srcAlphaBlendFactor = blend == FS_ADD || blend == FS_PREMUL || blend == FS_VOLFOG ? VK_BLEND_FACTOR_ONE
                                                      : blend == FS_SCREEN   ? VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR
                                                      : blend == FS_MULTIPLY || blend == FS_LIGHTEN ? VK_BLEND_FACTOR_DST_COLOR
                                                                                                    : VK_BLEND_FACTOR_SRC_ALPHA;
    cb.dstColorBlendFactor = cb.dstAlphaBlendFactor = blend == FS_ADD || blend == FS_SCREEN || blend == FS_LIGHTEN ? VK_BLEND_FACTOR_ONE
                                                      : blend == FS_MULTIPLY                 ? VK_BLEND_FACTOR_ZERO
                                                      : blend == FS_VOLFOG                   ? VK_BLEND_FACTOR_SRC_ALPHA
                                                                                             : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cb.colorBlendOp = cb.alphaBlendOp = VK_BLEND_OP_ADD;
    cb.colorWriteMask = blend ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT : 15;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    const VkPipelineColorBlendAttachmentState cbn[3] = {cb, cb, cb};   // attachments 1-3, written alike (--taa: colour, distance, upscaled depth)
    cbs.attachmentCount = attachments;
    cbs.pAttachments = attachments > 1 ? cbn : &cb;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cbs;
    pi.pDynamicState = &dy;
    pi.layout = layout;
    pi.renderPass = pass;
    VkPipeline p = VK_NULL_HANDLE;
    if (VKC(vkCreateGraphicsPipelines, s.dev, s.pcache, 1, &pi, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    return p;
}

static VkPipeline glass_pipeline(VkShaderModule fs, VkRenderPass pass, VkSampleCountFlagBits samples, int blend) {
    return fullscreen_pipeline(s.glass.vs, s.glass.layout, fs, pass, samples, blend);
}

// The pass a split resumes the frame with: s.resume_pass, or with soft particles live the soft
// variant (depth read-only).
static VkRenderPass resume_pass() {
    s.soft.in_pass = s.soft.live;
    return s.soft.live ? s.soft.pass : s.resume_pass;
}

// The main pass variants a split needs (glass, bloom), on first use.
static bool split_passes_ready() {
    if (s.split_pass && s.resume_pass) return true;
    return !s.split_pass && !create_main_pass(&s.split_pass, SPLIT_PASS) && !create_main_pass(&s.resume_pass, RESUME_PASS);
}

// Creates the glass resources on first use (no submission). Returns false when the device cannot
// (the operations are then skipped and the frame is recorded as without them).
static bool glass_ready() {
    Glass &g = s.glass;
    if (g.tried) return g.ok;
    g.tried = true;
    const uint32_t ow = (uint32_t)s.ow, oh = (uint32_t)s.oh;
    const VkImageUsageFlags use = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (make_image(&g.a, VK_FORMAT_B8G8R8A8_UNORM, use | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, ow, oh) ||
        make_image(&g.b, VK_FORMAT_B8G8R8A8_UNORM, use, VK_IMAGE_ASPECT_COLOR_BIT, ow, oh) || !split_passes_ready())
        return false;
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_B8G8R8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    if (VKC(vkCreateRenderPass, s.dev, &rp, nullptr, &g.blur_pass) != VK_SUCCESS) return fail("glass: vkCreateRenderPass failed"), false;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = g.blur_pass;
    fi.attachmentCount = 1;
    fi.width = ow;
    fi.height = oh;
    fi.layers = 1;
    fi.pAttachments = &g.a.view;
    if (VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &g.fb_a) != VK_SUCCESS) return false;
    fi.pAttachments = &g.b.view;
    if (VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &g.fb_b) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &g.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &g.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[2] = {g.set_layout, g.set_layout};
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g.pool;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, sets) != VK_SUCCESS) return false;
    g.set_a = sets[0];
    g.set_b = sets[1];
    VkDescriptorImageInfo ii[2] = {{s.clamp_sampler, g.a.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {s.clamp_sampler, g.b.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[2]{};
    for (int i = 0; i < 2; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = sets[i];
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(s.dev, 2, w, 0, nullptr);
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &g.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &g.layout) != VK_SUCCESS) return false;
    g.vs = shader(glass_vert, sizeof glass_vert);
    g.blur_fs = shader(glass_blur_frag, sizeof glass_blur_frag);
    g.comp_fs = shader(glass_comp_frag, sizeof glass_comp_frag);
    if (!g.vs || !g.blur_fs || !g.comp_fs) return false;
    g.blur = glass_pipeline(g.blur_fs, g.blur_pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE);
    g.comp = glass_pipeline(g.comp_fs, s.pass, s.samples, FS_COVER);
    g.ok = g.blur && g.comp;
    return g.ok;
}

static void glass_free() {
    Glass &g = s.glass;
    if (g.blur) vkDestroyPipeline(s.dev, g.blur, nullptr);
    if (g.comp) vkDestroyPipeline(s.dev, g.comp, nullptr);
    if (g.vs) vkDestroyShaderModule(s.dev, g.vs, nullptr);
    if (g.blur_fs) vkDestroyShaderModule(s.dev, g.blur_fs, nullptr);
    if (g.comp_fs) vkDestroyShaderModule(s.dev, g.comp_fs, nullptr);
    if (g.layout) vkDestroyPipelineLayout(s.dev, g.layout, nullptr);
    if (g.pool) vkDestroyDescriptorPool(s.dev, g.pool, nullptr);
    if (g.set_layout) vkDestroyDescriptorSetLayout(s.dev, g.set_layout, nullptr);
    if (g.fb_a) vkDestroyFramebuffer(s.dev, g.fb_a, nullptr);
    if (g.fb_b) vkDestroyFramebuffer(s.dev, g.fb_b, nullptr);
    if (g.blur_pass) vkDestroyRenderPass(s.dev, g.blur_pass, nullptr);
    free_image(&g.a);
    free_image(&g.b);
    g = Glass();
}

// One blur direction into dst (framebuffer fb) over area, sampling set (the other scratch image).
static void glass_blur_pass(Image &dst, VkFramebuffer fb, VkDescriptorSet set, VkRect2D area, const int32_t lim[4], int dx, int dy,
                            int radius, float sigma) {
    barrier(dst.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = s.glass.blur_pass;
    rb.framebuffer = fb;
    rb.renderArea = area;
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.glass.blur);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.glass.layout, 0, 1, &set, 0, nullptr);
    struct { int32_t lim[4], dir[2], radius; float sigma; } pc = {{lim[0], lim[1], lim[2], lim[3]}, {dx, dy}, radius, sigma};
    vkCmdPushConstants(s.cmd, s.glass.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport v{0, 0, (float)s.ow, (float)s.oh, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &area);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    barrier(dst.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

// Executes a glass operation inside the frame's render pass (begun with s.split_pass or
// s.resume_pass): ends it (the target is stored and, with MSAA, resolved into s.color), copies the
// rect plus the blur radius from s.color, blurs it horizontally into b and vertically back into a,
// resumes the pass (resume_pass) and composites a over the rect. Returns false when the rect is
// empty (nothing was split).
static bool glass_exec(const RGlass &gl, float sx, float sy) {
    const int ow = s.ow, oh = s.oh;
    int x0 = (int)floorf(gl.x0 * sx), y0 = (int)floorf(gl.y0 * sy), x1 = (int)ceilf(gl.x1 * sx), y1 = (int)ceilf(gl.y1 * sy);
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > ow ? ow : x1;
    y1 = y1 > oh ? oh : y1;
    if (x1 <= x0 || y1 <= y0) return false;
    int radius = (int)lroundf(gl.blur * sy);
    radius = radius < 1 ? 1 : radius > 64 ? 64 : radius;
    const float sigma = radius * 0.5f > 0.5f ? radius * 0.5f : 0.5f;
    const int e0 = x0 - radius < 0 ? 0 : x0 - radius, f0 = y0 - radius < 0 ? 0 : y0 - radius;
    const int e1 = x1 + radius > ow ? ow : x1 + radius, f1 = y1 + radius > oh ? oh : y1 + radius;
    const int32_t lim[4] = {e0, f0, e1 - 1, f1 - 1};
    Glass &g = s.glass;
    vkCmdEndRenderPass(s.cmd);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};   // attachment writes -> the copy and the resumed pass
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
    barrier(g.a.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy cp{};
    cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.srcOffset = cp.dstOffset = {e0, f0, 0};
    cp.extent = {(uint32_t)(e1 - e0), (uint32_t)(f1 - f0), 1};
    vkCmdCopyImage(s.cmd, s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.a.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    barrier(g.a.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    const VkRect2D wide{{e0, f0}, {(uint32_t)(e1 - e0), (uint32_t)(f1 - f0)}};
    const VkRect2D box{{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    glass_blur_pass(g.b, g.fb_b, g.set_a, wide, lim, 1, 0, radius, sigma);
    glass_blur_pass(g.a, g.fb_a, g.set_b, box, lim, 0, 1, radius, sigma);
    barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = resume_pass();
    rb.framebuffer = s.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)ow, (uint32_t)oh}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.comp);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &g.set_a, 0, nullptr);
    auto unpack = [](uint32_t c, float *o) {
        o[0] = (c >> 16 & 0xFF) / 255.0f;
        o[1] = (c >> 8 & 0xFF) / 255.0f;
        o[2] = (c & 0xFF) / 255.0f;
        o[3] = (c >> 24) / 255.0f;
    };
    struct { float box[4], tint[4], rim[4], corner, rim_w, pad[2]; } pc{};
    pc.box[0] = gl.x0 * sx;
    pc.box[1] = gl.y0 * sy;
    pc.box[2] = gl.x1 * sx;
    pc.box[3] = gl.y1 * sy;
    unpack(gl.tint, pc.tint);
    unpack(gl.rim, pc.rim);
    pc.corner = gl.corner * sy;
    pc.rim_w = sy > 1 ? sy : 1;
    vkCmdPushConstants(s.cmd, g.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport v{0, 0, (float)ow, (float)oh, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &box);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    g.executed++;
    return true;
}

// ---- Sharpening (RendererConfig.sharpen) ----

static bool sharpen_create() {
    Sharpen &h = s.sharp;
    const uint32_t fw = (uint32_t)s.fw, fh = (uint32_t)s.fh;
    // sampled: the gamma ramp reads it
    if (make_image(&h.out, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, fw, fh))
        return false;
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_B8G8R8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // every pixel is written
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;   // barriers around the pass
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    if (VKC(vkCreateRenderPass, s.dev, &rp, nullptr, &h.pass) != VK_SUCCESS) return false;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = h.pass;
    fi.attachmentCount = 1;
    fi.pAttachments = &h.out.view;
    fi.width = fw;
    fi.height = fh;
    fi.layers = 1;
    if (VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &h.fb) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &h.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &h.pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = h.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &h.set_layout;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, &h.set) != VK_SUCCESS) return false;
    // texelFetch: the sampler's filter is not used.
    VkDescriptorImageInfo ii{s.clamp_sampler, frame_image().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = h.set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(s.dev, 1, &w, 0, nullptr);
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &h.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &h.layout) != VK_SUCCESS) return false;
    h.vs = shader(glass_vert, sizeof glass_vert);
    h.fs = shader(cas_frag, sizeof cas_frag);
    if (!h.vs || !h.fs) return false;
    h.pipe = fullscreen_pipeline(h.vs, h.layout, h.fs, h.pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE);
    return h.pipe != VK_NULL_HANDLE;
}

// At renderer_init: the sharpening resources when RendererConfig.sharpen > 0. A failure frees them
// and leaves sharpening off (logged by the caller through renderer_sharpen()).
static void sharpen_init() {
    if (cfg.sharpen <= 0 || s.fsr.req) return;   // with FSR, RCAS sharpens instead
    Sharpen &h = s.sharp;
    h.amount = cfg.sharpen > 100 ? 100 : cfg.sharpen;
    h.peak = -1.0f / (8.0f + (5.0f - 8.0f) * (h.amount / 100.0f));   // CAS: -1 / lerp(8, 5, sharpness)
    if (sharpen_create()) {
        h.ok = true;
        return;
    }
    sharpen_free();
}

static void sharpen_free() {
    Sharpen &h = s.sharp;
    if (h.pipe) vkDestroyPipeline(s.dev, h.pipe, nullptr);
    if (h.vs) vkDestroyShaderModule(s.dev, h.vs, nullptr);
    if (h.fs) vkDestroyShaderModule(s.dev, h.fs, nullptr);
    if (h.layout) vkDestroyPipelineLayout(s.dev, h.layout, nullptr);
    if (h.pool) vkDestroyDescriptorPool(s.dev, h.pool, nullptr);
    if (h.set_layout) vkDestroyDescriptorSetLayout(s.dev, h.set_layout, nullptr);
    if (h.fb) vkDestroyFramebuffer(s.dev, h.fb, nullptr);
    if (h.pass) vkDestroyRenderPass(s.dev, h.pass, nullptr);
    free_image(&h.out);
    h = Sharpen();
}

// After the main pass and the downsample: the frame (TRANSFER_SRC_OPTIMAL) is sampled into the
// sharpened image, which ends in TRANSFER_SRC_OPTIMAL for the readback copy or the swapchain blit;
// the frame goes back to TRANSFER_SRC_OPTIMAL, the layout the next frame's barriers expect.
static void record_sharpen(void) {
    Sharpen &h = s.sharp;
    if (!h.ok) return;
    VkImage src = frame_image().img;
    const VkPipelineStageFlags written = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    barrier(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, written,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    // After the previous frame's readback copy or blit of it (same queue).
    barrier(h.out.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = h.pass;
    rb.framebuffer = h.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)s.fw, (uint32_t)s.fh}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)s.fw, (float)s.fh, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &vp);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, h.pipe);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, h.layout, 0, 1, &h.set, 0, nullptr);
    vkCmdPushConstants(s.cmd, h.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &h.peak);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    barrier(h.out.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
}

// ---- FSR 1 (RendererConfig.fsr_w / fsr_h) ----

static bool fsr_create() {
    Fsr &f = s.fsr;
    const uint32_t dw = (uint32_t)s.dispw, dh = (uint32_t)s.disph;
    // EASU's image is sampled by RCAS; RCAS's by the gamma ramp, copied by the readback and the
    // swapchain blit, and drawn on by the frame counter.
    if (make_image(&f.easu, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, dw, dh) ||
        make_image(&f.out, VK_FORMAT_B8G8R8A8_UNORM,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, dw, dh))
        return false;
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_B8G8R8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // every pixel is written
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;   // barriers around the passes
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    if (VKC(vkCreateRenderPass, s.dev, &rp, nullptr, &f.pass) != VK_SUCCESS) return false;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = f.pass;
    fi.attachmentCount = 1;
    fi.width = dw;
    fi.height = dh;
    fi.layers = 1;
    fi.pAttachments = &f.easu.view;
    if (VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &f.easu_fb) != VK_SUCCESS) return false;
    fi.pAttachments = &f.out.view;
    if (VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &f.rcas_fb) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &f.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &f.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[2] = {f.set_layout, f.set_layout};
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = f.pool;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, sets) != VK_SUCCESS) return false;
    f.easu_set = sets[0];
    f.rcas_set = sets[1];
    // EASU gathers (the filter is not used; clamped to the edge), RCAS uses texelFetch.
    VkDescriptorImageInfo ii[2] = {{s.clamp_sampler, frame_image().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {s.clamp_sampler, f.easu.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[2]{};
    for (int k = 0; k < 2; k++) {
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[k].dstSet = sets[k];
        w[k].descriptorCount = 1;
        w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[k].pImageInfo = &ii[k];
    }
    vkUpdateDescriptorSets(s.dev, 2, w, 0, nullptr);
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof f.easu_con};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &f.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &f.layout) != VK_SUCCESS) return false;
    f.vs = shader(glass_vert, sizeof glass_vert);
    f.easu_fs = shader(fsr_easu_frag, sizeof fsr_easu_frag);
    f.rcas_fs = shader(fsr_rcas_frag, sizeof fsr_rcas_frag);
    if (!f.vs || !f.easu_fs || !f.rcas_fs) return false;
    f.easu_pipe = fullscreen_pipeline(f.vs, f.layout, f.easu_fs, f.pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE);
    f.rcas_pipe = fullscreen_pipeline(f.vs, f.layout, f.rcas_fs, f.pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE);
    return f.easu_pipe != VK_NULL_HANDLE && f.rcas_pipe != VK_NULL_HANDLE;
}

// At renderer_init, after create_targets made the scene targets at the internal size: the
// upscale resources and constants. A failure frees them and presents (and reads back) the frame
// at the internal size instead (renderer_fsr() = 0, renderer_output_size the internal size).
static void fsr_init() {
    Fsr &f = s.fsr;
    if (!f.req) return;
    const float sh = cfg.fsr_sharpness;
    f.sharpness = sh >= 0 ? (sh > FSR_SHARPNESS_MAX ? FSR_SHARPNESS_MAX : sh) : 0;
    fsr_easu_con(f.easu_con, s.fw, s.fh, s.dispw, s.disph);
    fsr_rcas_con(f.rcas_con, f.sharpness);
    if (fsr_create()) {
        f.ok = true;
        return;
    }
    fsr_free();
    s.dispw = s.fw;
    s.disph = s.fh;
}

static void fsr_free() {
    Fsr &f = s.fsr;
    if (f.easu_pipe) vkDestroyPipeline(s.dev, f.easu_pipe, nullptr);
    if (f.rcas_pipe) vkDestroyPipeline(s.dev, f.rcas_pipe, nullptr);
    if (f.vs) vkDestroyShaderModule(s.dev, f.vs, nullptr);
    if (f.easu_fs) vkDestroyShaderModule(s.dev, f.easu_fs, nullptr);
    if (f.rcas_fs) vkDestroyShaderModule(s.dev, f.rcas_fs, nullptr);
    if (f.layout) vkDestroyPipelineLayout(s.dev, f.layout, nullptr);
    if (f.pool) vkDestroyDescriptorPool(s.dev, f.pool, nullptr);
    if (f.set_layout) vkDestroyDescriptorSetLayout(s.dev, f.set_layout, nullptr);
    if (f.easu_fb) vkDestroyFramebuffer(s.dev, f.easu_fb, nullptr);
    if (f.rcas_fb) vkDestroyFramebuffer(s.dev, f.rcas_fb, nullptr);
    if (f.pass) vkDestroyRenderPass(s.dev, f.pass, nullptr);
    free_image(&f.easu);
    free_image(&f.out);
    f = Fsr();
}

// One full-target FSR pass at the displayed size into fb with the source set and constants.
static void fsr_pass(VkFramebuffer fb, VkPipeline pipe, VkDescriptorSet set, const uint32_t *con, uint32_t bytes) {
    Fsr &f = s.fsr;
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = f.pass;
    rb.framebuffer = fb;
    rb.renderArea = {{0, 0}, {(uint32_t)s.dispw, (uint32_t)s.disph}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)s.dispw, (float)s.disph, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &vp);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, f.layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(s.cmd, f.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, bytes, con);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
}

// After the main pass: the frame (TRANSFER_SRC_OPTIMAL, internal size) is upscaled by EASU into
// f.easu, which RCAS sharpens into f.out; f.out ends in TRANSFER_SRC_OPTIMAL like the sharpened
// image (for the gamma ramp, the frame counter, the readback copy or the swapchain blit) and the
// frame goes back to TRANSFER_SRC_OPTIMAL, the layout the next frame's barriers expect.
static void record_fsr(void) {
    Fsr &f = s.fsr;
    if (!f.ok) return;
    VkImage src = frame_image().img;
    const VkPipelineStageFlags written = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    barrier(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, written,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    // After the previous frame's RCAS read of it (same queue).
    barrier(f.easu.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    fsr_pass(f.easu_fb, f.easu_pipe, f.easu_set, f.easu_con, sizeof f.easu_con);
    barrier(f.easu.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    // After the previous frame's readback copy, blit, gamma ramp read or frame counter on it.
    barrier(f.out.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    fsr_pass(f.rcas_fb, f.rcas_pipe, f.rcas_set, f.rcas_con, sizeof f.rcas_con);
    barrier(f.out.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
}

// ---- Frame counter overlay (RendererConfig.fps_overlay) ----

// A pass that keeps the target (LOAD) in COLOR_ATTACHMENT_OPTIMAL (barriers around it) and the
// overlay pipeline for it.
static bool overlay_pass(OverlayPass &o, VkFormat fmt) {
    Overlay &v = s.ov;
    if (o.fmt == fmt && o.pipe) return true;
    if (o.pipe) vkDestroyPipeline(s.dev, o.pipe, nullptr);
    if (o.pass) vkDestroyRenderPass(s.dev, o.pass, nullptr);
    o = OverlayPass();
    VkAttachmentDescription att{};
    att.format = fmt;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    if (VKC(vkCreateRenderPass, s.dev, &rp, nullptr, &o.pass) != VK_SUCCESS) return false;
    o.pipe = fullscreen_pipeline(v.vs, v.layout, v.fs, o.pass, VK_SAMPLE_COUNT_1_BIT, FS_COVER);
    o.fmt = fmt;
    return o.pipe != VK_NULL_HANDLE;
}

static void overlay_pass_free(OverlayPass &o) {
    if (o.pipe) vkDestroyPipeline(s.dev, o.pipe, nullptr);
    if (o.pass) vkDestroyRenderPass(s.dev, o.pass, nullptr);
    o = OverlayPass();
}

static VkFramebuffer overlay_fb(VkRenderPass pass, VkImageView view, uint32_t w, uint32_t h) {
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = pass;
    fi.attachmentCount = 1;
    fi.pAttachments = &view;
    fi.width = w;
    fi.height = h;
    fi.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    return VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &fb) == VK_SUCCESS ? fb : VK_NULL_HANDLE;
}

// GPU % (RendererConfig.fps_overlay_gpu): the timestamp query pool, two per frame slot, when the
// queue has timestamps (else ts_bits stays 0: "GPU --").
static void overlay_queries() {
    Overlay &v = s.ov;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s.phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qs(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(s.phys, &nq, qs.data());
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(s.phys, &props);
    const uint32_t bits = s.qfamily < nq ? qs[s.qfamily].timestampValidBits : 0;
    if (!bits || props.limits.timestampPeriod <= 0) return;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 2 * MAX_FRAMES_IN_FLIGHT;
    if (VKC(vkCreateQueryPool, s.dev, &qi, nullptr, &v.queries) != VK_SUCCESS) {
        v.queries = VK_NULL_HANDLE;
        return;
    }
    v.ts_bits = bits;
    v.ts_period = props.limits.timestampPeriod;
}

// record_frame, first after vkBeginCommandBuffer: the slot's previous frame has completed
// (wait_slot), so its two timestamps are read without waiting; then this frame's first one.
static void overlay_stamp_begin() {
    Overlay &v = s.ov;
    if (!v.queries) return;
    const uint32_t q = 2 * (uint32_t)s.slot;
    if (v.stamped[s.slot]) {
        uint64_t t[2];
        if (vkGetQueryPoolResults(s.dev, v.queries, q, 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const double ms = fps_gpu_busy_ms(t[0], t[1], v.ts_bits, v.ts_period);
            v.busy_ms += ms;
            v.busy_frames++;
            v.last_ms = (float)ms;
        }
        v.stamped[s.slot] = false;
    }
    vkCmdResetQueryPool(s.cmd, v.queries, q, 2);
    vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, v.queries, q);
}
// record_frame, last: after the scene, its post-processing and the downsample, before the
// readback copy or the swapchain blit (the blit waits for the image: not the game's GPU work).
static void overlay_stamp_end() {
    Overlay &v = s.ov;
    if (!v.queries) return;
    vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, v.queries, 2 * (uint32_t)s.slot + 1);
    v.stamped[s.slot] = true;
}

// RendererConfig.pass_stats: one query pool per frame slot, when the queue has timestamps.
static void gp_init() {
    GpuProf &g = s.gp;
    if (!cfg.pass_stats) return;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s.phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qs(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(s.phys, &nq, qs.data());
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(s.phys, &props);
    const uint32_t bits = s.qfamily < nq ? qs[s.qfamily].timestampValidBits : 0;
    if (!bits || props.limits.timestampPeriod <= 0) return;
    for (VkQueryPool &p : g.pool) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = GP_MAX_MARKS;
        if (VKC(vkCreateQueryPool, s.dev, &qi, nullptr, &p) != VK_SUCCESS) {
            p = VK_NULL_HANDLE;
            return;
        }
    }
    g.mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    g.period = props.limits.timestampPeriod;
}
static void gp_free() {
    for (VkQueryPool p : s.gp.pool)
        if (p) vkDestroyQueryPool(s.dev, p, nullptr);
    s.gp = GpuProf();
}
// The interval since the previous timestamp of the frame is pass's (none past GP_MAX_MARKS: the
// next mark takes it).
static void gp_mark(GpPass pass) {
    GpuProf &g = s.gp;
    VkQueryPool p = g.pool[s.slot];
    uint32_t &n = g.n[s.slot];
    if (!p || !n || n >= GP_MAX_MARKS) return;
    vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, p, n);
    g.label[s.slot][n++] = pass;
}
// record_frame, first: the slot's previous frame has completed (wait_slot), so its timestamps are
// read without waiting and added to both windows; then this frame's start.
static void gp_begin() {
    GpuProf &g = s.gp;
    VkQueryPool p = g.pool[s.slot];
    if (!p) return;
    uint32_t &n = g.n[s.slot];
    if (n > 1) {
        uint64_t t[GP_MAX_MARKS];
        if (vkGetQueryPoolResults(s.dev, p, 0, n, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const double k = g.period / 1e6;
            for (int w = 0; w < 2; w++) {
                for (uint32_t i = 1; i < n; i++) g.sum[w][g.label[s.slot][i]] += (double)((t[i] - t[i - 1]) & g.mask) * k;
                g.total[w] += (double)((t[n - 1] - t[0]) & g.mask) * k;
                g.frames[w]++;
            }
        }
    }
    vkCmdResetQueryPool(s.cmd, p, 0, GP_MAX_MARKS);
    vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, p, 0);
    n = 1;
}
uint32_t renderer_gpu_passes(int window, char *buf, size_t len) {
    GpuProf &g = s.gp;
    if (window < 0 || window > 1 || !len) return 0;
    const uint32_t frames = g.frames[window];
    if (frames) {
        int order[GP_COUNT];
        for (int i = 0; i < GP_COUNT; i++) order[i] = i;
        std::sort(order, order + GP_COUNT, [&](int a, int b) { return g.sum[window][a] > g.sum[window][b]; });
        size_t k = (size_t)snprintf(buf, len, "total %.2f ms:", g.total[window] / frames);
        bool first = true;
        for (int i : order) {
            const double ms = g.sum[window][i] / frames;
            if (ms < 0.005 || k >= len) continue;
            k += (size_t)snprintf(buf + k, len - k, "%s %s %.2f", first ? "" : ",", gp_name[i], ms);
            first = false;
        }
    }
    memset(g.sum[window], 0, sizeof g.sum[window]);
    g.total[window] = 0;
    g.frames[window] = 0;
    return frames;
}

// At renderer_init (after sharpen_init: the read-back frame is the sharpened image with it). A
// failure leaves the overlay off (renderer_fps_overlay_on() = 0).
static void overlay_init() {
    if (!cfg.fps_overlay) {
        if (cfg.gpu_timestamps) overlay_queries();   // the timestamps alone, nothing drawn
        return;
    }
    Overlay &v = s.ov;
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(OverlayPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    Image &out = pre_gamma_image();
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &v.layout) == VK_SUCCESS && (v.vs = shader(glass_vert, sizeof glass_vert)) &&
        (v.fs = shader(fps_overlay_frag, sizeof fps_overlay_frag)) && overlay_pass(v.rb, VK_FORMAT_B8G8R8A8_UNORM) &&
        (v.rb_fb = overlay_fb(v.rb.pass, out.view, (uint32_t)s.dispw, (uint32_t)s.disph))) {
        v.ok = true;
        if (cfg.fps_overlay_gpu || cfg.gpu_timestamps) overlay_queries();
        return;
    }
    overlay_free();
    if (cfg.gpu_timestamps) overlay_queries();
}

static void overlay_free() {
    Overlay &v = s.ov;
    overlay_sc_free();
    overlay_pass_free(v.rb);
    overlay_pass_free(v.sc);
    if (v.rb_fb) vkDestroyFramebuffer(s.dev, v.rb_fb, nullptr);
    if (v.vs) vkDestroyShaderModule(s.dev, v.vs, nullptr);
    if (v.fs) vkDestroyShaderModule(s.dev, v.fs, nullptr);
    if (v.layout) vkDestroyPipelineLayout(s.dev, v.layout, nullptr);
    if (v.queries) vkDestroyQueryPool(s.dev, v.queries, nullptr);
    v = Overlay();
}

// The box on a target (in COLOR_ATTACHMENT_OPTIMAL) of w x h pixels: one scissored triangle.
static void overlay_draw(const OverlayPass &o, VkFramebuffer fb, uint32_t w, uint32_t h) {
    Overlay &v = s.ov;
    FpsBox b = fps_overlay_box((int)w, (int)h, cfg.fps_overlay_corner, v.n);
    int32_t x0 = b.x < 0 ? 0 : b.x, y0 = b.y < 0 ? 0 : b.y;
    int32_t x1 = b.x + b.w > (int)w ? (int)w : b.x + b.w, y1 = b.y + b.h > (int)h ? (int)h : b.y + b.h;
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = o.pass;
    rb.framebuffer = fb;
    rb.renderArea = {{0, 0}, {w, h}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    if (x1 > x0 && y1 > y0) {
        OverlayPush pc{};
        pc.box[0] = b.x;
        pc.box[1] = b.y;
        pc.box[2] = b.w;
        pc.box[3] = b.h;
        pc.scale = b.scale;
        pc.n = v.n;
        for (int i = 0; i < v.n; i++) pc.text[i >> 2] |= (uint32_t)v.glyph[i] << (8 * (i & 3));
        VkViewport vp{0, 0, (float)w, (float)h, 0, 1};
        VkRect2D sc{{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
        vkCmdSetViewport(s.cmd, 0, 1, &vp);
        vkCmdSetScissor(s.cmd, 0, 1, &sc);
        vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, o.pipe);
        vkCmdPushConstants(s.cmd, v.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        vkCmdDraw(s.cmd, 3, 1, 0, 0);
        v.executed++;
    }
    vkCmdEndRenderPass(s.cmd);
}

// renderer_end: on the finished frame (TRANSFER_SRC_OPTIMAL before and after), before the copy.
static void record_overlay_rb() {
    Overlay &v = s.ov;
    v.executed = 0;
    if (!v.n) return;
    VkImage img = output_image();
    barrier(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    overlay_draw(v.rb, s.gamma.active ? s.gamma.overlay_fb : v.rb_fb, (uint32_t)s.dispw, (uint32_t)s.disph);
    barrier(img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
}

// renderer_present: on the swapchain image after the blit (TRANSFER_DST_OPTIMAL), ending it in
// PRESENT_SRC_KHR. Returns false (nothing recorded: the blit ends the image) when there is no
// text or the swapchain images cannot be drawn to.
namespace {   // declared beside record_blit
bool record_overlay_sc(uint32_t index) {
    Overlay &v = s.ov;
    v.executed = 0;
    if (!v.n || !v.sc_color || index >= s.sc_images.size()) return false;
    if (v.sc_fbs.size() != s.sc_images.size()) {   // first frame on this swapchain
        overlay_sc_free();
        if (!overlay_pass(v.sc, s.sc_format)) {
            v.sc_color = false;
            return false;
        }
        for (VkImage img : s.sc_images) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = img;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = s.sc_format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView iv = VK_NULL_HANDLE;
            if (VKC(vkCreateImageView, s.dev, &vi, nullptr, &iv) != VK_SUCCESS) iv = VK_NULL_HANDLE;
            v.sc_views.push_back(iv);
            v.sc_fbs.push_back(iv ? overlay_fb(v.sc.pass, iv, s.sc_extent.width, s.sc_extent.height) : VK_NULL_HANDLE);
        }
    }
    if (!v.sc_fbs[index]) return false;
    VkImage img = s.sc_images[index];
    barrier(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    overlay_draw(v.sc, v.sc_fbs[index], s.sc_extent.width, s.sc_extent.height);
    barrier(img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    return true;
}
}  // namespace

// ---- Gamma ramp (renderer_gamma_ramp, src/gamma_ramp.h) ----

static void gamma_free() {
    Gamma &g = s.gamma;
    if (g.pipe) vkDestroyPipeline(s.dev, g.pipe, nullptr);
    if (g.vs) vkDestroyShaderModule(s.dev, g.vs, nullptr);
    if (g.fs) vkDestroyShaderModule(s.dev, g.fs, nullptr);
    if (g.layout) vkDestroyPipelineLayout(s.dev, g.layout, nullptr);
    if (g.pool) vkDestroyDescriptorPool(s.dev, g.pool, nullptr);
    if (g.set_layout) vkDestroyDescriptorSetLayout(s.dev, g.set_layout, nullptr);
    if (g.overlay_fb) vkDestroyFramebuffer(s.dev, g.overlay_fb, nullptr);
    if (g.fb) vkDestroyFramebuffer(s.dev, g.fb, nullptr);
    if (g.pass) vkDestroyRenderPass(s.dev, g.pass, nullptr);
    free_buffer(&g.ubo);
    free_image(&g.out);
    g = Gamma();
}

// At the first table: the image, the pass (like the sharpening's: every pixel written, barriers
// around it), the table's buffer and the pipeline reading the finished frame.
static bool gamma_create() {
    Gamma &g = s.gamma;
    const uint32_t fw = (uint32_t)s.dispw, fh = (uint32_t)s.disph;
    if (make_image(&g.out, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, fw, fh))
        return false;
    if (make_buffer(&g.ubo, sizeof g.lut, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_B8G8R8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // every pixel is written
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;   // barriers around the pass
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    if (VKC(vkCreateRenderPass, s.dev, &rp, nullptr, &g.pass) != VK_SUCCESS) return false;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = g.pass;
    fi.attachmentCount = 1;
    fi.pAttachments = &g.out.view;
    fi.width = fw;
    fi.height = fh;
    fi.layers = 1;
    if (VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &g.fb) != VK_SUCCESS) return false;
    if (s.ov.ok && !(g.overlay_fb = overlay_fb(s.ov.rb.pass, g.out.view, fw, fh))) return false;
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                         {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2;
    li.pBindings = b;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &g.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &g.pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g.set_layout;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, &g.set) != VK_SUCCESS) return false;
    // texelFetch: the sampler's filter is not used.
    VkDescriptorImageInfo ii{s.clamp_sampler, pre_gamma_image().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo bi{g.ubo.buf, 0, sizeof g.lut};
    VkWriteDescriptorSet w[2]{};
    for (int i = 0; i < 2; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = g.set;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
    }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &ii;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[1].pBufferInfo = &bi;
    vkUpdateDescriptorSets(s.dev, 2, w, 0, nullptr);
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &g.set_layout;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &g.layout) != VK_SUCCESS) return false;
    g.vs = shader(glass_vert, sizeof glass_vert);
    g.fs = shader(gamma_frag, sizeof gamma_frag);
    if (!g.vs || !g.fs) return false;
    g.pipe = fullscreen_pipeline(g.vs, g.layout, g.fs, g.pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE);
    return g.pipe != VK_NULL_HANDLE;
}

// After the sharpening, the last pass on the frame (the console applies the ramp on scanout, after
// everything the title drew; the port's post effects are part of that picture): the finished
// frame (TRANSFER_SRC_OPTIMAL) is sampled through the table into the ramp's image, which ends in
// TRANSFER_SRC_OPTIMAL for the readback copy or the swapchain blit; the frame goes back to
// TRANSFER_SRC_OPTIMAL. Without a table nothing is recorded and output_image() is the frame.
static void record_gamma() {
    Gamma &g = s.gamma;
    g.active = g.ok && g.on;
    if (!g.active) return;
    if (g.dirty) {   // after the previous frames' reads of the table (one queue), before this one's
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        vkCmdUpdateBuffer(s.cmd, g.ubo.buf, 0, sizeof g.lut, g.lut);
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
        vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        g.dirty = false;
    }
    VkImage src = pre_gamma_image().img;
    const VkPipelineStageFlags written = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    barrier(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, written,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    // After the previous frame's readback copy, blit or frame counter on it (same queue).
    barrier(g.out.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g.pass;
    rb.framebuffer = g.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)s.dispw, (uint32_t)s.disph}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)s.dispw, (float)s.disph, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &vp);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &g.set, 0, nullptr);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    barrier(g.out.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    g.executed++;
}

extern "C" {
void renderer_gamma_ramp(const uint32_t *lut) {
    Gamma &g = s.gamma;
    if (!lut) {
        g.on = false;
        return;
    }
    if (!s.dev || g.failed) return;
    if (!g.ok) {
        if (!gamma_create()) {
            gamma_free();
            g.failed = true;
            return;
        }
        g.ok = true;
    }
    if (!g.on || memcmp(g.lut, lut, sizeof g.lut)) {
        memcpy(g.lut, lut, sizeof g.lut);
        g.dirty = true;
    }
    g.on = true;
}
int renderer_gamma_ramp_on(void) { return s.dev && s.gamma.ok && s.gamma.on; }
uint32_t renderer_gamma_ramp_count(void) { return s.gamma.executed; }
}

// ---- Bloom (RendererConfig.bloom, renderer_bloom) ----

// Push constants of the bloom shaders (shaders/bloom_down.frag, bloom_up.frag).
struct BloomPush {
    float texel[2], inv_size[2];
    float threshold, knee, boost, last;   // last: prefilter (downsample) or scale (upsample)
    float hdr;                            // downsample: Bloom::hdr (--filmic)
};
const float BLOOM_BOOST = 0.3f;   // light mask: up to 1 + BLOOM_BOOST times the glow

// A level render pass: one color attachment of the level format, kept in COLOR_ATTACHMENT_OPTIMAL
// (barriers around each pass); load = false discards (every texel is written), true keeps the
// level for the additive upsample.
static bool bloom_pass(VkFormat fmt, bool load, VkRenderPass *out) {
    VkAttachmentDescription att{};
    att.format = fmt;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    return VKC(vkCreateRenderPass, s.dev, &rp, nullptr, out) == VK_SUCCESS;
}

// Levels: level 0 at half the internal size, then halved, BLOOM_LEVELS of them (down to 1/16 of the
// size: the glow hugs the light and spreads over the same share of the screen at any resolution;
// stopping at about 12 pixels high gave a radius of a sixth of the screen; with --filmic
// BLOOM_LEVELS_FILMIC, down to 1/64, for the wide soft glow of film), fewer when a level would be
// under 2 pixels high.
// RGBA16F (required by Vulkan 1.0 as a blendable, linearly filtered color attachment), B8G8R8A8 if
// a device lacks it.
static bool bloom_create() {
    Bloom &b = s.bloom;
    if (!split_passes_ready()) return false;
    VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(s.phys, fmt, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((fp.optimalTilingFeatures & need) != need) fmt = VK_FORMAT_B8G8R8A8_UNORM;
    int w = s.ow / 2 > 1 ? s.ow / 2 : 1, h = s.oh / 2 > 1 ? s.oh / 2 : 1;
    b.levels = 1;
    const int want = cfg.filmic ? BLOOM_LEVELS_FILMIC : BLOOM_LEVELS;
    while (b.levels < want && (h >> b.levels) >= 2) b.levels++;
    if (b.levels < 2) b.levels = 2;
    if (!bloom_pass(fmt, false, &b.down_pass) || !bloom_pass(fmt, true, &b.up_pass)) return false;
    for (int i = 0; i < b.levels; i++, w = w / 2 > 1 ? w / 2 : 1, h = h / 2 > 1 ? h / 2 : 1) {
        b.w[i] = w;
        b.h[i] = h;
        if (make_image(&b.lv[i], fmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                       (uint32_t)w, (uint32_t)h))
            return false;
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = b.down_pass;   // compatible with up_pass (same attachment)
        fi.attachmentCount = 1;
        fi.pAttachments = &b.lv[i].view;
        fi.width = (uint32_t)w;
        fi.height = (uint32_t)h;
        fi.layers = 1;
        if (VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &b.fb[i]) != VK_SUCCESS) return false;
    }
    VkDescriptorSetLayoutBinding lb{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &lb;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &b.set_layout) != VK_SUCCESS) return false;
    const uint32_t nsets = (uint32_t)b.levels + 1;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nsets};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = nsets;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &b.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[MAX_BLOOM_LEVELS + 1];
    VkDescriptorSet sets[MAX_BLOOM_LEVELS + 1];
    for (uint32_t i = 0; i < nsets; i++) layouts[i] = b.set_layout;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = b.pool;
    ai.descriptorSetCount = nsets;
    ai.pSetLayouts = layouts;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, sets) != VK_SUCCESS) return false;
    VkDescriptorImageInfo ii[MAX_BLOOM_LEVELS + 1];
    VkWriteDescriptorSet wr[MAX_BLOOM_LEVELS + 1]{};
    for (uint32_t i = 0; i < nsets; i++) {
        // The scene (the resolved color target, bilinear) and each level.
        ii[i] = {s.clamp_sampler, i ? b.lv[i - 1].view : s.color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = sets[i];
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(s.dev, nsets, wr, 0, nullptr);
    b.scene_set = sets[0];
    for (int i = 0; i < b.levels; i++) b.lv_set[i] = sets[i + 1];
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BloomPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &b.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &b.layout) != VK_SUCCESS) return false;
    b.vs = shader(glass_vert, sizeof glass_vert);
    b.down_fs = shader(bloom_down_frag, sizeof bloom_down_frag);
    b.up_fs = shader(bloom_up_frag, sizeof bloom_up_frag);
    b.comp_fs = shader(bloom_comp_frag, sizeof bloom_comp_frag);
    if (!b.vs || !b.down_fs || !b.up_fs || !b.comp_fs) return false;
    b.down = fullscreen_pipeline(b.vs, b.layout, b.down_fs, b.down_pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE);
    b.up = fullscreen_pipeline(b.vs, b.layout, b.up_fs, b.up_pass, VK_SAMPLE_COUNT_1_BIT, FS_ADD);
    b.comp = fullscreen_pipeline(b.vs, b.layout, b.comp_fs, s.pass, s.samples, FS_SCREEN);
    return b.down && b.up && b.comp;
}

static void bloom_free() {
    Bloom &b = s.bloom;
    for (VkPipeline p : {b.down, b.up, b.comp})
        if (p) vkDestroyPipeline(s.dev, p, nullptr);
    for (VkShaderModule m : {b.vs, b.down_fs, b.up_fs, b.comp_fs})
        if (m) vkDestroyShaderModule(s.dev, m, nullptr);
    if (b.layout) vkDestroyPipelineLayout(s.dev, b.layout, nullptr);
    if (b.pool) vkDestroyDescriptorPool(s.dev, b.pool, nullptr);
    if (b.set_layout) vkDestroyDescriptorSetLayout(s.dev, b.set_layout, nullptr);
    for (int i = 0; i < MAX_BLOOM_LEVELS; i++) {
        if (b.fb[i]) vkDestroyFramebuffer(s.dev, b.fb[i], nullptr);
        free_image(&b.lv[i]);
    }
    if (b.down_pass) vkDestroyRenderPass(s.dev, b.down_pass, nullptr);
    if (b.up_pass) vkDestroyRenderPass(s.dev, b.up_pass, nullptr);
    b = Bloom();
}

// At renderer_init: the bloom resources when RendererConfig.bloom is set. A failure frees them and
// leaves bloom off (logged by the caller through renderer_bloom_on()).
static void bloom_init() {
    if (!cfg.bloom) return;
    Bloom &b = s.bloom;
    if (bloom_create()) {
        b.ok = true;
        b.strength = cfg.bloom_strength > 0 ? cfg.bloom_strength : 0;
        b.threshold = cfg.bloom_threshold < 0 ? 0 : cfg.bloom_threshold > 1 ? 1 : cfg.bloom_threshold;
        b.hdr = cfg.filmic && cfg.filmic->hdr > 0 ? cfg.filmic->hdr : 0;
        return;
    }
    bloom_free();
}

// One level pass: src (set) sampled into level i, discarding it (down) or adding to it (up).
static void bloom_level(int i, VkDescriptorSet set, int sw, int sh, bool up, float last) {
    Bloom &b = s.bloom;
    // After the previous frame's reads of the level (one queue) and, for up, this frame's
    // downsample write and read of it.
    barrier(b.lv[i].img, up ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = up ? b.up_pass : b.down_pass;
    rb.framebuffer = b.fb[i];
    rb.renderArea = {{0, 0}, {(uint32_t)b.w[i], (uint32_t)b.h[i]}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, up ? b.up : b.down);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, b.layout, 0, 1, &set, 0, nullptr);
    BloomPush pc = {{1.0f / sw, 1.0f / sh}, {1.0f / b.w[i], 1.0f / b.h[i]}, b.threshold, 0.5f * b.threshold, BLOOM_BOOST, last, b.hdr};
    vkCmdPushConstants(s.cmd, b.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport v{0, 0, (float)b.w[i], (float)b.h[i], 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    barrier(b.lv[i].img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

// Executes the bloom split inside the frame's render pass (begun with s.split_pass or
// s.resume_pass): ends it (stored, with MSAA resolved into s.color), downsamples s.color through
// the levels (the first with the bright pass), adds them back up from the smallest, resumes the
// pass (s.resume_pass) and adds level 0 over the whole target (one bilinear tap per pixel,
// shaders/bloom_comp.frag), times strength / levels.
static void bloom_exec() {
    Bloom &b = s.bloom;
    vkCmdEndRenderPass(s.cmd);
    // The pass's outgoing dependency covers its attachment writes for the fragment shader reads.
    barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    for (int i = 0; i < b.levels; i++)
        bloom_level(i, i ? b.lv_set[i - 1] : b.scene_set, i ? b.w[i - 1] : s.ow, i ? b.h[i - 1] : s.oh, false, i ? 0.0f : 1.0f);
    for (int i = b.levels - 2; i >= 0; i--) bloom_level(i, b.lv_set[i + 1], b.w[i + 1], b.h[i + 1], true, 1.0f);
    barrier(s.color.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = resume_pass();
    rb.framebuffer = s.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, b.comp);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, b.layout, 0, 1, &b.lv_set[0], 0, nullptr);
    BloomPush pc = {{1.0f / b.w[0], 1.0f / b.h[0]}, {1.0f / s.ow, 1.0f / s.oh}, 0, 0, 0, b.strength / b.levels, 0};
    vkCmdPushConstants(s.cmd, b.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport v{0, 0, (float)s.ow, (float)s.oh, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    b.executed++;
}

// ---- Filmic look (RendererConfig.filmic, renderer_filmic) ----

// Push constants of shaders/filmic.frag.
struct FilmicPush {
    float a[4];   // 1 / target size (2), exposure, white
    float b[4];   // blend, hdr, lut_mix, halation
    float c[4];   // grain, vignette, frame seed, LUT size
    float d[4];   // norm, target aspect, 0, 0
    float e[4];   // --auto-exposure (the AUTO_EXPOSURE variant only): roll-off knee, dither, 0, 0
};
const uint32_t FILMIC_PUSH_PLAIN = offsetof(FilmicPush, e);   // the push block without auto exposure
// Push constants of shaders/ae_adapt.frag (ae_meter.frag reads the first vector: the grid size).
struct AdaptPush {
    float a[4];   // dt, snap, key, adapt
    float b[4];   // ev_min, ev_max, tau_up, tau_down
    float c[4];   // rate_up, 0, 0, 0
    float d[4];   // grid width, height, 0, 0
};
static_assert(MAX_FRAMES_IN_FLIGHT <= AE_SLOTS, "one auto-exposure readback per frame slot");

// ---- Depth of field (RendererConfig.dof, renderer_dof) ----

// Push constants of the shaders/dof_*.frag passes (dof_common.glsl).
struct DofPush {
    float proj[4];    // depth_a, depth_b, weight, dt
    float size[4];    // internal width, height, 1 / width, 1 / height
    float coc[4];     // coc x height (pixels), near_max, focus_ref, focus_min_scale
    float focus[4];   // focus_min, pct, tau, snap
    float half_[4];   // half width, half height, the gather's largest radius (half pixels), char_margin
    float spk[4];     // the speaker's weight (0: none), his view distance, char_blur, char_cap
};
const float DOF_MAX_RADIUS = 16.0f;   // half pixels: the gather's disc at most (strength 2 at 1080p reaches it)

static void dof_free() {
    Dof &d = s.dof;
    for (VkPipeline p : {d.focus_pipe, d.down_pipe, d.gather_pipe, d.comp_pipe, d.copy_pipe})
        if (p) vkDestroyPipeline(s.dev, p, nullptr);
    for (VkShaderModule m : {d.vs, d.focus_fs, d.down_fs, d.gather_fs, d.comp_fs, d.copy_fs})
        if (m) vkDestroyShaderModule(s.dev, m, nullptr);
    if (d.layout) vkDestroyPipelineLayout(s.dev, d.layout, nullptr);
    if (d.pool) vkDestroyDescriptorPool(s.dev, d.pool, nullptr);
    if (d.set_layout) vkDestroyDescriptorSetLayout(s.dev, d.set_layout, nullptr);
    for (VkFramebuffer b : {d.pre_fb, d.bokeh_fb, d.out_fb, d.state_fb[0], d.state_fb[1]})
        if (b) vkDestroyFramebuffer(s.dev, b, nullptr);
    for (VkRenderPass r : {d.half_pass, d.state_pass, d.out_pass})
        if (r) vkDestroyRenderPass(s.dev, r, nullptr);
    for (Buffer &b : d.readback) free_buffer(&b);
    for (Image *im : {&d.pre, &d.bokeh, &d.out, &d.state[0], &d.state[1]}) free_image(im);
    d = Dof();
}

static bool fb_create(VkRenderPass pass, const Image &im, uint32_t w, uint32_t h, VkFramebuffer *out);

// The depth of field's resources (after the filmic src image: the passes read it). Needs the depth
// sampled (create_targets: cfg.dof asks for it) and RGBA16F / RGBA32F colour attachments.
static bool dof_create() {
    Dof &d = s.dof;
    const VkFormat hfmt = VK_FORMAT_R16G16B16A16_SFLOAT, sfmt = VK_FORMAT_R32G32B32A32_SFLOAT, ofmt = VK_FORMAT_B8G8R8A8_UNORM;
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    for (VkFormat f : {hfmt, sfmt}) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(s.phys, f, &fp);
        if ((fp.optimalTilingFeatures & need) != need) return false;
    }
    if (!s.depth_sampled || !s.filmic.src.view || !s.chars.ok) return false;   // characters stay sharp: it reads their mask
    d.p = *cfg.dof;
    d.hw = (s.ow + 1) / 2;
    d.hh = (s.oh + 1) / 2;
    const VkImageUsageFlags use = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (make_image(&d.pre, hfmt, use, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)d.hw, (uint32_t)d.hh) ||
        make_image(&d.bokeh, hfmt, use, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)d.hw, (uint32_t)d.hh) ||
        make_image(&d.out, ofmt, use, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)s.ow, (uint32_t)s.oh))
        return false;
    for (Image &im : d.state)
        if (make_image(&im, sfmt, use | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1))
            return false;
    if (!bloom_pass(hfmt, false, &d.half_pass) || !bloom_pass(sfmt, false, &d.state_pass) || !bloom_pass(ofmt, false, &d.out_pass)) return false;
    if (!fb_create(d.half_pass, d.pre, (uint32_t)d.hw, (uint32_t)d.hh, &d.pre_fb) ||
        !fb_create(d.half_pass, d.bokeh, (uint32_t)d.hw, (uint32_t)d.hh, &d.bokeh_fb) ||
        !fb_create(d.out_pass, d.out, (uint32_t)s.ow, (uint32_t)s.oh, &d.out_fb) || !fb_create(d.state_pass, d.state[0], 1, 1, &d.state_fb[0]) ||
        !fb_create(d.state_pass, d.state[1], 1, 1, &d.state_fb[1]))
        return false;
    // Every image starts readable: the state cleared to zero (the first frame snaps the focus), the
    // others in the layout the sets name until a pass writes them.
    VkCommandBuffer saved;
    begin_once(&saved);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (Image &im : d.state) {
        barrier(im.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdClearColorImage(s.cmd, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        barrier(im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    if (submit_once(saved)) return false;
    for (Buffer &b : d.readback)
        if (make_buffer(&b, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    const uint32_t NB = 6;
    VkDescriptorSetLayoutBinding lb[NB];
    for (uint32_t i = 0; i < NB; i++) lb[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = NB;
    li.pBindings = lb;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &d.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5 * NB};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 5;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &d.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[5] = {d.set_layout, d.set_layout, d.set_layout, d.set_layout, d.set_layout};
    VkDescriptorSet sets[5];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = d.pool;
    ai.descriptorSetCount = 5;
    ai.pSetLayouts = layouts;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, sets) != VK_SUCCESS) return false;
    d.focus_set[0] = sets[0];
    d.focus_set[1] = sets[1];
    d.set[0] = sets[2];
    d.set[1] = sets[3];
    d.copy_set = sets[4];
    VkDescriptorImageInfo ii[5 * NB];
    VkWriteDescriptorSet wr[5 * NB]{};
    for (uint32_t k = 0, n = 0; k < 5; k++)
        for (uint32_t b = 0; b < NB; b++, n++) {
            // The state image: focus_set[i] reads the other one, set[i] its own (copy_set: unused).
            const VkImageView st = k < 2 ? d.state[1 - k].view : d.state[k == 4 ? 0 : k - 2].view;
            const VkImageView v = b == 0   ? s.depth.view
                                  : b == 1 ? (k == 4 ? d.out.view : s.filmic.src.view)
                                  : b == 2 ? d.pre.view
                                  : b == 3 ? d.bokeh.view
                                  : b == 4 ? st
                                           : chars_post_view();
            ii[n] = {b == 1 ? s.clamp_sampler : s.nearest_sampler, v,
                     b == 5 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[n].dstSet = sets[k];
            wr[n].dstBinding = b;
            wr[n].descriptorCount = 1;
            wr[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[n].pImageInfo = &ii[n];
        }
    vkUpdateDescriptorSets(s.dev, 5 * NB, wr, 0, nullptr);
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DofPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &d.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &d.layout) != VK_SUCCESS) return false;
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    d.vs = shader(glass_vert, sizeof glass_vert);
    d.focus_fs = ms ? shader(dof_focus_ms_frag, sizeof dof_focus_ms_frag) : shader(dof_focus_frag, sizeof dof_focus_frag);
    d.down_fs = ms ? shader(dof_down_ms_frag, sizeof dof_down_ms_frag) : shader(dof_down_frag, sizeof dof_down_frag);
    d.gather_fs = shader(dof_gather_frag, sizeof dof_gather_frag);
    d.comp_fs = ms ? shader(dof_comp_ms_frag, sizeof dof_comp_ms_frag) : shader(dof_comp_frag, sizeof dof_comp_frag);
    d.copy_fs = shader(dof_copy_frag, sizeof dof_copy_frag);
    if (!d.vs || !d.focus_fs || !d.down_fs || !d.gather_fs || !d.comp_fs || !d.copy_fs) return false;
    d.focus_pipe = fullscreen_pipeline(d.vs, d.layout, d.focus_fs, d.state_pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE);
    d.down_pipe = fullscreen_pipeline(d.vs, d.layout, d.down_fs, d.half_pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE);
    d.gather_pipe = fullscreen_pipeline(d.vs, d.layout, d.gather_fs, d.half_pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE);
    d.comp_pipe = fullscreen_pipeline(d.vs, d.layout, d.comp_fs, d.out_pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE);
    d.copy_pipe = fullscreen_pipeline(d.vs, d.layout, d.copy_fs, s.pass, s.samples, FS_REPLACE);
    return d.focus_pipe && d.down_pipe && d.gather_pipe && d.comp_pipe && d.copy_pipe;
}

// --stats: the newest focus state of a frame whose fence has been waited (as ae_collect).
static void dof_collect() {
    Dof &d = s.dof;
    int best = -1;
    for (int k = 0; k < s.nframes && k < AE_SLOTS; k++)
        if (d.rb_seq[k] > d.read_seq && !s.frames[k].pending && (best < 0 || d.rb_seq[k] > d.rb_seq[best])) best = k;
    if (best < 0) return;
    memcpy(d.last, d.readback[best].map, sizeof d.last);
    d.read_seq = d.rb_seq[best];
    d.have = true;
}

// One full-target triangle into a w x h single-image pass (the image readable before and after).
static void dof_pass(Image &im, VkRenderPass pass, VkFramebuffer fb, int w, int h, VkPipeline pipe, VkDescriptorSet set, const DofPush &pc) {
    Dof &d = s.dof;
    barrier(im.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = pass;
    rb.framebuffer = fb;
    rb.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(s.cmd, d.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport v{0, 0, (float)w, (float)h, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    barrier(im.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

// Inside filmic_exec outside any render pass, after src is filled (the frame's pass ended: its
// depth stored): the focus into state[cur] (copied to this slot's readback buffer), the prefilter,
// the gather and the composite into out; the depth goes back to an attachment for the resumed pass.
static void dof_exec() {
    Dof &d = s.dof;
    const RDofFrame &fr = d.frame;
    const RDof &p = d.p;
    dof_collect();   // this slot's previous frame among them: its fence was waited
    barrier(s.depth.img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 1,
            VK_IMAGE_ASPECT_DEPTH_BIT);
    const float k = p.coc * (float)s.oh, w = fr.weight < 1 ? fr.weight : 1.0f, dt = fr.dt > 0 ? (fr.dt < 0.1f ? fr.dt : 0.1f) : 0.0f;
    const float rmax = 0.5f * k * p.near_max;
    const DofPush pc = {{fr.depth_a, fr.depth_b, w, dt},
                        {(float)s.ow, (float)s.oh, 1.0f / s.ow, 1.0f / s.oh},
                        {k, p.near_max, p.focus_ref, p.focus_min_scale},
                        {p.focus_min, p.pct, p.tau, fr.snap ? 1.0f : 0.0f},
                        {(float)d.hw, (float)d.hh, rmax < DOF_MAX_RADIUS ? rmax : DOF_MAX_RADIUS, p.char_margin},
                        {fr.speaker > 0 && fr.speaker_dist > 0 ? (fr.speaker < 1 ? fr.speaker : 1.0f) : 0.0f, fr.speaker_dist, p.char_blur,
                         p.char_cap}};
    const int i = d.cur;
    dof_pass(d.state[i], d.state_pass, d.state_fb[i], 1, 1, d.focus_pipe, d.focus_set[i], pc);
    barrier(d.state[i].img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {1, 1, 1};
    vkCmdCopyImageToBuffer(s.cmd, d.state[i].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.readback[s.slot].buf, 1, &region);
    barrier(d.state[i].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};   // the copy -> the host read after the fence
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    d.rb_seq[s.slot] = ++d.seq;
    dof_pass(d.pre, d.half_pass, d.pre_fb, d.hw, d.hh, d.down_pipe, d.set[i], pc);
    dof_pass(d.bokeh, d.half_pass, d.bokeh_fb, d.hw, d.hh, d.gather_pipe, d.set[i], pc);
    dof_pass(d.out, d.out_pass, d.out_fb, s.ow, s.oh, d.comp_pipe, d.set[i], pc);
    barrier(s.depth.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
    d.cur = 1 - i;
    d.executed++;
}

static void filmic_free() {
    dof_free();
    Filmic &f = s.filmic;
    for (VkPipeline p : {f.pipe, f.meter_pipe, f.adapt_pipe})
        if (p) vkDestroyPipeline(s.dev, p, nullptr);
    for (VkShaderModule m : {f.vs, f.fs, f.meter_fs, f.adapt_fs})
        if (m) vkDestroyShaderModule(s.dev, m, nullptr);
    for (VkPipelineLayout l : {f.layout, f.ae_layout})
        if (l) vkDestroyPipelineLayout(s.dev, l, nullptr);
    for (VkDescriptorPool d : {f.pool, f.ae_pool})
        if (d) vkDestroyDescriptorPool(s.dev, d, nullptr);
    for (VkDescriptorSetLayout l : {f.set_layout, f.ae_set_layout})
        if (l) vkDestroyDescriptorSetLayout(s.dev, l, nullptr);
    for (VkFramebuffer b : {f.meter_fb, f.state_fb[0], f.state_fb[1]})
        if (b) vkDestroyFramebuffer(s.dev, b, nullptr);
    for (VkRenderPass r : {f.meter_pass, f.state_pass})
        if (r) vkDestroyRenderPass(s.dev, r, nullptr);
    for (Buffer &b : f.readback) free_buffer(&b);
    free_image(&f.src);
    free_image(&f.lut);
    free_image(&f.meter);
    free_image(&f.state[0]);
    free_image(&f.state[1]);
    f = Filmic();
}

// The n^3 RGBA8 LUT as a 3D image (linearly filtered), uploaded through a one-time submission.
static bool filmic_lut(const uint8_t *data, int n) {
    Filmic &f = s.filmic;
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;   // sampled with linear filtering: required by Vulkan 1.0
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_3D;
    ii.format = fmt;
    ii.extent = {(uint32_t)n, (uint32_t)n, (uint32_t)n};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (VKC(vkCreateImage, s.dev, &ii, nullptr, &f.lut.img) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(s.dev, f.lut.img, &req);
    int type = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = find_memory(req.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)type;
    if (type < 0 || VKC(vkAllocateMemory, s.dev, &ai, nullptr, &f.lut.mem) != VK_SUCCESS) return false;
    vram_add(type, req.size, false, &f.lut.vram, &f.lut.cat);
    VKC(vkBindImageMemory, s.dev, f.lut.img, f.lut.mem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = f.lut.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (VKC(vkCreateImageView, s.dev, &vi, nullptr, &f.lut.view) != VK_SUCCESS) return false;
    const VkDeviceSize bytes = (VkDeviceSize)n * n * n * 4;
    Buffer staging;
    if (make_buffer(&staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
        free_buffer(&staging);
        return false;
    }
    memcpy(staging.map, data, (size_t)bytes);
    VkCommandBuffer saved;
    begin_once(&saved);
    barrier(f.lut.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = ii.extent;
    vkCmdCopyBufferToImage(s.cmd, staging.buf, f.lut.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(f.lut.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    const bool ok = !submit_once(saved);
    free_buffer(&staging);
    return ok;
}

static bool fb_create(VkRenderPass pass, const Image &im, uint32_t w, uint32_t h, VkFramebuffer *out) {
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = pass;
    fi.attachmentCount = 1;
    fi.pAttachments = &im.view;
    fi.width = w;
    fi.height = h;
    fi.layers = 1;
    return VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, out) == VK_SUCCESS;
}

// --auto-exposure: the meter grid, the two state images (cleared to zero: the first adaptation
// snaps), their passes, sets and pipelines, and the readback buffers. After src and f.vs.
static bool ae_create() {
    Filmic &f = s.filmic;
    const VkFormat mfmt = VK_FORMAT_R16G16B16A16_SFLOAT, sfmt = VK_FORMAT_R32G32B32A32_SFLOAT;   // color attachments required by Vulkan 1.0
    const VkImageUsageFlags use = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (make_image(&f.meter, mfmt, use, VK_IMAGE_ASPECT_COLOR_BIT, AE_GRID_W, AE_GRID_H)) return false;
    for (Image &im : f.state)
        if (make_image(&im, sfmt, use | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1))
            return false;
    if (!bloom_pass(mfmt, false, &f.meter_pass) || !bloom_pass(sfmt, false, &f.state_pass)) return false;
    if (!fb_create(f.meter_pass, f.meter, AE_GRID_W, AE_GRID_H, &f.meter_fb) || !fb_create(f.state_pass, f.state[0], 1, 1, &f.state_fb[0]) ||
        !fb_create(f.state_pass, f.state[1], 1, 1, &f.state_fb[1]))
        return false;
    VkCommandBuffer saved;
    begin_once(&saved);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (Image &im : f.state) {
        barrier(im.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdClearColorImage(s.cmd, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        barrier(im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    if (submit_once(saved)) return false;
    for (Buffer &b : f.readback)
        if (make_buffer(&b, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    VkDescriptorSetLayoutBinding lb[2];
    for (uint32_t i = 0; i < 2; i++) lb[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2;
    li.pBindings = lb;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &f.ae_set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 3;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &f.ae_pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[3] = {f.ae_set_layout, f.ae_set_layout, f.ae_set_layout};
    VkDescriptorSet sets[3];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = f.ae_pool;
    ai.descriptorSetCount = 3;
    ai.pSetLayouts = layouts;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, sets) != VK_SUCCESS) return false;
    f.meter_set = sets[0];
    f.adapt_set[0] = sets[1];
    f.adapt_set[1] = sets[2];
    // meter: (src bilinear, src); adapt_set[i] writes state[i]: (the grid, state[1 - i]).
    VkDescriptorImageInfo ii[6] = {
        {s.clamp_sampler, f.src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {s.clamp_sampler, f.src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {s.nearest_sampler, f.meter.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {s.nearest_sampler, f.state[1].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {s.nearest_sampler, f.meter.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {s.nearest_sampler, f.state[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet wr[6]{};
    for (int i = 0; i < 6; i++) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = sets[i / 2];
        wr[i].dstBinding = (uint32_t)(i % 2);
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(s.dev, 6, wr, 0, nullptr);
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(AdaptPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &f.ae_set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &f.ae_layout) != VK_SUCCESS) return false;
    f.meter_fs = shader(ae_meter_frag, sizeof ae_meter_frag);
    f.adapt_fs = shader(ae_adapt_frag, sizeof ae_adapt_frag);
    if (!f.meter_fs || !f.adapt_fs) return false;
    f.meter_pipe = fullscreen_pipeline(f.vs, f.ae_layout, f.meter_fs, f.meter_pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE);
    f.adapt_pipe = fullscreen_pipeline(f.vs, f.ae_layout, f.adapt_fs, f.state_pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE);
    return f.meter_pipe && f.adapt_pipe;
}

// The filmic look (cfg.filmic) and/or the auto exposure (with_ae: cfg.auto_exposure); without the
// look the pass is exposure.frag with the identity parameters. with_dof: the depth of field too
// (cfg.dof); without the look and the auto exposure only the scene copy and the depth of field.
static bool filmic_create(bool with_ae, bool with_dof) {
    Filmic &f = s.filmic;
    if (!cfg.filmic && !with_ae) {   // the depth of field alone
        if (!with_dof || !split_passes_ready()) return false;
        f.dof_only = true;
        if (make_image(&f.src, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                       (uint32_t)s.ow, (uint32_t)s.oh))
            return false;
        return dof_create();
    }
    static const uint8_t ident[2 * 2 * 2 * 4] = {0, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 255, 255, 0, 255,
                                                 0, 0, 255, 255, 255, 0, 255, 255, 0, 255, 255, 255, 255, 255, 255, 255};
    RFilmic none{};
    none.exposure = none.white = none.norm = 1;
    none.lut = ident;
    none.lut_size = 2;
    const RFilmic &p = cfg.filmic ? *cfg.filmic : none;
    if (!p.lut || p.lut_size < 2 || p.lut_size > 64 || !split_passes_ready()) return false;
    f.ae = with_ae;
    f.exposure_only = !cfg.filmic;
    if (f.ae) f.ae_p = *cfg.auto_exposure;
    f.p = p;
    f.p.lut = nullptr;
    f.lut_size = p.lut_size;
    if (!filmic_lut(p.lut, p.lut_size)) return false;
    if (make_image(&f.src, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                   (uint32_t)s.ow, (uint32_t)s.oh))
        return false;
    f.vs = shader(glass_vert, sizeof glass_vert);
    if (!f.vs || (f.ae && !ae_create()) || (with_dof && !dof_create())) return false;
    // Bindings: 0 src (or the depth of field's result: the second half of the sets), 1 the LUT, 2
    // the halation glow, 3 (auto exposure) a state image.
    const uint32_t nb = f.ae ? 4 : 3, nbase = f.ae ? 4 : 2, nsets = nbase * (with_dof ? 2 : 1);
    VkDescriptorSetLayoutBinding lb[4];
    for (uint32_t i = 0; i < nb; i++) lb[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = nb;
    li.pBindings = lb;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &f.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nb * nsets};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = nsets;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &f.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[8];
    for (VkDescriptorSetLayout &l : layouts) l = f.set_layout;
    VkDescriptorSet sets[8];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = f.pool;
    ai.descriptorSetCount = nsets;
    ai.pSetLayouts = layouts;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, sets) != VK_SUCCESS) return false;
    // Set 2k: with the glow, 2k + 1: plain; k: the state image. The halation glow: bloom level 1 (sum
    // of the coarser levels after the upsample chain).
    f.has_glow = !f.exposure_only && s.bloom.ok && s.bloom.levels > 1;
    VkDescriptorImageInfo ii[32];
    VkWriteDescriptorSet wr[32]{};
    uint32_t n = 0;
    for (uint32_t k = 0; k < nsets; k++)
        for (uint32_t bind = 0; bind < nb; bind++, n++) {
            const bool glow_set = k % 2 == 0, dof_set = k >= nbase;
            const VkImageView v = bind == 0   ? (dof_set ? s.dof.out.view : f.src.view)
                                  : bind == 1 ? f.lut.view
                                  : bind == 2 ? (glow_set && f.has_glow ? s.bloom.lv[1].view : f.src.view)
                                              : f.state[(k % nbase) / 2].view;
            ii[n] = {bind == 3 ? s.nearest_sampler : s.clamp_sampler, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[n].dstSet = sets[k];
            wr[n].dstBinding = bind;
            wr[n].descriptorCount = 1;
            wr[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[n].pImageInfo = &ii[n];
        }
    vkUpdateDescriptorSets(s.dev, n, wr, 0, nullptr);
    for (uint32_t k = 0; k < nbase / 2; k++) {
        f.set_glow[k] = sets[2 * k];
        f.set_plain[k] = sets[2 * k + 1];
        if (with_dof) {
            f.set_glow_dof[k] = sets[nbase + 2 * k];
            f.set_plain_dof[k] = sets[nbase + 2 * k + 1];
        }
    }
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, f.ae ? (uint32_t)sizeof(FilmicPush) : FILMIC_PUSH_PLAIN};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &f.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &f.layout) != VK_SUCCESS) return false;
    f.fs = f.exposure_only ? shader(exposure_frag, sizeof exposure_frag)
           : f.ae          ? shader(filmic_ae_frag, sizeof filmic_ae_frag)
                           : shader(filmic_frag, sizeof filmic_frag);
    if (!f.fs) return false;
    f.pipe = fullscreen_pipeline(f.vs, f.layout, f.fs, s.pass, s.samples, FS_REPLACE);
    return f.pipe != VK_NULL_HANDLE;
}

// At renderer_init: the filmic resources when RendererConfig.filmic or auto_exposure is set. A
// failure frees them and leaves the pass off (renderer_filmic_on()); with the look, a failure of
// the auto exposure retries the look alone (renderer_auto_exposure_on() 0).
// With the depth of field (cfg.dof), a failure of it retries without it first.
static void filmic_init() {
    if (!cfg.filmic && !cfg.auto_exposure && !cfg.dof) return;
    const bool ae = cfg.auto_exposure != nullptr, dof = cfg.dof != nullptr;
    const bool tries[3][2] = {{ae, dof}, {ae, false}, {false, false}};
    for (int t = 0; t < 3; t++) {
        if (t && (tries[t][0] == tries[t - 1][0] && tries[t][1] == tries[t - 1][1])) continue;
        if (t == 2 && !cfg.filmic) break;
        if (filmic_create(tries[t][0], tries[t][1])) {
            s.filmic.ok = true;
            s.dof.ok = tries[t][1];
            return;
        }
        filmic_free();
    }
}

// --auto-exposure: the newest readback of a frame whose fence has been waited (outside recording:
// a slot's buffer is written by the frame recorded in it, read once that frame completed).
static void ae_collect() {
    Filmic &f = s.filmic;
    int best = -1;
    for (int k = 0; k < s.nframes && k < AE_SLOTS; k++)
        if (f.rb_seq[k] > f.ae_read_seq && !s.frames[k].pending && (best < 0 || f.rb_seq[k] > f.rb_seq[best])) best = k;
    if (best < 0) return;
    memcpy(f.ae_last, f.readback[best].map, sizeof f.ae_last);
    f.ae_read_seq = f.rb_seq[best];
    f.ae_have = true;
}

// --auto-exposure, inside filmic_exec outside any render pass, after src is filled: the meter grid,
// then the adaptation into state[ae_cur] (dt from the host clock, at most 0.1 s; a snap on the first
// one, after a gap of more than 0.5 s without one: a level load, a movie, the menus, and after
// renderer_auto_exposure_snap: a camera cut), copied to
// this slot's readback buffer. Returns the state image the frame's pass reads.
static int ae_exec() {
    Filmic &f = s.filmic;
    ae_collect();   // this slot's previous frame among them: its fence was waited
    const double now = now_ms();
    const double gap = f.ae_t < 0 ? 1e9 : (now - f.ae_t) / 1000;
    f.ae_t = now;
    const RAutoExposure &a = f.ae_p;
    const bool snap = gap > 0.5 || f.ae_snap;
    f.ae_snap = false;
    const AdaptPush pc = {{(float)(gap < 0.1 ? gap : 0.1), snap ? 1.0f : 0.0f, a.key, a.adapt},
                          {a.ev_min, a.ev_max, a.tau_up, a.tau_down},
                          {a.rate_up, 0, 0, 0},
                          {(float)AE_GRID_W, (float)AE_GRID_H, 0, 0}};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    barrier(f.meter.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    rb.renderPass = f.meter_pass;
    rb.framebuffer = f.meter_fb;
    rb.renderArea = {{0, 0}, {(uint32_t)AE_GRID_W, (uint32_t)AE_GRID_H}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, f.meter_pipe);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, f.ae_layout, 0, 1, &f.meter_set, 0, nullptr);
    vkCmdPushConstants(s.cmd, f.ae_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport v{0, 0, (float)AE_GRID_W, (float)AE_GRID_H, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    barrier(f.meter.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    const int i = f.ae_cur;
    barrier(f.state[i].img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    rb.renderPass = f.state_pass;
    rb.framebuffer = f.state_fb[i];
    rb.renderArea = {{0, 0}, {1, 1}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, f.adapt_pipe);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, f.ae_layout, 0, 1, &f.adapt_set[i], 0, nullptr);
    vkCmdPushConstants(s.cmd, f.ae_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    v = {0, 0, 1, 1, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    barrier(f.state[i].img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {1, 1, 1};
    vkCmdCopyImageToBuffer(s.cmd, f.state[i].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f.readback[s.slot].buf, 1, &region);
    barrier(f.state[i].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};   // the copy -> the host read after the fence
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    f.rb_seq[s.slot] = ++f.ae_seq;
    f.ae_cur = 1 - i;
    return i;
}

// Executes the filmic pass inside the frame's render pass (begun with s.split_pass or
// s.resume_pass): ends it (the color resolved into s.color), copies the colour into src, resumes
// the pass and replaces every pixel of the target with the graded scene (alpha, the light mask,
// kept). The halation reads bloom level 1 when this frame ran the bloom split.
static void filmic_exec() {
    Filmic &f = s.filmic;
    vkCmdEndRenderPass(s.cmd);
    barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(f.src.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy cp{};
    cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.dstSubresource = cp.srcSubresource;
    cp.extent = {(uint32_t)s.ow, (uint32_t)s.oh, 1};
    vkCmdCopyImage(s.cmd, s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f.src.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    barrier(f.src.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    const int st = f.ae ? ae_exec() : 0;
    const bool dof = s.dof.ok && s.dof.want;
    if (dof) dof_exec();
    barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = resume_pass();
    rb.framebuffer = s.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport v{0, 0, (float)s.ow, (float)s.oh, 0, 1};
    if (f.dof_only) {   // the depth of field's result drawn into the frame as is
        Dof &d = s.dof;
        vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.copy_pipe);
        vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, 0, 1, &d.copy_set, 0, nullptr);
        vkCmdSetViewport(s.cmd, 0, 1, &v);
        vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
        vkCmdDraw(s.cmd, 3, 1, 0, 0);
        f.executed++;
        return;
    }
    const bool glow = s.bloom.executed > 0 && f.has_glow;
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, f.pipe);
    const VkDescriptorSet *set = dof ? (glow ? &f.set_glow_dof[st] : &f.set_plain_dof[st]) : glow ? &f.set_glow[st] : &f.set_plain[st];
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, f.layout, 0, 1, set, 0, nullptr);
    const RFilmic &p = f.p;
    const float seed = (float)(f.frame++ % 4096u), dither = f.ae && f.ae_p.dither ? 1.0f : 0.0f;
    if (f.exposure_only) {   // exposure.frag: knee, dither, seed
        const float a[4] = {f.ae_p.knee, dither, seed, 0};
        vkCmdPushConstants(s.cmd, f.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof a, a);
    } else {
        FilmicPush pc = {{1.0f / s.ow, 1.0f / s.oh, p.exposure, p.white},
                         {p.blend, p.hdr, p.lut_mix, glow ? p.halation : 0.0f},
                         {p.grain, p.vignette, seed, (float)f.lut_size},
                         {p.norm, (float)s.ow / s.oh, 0, 0},
                         {f.ae_p.knee, dither, 0, 0}};
        vkCmdPushConstants(s.cmd, f.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, f.ae ? (uint32_t)sizeof pc : FILMIC_PUSH_PLAIN, &pc);
    }
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    f.executed++;
}

// ---- Temporal anti-aliasing (RendererConfig.taa, renderer_taa) ----

static void taa_free() {
    Taa &t = s.taa;
    for (VkPipeline p : {t.resolve_pipe, t.out_pipe, t.up_pipe, t.up_out_pipe})
        if (p) vkDestroyPipeline(s.dev, p, nullptr);
    for (VkShaderModule m : {t.vs, t.resolve_fs, t.out_fs, t.up_fs, t.up_out_fs})
        if (m) vkDestroyShaderModule(s.dev, m, nullptr);
    for (VkFramebuffer b : t.up_fb)
        if (b) vkDestroyFramebuffer(s.dev, b, nullptr);
    if (t.up_pass) vkDestroyRenderPass(s.dev, t.up_pass, nullptr);
    free_image(&t.upz);
    if (t.layout) vkDestroyPipelineLayout(s.dev, t.layout, nullptr);
    if (t.pool) vkDestroyDescriptorPool(s.dev, t.pool, nullptr);
    if (t.set_layout) vkDestroyDescriptorSetLayout(s.dev, t.set_layout, nullptr);
    for (VkFramebuffer b : t.fb)
        if (b) vkDestroyFramebuffer(s.dev, b, nullptr);
    if (t.pass) vkDestroyRenderPass(s.dev, t.pass, nullptr);
    for (int i = 0; i < 2; i++) {
        free_image(&t.hist[i]);
        free_image(&t.dist[i]);
    }
    if (t.vel_pipe) vkDestroyPipeline(s.dev, t.vel_pipe, nullptr);
    for (VkShaderModule m : {t.vel_vs, t.vel_fs})
        if (m) vkDestroyShaderModule(s.dev, m, nullptr);
    if (t.vel_layout) vkDestroyPipelineLayout(s.dev, t.vel_layout, nullptr);
    if (t.vel_fb) vkDestroyFramebuffer(s.dev, t.vel_fb, nullptr);
    if (t.vel_pass) vkDestroyRenderPass(s.dev, t.vel_pass, nullptr);
    free_image(&t.vel_img);
    free_image(&t.vel_depth);
    t = Taa();
}

// The velocity pass's target (RGBA16F, cleared and sampled), depth, pass, framebuffer and pipeline
// (two vertex streams of RVertex positions: current, previous). Only with GPU skinning.
static bool taa_vel_create() {
    Taa &t = s.taa;
    if (!cfg.gpu_skin) return false;
    const uint32_t w = (uint32_t)s.ow, h = (uint32_t)s.oh;
    const VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    if (make_image(&t.vel_img, fmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, w, h) ||
        make_image(&t.vel_depth, s.depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, w, h))
        return false;
    VkAttachmentDescription att[2]{};
    att[0].format = fmt;
    att[1].format = s.depth_format;
    for (int k = 0; k < 2; k++) {
        att[k].samples = VK_SAMPLE_COUNT_1_BIT;
        att[k].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[k].storeOp = k ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        att[k].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[k].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[k].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[k].finalLayout = k ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    const VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;
    // In: the skinning output (compute writes) and the last resolve's reads of the target; out: the
    // resolve's read.
    VkSubpassDependency dep[2]{};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep[0].dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 2;
    rp.pAttachments = att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = dep;
    if (VKC(vkCreateRenderPass, s.dev, &rp, nullptr, &t.vel_pass) != VK_SUCCESS) return false;
    const VkImageView views[2] = {t.vel_img.view, t.vel_depth.view};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = t.vel_pass;
    fi.attachmentCount = 2;
    fi.pAttachments = views;
    fi.width = w;
    fi.height = h;
    fi.layers = 1;
    if (VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &t.vel_fb) != VK_SUCCESS) return false;
    VkCommandBuffer saved;   // cleared and sampled until the first velocity pass
    begin_once(&saved);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier(t.vel_img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdClearColorImage(s.cmd, t.vel_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    barrier(t.vel_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (submit_once(saved)) return false;
    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &t.vel_layout) != VK_SUCCESS) return false;
    t.vel_vs = shader(taa_vel_vert, sizeof taa_vel_vert);
    t.vel_fs = shader(taa_vel_frag, sizeof taa_vel_frag);
    if (!t.vel_vs || !t.vel_fs) return false;
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = t.vel_vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = t.vel_fs;
    st[1].pName = "main";
    const VkVertexInputBindingDescription vb[2] = {{0, sizeof(RVertex), VK_VERTEX_INPUT_RATE_VERTEX}, {1, sizeof(RVertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    const VkVertexInputAttributeDescription va[2] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 2;
    vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    rs.depthClampEnable = VK_FALSE;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cbs;
    gp.pDynamicState = &dy;
    gp.layout = t.vel_layout;
    gp.renderPass = t.vel_pass;
    return VKC(vkCreateGraphicsPipelines, s.dev, s.pcache, 1, &gp, nullptr, &t.vel_pipe) == VK_SUCCESS;
}

// Before the frame's pass (after skin_record): the frame's draws with a previous pose that the
// TAA op jitters (frame target, depth-tested, before taa_at) into the velocity target.
static void taa_vel_record(VkDeviceSize index_offset, float sx, float sy, size_t taa_at) {
    Taa &t = s.taa;
    Skin &k = s.skin;
    t.vel_live = false;
    t.vel_draws = 0;
    if (!t.vel_ok || t.vel.empty() || !k.set || taa_at >= s.ops.size()) return;
    SkinSlot &sl = k.slot[s.slot];
    VkClearValue clears[2]{};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = t.vel_pass;
    rb.framebuffer = t.vel_fb;
    rb.renderArea = {{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    rb.clearValueCount = 2;
    rb.pClearValues = clears;
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, t.vel_pipe);
    vkCmdBindIndexBuffer(s.cmd, cur().geometry.buf, index_offset, VK_INDEX_TYPE_UINT32);
    for (const Taa::VelDraw &d : t.vel) {
        if (d.op >= taa_at) continue;
        const Op &op = s.ops[d.op];
        VkViewport v{op.vp.x * sx, op.vp.y * sy, op.vp.w * sx, op.vp.h * sy, op.vp.minz, op.vp.maxz};
        int x0 = (int)v.x, y0 = (int)v.y, x1 = (int)(v.x + v.width), y1 = (int)(v.y + v.height);
        x0 = x0 < 0 ? 0 : x0;
        y0 = y0 < 0 ? 0 : y0;
        x1 = x1 > s.ow ? s.ow : x1;
        y1 = y1 > s.oh ? s.oh : y1;
        if (x1 <= x0 || y1 <= y0 || v.width <= 0 || v.height <= 0) continue;
        const VkRect2D sc{{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
        vkCmdSetViewport(s.cmd, 0, 1, &v);
        vkCmdSetScissor(s.cmd, 0, 1, &sc);
        const float pc[4] = {2 * t.jit[0] / v.width, 2 * t.jit[1] / v.height, 0, 0};
        vkCmdPushConstants(s.cmd, t.vel_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
        const VkBuffer bufs[2] = {sl.dev.buf, sl.dev.buf};
        const VkDeviceSize offs[2] = {sl.off[8], sl.off[8] + (VkDeviceSize)(d.prev - d.cur) * sizeof(RVertex)};
        vkCmdBindVertexBuffers(s.cmd, 0, 2, bufs, offs);
        vkCmdDrawIndexed(s.cmd, op.index_count, 1, op.first_index, (int32_t)d.cur, 0);
        t.vel_draws++;
    }
    vkCmdEndRenderPass(s.cmd);
    t.vel_live = true;
}

static bool taa_create() {
    Taa &t = s.taa;
    if (!s.depth_sampled || !split_passes_ready()) return false;
    MemTag tag(RENDERER_VRAM_EFFECTS);
    t.vel_ok = taa_vel_create();   // optional: without it the camera's motion only
    const VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT, dfmt = VK_FORMAT_R16_SFLOAT;   // colour attachments required by Vulkan 1.0 (the first linearly filtered)
    const uint32_t w = (uint32_t)s.ow, h = (uint32_t)s.oh;
    const VkImageUsageFlags use = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (int i = 0; i < 2; i++)
        if (make_image(&t.hist[i], fmt, use, VK_IMAGE_ASPECT_COLOR_BIT, w, h) || make_image(&t.dist[i], dfmt, use, VK_IMAGE_ASPECT_COLOR_BIT, w, h))
            return false;
    {   // one subpass writing the colour and the distance, both stored
        VkAttachmentDescription att[2]{};
        for (int k = 0; k < 2; k++) {
            att[k].format = k ? dfmt : fmt;
            att[k].samples = VK_SAMPLE_COUNT_1_BIT;
            att[k].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att[k].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att[k].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            att[k].initialLayout = att[k].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        const VkAttachmentReference cref[2] = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 2;
        sub.pColorAttachments = cref;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 2;
        rp.pAttachments = att;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        if (VKC(vkCreateRenderPass, s.dev, &rp, nullptr, &t.pass) != VK_SUCCESS) return false;
    }
    for (int i = 0; i < 2; i++) {
        const VkImageView views[2] = {t.hist[i].view, t.dist[i].view};
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = t.pass;
        fi.attachmentCount = 2;
        fi.pAttachments = views;
        fi.width = w;
        fi.height = h;
        fi.layers = 1;
        if (VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &t.fb[i]) != VK_SUCCESS) return false;
    }
    const bool up = taa_up_wanted(&t.rw, &t.rh);
    const VkFormat zfmt = VK_FORMAT_R32_SFLOAT;   // --taa-upscale's output-size depth (a colour attachment Vulkan 1.0 requires)
    if (up) {
        if (make_image(&t.upz, zfmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, w, h))
            return false;
        VkAttachmentDescription att[3]{};   // the history, its distances, the depth: all stored
        for (int k = 0; k < 3; k++) {
            att[k].format = k == 0 ? fmt : k == 1 ? dfmt : zfmt;
            att[k].samples = VK_SAMPLE_COUNT_1_BIT;
            att[k].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att[k].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att[k].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            att[k].initialLayout = att[k].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        const VkAttachmentReference cref[3] = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                               {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                               {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 3;
        sub.pColorAttachments = cref;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 3;
        rp.pAttachments = att;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        if (VKC(vkCreateRenderPass, s.dev, &rp, nullptr, &t.up_pass) != VK_SUCCESS) return false;
        for (int i = 0; i < 2; i++) {
            const VkImageView views[3] = {t.hist[i].view, t.dist[i].view, t.upz.view};
            VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fi.renderPass = t.up_pass;
            fi.attachmentCount = 3;
            fi.pAttachments = views;
            fi.width = w;
            fi.height = h;
            fi.layers = 1;
            if (VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &t.up_fb[i]) != VK_SUCCESS) return false;
        }
    }
    VkCommandBuffer saved;   // every history cleared and sampled (the first resolve reads one, then resets)
    begin_once(&saved);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (Image *imp : {&t.hist[0], &t.hist[1], &t.dist[0], &t.dist[1], &t.upz}) {
        Image &im = *imp;
        if (!im.img) continue;
        barrier(im.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdClearColorImage(s.cmd, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        barrier(im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    if (submit_once(saved)) return false;
    VkDescriptorSetLayoutBinding lb[6];
    for (uint32_t i = 0; i < 6; i++) lb[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 6;
    li.pBindings = lb;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &t.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 24};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 4;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &t.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[4] = {t.set_layout, t.set_layout, t.set_layout, t.set_layout};
    VkDescriptorSet sets[4];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = t.pool;
    ai.descriptorSetCount = 4;
    ai.pSetLayouts = layouts;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, sets) != VK_SUCCESS) return false;
    // resolve_set[i] (writes hist[i]): the frame, the depth, hist[1 - i], dist[1 - i]; out_set[i]:
    // hist[i] and (--taa-upscale) the output-size depth (the other bindings valid but unread).
    VkDescriptorImageInfo ii[24];
    VkWriteDescriptorSet wr[24]{};
    for (uint32_t k = 0; k < 4; k++) {
        const int i = (int)(k % 2);
        const bool out = k >= 2;
        const VkImageView v0 = out ? t.hist[i].view : s.color.view;
        ii[k * 6 + 0] = {s.nearest_sampler, v0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        ii[k * 6 + 1] = {s.nearest_sampler, s.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        ii[k * 6 + 2] = {s.clamp_sampler, t.hist[1 - i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        ii[k * 6 + 3] = {s.nearest_sampler, t.dist[1 - i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        ii[k * 6 + 4] = {s.nearest_sampler, t.vel_ok ? t.vel_img.view : t.dist[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};   // unread without it
        ii[k * 6 + 5] = {s.nearest_sampler, up ? t.upz.view : t.dist[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};   // likewise
        for (uint32_t b = 0; b < 6; b++) {
            VkWriteDescriptorSet &x = wr[k * 6 + b];
            x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            x.dstSet = sets[k];
            x.dstBinding = b;
            x.descriptorCount = 1;
            x.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            x.pImageInfo = &ii[k * 6 + b];
        }
    }
    vkUpdateDescriptorSets(s.dev, 24, wr, 0, nullptr);
    t.resolve_set[0] = sets[0];
    t.resolve_set[1] = sets[1];
    t.out_set[0] = sets[2];
    t.out_set[1] = sets[3];
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(TaaPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &t.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &t.layout) != VK_SUCCESS) return false;
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    t.vs = shader(glass_vert, sizeof glass_vert);
    t.resolve_fs = ms ? shader(taa_resolve_ms_frag, sizeof taa_resolve_ms_frag) : shader(taa_resolve_frag, sizeof taa_resolve_frag);
    t.out_fs = shader(taa_out_frag, sizeof taa_out_frag);
    if (!t.vs || !t.resolve_fs || !t.out_fs) return false;
    t.resolve_pipe = fullscreen_pipeline(t.vs, t.layout, t.resolve_fs, t.pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE, false, nullptr, 2);
    t.out_pipe = fullscreen_pipeline(t.vs, t.layout, t.out_fs, s.pass, s.samples, FS_REPLACE);
    if (!t.resolve_pipe || !t.out_pipe) return false;
    if (up) {
        t.up_fs = ms ? shader(taa_up_ms_frag, sizeof taa_up_ms_frag) : shader(taa_up_frag, sizeof taa_up_frag);
        t.up_out_fs = shader(taa_out_up_frag, sizeof taa_out_up_frag);
        if (!t.up_fs || !t.up_out_fs) return false;
        t.up_pipe = fullscreen_pipeline(t.vs, t.layout, t.up_fs, t.up_pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE, false, nullptr, 3);
        t.up_out_pipe = fullscreen_pipeline(t.vs, t.layout, t.up_out_fs, s.pass, s.samples, FS_REPLACE, false, nullptr, 1, true);
        if (!t.up_pipe || !t.up_out_pipe) return false;
        t.up = true;
        t.phases = taa_jitter_phases(t.rw, (int)w);
        t.mw = t.rw;
        t.mh = t.rh;
        t.ow = (int)w;
    }
    return true;
}

// At renderer_init with RendererConfig.taa: a failure frees everything and leaves it off
// (renderer_taa_on() 0: frames as without it).
static void taa_init() {
    if (!cfg.taa) return;
    if (taa_create()) {
        Taa &t = s.taa;
        t.ok = true;
        t.sharpness = cfg.taa_sharpness < 0 ? 0 : cfg.taa_sharpness > TAA_SHARPNESS_MAX ? TAA_SHARPNESS_MAX : cfg.taa_sharpness;
        return;
    }
    taa_free();
}

// Executes the temporal resolve inside the frame's render pass (begun with s.split_pass or
// s.resume_pass): ends it (the colour resolved into s.color), resolves s.color and the depth with
// the previous history into hist[cur] (its own pass), resumes the frame's pass and draws hist[cur]
// over every pixel of the target (sharpened; alpha, the light mask, from the history).
static void taa_up_exec(float sx, float sy);
static void soft_split();
static void taa_exec(float sx, float sy) {
    Taa &t = s.taa;
    const RTaa &p = t.p;
    if (t.up) {
        taa_up_exec(sx, sy);
        return;
    }
    vkCmdEndRenderPass(s.cmd);
    barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    barrier(s.depth.img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 1,
            VK_IMAGE_ASPECT_DEPTH_BIT);
    const int i = t.cur;
    for (Image *im : {&t.hist[i], &t.dist[i]})
        barrier(im->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    const bool reset = p.reset || t.last_serial == 0 || t.last_serial + 1 != s.frame_serial;
    static const float ident[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    const float (*m)[4] = p.valid ? p.reproject : ident;
    const float vw = p.vp[2] > 0 ? p.vp[2] * sx : (float)s.ow, vh = p.vp[3] > 0 ? p.vp[3] * sy : (float)s.oh;
    TaaPush pc = {{m[0][0], m[0][1], m[0][2], m[0][3]},
                  {m[1][0], m[1][1], m[1][2], m[1][3]},
                  {m[2][0], m[2][1], m[2][2], m[2][3]},
                  {p.valid ? p.depth_a : 0.0f, p.valid ? p.depth_b : 0.0f, TAA_BLEND, reset ? 1.0f : 0.0f},
                  {(float)s.ow, (float)s.oh, 1.0f / s.ow, 1.0f / s.oh},
                  {p.vp[2] > 0 ? p.vp[0] * sx : 0.0f, p.vp[3] > 0 ? p.vp[1] * sy : 0.0f, 2.0f / vw, 2.0f / vh},
                  {t.vel_live ? 1.0f : 0.0f, t.jit[0], t.jit[1], 0}};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = t.pass;
    rb.framebuffer = t.fb[i];
    rb.renderArea = {{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    VkViewport v{0, 0, (float)s.ow, (float)s.oh, 0, 1};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, t.resolve_pipe);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, t.layout, 0, 1, &t.resolve_set[i], 0, nullptr);
    vkCmdPushConstants(s.cmd, t.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    for (Image *im : {&t.hist[i], &t.dist[i]})
        barrier(im->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    barrier(s.color.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    barrier(s.depth.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
    rb.renderPass = resume_pass();
    rb.framebuffer = s.fb;
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, t.out_pipe);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, t.layout, 0, 1, &t.out_set[i], 0, nullptr);
    pc.proj[2] = t.sharpness;
    vkCmdPushConstants(s.cmd, t.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    t.cur = 1 - i;
    t.last_serial = s.frame_serial;
    t.frame++;
    t.executed++;
}

// --taa-upscale: taa_exec with the frame's scene in its top-left rw x rh (record_frame has restored
// the output size): the upscale resolve writes hist[cur], its distances and the output-size depth
// (up_pass), then the frame's pass resumes with a writable depth (s.resume_pass) and the output pass
// draws hist[cur] over every pixel of the target and writes that depth; with soft particles live the
// pass is split again into their read-only-depth variant (soft_split).
static void taa_up_exec(float sx, float sy) {
    Taa &t = s.taa;
    const RTaa &p = t.p;
    vkCmdEndRenderPass(s.cmd);
    barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    barrier(s.depth.img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 1,
            VK_IMAGE_ASPECT_DEPTH_BIT);
    const int i = t.cur;
    for (Image *im : {&t.hist[i], &t.dist[i], &t.upz})
        barrier(im->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    const bool reset = p.reset || t.last_serial == 0 || t.last_serial + 1 != s.frame_serial;
    static const float ident[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    const float (*m)[4] = p.valid ? p.reproject : ident;
    const float vw = p.vp[2] > 0 ? p.vp[2] * sx : (float)s.ow, vh = p.vp[3] > 0 ? p.vp[3] * sy : (float)s.oh;
    TaaPush pc = {{m[0][0], m[0][1], m[0][2], m[0][3]},
                  {m[1][0], m[1][1], m[1][2], m[1][3]},
                  {m[2][0], m[2][1], m[2][2], m[2][3]},
                  {p.valid ? p.depth_a : 0.0f, p.valid ? p.depth_b : 0.0f, TAA_BLEND, reset ? 1.0f : 0.0f},
                  {(float)s.ow, (float)s.oh, 1.0f / s.ow, 1.0f / s.oh},
                  {p.vp[2] > 0 ? p.vp[0] * sx : 0.0f, p.vp[3] > 0 ? p.vp[1] * sy : 0.0f, 2.0f / vw, 2.0f / vh},
                  {t.vel_live ? 1.0f : 0.0f, t.jit[0], t.jit[1], 0},
                  {(float)t.rw, (float)t.rh, (float)t.rw / s.ow, (float)t.rh / s.oh}};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = t.up_pass;
    rb.framebuffer = t.up_fb[i];
    rb.renderArea = {{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    VkViewport v{0, 0, (float)s.ow, (float)s.oh, 0, 1};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, t.up_pipe);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, t.layout, 0, 1, &t.resolve_set[i], 0, nullptr);
    vkCmdPushConstants(s.cmd, t.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    for (Image *im : {&t.hist[i], &t.dist[i], &t.upz})
        barrier(im->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    barrier(s.color.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    barrier(s.depth.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
    s.soft.in_pass = false;   // the depth is written here: the plain resume variant
    rb.renderPass = s.resume_pass;
    rb.framebuffer = s.fb;
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, t.up_out_pipe);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, t.layout, 0, 1, &t.out_set[i], 0, nullptr);
    pc.proj[2] = t.sharpness;
    vkCmdPushConstants(s.cmd, t.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    if (s.soft.live) soft_split();   // the soft particles after it read the depth (now the output size's)
    t.cur = 1 - i;
    t.last_serial = s.frame_serial;
    t.frame++;
    t.executed++;
}

// ---- Frame generation (RendererConfig.frame_gen, renderer_frame_gen) ----

static void fg_free() {
    FrameGen &g = s.fg;
    for (VkPipeline p : {g.mv_pipe, g.splat_pipe, g.interp_pipe, g.flow_pipe, g.hud_pipe})
        if (p) vkDestroyPipeline(s.dev, p, nullptr);
    for (VkShaderModule m : {g.mv_cs, g.splat_cs, g.vs, g.interp_fs, g.flow_cs, g.hud_cs})
        if (m) vkDestroyShaderModule(s.dev, m, nullptr);
    for (VkPipelineLayout l : {g.mv_layout, g.splat_layout, g.interp_layout, g.flow_layout, g.hud_layout})
        if (l) vkDestroyPipelineLayout(s.dev, l, nullptr);
    for (VkDescriptorSetLayout l : {g.mv_set_layout, g.splat_set_layout, g.interp_set_layout, g.flow_set_layout})
        if (l) vkDestroyDescriptorSetLayout(s.dev, l, nullptr);
    if (g.pool) vkDestroyDescriptorPool(s.dev, g.pool, nullptr);
    if (g.fb) vkDestroyFramebuffer(s.dev, g.fb, nullptr);
    if (g.pass) vkDestroyRenderPass(s.dev, g.pass, nullptr);
    for (Image *im : {&g.hl_int, &g.hud_b, &g.hl[0], &g.hl[1], &g.mv[0], &g.mv[1], &g.dist, &g.index, &g.gen, &g.flow}) free_image(im);
    free_buffer(&g.rb);
    g = FrameGen();
}

static VkDescriptorSetLayout fg_set_layout(const VkDescriptorType *types, uint32_t n, VkShaderStageFlags stage) {
    VkDescriptorSetLayoutBinding b[8];
    for (uint32_t i = 0; i < n; i++) b[i] = {i, types[i], 1, stage, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = n;
    li.pBindings = b;
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    return VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &l) == VK_SUCCESS ? l : VK_NULL_HANDLE;
}
static VkPipelineLayout fg_pipe_layout(VkDescriptorSetLayout set, VkShaderStageFlags stage, uint32_t push) {
    VkPushConstantRange pc{stage, 0, push};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    VkPipelineLayout l = VK_NULL_HANDLE;
    return VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &l) == VK_SUCCESS ? l : VK_NULL_HANDLE;
}
static VkPipeline fg_compute(VkShaderModule cs, VkPipelineLayout layout) {
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = cs;
    ci.stage.pName = "main";
    ci.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    return VKC(vkCreateComputePipelines, s.dev, s.pcache, 1, &ci, nullptr, &p) == VK_SUCCESS ? p : VK_NULL_HANDLE;
}

// The images (every one cleared and in the layout its first use expects), the passes, sets and
// pipelines. Needs a sampled depth target, storage images of RGBA16F and R32_UINT with atomics.
static bool fg_create() {
    FrameGen &g = s.fg;
    if (!s.depth_sampled || !split_passes_ready()) return false;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(s.phys, VK_FORMAT_R32_UINT, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT)) return false;
    vkGetPhysicalDeviceFormatProperties(s.phys, VK_FORMAT_R16G16B16A16_SFLOAT, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return false;
    MemTag tag(RENDERER_VRAM_EFFECTS);
    const uint32_t dw = (uint32_t)s.dispw, dh = (uint32_t)s.disph, ow = (uint32_t)s.ow, oh = (uint32_t)s.oh;
    const VkFormat cf = VK_FORMAT_B8G8R8A8_UNORM;
    const VkImageUsageFlags copy = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (make_image(&g.hl_int, cf, copy | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, ow, oh) ||
        make_image(&g.hud_b, cf, copy | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, ow, oh) ||
        make_image(&g.gen, cf, copy | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, dw, dh) ||
        make_image(&g.dist, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, dw, dh) ||
        make_image(&g.index, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, dw, dh))
        return false;
    for (int i = 0; i < 2; i++)
        if (make_image(&g.hl[i], cf, copy | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, dw, dh) ||
            make_image(&g.mv[i], VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, dw, dh))
            return false;
    if (make_image(&g.flow, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, (dw + 15) / 16, (dh + 15) / 16))
        return false;
    if (make_buffer(&g.rb, (VkDeviceSize)dw * dh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    {   // the generated frame's pass: every pixel written
        VkAttachmentDescription att{};
        att.format = cf;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &cref;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1;
        rp.pAttachments = &att;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        if (VKC(vkCreateRenderPass, s.dev, &rp, nullptr, &g.pass) != VK_SUCCESS) return false;
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = g.pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &g.gen.view;
        fi.width = dw;
        fi.height = dh;
        fi.layers = 1;
        if (VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &g.fb) != VK_SUCCESS) return false;
    }
    VkCommandBuffer saved;   // first layouts: the fields and motion fields GENERAL, the rest sampled or copied
    begin_once(&saved);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkClearColorValue zero{}, none{.uint32 = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}};
    for (Image *im : {&g.mv[0], &g.mv[1], &g.dist, &g.index, &g.flow}) {
        barrier(im->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdClearColorImage(s.cmd, im->img, VK_IMAGE_LAYOUT_GENERAL, im == &g.index || im == &g.dist ? &none : &zero, 1, &range);
    }
    for (Image *im : {&g.hl[0], &g.hl[1], &g.hl_int, &g.hud_b}) {
        barrier(im->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdClearColorImage(s.cmd, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        barrier(im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                         &mb, 0, nullptr, 0, nullptr);
    if (submit_once(saved)) return false;
    const VkDescriptorType SAMP = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, STOR = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const VkDescriptorType mv_t[3] = {SAMP, SAMP, STOR}, splat_t[3] = {SAMP, STOR, STOR}, interp_t[7] = {SAMP, SAMP, SAMP, SAMP, SAMP, SAMP, SAMP},
                           flow_t[3] = {SAMP, SAMP, STOR};
    g.mv_set_layout = fg_set_layout(mv_t, 3, VK_SHADER_STAGE_COMPUTE_BIT);
    g.splat_set_layout = fg_set_layout(splat_t, 3, VK_SHADER_STAGE_COMPUTE_BIT);
    g.interp_set_layout = fg_set_layout(interp_t, 7, VK_SHADER_STAGE_FRAGMENT_BIT);
    g.flow_set_layout = fg_set_layout(flow_t, 3, VK_SHADER_STAGE_COMPUTE_BIT);
    if (!g.mv_set_layout || !g.splat_set_layout || !g.interp_set_layout || !g.flow_set_layout) return false;
    const VkDescriptorPoolSize ps[2] = {{SAMP, 2 * (2 + 1 + 7 + 2 + 2)}, {STOR, 2 * (1 + 2 + 1 + 1)}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 10;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &g.pool) != VK_SUCCESS) return false;
    const VkDescriptorSetLayout layouts[10] = {g.mv_set_layout,     g.mv_set_layout,     g.splat_set_layout, g.splat_set_layout,
                                               g.interp_set_layout, g.interp_set_layout, g.flow_set_layout,  g.flow_set_layout,
                                               g.flow_set_layout,   g.flow_set_layout};
    VkDescriptorSet sets[10];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g.pool;
    ai.descriptorSetCount = 10;
    ai.pSetLayouts = layouts;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, sets) != VK_SUCCESS) return false;
    // Set i writes or reads ring entry i as this frame's, 1 - i as the previous frame's. The velocity
    // target is read only while live (the TAA pass ran); without it a sampled image stands in.
    const VkImageView vel = s.taa.ok && s.taa.vel_ok ? s.taa.vel_img.view : g.hud_b.view;
    const VkImageLayout RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GEN = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo ii[2][19];
    VkWriteDescriptorSet wr[38]{};
    uint32_t nw = 0;
    for (int i = 0; i < 2; i++) {
        g.mv_set[i] = sets[i];
        g.splat_set[i] = sets[2 + i];
        g.interp_set[i] = sets[4 + i];
        g.flow_set[i] = sets[6 + i];
        g.hud_set[i] = sets[8 + i];
        VkDescriptorImageInfo *x = ii[i];
        x[0] = {s.nearest_sampler, s.depth.view, RO};
        x[1] = {s.nearest_sampler, vel, RO};
        x[2] = {VK_NULL_HANDLE, g.mv[i].view, GEN};
        x[3] = {s.nearest_sampler, g.mv[i].view, GEN};
        x[4] = {VK_NULL_HANDLE, g.dist.view, GEN};
        x[5] = {VK_NULL_HANDLE, g.index.view, GEN};
        x[6] = {s.clamp_sampler, g.hl[i].view, RO};
        x[7] = {s.clamp_sampler, g.hl[1 - i].view, RO};
        x[8] = {s.nearest_sampler, g.mv[i].view, GEN};
        x[9] = {s.nearest_sampler, g.mv[1 - i].view, GEN};
        x[10] = {s.nearest_sampler, g.index.view, GEN};
        x[11] = {s.nearest_sampler, g.hl[i].view, RO};   // binding 5: unused by the shader
        x[12] = {s.nearest_sampler, g.flow.view, GEN};
        x[13] = {s.clamp_sampler, g.hl[i].view, RO};
        x[14] = {s.clamp_sampler, g.hl[1 - i].view, RO};
        x[15] = {VK_NULL_HANDLE, g.flow.view, GEN};
        x[16] = {s.nearest_sampler, g.hl_int.view, RO};
        x[17] = {s.nearest_sampler, g.hud_b.view, RO};
        x[18] = {VK_NULL_HANDLE, g.mv[i].view, GEN};
        const VkDescriptorSet dst[19] = {g.mv_set[i],     g.mv_set[i],     g.mv_set[i],     g.splat_set[i],  g.splat_set[i],  g.splat_set[i],
                                         g.interp_set[i], g.interp_set[i], g.interp_set[i], g.interp_set[i], g.interp_set[i], g.interp_set[i],
                                         g.interp_set[i], g.flow_set[i],   g.flow_set[i],   g.flow_set[i],   g.hud_set[i],    g.hud_set[i],
                                         g.hud_set[i]};
        const uint32_t bind[19] = {0, 1, 2, 0, 1, 2, 0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 0, 1, 2};
        const VkDescriptorType type[19] = {SAMP, SAMP, STOR, SAMP, STOR, STOR, SAMP, SAMP, SAMP, SAMP, SAMP, SAMP, SAMP, SAMP, SAMP, STOR, SAMP, SAMP, STOR};
        for (int k = 0; k < 19; k++) {
            VkWriteDescriptorSet &w = wr[nw++];
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = dst[k];
            w.dstBinding = bind[k];
            w.descriptorCount = 1;
            w.descriptorType = type[k];
            w.pImageInfo = &x[k];
        }
    }
    vkUpdateDescriptorSets(s.dev, nw, wr, 0, nullptr);
    g.mv_layout = fg_pipe_layout(g.mv_set_layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(FgPush));
    g.splat_layout = fg_pipe_layout(g.splat_set_layout, VK_SHADER_STAGE_COMPUTE_BIT, 16);
    g.interp_layout = fg_pipe_layout(g.interp_set_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 32);
    g.flow_layout = fg_pipe_layout(g.flow_set_layout, VK_SHADER_STAGE_COMPUTE_BIT, 16);
    g.hud_layout = fg_pipe_layout(g.flow_set_layout, VK_SHADER_STAGE_COMPUTE_BIT, 32);
    if (!g.mv_layout || !g.splat_layout || !g.interp_layout || !g.flow_layout || !g.hud_layout) return false;
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    g.mv_cs = ms ? shader(fg_mv_ms_comp, sizeof fg_mv_ms_comp) : shader(fg_mv_comp, sizeof fg_mv_comp);
    g.splat_cs = shader(fg_splat_comp, sizeof fg_splat_comp);
    g.vs = shader(glass_vert, sizeof glass_vert);
    g.interp_fs = shader(fg_interp_frag, sizeof fg_interp_frag);
    g.flow_cs = shader(fg_flow_comp, sizeof fg_flow_comp);
    g.hud_cs = shader(fg_hud_comp, sizeof fg_hud_comp);
    if (!g.mv_cs || !g.splat_cs || !g.vs || !g.interp_fs || !g.flow_cs || !g.hud_cs) return false;
    g.mv_pipe = fg_compute(g.mv_cs, g.mv_layout);
    g.splat_pipe = fg_compute(g.splat_cs, g.splat_layout);
    g.flow_pipe = fg_compute(g.flow_cs, g.flow_layout);
    g.hud_pipe = fg_compute(g.hud_cs, g.hud_layout);
    g.interp_pipe = fullscreen_pipeline(g.vs, g.interp_layout, g.interp_fs, g.pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE);
    return g.mv_pipe && g.splat_pipe && g.flow_pipe && g.hud_pipe && g.interp_pipe;
}

// At renderer_init with RendererConfig.frame_gen (after taa_init: it reads the velocity target). A
// failure frees everything and leaves it off (renderer_frame_gen_on() 0: frames as without it).
static void fg_init() {
    if (!cfg.frame_gen) return;
    if (fg_create()) {
        s.fg.ok = true;
        s.fg.of = cfg.frame_gen >= 2;
        return;
    }
    fg_free();
}

// The frame target (TRANSFER_SRC_OPTIMAL, after a memory barrier from the pass's writes) into hl_int or
// hud_b (after the previous frame's HUD pass read; SHADER_READ_ONLY_OPTIMAL after).
static void fg_take_target(Image &dst) {
    barrier(dst.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy cp{};
    cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.extent = {(uint32_t)s.ow, (uint32_t)s.oh, 1};
    vkCmdCopyImage(s.cmd, s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    barrier(dst.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

// OP_FG inside the frame's render pass (begun with s.split_pass or s.resume_pass): ends it (the
// colour resolved into s.color), copies s.color into hl_int, writes this frame's motion field from
// the depth (and the TAA velocity target when live) and resumes the pass.
static void fg_capture_exec(float sx, float sy) {
    FrameGen &g = s.fg;
    const RFrameGen &p = g.p;
    vkCmdEndRenderPass(s.cmd);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};   // the pass's colour writes (and resolve) -> the copy
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    fg_take_target(g.hl_int);
    // The motion field: the depth sampled, this ring entry written (after the previous frame's reads).
    barrier(s.depth.img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1,
            VK_IMAGE_ASPECT_DEPTH_BIT);
    const int i = g.cur;
    barrier(g.mv[i].img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    const float dx = (float)s.dispw / s.w, dy = (float)s.disph / s.h;   // back-buffer pixels -> display pixels
    static const float ident[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    const float (*m)[4] = p.valid ? p.reproject : ident;
    const bool vel = s.taa.ok && s.taa.vel_ok && s.taa.vel_live;
    // The depth's nearest of 3 x 3 render texels (fg_mv.comp) where the colour's edges are not the
    // depth's: the scene was jittered and resolved (TAA) or resampled to the display size.
    const bool dilate = (s.taa.ok && s.taa.marked) || s.ow != s.dispw || s.oh != s.disph;
    const float vsx = s.taa.up ? (float)s.taa.rw / s.dispw : (float)s.ow / s.dispw, vsy = s.taa.up ? (float)s.taa.rh / s.disph : (float)s.oh / s.disph;
    FgPush pc = {{m[0][0], m[0][1], m[0][2], m[0][3]},
                 {m[1][0], m[1][1], m[1][2], m[1][3]},
                 {m[2][0], m[2][1], m[2][2], m[2][3]},
                 {p.valid ? p.depth_a : 0.0f, p.valid ? p.depth_b : 0.0f, p.valid ? 1.0f : 0.0f, vel ? 1.0f : 0.0f},
                 {(float)s.dispw, (float)s.disph, (float)s.ow / s.dispw, (float)s.oh / s.disph},
                 {p.vp[2] > 0 ? p.vp[0] * dx : 0.0f, p.vp[3] > 0 ? p.vp[1] * dy : 0.0f, p.vp[2] > 0 ? p.vp[2] * dx : (float)s.dispw,
                  p.vp[3] > 0 ? p.vp[3] * dy : (float)s.disph},
                 {vsx, vsy, dilate ? 1.0f : 0.0f, 0},
                 {s.taa.ok && s.taa.marked ? s.taa.jit[0] : 0.0f, s.taa.ok && s.taa.marked ? s.taa.jit[1] : 0.0f, 0, 0}};
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.mv_pipe);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.mv_layout, 0, 1, &g.mv_set[i], 0, nullptr);
    vkCmdPushConstants(s.cmd, g.mv_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(s.cmd, ((uint32_t)s.dispw + 7) / 8, ((uint32_t)s.disph + 7) / 8, 1);
    barrier(g.mv[i].img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    barrier(s.depth.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
    barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = resume_pass();
    rb.framebuffer = s.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    g.captured = true;
    (void)sx;
    (void)sy;
}

// One image copy of a whole colour image (both in the given layouts).
static void fg_copy(VkImage src, VkImage dst, uint32_t w, uint32_t h) {
    VkImageCopy cp{};
    cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.extent = {w, h, 1};
    vkCmdCopyImage(s.cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
}
// The output (TRANSFER_SRC_OPTIMAL) into a display-size sampled image (from its layout `from`, after the
// previous frame's reads; SHADER_READ_ONLY_OPTIMAL after).
static void fg_take_output(Image &dst, VkImageLayout from) {
    barrier(dst.img, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    fg_copy(output_image(), dst.img, (uint32_t)s.dispw, (uint32_t)s.disph);
    barrier(dst.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

// OP_FG_END inside the frame's render pass: ends it, copies the frame target into hud_b and resumes.
static void fg_hud_end_exec() {
    vkCmdEndRenderPass(s.cmd);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};   // the pass's colour writes (and resolve) -> the copy
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    fg_take_target(s.fg.hud_b);
    barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = resume_pass();
    rb.framebuffer = s.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    s.fg.hud_taken = true;
}

// After the post chain (the output, TRANSFER_SRC_OPTIMAL) in a frame that captured: the output into
// this frame's hl entry, the HUD (hl_int against hud_b) zeroed in the motion field, then the generated
// frame. Without a capture right before (reset, a cut) it is the output as is.
static void fg_build() {
    FrameGen &g = s.fg;
    const int i = g.cur;
    const bool reset = g.p.reset || g.last_serial == 0 || g.last_serial + 1 != s.frame_serial;
    fg_take_output(g.hl[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    {   // the HUD: still and in front of everything in this frame's motion field
        barrier(g.mv[i].img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.hud_pipe);
        vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.hud_layout, 0, 1, &g.hud_set[i], 0, nullptr);
        const bool resampled = s.ow != s.dispw || s.oh != s.disph || s.sharp.ok || s.fsr.ok;   // the post chain spreads the HUD's edges
        const float pc[8] = {(float)s.dispw, (float)s.disph, (float)s.ow / s.dispw, (float)s.oh / s.disph, (float)s.ow, (float)s.oh,
                             resampled ? 1.0f : 0.0f, 0};
        vkCmdPushConstants(s.cmd, g.hud_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        vkCmdDispatch(s.cmd, ((uint32_t)s.dispw + 7) / 8, ((uint32_t)s.disph + 7) / 8, 1);
        barrier(g.mv[i].img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    const uint32_t gx = ((uint32_t)s.dispw + 7) / 8, gy = ((uint32_t)s.disph + 7) / 8;
    if (!reset) {
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkClearColorValue none{.uint32 = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}};
        for (Image *im : {&g.dist, &g.index}) {   // after the previous frame's splat and interpolation
            barrier(im->img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdClearColorImage(s.cmd, im->img, VK_IMAGE_LAYOUT_GENERAL, &none, 1, &range);
            barrier(im->img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.splat_pipe);
        vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.splat_layout, 0, 1, &g.splat_set[i], 0, nullptr);
        for (int pass = 0; pass < 2; pass++) {
            const int32_t pc[4] = {s.dispw, s.disph, pass, 0};
            vkCmdPushConstants(s.cmd, g.splat_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
            vkCmdDispatch(s.cmd, gx, gy, 1);
            barrier(pass ? g.index.img : g.dist.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        if (g.of) {   // --frame-gen-of: the block flow between the two HUD-less outputs (after the previous frame's read)
            barrier(g.flow.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.flow_pipe);
            vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.flow_layout, 0, 1, &g.flow_set[i], 0, nullptr);
            const float pc[4] = {(float)s.dispw, (float)s.disph, 1.0f / s.dispw, 1.0f / s.disph};
            vkCmdPushConstants(s.cmd, g.flow_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
            vkCmdDispatch(s.cmd, ((uint32_t)s.dispw + 15) / 16, ((uint32_t)s.disph + 15) / 16, 1);
            barrier(g.flow.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
    }
    // After the previous frame's readback copy or blit of it (same queue).
    barrier(g.gen.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g.pass;
    rb.framebuffer = g.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)s.dispw, (uint32_t)s.disph}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)s.dispw, (float)s.disph, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &vp);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.interp_pipe);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.interp_layout, 0, 1, &g.interp_set[i], 0, nullptr);
    const float pc[8] = {(float)s.dispw, (float)s.disph, 1.0f / s.dispw, 1.0f / s.disph, reset ? 1.0f : 0.0f, FG_COLOUR_LIMIT, g.of ? 1.0f : 0.0f, 0};
    vkCmdPushConstants(s.cmd, g.interp_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    barrier(g.gen.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    g.cur = 1 - i;
    g.last_serial = s.frame_serial;
    g.built = true;
    g.executed++;
}

// ---- Rain (RendererConfig.rain, renderer_rain) ----

// Push constants of shaders/rain.vert and rain.frag (both stages).
struct RainPush {
    float frame[4];    // screen heights per pixel, target aspect, cos(tilt), sin(tilt)
    float cell[4];     // cell width, cell height, streak length (fraction of the cell), width
    float scroll[4];   // scroll x, y, alpha, fill
    float color[4];    // rgb, slice depth
    float haze[4];     // veil opacity, pass (0 colour and veil, 1 lit), lit, spread
    float depth[4];    // depth projection a, b (a = 0: slice depth only), slice distance, log2(spread)
};

static void rain_free() {
    Rain &r = s.rain;
    for (VkPipeline p : {r.pipe, r.lit})
        if (p) vkDestroyPipeline(s.dev, p, nullptr);
    for (VkShaderModule m : {r.vs, r.fs})
        if (m) vkDestroyShaderModule(s.dev, m, nullptr);
    if (r.layout) vkDestroyPipelineLayout(s.dev, r.layout, nullptr);
    r = Rain();
}

// At renderer_init: the rain pipeline when RendererConfig.rain is set (compatible with the main pass
// and its split variants: same attachments). A failure leaves rain off (renderer_rain_on()).
static void rain_init() {
    if (!cfg.rain) return;
    Rain &r = s.rain;
    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(RainPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (s.cover.ok) {   // the RAIN_COVER variant: set 0 the cover's
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &s.cover.set_layout;
    }
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &r.layout) == VK_SUCCESS) {
        r.vs = shader(rain_vert, sizeof rain_vert);
        r.fs = s.cover.ok ? shader(rain_cover_frag, sizeof rain_cover_frag) : shader(rain_frag, sizeof rain_frag);
        if (r.vs && r.fs) {
            r.pipe = fullscreen_pipeline(r.vs, r.layout, r.fs, s.pass, s.samples, FS_COVER, true);
            r.lit = fullscreen_pipeline(r.vs, r.layout, r.fs, s.pass, s.samples, FS_LIGHTEN, true);
        }
    }
    if (r.pipe && r.lit) {
        r.ok = true;
        return;
    }
    rain_free();
}

// One rain operation inside the frame's render pass: its slices, far first, each a full-target
// triangle (viewport depth 0-1, so the slice depth is the NDC depth the scene was drawn with), drawn
// with the colour and veil pass, then the lit pass.
static void rain_exec(const RRain &r) {
    Rain &st = s.rain;
    VkPipeline bound = VK_NULL_HANDLE;
    VkViewport v{0, 0, (float)s.ow, (float)s.oh, 0, 1};
    VkRect2D all{{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &all);
    if (s.cover.ok) vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, st.layout, 0, 1, &s.cover.set, 0, nullptr);
    const float rgb[3] = {(float)(r.color >> 16 & 255) / 255.0f, (float)(r.color >> 8 & 255) / 255.0f, (float)(r.color & 255) / 255.0f};
    for (uint32_t i = 0; i < r.slices; i++) {
        const RRainSlice &q = r.slice[i];
        const bool streaks = q.fill > 0, flat = (streaks && q.alpha > 0) || q.haze > 0, lit = streaks && q.lit > 0;
        if (q.cell_w <= 0 || q.cell_h <= 0 || q.length <= 0 || (!flat && !lit)) continue;
        RainPush pc = {{1.0f / (float)s.oh, (float)s.ow / (float)s.oh, cosf(r.tilt), sinf(r.tilt)},
                       {q.cell_w, q.cell_h, q.length > 1 ? 1.0f : q.length, q.width},
                       {q.scroll_x, q.scroll_y, q.alpha, q.fill},
                       {rgb[0], rgb[1], rgb[2], q.depth < 0 ? 0.0f : q.depth > 1 ? 1.0f : q.depth},
                       {q.haze < 0 ? 0.0f : q.haze > 1 ? 1.0f : q.haze, 0, q.lit > 2 ? 2.0f : q.lit, q.spread > 1 ? q.spread : 1.0f},
                       {r.depth_a > 0 && q.dist > 0 ? r.depth_a : 0.0f, r.depth_b, q.dist, q.spread > 1 ? log2f(q.spread) : 0.0f}};
        for (int pass = 0; pass < 2; pass++) {
            if (!(pass ? lit : flat)) continue;
            VkPipeline p = pass ? st.lit : st.pipe;
            if (p != bound) vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = p);
            pc.haze[1] = (float)pass;
            vkCmdPushConstants(s.cmd, st.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
            vkCmdDraw(s.cmd, 3, 1, 0, 0);
        }
    }
    st.executed++;
}

// ---- Sky (RendererConfig.sky, renderer_sky / renderer_skyline) ----

// Push constants of shaders/sky.frag (fragment stage) and skyline.frag.
struct SkyPush {
    float right[4];      // xyz, seconds
    float up[4];         // xyz, cloud cover
    float fwd[4];        // xyz, stars
    float moon[4];       // direction, angular radius
    uint32_t ramp[8];    // XRGB
    uint32_t misc[4];    // silhouette, moon light (float bits)
};
struct SkylinePush {
    float haze[4];       // rgb, strength
    float params[4];     // haze distance, windows, seconds, 0
};
static_assert(sizeof(SkyPush) <= 128 && sizeof(SkylinePush) <= 128, "push constants within the guaranteed 128 bytes");

static void sky_free() {
    Sky &k = s.sky;
    for (VkPipeline p : {k.pipe, k.line})
        if (p) vkDestroyPipeline(s.dev, p, nullptr);
    for (VkShaderModule m : {k.vs, k.fs, k.line_vs, k.line_fs})
        if (m) vkDestroyShaderModule(s.dev, m, nullptr);
    for (VkPipelineLayout l : {k.layout, k.line_layout})
        if (l) vkDestroyPipelineLayout(s.dev, l, nullptr);
    k = Sky();
}

// The skyline overlay pipeline (see struct Sky): RVertex input as the geometry pipelines, the main
// pass's samples and depth clamp, so it lands exactly on the title's skyline draw.
static VkPipeline skyline_pipeline() {
    Sky &k = s.sky;
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = k.line_vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = k.line_fs;
    st[1].pName = "main";
    VkVertexInputBindingDescription bind{0, sizeof(RVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[4 + RENDERER_STAGES] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RVertex, x)},
        {1, 0, VK_FORMAT_B8G8R8A8_UNORM, offsetof(RVertex, color)},
        {2, 0, VK_FORMAT_B8G8R8A8_UNORM, offsetof(RVertex, specular)}};
    for (uint32_t i = 0; i < RENDERER_STAGES; i++)
        attrs[3 + i] = {3 + i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)(offsetof(RVertex, tex) + i * sizeof(float[4]))};
    attrs[3 + RENDERER_STAGES] = {3 + RENDERER_STAGES, 0, VK_FORMAT_R32_SFLOAT, offsetof(RVertex, fog)};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 4 + RENDERER_STAGES;
    vi.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = s.depth_clamp ? VK_TRUE : VK_FALSE;   // as the geometry pipelines
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = s.samples;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cb{};
    cb.blendEnable = VK_TRUE;
    cb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cb.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cb.colorBlendOp = cb.alphaBlendOp = VK_BLEND_OP_ADD;
    cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cbs;
    pi.pDynamicState = &dy;
    pi.layout = k.line_layout;
    pi.renderPass = s.pass;
    VkPipeline p = VK_NULL_HANDLE;
    if (VKC(vkCreateGraphicsPipelines, s.dev, s.pcache, 1, &pi, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    return p;
}

// At renderer_init: the sky and skyline pipelines when RendererConfig.sky is set (compatible with
// the main pass and its split variants). A failure leaves the sky off (renderer_sky_on()).
static void sky_init() {
    if (!cfg.sky) return;
    Sky &k = s.sky;
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SkyPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    VkPushConstantRange lpc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SkylinePush)};
    VkPipelineLayoutCreateInfo lli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lli.pushConstantRangeCount = 1;
    lli.pPushConstantRanges = &lpc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &k.layout) == VK_SUCCESS &&
        VKC(vkCreatePipelineLayout, s.dev, &lli, nullptr, &k.line_layout) == VK_SUCCESS) {
        k.vs = shader(sky_vert, sizeof sky_vert);
        k.fs = shader(sky_frag, sizeof sky_frag);
        k.line_vs = shader(geom_vert, sizeof geom_vert);
        k.line_fs = shader(skyline_frag, sizeof skyline_frag);
        if (k.vs && k.fs && k.line_vs && k.line_fs) {
            k.pipe = fullscreen_pipeline(k.vs, k.layout, k.fs, s.pass, s.samples, FS_REPLACE);
            k.line = skyline_pipeline();
        }
    }
    if (k.pipe && k.line) {
        k.ok = true;
        return;
    }
    sky_free();
}

// One sky operation inside the frame's render pass: a full-target triangle.
static void sky_exec(const RSky &p) {
    Sky &k = s.sky;
    VkViewport v{0, 0, (float)s.ow, (float)s.oh, 0, 1};
    VkRect2D all{{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &all);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, k.pipe);
    SkyPush pc{};
    for (int i = 0; i < 3; i++) {
        pc.right[i] = p.right[i];
        pc.up[i] = p.up[i];
        pc.fwd[i] = p.fwd[i];
        pc.moon[i] = p.moon[i];
    }
    pc.right[3] = p.seconds;
    pc.up[3] = p.cover < 0 ? 0.0f : p.cover > 1 ? 1.0f : p.cover;
    pc.fwd[3] = p.stars < 0 ? 0.0f : p.stars > 1 ? 1.0f : p.stars;
    pc.moon[3] = p.moon_size > 0 ? p.moon_size : 0.0f;
    for (int i = 0; i < RENDERER_SKY_RAMP; i++) pc.ramp[i] = p.ramp[i];
    pc.misc[0] = p.silhouette;
    const float ml = p.moon_light < 0 ? 0.0f : p.moon_light > 1 ? 1.0f : p.moon_light;
    memcpy(&pc.misc[1], &ml, 4);
    vkCmdPushConstants(s.cmd, k.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(s.cmd, 3, 1, 0, s.taa.jitter_now ? taa_instance((float)s.ow, (float)s.oh) : 0);
    k.executed++;
}

// One skyline overlay inside the frame's render pass: one pass over the recorded vertices,
// with the draw's viewport (as exec_op).
static void skyline_exec(const Op &op, VkDeviceSize index_offset, float sx, float sy) {
    Sky &k = s.sky;
    const RSkyline &p = k.lines[op.push[0]];
    VkViewport v{op.vp.x * sx, op.vp.y * sy, op.vp.w * sx, op.vp.h * sy, op.vp.minz, op.vp.maxz};
    int x0 = (int)v.x, y0 = (int)v.y, x1 = (int)(v.x + v.width), y1 = (int)(v.y + v.height);
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > s.ow ? s.ow : x1;
    y1 = y1 > s.oh ? s.oh : y1;
    if (x1 <= x0 || y1 <= y0 || v.width <= 0 || v.height <= 0) return;
    VkRect2D sc{{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &sc);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(s.cmd, 0, 1, &cur().geometry.buf, &zero);
    vkCmdBindIndexBuffer(s.cmd, cur().geometry.buf, index_offset, VK_INDEX_TYPE_UINT32);
    SkylinePush pc = {{(float)(p.haze >> 16 & 255) / 255.0f, (float)(p.haze >> 8 & 255) / 255.0f, (float)(p.haze & 255) / 255.0f,
                       p.haze_max < 0 ? 0.0f : p.haze_max > 1 ? 1.0f : p.haze_max},
                      {p.haze_dist > 1 ? p.haze_dist : 1.0f, p.windows < 0 ? 0.0f : p.windows > 1 ? 1.0f : p.windows, p.seconds, 0}};
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, k.line);
    vkCmdPushConstants(s.cmd, k.line_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDrawIndexed(s.cmd, op.index_count, 1, op.first_index, op.vertex_offset, s.taa.jitter_now ? taa_instance(v.width, v.height) : 0);
    k.lines_executed++;
}

// ---- Ambient occlusion (RendererConfig.ssao, renderer_ssao) ----

// Push constants of shaders/ssao_ao.frag, ssao_blur.frag and ssao_comp.frag (ssao_common.glsl).
struct SsaoPush {
    float proj[4];     // depth_a, depth_b, 1 / px, 1 / py
    float size[4];     // internal width, height, 1 / width, 1 / height
    float params[4];   // radius, strength, bias, largest sampling radius (pixels)  | splash: time, cell, period, chance
    float dir[4];      // blur step, half width, half height                         | splash: splash, ripple, fade, mask scale / output scale
    float cam[4][4];   // splash only: eye, right, up, back (world, z up) with the colour's r, g, b in [0..2][3]
};
const float SSAO_BIAS = 0.25f;        // cosine below which a tap does not occlude (flat and gently curved surfaces)
const float SSAO_MAX_RADIUS = 0.06f;  // largest sampling radius, a share of the internal height (close-ups: faces)

// A one-attachment render pass kept in COLOR_ATTACHMENT_OPTIMAL (barriers around it): the half-size
// images (contents discarded, every texel written) or the composite's color target (samples, loaded).
static bool ssao_pass(VkFormat fmt, VkSampleCountFlagBits samples, bool load, VkRenderPass *out) {
    VkAttachmentDescription att{};
    att.format = fmt;
    att.samples = samples;
    att.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    return VKC(vkCreateRenderPass, s.dev, &rp, nullptr, out) == VK_SUCCESS;
}

static VkFramebuffer ssao_fb(VkRenderPass pass, VkImageView view, int w, int h) {
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = pass;
    fi.attachmentCount = 1;
    fi.pAttachments = &view;
    fi.width = (uint32_t)w;
    fi.height = (uint32_t)h;
    fi.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &fb);
    return fb;
}

static bool ssao_create() {
    Ssao &a = s.ssao;
    const VkFormat fmt = VK_FORMAT_R16G16_SFLOAT;   // Vulkan 1.0 requires it as a sampled color attachment
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(s.phys, fmt, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (!s.depth_sampled || (fp.optimalTilingFeatures & need) != need || !split_passes_ready()) return false;
    const SsaoWorkSize ws = ssao_work_size(s.ow, s.oh, s.ss);
    a.hw = ws.w;
    a.hh = ws.h;
    a.scale = ws.scale;
    // The light mask: the scene's alpha (mask_role, set for every draw before the bloom split, so
    // also without --bloom), read from the single-sample color (the resolve target with MSAA).
    a.mask = true;
    const VkImageUsageFlags use = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (make_image(&a.a, fmt, use, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)a.hw, (uint32_t)a.hh) ||
        make_image(&a.b, fmt, use, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)a.hw, (uint32_t)a.hh))
        return false;
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    if (!ssao_pass(fmt, VK_SAMPLE_COUNT_1_BIT, false, &a.pass) || !ssao_pass(VK_FORMAT_B8G8R8A8_UNORM, s.samples, true, &a.comp_pass))
        return false;
    a.fb_a = ssao_fb(a.pass, a.a.view, a.hw, a.hh);
    a.fb_b = ssao_fb(a.pass, a.b.view, a.hw, a.hh);
    a.comp_fb = ssao_fb(a.comp_pass, ms ? s.msaa.view : s.color.view, s.ow, s.oh);
    if (!a.fb_a || !a.fb_b || !a.comp_fb) return false;
    const uint32_t nb = a.mask ? 3 : 2;   // bindings per set
    VkDescriptorSetLayoutBinding lb[3] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = nb;
    li.pBindings = lb;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &a.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * nb};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &a.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[2] = {a.set_layout, a.set_layout};
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = a.pool;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, sets) != VK_SUCCESS) return false;
    a.set_a = sets[0];
    a.set_b = sets[1];
    // Every read is a texelFetch: the nearest sampler (clamped) for all of them.
    VkDescriptorImageInfo ii[6] = {{s.nearest_sampler, s.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {s.nearest_sampler, a.a.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {s.nearest_sampler, s.color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {s.nearest_sampler, s.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {s.nearest_sampler, a.b.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {s.nearest_sampler, s.color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet wr[6]{};
    uint32_t nw = 0;
    for (uint32_t i = 0; i < 6; i++) {
        if (i % 3 >= nb) continue;
        wr[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[nw].dstSet = sets[i / 3];
        wr[nw].dstBinding = i % 3;
        wr[nw].descriptorCount = 1;
        wr[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[nw].pImageInfo = &ii[i];
        nw++;
    }
    vkUpdateDescriptorSets(s.dev, nw, wr, 0, nullptr);
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SsaoPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &a.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &a.layout) != VK_SUCCESS) return false;
    a.vs = shader(glass_vert, sizeof glass_vert);
    a.ao_fs = ms ? shader(ssao_ao_ms_frag, sizeof ssao_ao_ms_frag) : shader(ssao_ao_frag, sizeof ssao_ao_frag);
    // The light mask per pixel in the composite with MSAA (it reads the resolve target, not its own
    // attachment), per texel in the last blur pass otherwise (the composite draws into s.color).
    a.blur_fs = a.mask && !ms ? shader(ssao_blur_mask_frag, sizeof ssao_blur_mask_frag) : shader(ssao_blur_frag, sizeof ssao_blur_frag);
    a.comp_fs = !ms  ? shader(ssao_comp_frag, sizeof ssao_comp_frag)
                : a.mask ? shader(ssao_comp_ms_mask_frag, sizeof ssao_comp_ms_mask_frag)
                         : shader(ssao_comp_ms_frag, sizeof ssao_comp_ms_frag);
    if (!a.vs || !a.ao_fs || !a.blur_fs || !a.comp_fs) return false;
    const int32_t scale = a.scale;
    const VkSpecializationMapEntry me{1, 0, sizeof(int32_t)};
    const VkSpecializationInfo spec{1, &me, sizeof scale, &scale};
    a.ao = fullscreen_pipeline(a.vs, a.layout, a.ao_fs, a.pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE, false, &spec);
    a.blur = fullscreen_pipeline(a.vs, a.layout, a.blur_fs, a.pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE, false, &spec);
    a.comp = fullscreen_pipeline(a.vs, a.layout, a.comp_fs, a.comp_pass, s.samples, FS_MULTIPLY, false, &spec);
    return a.ao && a.blur && a.comp;
}

// ---- XeGTAO (RendererConfig.ssao_mode) ----

// Push constants of shaders/gtao_*.comp (gtao_common.glsl PC).
struct GtaoPush {
    float depth[4], ndc[4], scr[4], size[4], rad[4], misc[4], den[4];
};
// Occlusion below this share counts as none (the last denoise pass): open faces keep their exact
// colour. XeGTAO leaves up to ~0.06 on an open plane seen at a slant (its fast acos and the
// projected-normal fudge; renderer_test's corner frame).
const float GTAO_DEAD_ZONE = 0.08f;
// Taps up to this many view units above the tangent plane count as on it (gtao_main.comp): decals
// drawn a little in front of walls cast no rim. Mission 7's graffiti needed about 0.07-0.14 (a
// depth-relative bias of 0.016); 0.1 keeps kerbs' contact lines.
const float GTAO_SURFACE_BIAS = 0.1f;

static VkPipeline gtao_pipeline(const uint32_t *code, size_t bytes, const VkSpecializationInfo *spec) {
    VkShaderModule m = shader(code, bytes);
    if (!m) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.stage.pSpecializationInfo = spec;
    ci.layout = s.ssao.glayout;
    VkPipeline p = VK_NULL_HANDLE;
    if (VKC(vkCreateComputePipelines, s.dev, s.pcache, 1, &ci, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(s.dev, m, nullptr);
    return p;
}

static void gtao_free() {
    Ssao &a = s.ssao;
    for (VkPipeline p : {a.gpre, a.gmain, a.gden, a.gtmp})
        if (p) vkDestroyPipeline(s.dev, p, nullptr);
    if (a.glayout) vkDestroyPipelineLayout(s.dev, a.glayout, nullptr);
    if (a.gpool) vkDestroyDescriptorPool(s.dev, a.gpool, nullptr);
    if (a.gset_layout) vkDestroyDescriptorSetLayout(s.dev, a.gset_layout, nullptr);
    if (a.gz_sampler) vkDestroySampler(s.dev, a.gz_sampler, nullptr);
    for (VkImageView &v : a.gz_mip)
        if (v) vkDestroyImageView(s.dev, v, nullptr), v = VK_NULL_HANDLE;
    free_image(&a.gz);
    free_image(&a.gw[0]);
    free_image(&a.gw[1]);
    free_image(&a.gh[0]);
    free_image(&a.gh[1]);
    a.gh_cur = 0;
    a.gh_serial = 0;
    a.gtao = false;
    a.levels = 0;
    a.gpre = a.gmain = a.gden = a.gtmp = VK_NULL_HANDLE;
    a.glayout = VK_NULL_HANDLE;
    a.gpool = VK_NULL_HANDLE;
    a.gset_layout = VK_NULL_HANDLE;
    a.gz_sampler = VK_NULL_HANDLE;
    a.gset_pre = a.gset_main = VK_NULL_HANDLE;
    a.gset_den[0] = a.gset_den[1] = a.gset_comp[0] = a.gset_comp[1] = VK_NULL_HANDLE;
    a.gset_tmp[0] = a.gset_tmp[1] = a.gset_den_h[0] = a.gset_den_h[1] = VK_NULL_HANDLE;
}

// After ssao_create (its composite, set layout and working size): XeGTAO's images, sets and compute
// pipelines. Only formats every Vulkan 1.0 device stores from shaders without
// shaderStorageImageExtendedFormats (R32_SFLOAT, R16G16B16A16_SFLOAT), so no 8- or 16-bit storage
// feature is needed; false (classic stays) without compute on the queue or those features.
static bool gtao_create() {
    Ssao &a = s.ssao;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s.phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qs(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(s.phys, &nq, qs.data());
    if (s.qfamily >= nq || !(qs[s.qfamily].queueFlags & VK_QUEUE_COMPUTE_BIT)) return false;
    const VkFormat zf = VK_FORMAT_R32_SFLOAT, wf = VK_FORMAT_R16G16B16A16_SFLOAT;
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    for (VkFormat f : {zf, wf}) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(s.phys, f, &fp);
        if ((fp.optimalTilingFeatures & need) != need) return false;
    }
    a.quality = cfg.gtao_quality < GTAO_QUALITY_LOW ? GTAO_QUALITY_LOW : cfg.gtao_quality > GTAO_QUALITY_HIGH ? GTAO_QUALITY_HIGH : cfg.gtao_quality;
    a.levels = 1;
    while (a.levels < GTAO_MIP_LEVELS && ((a.hw > a.hh ? a.hw : a.hh) >> a.levels) > 0) a.levels++;
    const VkImageUsageFlags use = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (make_image(&a.gz, zf, use, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)a.hw, (uint32_t)a.hh, (uint32_t)a.levels) ||
        make_image(&a.gw[0], wf, use, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)a.hw, (uint32_t)a.hh) ||
        make_image(&a.gw[1], wf, use, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)a.hw, (uint32_t)a.hh) ||
        make_image(&a.gh[0], wf, use, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)a.hw, (uint32_t)a.hh) ||
        make_image(&a.gh[1], wf, use, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)a.hw, (uint32_t)a.hh))
        return false;
    for (int k = 0; k < GTAO_MIP_LEVELS; k++) {   // levels past the chain: views of the last one (bound, never written)
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = a.gz.img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = zf;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)(k < a.levels ? k : a.levels - 1), 1, 0, 1};
        if (VKC(vkCreateImageView, s.dev, &vi, nullptr, &a.gz_mip[k]) != VK_SUCCESS) return false;
    }
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = (float)(a.levels - 1);
    if (VKC(vkCreateSampler, s.dev, &si, nullptr, &a.gz_sampler) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding lb[7];
    for (uint32_t i = 0; i < 7; i++)
        lb[i] = {i, i == 0 || i == 6 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 7;
    li.pBindings = lb;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &a.gset_layout) != VK_SUCCESS) return false;
    // Sized for exactly the 10 sets below: 8 compute sets (2 samplers + 5 storage images each) and the
    // 2 composite sets (a.set_layout: 2 or 3 samplers). Drivers that enforce pool sizes (Intel on
    // Windows) refused the allocation with the old 20 / 12 (VK_ERROR_OUT_OF_POOL_MEMORY), which left
    // XeGTAO off there as "unavailable on this device".
    const uint32_t comp_bindings = a.mask ? 3 : 2;
    const VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 * 2 + 2 * comp_bindings},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8 * 5}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 10;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &a.gpool) != VK_SUCCESS) return false;
    const VkDescriptorSetLayout layouts[10] = {a.gset_layout, a.gset_layout, a.gset_layout, a.gset_layout, a.set_layout,
                                               a.set_layout,  a.gset_layout, a.gset_layout, a.gset_layout, a.gset_layout};
    VkDescriptorSet sets[10];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = a.gpool;
    ai.descriptorSetCount = 10;
    ai.pSetLayouts = layouts;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, sets) != VK_SUCCESS) return false;
    a.gset_pre = sets[0];
    a.gset_main = sets[1];
    a.gset_den[0] = sets[2];
    a.gset_den[1] = sets[3];
    a.gset_comp[0] = sets[4];
    a.gset_comp[1] = sets[5];
    a.gset_tmp[0] = sets[6];
    a.gset_tmp[1] = sets[7];
    a.gset_den_h[0] = sets[8];
    a.gset_den_h[1] = sets[9];
    const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL, RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    std::vector<VkDescriptorImageInfo> ii;
    std::vector<VkWriteDescriptorSet> wr;
    ii.reserve(48);
    auto put = [&](VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkSampler smp, VkImageView view, VkImageLayout lay) {
        ii.push_back({smp, view, lay});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = type;
        wr.push_back(w);
    };
    const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    put(a.gset_pre, 0, CIS, s.nearest_sampler, s.depth.view, RO);   // the raw depth (sample 0 with MSAA)
    for (uint32_t k = 0; k < GTAO_MIP_LEVELS; k++) put(a.gset_pre, 1 + k, SI, VK_NULL_HANDLE, a.gz_mip[k], G);
    put(a.gset_main, 0, CIS, a.gz_sampler, a.gz.view, G);
    put(a.gset_main, 1, SI, VK_NULL_HANDLE, a.gw[0].view, G);
    for (int i = 0; i < 2; i++) {
        put(a.gset_den[i], 0, CIS, s.nearest_sampler, a.gw[i].view, G);
        put(a.gset_den[i], 1, SI, VK_NULL_HANDLE, a.gw[1 - i].view, G);
        put(a.gset_den[i], 6, CIS, s.nearest_sampler, s.color.view, RO);   // the light mask (LIGHT_MASK variant)
        put(a.gset_comp[i], 0, CIS, s.nearest_sampler, s.depth.view, RO);   // as set_a, with gw[i] as the occlusion
        put(a.gset_comp[i], 1, CIS, s.nearest_sampler, a.gw[i].view, G);
        put(a.gset_comp[i], 2, CIS, s.nearest_sampler, s.color.view, RO);
        put(a.gset_tmp[i], 0, CIS, s.nearest_sampler, a.gw[0].view, G);
        put(a.gset_tmp[i], 1, SI, VK_NULL_HANDLE, a.gh[i].view, G);
        put(a.gset_tmp[i], 6, CIS, s.nearest_sampler, a.gh[1 - i].view, G);
        put(a.gset_den_h[i], 0, CIS, s.nearest_sampler, a.gh[i].view, G);
        put(a.gset_den_h[i], 1, SI, VK_NULL_HANDLE, a.gw[1].view, G);
        put(a.gset_den_h[i], 6, CIS, s.nearest_sampler, s.color.view, RO);
    }
    for (size_t i = 0; i < wr.size(); i++) wr[i].pImageInfo = &ii[i];
    vkUpdateDescriptorSets(s.dev, (uint32_t)wr.size(), wr.data(), 0, nullptr);
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GtaoPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &a.gset_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &a.glayout) != VK_SUCCESS) return false;
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    const int32_t sc[3] = {a.scale, gtao_slices(a.quality), gtao_steps(a.quality)};
    const VkSpecializationMapEntry me[3] = {{1, 0, 4}, {2, 4, 4}, {3, 8, 4}};
    const VkSpecializationInfo spec1{1, me, 4, sc}, spec3{3, me, sizeof sc, sc};
    a.gpre = ms ? gtao_pipeline(gtao_prefilter_ms_comp, sizeof gtao_prefilter_ms_comp, &spec1)
                : gtao_pipeline(gtao_prefilter_comp, sizeof gtao_prefilter_comp, &spec1);
    a.gmain = gtao_pipeline(gtao_main_comp, sizeof gtao_main_comp, &spec3);
    a.gden = a.mask && !ms ? gtao_pipeline(gtao_denoise_mask_comp, sizeof gtao_denoise_mask_comp, &spec1)
                           : gtao_pipeline(gtao_denoise_comp, sizeof gtao_denoise_comp, &spec1);
    a.gtmp = gtao_pipeline(gtao_temporal_comp, sizeof gtao_temporal_comp, nullptr);
    return a.gpre && a.gmain && a.gden && a.gtmp;
}

// One storage image about to be written by a compute pass (contents discarded: every texel is
// written), after the previous frame's or pass's reads (compute and the composite's fragment).
static void gtao_to_write(VkImage img, uint32_t levels = 1) {
    barrier(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, levels);
}
static void gtao_to_read(VkImage img, uint32_t levels = 1) {
    barrier(img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, levels);
}
static void gtao_dispatch(VkPipeline pipe, VkDescriptorSet set, const GtaoPush &pc, uint32_t gx, uint32_t gy) {
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.ssao.glayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(s.cmd, s.ssao.glayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(s.cmd, gx, gy, 1);
}

// ---- Character lighting (RendererConfig.char_light, renderer_char_lights, renderer_draw_lit) ----

// The uniform block of shaders/char_lit.glsl (std140).
struct CharLitUbo {
    float cam[4];      // 1 / px, 1 / py, lights in use, character gain
    float params[4];   // falloff k, Lambert wrap, world gain, world Lambert wrap
    float pos_r[RENDERER_LAMP_MAX][4];
    float col[RENDERER_LAMP_MAX][4];
    float opts[4];     // x: 1 = the title's shadow darkens the added light too (RLampLights.title_shadow_full); y: --parallax depth
    float wto[3][4];   // light walls (light_walls_fill): view -> world rows
    float wall[4];     // the wall map: x0, y0, 1 / cell, 1 = test the walls
    float wallz[4];    // z0, zstep, the receiver's own wall (world units), -
    float hair[4];     // --hair: world up in view space, strength (RendererConfig.hair); world up for --skin too
    float skin[4];     // --skin: strength (RendererConfig.skin); --eyes, --cloth strengths, -
};
// RLampLights.walls into a uniform block's wto, wall and wallz: left 0
// (no wall test, the frame exactly as without it) unless walls is set and a level's map is loaded.
static void light_walls_fill(const RLampLights &p, float wto[3][4], float wall[4], float wallz[4]) {
    if (!p.walls || !s.walls.ok || !s.walls.real) return;
    memcpy(wto, p.to_world, sizeof p.to_world);
    memcpy(wall, s.walls.box, 4 * sizeof(float));
    memcpy(wallz, s.walls.box + 4, 4 * sizeof(float));
}
// Lambert wrap: softer than the lamp pass's (LAMP_WRAP): the smooth normals turn away from a lamp
// more than the depth's do, and the title's own vertex lighting is soft.
const float CHARLIT_WRAP = 0.5f;
// World lighting's wrap: the world's normals are the mesh's own (flat walls, sharp corners), so a
// light just behind a wall's plane barely reaches it.
const float WORLDLIT_WRAP = 0.15f;

static void charlit_free() {
    CharLit &c = s.charlit;
    for (VkShaderModule m : {c.vs, c.fs})
        if (m) vkDestroyShaderModule(s.dev, m, nullptr);
    if (c.layout) vkDestroyPipelineLayout(s.dev, c.layout, nullptr);
    if (c.pool) vkDestroyDescriptorPool(s.dev, c.pool, nullptr);
    if (c.set_layout) vkDestroyDescriptorSetLayout(s.dev, c.set_layout, nullptr);
    free_buffer(&c.ubo);
    c = CharLit();
}
static bool charlit_create() {
    CharLit &c = s.charlit;
    if (!s.set_layout || make_buffer(&c.ubo, sizeof(CharLitUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return false;
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &c.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pi, nullptr, &c.pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = c.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &c.set_layout;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, &c.set) != VK_SUCCESS) return false;
    VkDescriptorBufferInfo bi{c.ubo.buf, 0, sizeof(CharLitUbo)};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = c.set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(s.dev, 1, &w, 0, nullptr);
    const VkDescriptorSetLayout sets[2] = {s.set_layout, c.set_layout};
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &c.layout) != VK_SUCCESS) return false;
    c.vs = shader(geom_lit_vert, sizeof geom_lit_vert);
    c.fs = shader(geom_lit_frag, sizeof geom_lit_frag);
    return c.vs && c.fs;
}
// At renderer_init: the character lighting resources when RendererConfig.char_light is set. A
// failure frees them and leaves it off (renderer_draw_lit draws as renderer_draw).
static void charlit_init() {
    if (!cfg.char_light && !cfg.world_light) return;
    if (charlit_create()) {
        s.charlit.ok = true;
        return;
    }
    charlit_free();
}
// At the start of the frame's commands (outside any render pass): the frame's lights into the
// uniform block, after the previous frames' reads of it (one queue), before this frame's.
static void charlit_update() {
    CharLit &c = s.charlit;
    if (!c.ok || !c.marked) return;
    const RLampLights &p = c.p;
    CharLitUbo u{};
    const float st = p.strength > 0 ? p.strength : 0;
    u.cam[0] = 1.0f / (p.px > 0 ? p.px : 1.0f);
    u.cam[1] = 1.0f / (p.py > 0 ? p.py : 1.0f);
    u.cam[2] = (float)(p.n < RENDERER_LAMP_MAX ? p.n : RENDERER_LAMP_MAX);
    u.cam[3] = p.char_gain > 0 ? p.char_gain : 0.0f;
    u.params[0] = LAMP_FALLOFF_K;
    u.params[1] = CHARLIT_WRAP;
    u.params[2] = p.world_gain > 0 ? p.world_gain : 0.0f;
    u.params[3] = WORLDLIT_WRAP;
    u.opts[0] = p.title_shadow_full ? 1.0f : 0.0f;
    u.opts[1] = s.relief.parallax;   // --parallax depth (relief draws with flag 8)
    light_walls_fill(p, u.wto, u.wall, u.wallz);
    if (cfg.hair > 0 || cfg.skin > 0 || cfg.eyes > 0 || cfg.cloth > 0) {   // the hair, skin and material variants' world up (view y when unknown) and strengths
        const float l = sqrtf(p.up_view[0] * p.up_view[0] + p.up_view[1] * p.up_view[1] + p.up_view[2] * p.up_view[2]);
        for (int k = 0; k < 3; k++) u.hair[k] = l > 1e-3f ? p.up_view[k] / l : k == 1 ? 1.0f : 0.0f;
        u.hair[3] = cfg.hair;
        u.skin[0] = cfg.skin;
        u.skin[1] = cfg.eyes;
        u.skin[2] = cfg.cloth;
    }
    for (uint32_t i = 0; i < p.n && i < RENDERER_LAMP_MAX; i++) {
        const RLamp &l = p.l[i];
        for (int k = 0; k < 3; k++) u.pos_r[i][k] = l.pos[k];
        u.pos_r[i][3] = l.radius > 1e-3f ? l.radius : 1e-3f;
        for (int k = 0; k < 3; k++) u.col[i][k] = l.rgb[k] * (l.intensity > 0 ? l.intensity : 0) * st;
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    const VkPipelineStageFlags shaders = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    vkCmdPipelineBarrier(s.cmd, shaders, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    vkCmdUpdateBuffer(s.cmd, c.ubo.buf, 0, sizeof u, &u);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, shaders, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

// ---- Lamp character shadows (RendererConfig.lamp_shadow, renderer_lamp_shadows) ----

static void lshadow_free() {
    free_buffer(&s.lshadow.buf);
    s.lshadow = LampShadow();
}
static void walls_free() {
    free_image(&s.walls.img);
    s.walls = WallMap();
}
// At renderer_init, before any descriptor set is written: the empty map, kept with lamp shadows or
// light walls, created for the lamp pass's binding alone otherwise. A failure leaves maps ignored
// (and the lamp pass off: lamp_create needs the image).
static void walls_init() {
    if (!cfg.lamp_shadow && !cfg.light_walls && !cfg.lamp && !cfg.volfog) return;
    if (!wet_material_image(&s.walls.img, 1, 1, nullptr)) {
        walls_free();
        return;
    }
    s.walls.ok = cfg.lamp_shadow || cfg.light_walls || cfg.volfog;
}
// At renderer_init, before any descriptor set is written (they bind the buffer). A failure leaves
// it off: the shadow word never gets bit 14.
static void lshadow_init() {
    if (!cfg.lamp_shadow) return;
    if (!make_buffer(&s.lshadow.buf, sizeof(LampShadowGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        s.lshadow.ok = true;
        s.lshadow.loaded = true;   // cleared by the first frame
        return;
    }
    lshadow_free();
}
// At the start of the frame's commands (outside any render pass): the frame's lights, or none,
// after the previous frames' reads (one queue), before this frame's.
static void lshadow_update() {
    LampShadow &c = s.lshadow;
    if (!c.ok) return;
    c.executed = 0;
    if (!c.marked && !c.loaded) return;
    LampShadowGpu u{};
    if (c.marked) {
        const RLampShadows &p = c.p;
        const uint32_t n = p.n < RENDERER_LAMP_SHADOW_MAX ? p.n : RENDERER_LAMP_SHADOW_MAX;
        memcpy(u.inv, p.to_world, sizeof u.inv);
        u.params[0] = p.depth_a;
        u.params[1] = p.depth_b;
        u.params[2] = p.strength < 0 ? 0 : p.strength > 1 ? 1 : p.strength;
        u.params[3] = p.depth_a > 0 && p.depth_b > 0 ? (float)n : 0.0f;
        u.shape[0] = p.height;
        u.shape[1] = p.length > 1e-3f ? p.length : 1e-3f;
        u.shape[2] = p.start;
        u.shape[3] = p.dir_share < 0 ? 0 : p.dir_share > 1 ? 1 : p.dir_share;
        u.fade[0] = p.min_angle;
        u.fade[1] = p.full_angle;
        u.fade[2] = p.low_length < 0 ? 0 : p.low_length > 1 ? 1 : p.low_length;
        u.fade[3] = p.title_keep ? 1.0f : 0.0f;
        memcpy(u.eye, p.eye, sizeof p.eye);
        if (!p.no_walls) {   // --lamp-shadows-walls 0: the map is loaded for the lights only
            memcpy(u.wall, s.walls.box, sizeof u.wall);
            memcpy(u.wallz, s.walls.box + 4, sizeof u.wallz);
        }
        for (uint32_t i = 0; i < n; i++) {
            const RLampShadowLight &l = p.l[i];
            memcpy(u.stq[i], l.stq, sizeof u.stq[i]);
            memcpy(u.pos[i], l.pos, sizeof l.pos);
            u.pos[i][3] = l.radius > 1e-3f ? l.radius : 1e-3f;
            u.wt[i][0] = l.weight < 0 ? 0 : l.weight > 1 ? 1 : l.weight;
        }
        c.executed = (uint32_t)u.params[3];
    }
    c.loaded = c.executed != 0;
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    vkCmdUpdateBuffer(s.cmd, c.buf.buf, 0, sizeof u, &u);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

// ---- Soft particles (RendererConfig.soft, renderer_soft) ----

static void soft_free() {
    Soft &f = s.soft;
    if (f.fs) vkDestroyShaderModule(s.dev, f.fs, nullptr);
    if (f.layout) vkDestroyPipelineLayout(s.dev, f.layout, nullptr);
    if (f.pool) vkDestroyDescriptorPool(s.dev, f.pool, nullptr);
    if (f.set_layout) vkDestroyDescriptorSetLayout(s.dev, f.set_layout, nullptr);
    if (f.pass) vkDestroyRenderPass(s.dev, f.pass, nullptr);
    f = Soft();
}
static bool soft_create() {
    Soft &f = s.soft;
    if (!s.depth_sampled || !split_passes_ready() || create_main_pass(&f.pass, SOFT_PASS)) return false;
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &f.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pi, nullptr, &f.pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = f.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &f.set_layout;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, &f.set) != VK_SUCCESS) return false;
    VkDescriptorImageInfo ii{s.nearest_sampler, s.depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = f.set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(s.dev, 1, &w, 0, nullptr);
    const VkDescriptorSetLayout sets[2] = {s.set_layout, f.set_layout};
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &f.layout) != VK_SUCCESS) return false;
    f.fs = s.samples != VK_SAMPLE_COUNT_1_BIT ? shader(geom_soft_ms_frag, sizeof geom_soft_ms_frag) : shader(geom_soft_frag, sizeof geom_soft_frag);
    return f.fs != VK_NULL_HANDLE;
}
// At renderer_init: the soft particle resources when RendererConfig.soft is set and the depth target
// can be sampled (create_targets). A failure frees them and leaves soft particles off.
static void soft_init() {
    if (!cfg.soft) return;
    if (soft_create()) {
        s.soft.ok = true;
        return;
    }
    soft_free();
}
// The soft split inside the frame's render pass (begun with s.split_pass): ends it (color and depth
// stored, MSAA resolved) and resumes it with the soft variant (s.soft.live), the depth read-only.
static void soft_split() {
    vkCmdEndRenderPass(s.cmd);
    barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = resume_pass();
    rb.framebuffer = s.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
}

static void ssao_free() {
    Ssao &a = s.ssao;
    gtao_free();
    for (VkPipeline p : {a.ao, a.blur, a.comp})
        if (p) vkDestroyPipeline(s.dev, p, nullptr);
    for (VkShaderModule m : {a.vs, a.ao_fs, a.blur_fs, a.comp_fs})
        if (m) vkDestroyShaderModule(s.dev, m, nullptr);
    if (a.layout) vkDestroyPipelineLayout(s.dev, a.layout, nullptr);
    if (a.pool) vkDestroyDescriptorPool(s.dev, a.pool, nullptr);
    if (a.set_layout) vkDestroyDescriptorSetLayout(s.dev, a.set_layout, nullptr);
    for (VkFramebuffer f : {a.fb_a, a.fb_b, a.comp_fb})
        if (f) vkDestroyFramebuffer(s.dev, f, nullptr);
    for (VkRenderPass p : {a.pass, a.comp_pass})
        if (p) vkDestroyRenderPass(s.dev, p, nullptr);
    free_image(&a.a);
    free_image(&a.b);
    a = Ssao();
}

// At renderer_init: the ssao resources when RendererConfig.ssao is set and the depth target can be
// sampled (create_targets). A failure frees them and leaves ssao off (renderer_ssao_on()).
static void ssao_init() {
    if (!cfg.ssao) return;
    if (ssao_create()) {
        s.ssao.ok = true;
        s.ssao.strength = cfg.ssao_strength > 0 ? cfg.ssao_strength : 0;
        s.ssao.radius = cfg.ssao_radius > 0 ? cfg.ssao_radius : 1.0f;
        if (cfg.ssao_mode == SSAO_MODE_GTAO) {   // XeGTAO, or the classic passes where it cannot run
            if (gtao_create()) s.ssao.gtao = true;
            else gtao_free();
        }
        return;
    }
    ssao_free();
}

// One half-size pass: pipeline pipe reading set into image dst (framebuffer fb).
static void ssao_half(Image &dst, VkFramebuffer fb, VkPipeline pipe, VkDescriptorSet set, const SsaoPush &pc) {
    Ssao &a = s.ssao;
    // After the previous frame's or pass's reads of the image (one queue).
    barrier(dst.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = a.pass;
    rb.framebuffer = fb;
    rb.renderArea = {{0, 0}, {(uint32_t)a.hw, (uint32_t)a.hh}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(s.cmd, a.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport v{0, 0, (float)a.hw, (float)a.hh, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    barrier(dst.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

// --ssao-mode gtao: XeGTAO's passes (outside any render pass; the depth readable by compute) for
// the frame's projection p: the depth MIPs, the main pass into gw[0]; with TAA (the noise changes
// per frame) the temporal pass accumulates it into gh[] through the frame's reprojection and one
// denoise pass reads that, without TAA two denoise passes (fixed noise). Returns the image the last
// pass wrote (occlusion, view depth), readable by the composite.
struct GtaoTemporalPush {
    float rx[4], ry[4], rw[4], scr[4], size[4], misc[4];
};
// The temporal pass: share of the current frame, plus this much per
// working texel moved beyond the first (up to 0.5), and the relative view-depth tolerance of "the
// same surface". Shares 0.06-0.1 and tolerances 0.05-0.15 measured alike in renderer_test.
const float GTAO_TEMPORAL_SHARE = 0.1f, GTAO_TEMPORAL_MOTION_SHARE = 0.1f, GTAO_TEMPORAL_DEPTH_TOL = 0.05f;
static int gtao_run(const RSsao &p) {
    Ssao &a = s.ssao;
    GtaoConsts k;
    const SsaoWorkSize ws = ssao_work_size(s.ow, s.oh, s.ss);
    gtao_constants(p.depth_a, p.depth_b, p.px, p.py, ws, s.ow, s.oh, a.radius, &k);
    GtaoPush pc = {{k.depth_unpack[0], k.depth_unpack[1], GTAO_DEAD_ZONE, (float)a.levels},
                   {k.ndc_to_view_mul[0], k.ndc_to_view_mul[1], k.ndc_to_view_add[0], k.ndc_to_view_add[1]},
                   {k.screen_mul[0], k.screen_mul[1], k.screen_add[0], k.screen_add[1]},
                   {(float)a.hw, (float)a.hh, (float)s.ow, (float)s.oh},
                   {k.effect_radius, k.falloff_range, k.sample_power, k.thin_compensation},
                   {k.final_power, k.mip_offset, SSAO_MAX_RADIUS * s.oh / (float)a.scale, a.taa_frame ? (float)(s.frame_serial % 64) : 0.0f},
                   {GTAO_DENOISE_BLUR_BETA, 0, GTAO_SURFACE_BIAS, 0}};
    const uint32_t gx = ((uint32_t)a.hw + 7) / 8, gy = ((uint32_t)a.hh + 7) / 8;
    gtao_to_write(a.gz.img, (uint32_t)a.levels);
    gtao_dispatch(a.gpre, a.gset_pre, pc, ((uint32_t)a.hw + 15) / 16, ((uint32_t)a.hh + 15) / 16);
    gtao_to_read(a.gz.img, (uint32_t)a.levels);
    gtao_to_write(a.gw[0].img);
    gtao_dispatch(a.gmain, a.gset_main, pc, gx, gy);
    gtao_to_read(a.gw[0].img);
    if (a.taa_frame) {
        const RTaa &t = s.taa.p;
        const bool hist = a.gh_serial && a.gh_serial + 1 == s.frame_serial && !t.reset && s.taa.size_serial != s.frame_serial;   // --dynres: not across a size change (no reprojection)
        const int n = 1 - a.gh_cur;
        GtaoTemporalPush tp = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0},   // no reprojection: no motion
                               {k.screen_mul[0], k.screen_mul[1], k.screen_add[0], k.screen_add[1]},
                               {(float)a.hw, (float)a.hh, 0, 0},
                               {GTAO_TEMPORAL_SHARE, hist ? 1.0f : 0.0f, GTAO_TEMPORAL_DEPTH_TOL, GTAO_TEMPORAL_MOTION_SHARE}};
        if (t.valid) {
            memcpy(tp.rx, t.reproject[0], sizeof tp.rx);
            memcpy(tp.ry, t.reproject[1], sizeof tp.ry);
            memcpy(tp.rw, t.reproject[2], sizeof tp.rw);
        }
        if (!hist) gtao_to_write(a.gh[a.gh_cur].img);   // bound but not read: a defined layout
        gtao_to_write(a.gh[n].img);
        vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, a.gtmp);
        vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, a.glayout, 0, 1, &a.gset_tmp[n], 0, nullptr);
        vkCmdPushConstants(s.cmd, a.glayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof tp, &tp);
        vkCmdDispatch(s.cmd, gx, gy, 1);
        gtao_to_read(a.gh[n].img);
        a.gh_cur = n;
        a.gh_serial = s.frame_serial;
        pc.den[1] = 1.0f;
        gtao_to_write(a.gw[1].img);
        gtao_dispatch(a.gden, a.gset_den_h[n], pc, gx, gy);
        gtao_to_read(a.gw[1].img);
        return 1;
    }
    a.gh_serial = 0;   // a frame without TAA: the next one starts its history afresh
    int cur = 0;
    for (int i = 0; i < 2; i++) {
        pc.den[1] = i == 1 ? 1.0f : 0.0f;
        gtao_to_write(a.gw[1 - cur].img);
        gtao_dispatch(a.gden, a.gset_den[cur], pc, gx, gy);
        gtao_to_read(a.gw[1 - cur].img);
        cur = 1 - cur;
    }
    return cur;
}

// Executes a depth split inside the frame's render pass (begun with s.split_pass or s.resume_pass):
// ends it (color and depth stored); with ao, samples the depth into the occlusion (image a), blurs
// it across (into b) and down (back into a) and multiplies it onto the color target in the
// composite pass (the multisampled color with MSAA: every sample of a pixel by the same factor);
// with sp, draws the rain splashes over it (splash_draw); then resumes the frame's pass
// (s.resume_pass) with the depth back as its attachment.
static void splash_mask(const RRainSplash &p);
static void splash_comp(const RRainSplash &p, bool in_pass);
static void lamp_light(const RLampLights &p);
static void lamp_comp(const RLampLights &p);
static void contact_mask(const RContact &p);
static void contact_comp(const RContact &p, bool in_pass);
// cs: the contact shadows, masked first and composited in the same pass as the others (multiplied
// after the lamp light, before the occlusion and splashes), or in a pass of their own.
static void ssao_exec(const RSsao *ao, const RRainSplash *sp, const RLampLights *lamp, const RContact *cs = nullptr) {
    Ssao &a = s.ssao;
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    // The shader stages reading the depth and the light mask: the compute passes too with gtao.
    const VkPipelineStageFlags rd = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | (ao && a.gtao ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : 0);
    vkCmdEndRenderPass(s.cmd);
    // The pass's outgoing dependency covers its depth writes for fragment shader reads.
    barrier(s.depth.img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, rd, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
    // The single-sample color (the target, or the resolve target with MSAA) back to an attachment;
    // with the light mask first readable (its alpha), until the reads are done (color_attach).
    // The lamp lights read the light mask in their light pass (outside the composite) whatever the
    // sample count.
    const bool mask = (ao && a.mask) || lamp;
    if (mask)
        barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, rd);
    else
        barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    auto color_attach = [rd] {
        barrier(s.color.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, rd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    };
    if (sp) splash_mask(*sp);
    gp_mark(GP_SPLASH);
    if (lamp) lamp_light(*lamp);
    gp_mark(GP_LAMP);
    if (cs) contact_mask(*cs);
    gp_mark(GP_CONTACT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderArea = {{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    const bool ms_read = ao && a.mask && ms;   // the ssao composite reads s.color with MSAA
    if (!ao && lamp) {   // the lamp composite alone (and the splashes in the same pass)
        color_attach();
        rb.renderPass = s.lamp.comp_pass;
        rb.framebuffer = s.lamp.comp_fb;
        vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        lamp_comp(*lamp);
        if (cs) contact_comp(*cs, true);
        if (sp) splash_comp(*sp, true);
        vkCmdEndRenderPass(s.cmd);
        barrier(ms ? s.msaa.img : s.color.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    } else if (ao) {
        const RSsao &p = *ao;
        const float px = p.px > 0 ? p.px : 1.0f, py = p.py > 0 ? p.py : 1.0f;
        SsaoPush pc = {{p.depth_a, p.depth_b, 1.0f / px, 1.0f / py},
                       {(float)s.ow, (float)s.oh, 1.0f / s.ow, 1.0f / s.oh},
                       {a.radius, a.strength, SSAO_BIAS, SSAO_MAX_RADIUS * s.oh},
                       {0, 0, (float)a.hw, (float)a.hh},
                       {}};
        VkDescriptorSet comp_set = a.set_a;   // the occlusion in image a (classic) or the last gtao image
        if (a.gtao) {
            comp_set = a.gset_comp[gtao_run(p)];
        } else {
            ssao_half(a.a, a.fb_a, a.ao, a.set_b, pc);   // set_b: binding 1 (image b) is not read by this pass
            pc.dir[0] = 1;
            ssao_half(a.b, a.fb_b, a.blur, a.set_a, pc);
            pc.dir[0] = 0;
            pc.dir[1] = 1;
            ssao_half(a.a, a.fb_a, a.blur, a.set_b, pc);
        }
        gp_mark(GP_AO);
        if (mask && !ms_read) color_attach();   // the composite draws into s.color (or does not read it)
        rb.renderPass = a.comp_pass;
        rb.framebuffer = a.comp_fb;
        vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        if (lamp) lamp_comp(*lamp);   // lit first, then occluded (the same pass: one load and store)
        if (cs) contact_comp(*cs, true);
        vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.comp);
        vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.layout, 0, 1, &comp_set, 0, nullptr);
        vkCmdPushConstants(s.cmd, a.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        VkViewport v{0, 0, (float)s.ow, (float)s.oh, 0, 1};
        vkCmdSetViewport(s.cmd, 0, 1, &v);
        vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
        vkCmdDraw(s.cmd, 3, 1, 0, 0);
        if (sp) splash_comp(*sp, true);   // in the same pass: no second load and store of the target
        vkCmdEndRenderPass(s.cmd);
        if (ms_read) color_attach();   // the composite read it
        barrier(ms ? s.msaa.img : s.color.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        a.executed++;
    } else if (cs) {   // the contact shadows (and the splashes in the same pass)
        contact_comp(*cs, false);
        if (sp) splash_comp(*sp, true);
        vkCmdEndRenderPass(s.cmd);
        barrier(ms ? s.msaa.img : s.color.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    } else if (sp) {
        splash_comp(*sp, false);
    }
    barrier(s.depth.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, rd,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
    gp_mark(GP_COMPOSITE);
    rb.renderPass = resume_pass();
    rb.framebuffer = s.fb;
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
}

// ---- Lamp lights (RendererConfig.lamp, renderer_lamp_lights) ----

// The uniform block of shaders/lamp.frag (std140): view position and radius, colour x intensity x
// strength per light.
struct LampUbo {
    float pos_r[RENDERER_LAMP_MAX][4];
    float col[RENDERER_LAMP_MAX][4];
    float wto[3][4];   // light walls (light_walls_fill), as CharLitUbo
    float wall[4];
    float wallz[4];
};
const float LAMP_WRAP = 0.35f;   // Lambert wrap: soft like the title's vertex lighting, forgiving to noisy depth normals

static void lamp_free() {
    Lamp &a = s.lamp;
    for (VkPipeline p : {a.light, a.comp})
        if (p) vkDestroyPipeline(s.dev, p, nullptr);
    for (VkShaderModule m : {a.vs, a.fs})
        if (m) vkDestroyShaderModule(s.dev, m, nullptr);
    if (a.layout) vkDestroyPipelineLayout(s.dev, a.layout, nullptr);
    if (a.pool) vkDestroyDescriptorPool(s.dev, a.pool, nullptr);
    if (a.set_layout) vkDestroyDescriptorSetLayout(s.dev, a.set_layout, nullptr);
    for (VkFramebuffer f : {a.fb, a.comp_fb})
        if (f) vkDestroyFramebuffer(s.dev, f, nullptr);
    for (VkRenderPass p : {a.pass, a.comp_pass})
        if (p) vkDestroyRenderPass(s.dev, p, nullptr);
    free_image(&a.img);
    free_buffer(&a.ubo);
    a = Lamp();
}

static bool lamp_create() {
    Lamp &a = s.lamp;
    const VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;   // Vulkan 1.0 requires it as a sampled color attachment
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(s.phys, fmt, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (!s.depth_sampled || (fp.optimalTilingFeatures & need) != need || !split_passes_ready()) return false;
    const SsaoWorkSize ws = ssao_work_size(s.ow, s.oh, s.ss);
    a.hw = ws.w;
    a.hh = ws.h;
    a.scale = ws.scale;
    a.chars = s.chars.ok;
    if (make_image(&a.img, fmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)a.hw,
                   (uint32_t)a.hh))
        return false;
    if (make_buffer(&a.ubo, sizeof(LampUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;
    memset(a.ubo.map, 0, sizeof(LampUbo));
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    if (!ssao_pass(fmt, VK_SAMPLE_COUNT_1_BIT, false, &a.pass) || !ssao_pass(VK_FORMAT_B8G8R8A8_UNORM, s.samples, true, &a.comp_pass)) return false;
    a.fb = ssao_fb(a.pass, a.img.view, a.hw, a.hh);
    a.comp_fb = ssao_fb(a.comp_pass, ms ? s.msaa.view : s.color.view, s.ow, s.oh);
    if (!a.fb || !a.comp_fb) return false;
    if (!s.walls.img.view) return false;   // binding 5 (walls_init)
    VkDescriptorSetLayoutBinding lb[6];
    for (uint32_t i = 0; i < 6; i++)
        lb[i] = {i, i == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 6;
    li.pBindings = lb;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &a.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &a.pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = a.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &a.set_layout;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, &a.set) != VK_SUCCESS) return false;
    // Every read is a texelFetch: the nearest sampler (clamped) for all of them.
    const VkDescriptorImageInfo ii[6] = {
        {s.nearest_sampler, s.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {s.nearest_sampler, a.img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        a.chars ? VkDescriptorImageInfo{s.nearest_sampler, s.chars.depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}
                : VkDescriptorImageInfo{s.nearest_sampler, s.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},   // placeholder, never read
        {s.nearest_sampler, s.color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {},   // 4: the uniform block
        {s.nearest_sampler, s.walls.img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};   // 5: the wall map (lamp_bind_walls)
    const VkDescriptorBufferInfo bi{a.ubo.buf, 0, sizeof(LampUbo)};
    VkWriteDescriptorSet wr[6]{};
    for (uint32_t i = 0; i < 6; i++) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = a.set;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = i == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        if (i == 4) wr[i].pBufferInfo = &bi;
        else wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(s.dev, 6, wr, 0, nullptr);
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SsaoPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &a.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &a.layout) != VK_SUCCESS) return false;
    a.vs = shader(glass_vert, sizeof glass_vert);
    a.fs = ms ? shader(lamp_ms_frag, sizeof lamp_ms_frag) : shader(lamp_frag, sizeof lamp_frag);
    if (!a.vs || !a.fs) return false;
    const int32_t consts[2][3] = {{0, a.scale, a.chars ? 1 : 0}, {1, a.scale, a.chars ? 1 : 0}};
    const VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    const VkSpecializationInfo spec0{3, me, sizeof consts[0], consts[0]}, spec1{3, me, sizeof consts[1], consts[1]};
    a.light = fullscreen_pipeline(a.vs, a.layout, a.fs, a.pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE, false, &spec0);
    a.comp = fullscreen_pipeline(a.vs, a.layout, a.fs, a.comp_pass, s.samples, FS_LIGHTEN, false, &spec1);
    return a.light && a.comp;
}

// After renderer_lamp_wall_map replaced the image (the GPU idle): the lamp pass's binding 5.
static void lamp_bind_walls() {
    if (!s.lamp.set || !s.walls.img.view) return;
    const VkDescriptorImageInfo ii{s.nearest_sampler, s.walls.img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = s.lamp.set;
    w.dstBinding = 5;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(s.dev, 1, &w, 0, nullptr);
}

// At renderer_init (after chars_init): the lamp resources when RendererConfig.lamp is set and the
// depth target can be sampled. A failure frees them and leaves lamp lights off.
static void lamp_init() {
    if (!cfg.lamp) return;
    if (lamp_create()) {
        s.lamp.ok = true;
        return;
    }
    lamp_free();
}

// Push constants of both lamp steps.
static SsaoPush lamp_push(const RLampLights &p) {
    const Lamp &a = s.lamp;
    const float px = p.px > 0 ? p.px : 1.0f, py = p.py > 0 ? p.py : 1.0f;
    return SsaoPush{{p.depth_a, p.depth_b, 1.0f / px, 1.0f / py},
                    {(float)s.ow, (float)s.oh, 1.0f / s.ow, 1.0f / s.oh},
                    {(float)p.n, p.char_gain >= 0 ? p.char_gain : 1.0f, LAMP_FALLOFF_K, LAMP_WRAP},
                    {p.no_world ? 1.0f : 0.0f, 0, (float)a.hw, (float)a.hh},
                    {}};
}

// Outside a render pass, the depth and s.color shader-readable: the lights into the uniform block,
// then the light at the working size into the image (left shader-readable).
static void lamp_light(const RLampLights &p) {
    Lamp &a = s.lamp;
    LampUbo u{};
    const float st = p.strength > 0 ? p.strength : 0;
    for (uint32_t i = 0; i < p.n && i < RENDERER_LAMP_MAX; i++) {
        const RLamp &l = p.l[i];
        u.pos_r[i][0] = l.pos[0];
        u.pos_r[i][1] = l.pos[1];
        u.pos_r[i][2] = l.pos[2];
        u.pos_r[i][3] = l.radius > 1e-3f ? l.radius : 1e-3f;
        for (int c = 0; c < 3; c++) u.col[i][c] = l.rgb[c] * (l.intensity > 0 ? l.intensity : 0) * st;
    }
    light_walls_fill(p, u.wto, u.wall, u.wallz);
    // After the previous frames' reads of the block (one queue), before this frame's.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    vkCmdUpdateBuffer(s.cmd, a.ubo.buf, 0, sizeof u, &u);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    const SsaoPush pc = lamp_push(p);
    barrier(a.img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = a.pass;
    rb.framebuffer = a.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)a.hw, (uint32_t)a.hh}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.light);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.layout, 0, 1, &a.set, 0, nullptr);
    vkCmdPushConstants(s.cmd, a.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport v{0, 0, (float)a.hw, (float)a.hh, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    barrier(a.img.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

// Inside a composite pass on the frame's color target (lamp or ssao comp_pass): dst x (1 + light).
static void lamp_comp(const RLampLights &p) {
    Lamp &a = s.lamp;
    const SsaoPush pc = lamp_push(p);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.comp);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.layout, 0, 1, &a.set, 0, nullptr);
    vkCmdPushConstants(s.cmd, a.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport v{0, 0, (float)s.ow, (float)s.oh, 0, 1};
    VkRect2D sc{{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &sc);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    a.executed++;
}

// ---- Light shafts (RendererConfig.shafts, renderer_light_shafts) ----

// The uniform block of shaders/shafts.frag (std140).
struct ShaftUbo {
    float pos_len[RENDERER_SHAFT_MAX][4];
    float col_kind[RENDERER_SHAFT_MAX][4];
    float axis[RENDERER_SHAFT_MAX][4];
};
const int SHAFT_STEPS = 10, SHAFT_STEPS_CHEAP = 5;

static void shaft_free() {
    Shaft &a = s.shaft;
    for (VkPipeline p : {a.light, a.comp})
        if (p) vkDestroyPipeline(s.dev, p, nullptr);
    for (VkShaderModule m : {a.vs, a.fs})
        if (m) vkDestroyShaderModule(s.dev, m, nullptr);
    if (a.layout) vkDestroyPipelineLayout(s.dev, a.layout, nullptr);
    if (a.pool) vkDestroyDescriptorPool(s.dev, a.pool, nullptr);
    if (a.set_layout) vkDestroyDescriptorSetLayout(s.dev, a.set_layout, nullptr);
    for (VkFramebuffer f : {a.fb, a.comp_fb})
        if (f) vkDestroyFramebuffer(s.dev, f, nullptr);
    for (VkRenderPass p : {a.pass, a.comp_pass})
        if (p) vkDestroyRenderPass(s.dev, p, nullptr);
    free_image(&a.img);
    free_buffer(&a.ubo);
    a = Shaft();
}

static bool shaft_create() {
    Shaft &a = s.shaft;
    const VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;   // Vulkan 1.0 requires it as a sampled color attachment
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(s.phys, fmt, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (!s.depth_sampled || (fp.optimalTilingFeatures & need) != need || !split_passes_ready()) return false;
    const SsaoWorkSize ws = ssao_work_size(s.ow, s.oh, s.ss);
    a.scale = ws.scale * (cfg.shafts_cheap ? 2 : 1);
    a.hw = (s.ow + a.scale - 1) / a.scale;
    a.hh = (s.oh + a.scale - 1) / a.scale;
    a.steps = cfg.shafts_cheap ? SHAFT_STEPS_CHEAP : SHAFT_STEPS;
    if (make_image(&a.img, fmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)a.hw,
                   (uint32_t)a.hh))
        return false;
    if (make_buffer(&a.ubo, sizeof(ShaftUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;
    memset(a.ubo.map, 0, sizeof(ShaftUbo));
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    if (!ssao_pass(fmt, VK_SAMPLE_COUNT_1_BIT, false, &a.pass) || !ssao_pass(VK_FORMAT_B8G8R8A8_UNORM, s.samples, true, &a.comp_pass)) return false;
    a.fb = ssao_fb(a.pass, a.img.view, a.hw, a.hh);
    a.comp_fb = ssao_fb(a.comp_pass, ms ? s.msaa.view : s.color.view, s.ow, s.oh);
    if (!a.fb || !a.comp_fb) return false;
    VkDescriptorSetLayoutBinding lb[3];
    for (uint32_t i = 0; i < 3; i++)
        lb[i] = {i, i == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 3;
    li.pBindings = lb;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &a.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &a.pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = a.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &a.set_layout;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, &a.set) != VK_SUCCESS) return false;
    // Every read is a texelFetch: the nearest sampler (clamped) for both images.
    const VkDescriptorImageInfo ii[2] = {{s.nearest_sampler, s.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                         {s.nearest_sampler, a.img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    const VkDescriptorBufferInfo bi{a.ubo.buf, 0, sizeof(ShaftUbo)};
    VkWriteDescriptorSet wr[3]{};
    for (uint32_t i = 0; i < 3; i++) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = a.set;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        if (i == 2) wr[i].pBufferInfo = &bi;
        else wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(s.dev, 3, wr, 0, nullptr);
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SsaoPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &a.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &a.layout) != VK_SUCCESS) return false;
    a.vs = shader(glass_vert, sizeof glass_vert);
    a.fs = ms ? shader(shafts_ms_frag, sizeof shafts_ms_frag) : shader(shafts_frag, sizeof shafts_frag);
    if (!a.vs || !a.fs) return false;
    const int32_t consts[2][3] = {{0, a.scale, a.steps}, {1, a.scale, a.steps}};
    const VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    const VkSpecializationInfo spec0{3, me, sizeof consts[0], consts[0]}, spec1{3, me, sizeof consts[1], consts[1]};
    a.light = fullscreen_pipeline(a.vs, a.layout, a.fs, a.pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE, false, &spec0);
    a.comp = fullscreen_pipeline(a.vs, a.layout, a.fs, a.comp_pass, s.samples, FS_ADD, false, &spec1);
    return a.light && a.comp;
}

// At renderer_init: the light shaft resources when RendererConfig.shafts is set and the depth
// target can be sampled. A failure frees them and leaves the shafts off.
static void shaft_init() {
    if (!cfg.shafts) return;
    if (shaft_create()) {
        s.shaft.ok = true;
        return;
    }
    shaft_free();
}

// A pass split of its own (inside the frame's main pass, as ssao_exec): the lights into the uniform
// block, the scattered light at the working size, then its composite added to the frame's color.
static void shaft_exec(const RLightShafts &p) {
    Shaft &a = s.shaft;
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    vkCmdEndRenderPass(s.cmd);
    barrier(s.depth.img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 1,
            VK_IMAGE_ASPECT_DEPTH_BIT);
    barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    ShaftUbo u{};
    for (uint32_t i = 0; i < p.n && i < RENDERER_SHAFT_MAX; i++) {
        const RShaft &l = p.l[i];
        for (int c = 0; c < 3; c++) {
            u.pos_len[i][c] = l.pos[c];
            u.col_kind[i][c] = l.rgb[c] > 0 ? l.rgb[c] : 0;
            u.axis[i][c] = l.axis[c];
        }
        u.pos_len[i][3] = l.length > 1e-2f ? l.length : 1e-2f;
        u.col_kind[i][3] = l.kind ? 1.0f : 0.0f;
    }
    // After the previous frames' reads of the block (one queue), before this frame's.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    vkCmdUpdateBuffer(s.cmd, a.ubo.buf, 0, sizeof u, &u);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    const float px = p.px > 0 ? p.px : 1.0f, py = p.py > 0 ? p.py : 1.0f;
    const SsaoPush pc{{p.depth_a, p.depth_b, 1.0f / px, 1.0f / py},
                      {(float)s.ow, (float)s.oh, 1.0f / s.ow, 1.0f / s.oh},
                      {(float)p.n, p.density > 0 ? p.density : 0, p.max_dist > 0 ? p.max_dist : 1.0f, 0},
                      {0, 0, (float)a.hw, (float)a.hh},
                      {}};
    barrier(a.img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = a.pass;
    rb.framebuffer = a.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)a.hw, (uint32_t)a.hh}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.light);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.layout, 0, 1, &a.set, 0, nullptr);
    vkCmdPushConstants(s.cmd, a.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport v{0, 0, (float)a.hw, (float)a.hh, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    barrier(a.img.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    rb.renderPass = a.comp_pass;
    rb.framebuffer = a.comp_fb;
    rb.renderArea = {{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.comp);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.layout, 0, 1, &a.set, 0, nullptr);
    vkCmdPushConstants(s.cmd, a.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    v = {0, 0, (float)s.ow, (float)s.oh, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    barrier(ms ? s.msaa.img : s.color.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    barrier(s.depth.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
    rb.renderPass = resume_pass();
    rb.framebuffer = s.fb;
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    a.executed++;
}

// ---- Volumetric fog (RendererConfig.volfog, renderer_volfog) ----

// The uniform block of shaders/volfog_common.glsl (std140).
struct VolfogUbo {
    float proj[4], range[4], grid[4], up[4], jit[4], fog[4];
    float prev[3][4], prevp[4], tw[3][4], wall[4], wallz[4], depth[4], mist[4];
    float lpos[RENDERER_VOLFOG_MAX][4], lcol[RENDERER_VOLFOG_MAX][4];
};

static void volfog_free() {
    Volfog &a = s.volfog;
    for (VkPipeline p : {a.polar_pipe, a.inject_pipe, a.integ_pipe, a.comp})
        if (p) vkDestroyPipeline(s.dev, p, nullptr);
    for (VkShaderModule m : {a.polar_cs, a.inject_cs, a.integ_cs, a.vs, a.fs})
        if (m) vkDestroyShaderModule(s.dev, m, nullptr);
    if (a.layout) vkDestroyPipelineLayout(s.dev, a.layout, nullptr);
    if (a.pool) vkDestroyDescriptorPool(s.dev, a.pool, nullptr);
    if (a.set_layout) vkDestroyDescriptorSetLayout(s.dev, a.set_layout, nullptr);
    if (a.comp_fb) vkDestroyFramebuffer(s.dev, a.comp_fb, nullptr);
    if (a.comp_pass) vkDestroyRenderPass(s.dev, a.comp_pass, nullptr);
    for (Image *im : {&a.vol[0], &a.vol[1], &a.integ}) free_image(im);
    free_buffer(&a.ubo);
    free_buffer(&a.polar);
    a = Volfog();
}

// An RGBA16F 3D image of the grid, storage and sampled.
static bool volfog_image(Image *im, const VolfogGrid &g) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_3D;
    ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent = {(uint32_t)g.w, (uint32_t)g.h, (uint32_t)g.d};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (VKC(vkCreateImage, s.dev, &ii, nullptr, &im->img) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(s.dev, im->img, &req);
    int type = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = find_memory(req.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)type;
    if (type < 0 || VKC(vkAllocateMemory, s.dev, &ai, nullptr, &im->mem) != VK_SUCCESS) return false;
    vram_add(type, req.size, false, &im->vram, &im->cat);
    VKC(vkBindImageMemory, s.dev, im->img, im->mem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im->img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return VKC(vkCreateImageView, s.dev, &vi, nullptr, &im->view) == VK_SUCCESS;
}

static VkPipeline volfog_compute(VkShaderModule cs) {
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = cs;
    ci.stage.pName = "main";
    ci.layout = s.volfog.layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (VKC(vkCreateComputePipelines, s.dev, s.pcache, 1, &ci, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    return p;
}

// Set binding 1 of both sets: the wall map (renderer_lamp_wall_map replaced it, the GPU idle).
static void volfog_bind_walls() {
    Volfog &a = s.volfog;
    if (!a.set[0] || !s.walls.img.view) return;
    const VkDescriptorImageInfo ii{s.nearest_sampler, s.walls.img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[2]{};
    for (int k = 0; k < 2; k++) {
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[k].dstSet = a.set[k];
        w[k].dstBinding = 1;
        w[k].descriptorCount = 1;
        w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[k].pImageInfo = &ii;
    }
    vkUpdateDescriptorSets(s.dev, 2, w, 0, nullptr);
}

static bool volfog_create() {
    Volfog &a = s.volfog;
    const VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;   // Vulkan 1.0 requires it as a storage image
    a.g = volfog_grid(cfg.volfog_quality);
    VkImageFormatProperties ip;
    if (vkGetPhysicalDeviceImageFormatProperties(s.phys, fmt, VK_IMAGE_TYPE_3D, VK_IMAGE_TILING_OPTIMAL,
                                                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, &ip) != VK_SUCCESS ||
        ip.maxExtent.width < (uint32_t)a.g.w || ip.maxExtent.height < (uint32_t)a.g.h || ip.maxExtent.depth < (uint32_t)a.g.d)
        return false;
    if (!s.depth_sampled || !s.walls.img.view || !split_passes_ready()) return false;
    MemTag tag(RENDERER_VRAM_EFFECTS);
    for (Image *im : {&a.vol[0], &a.vol[1], &a.integ})
        if (!volfog_image(im, a.g)) return false;
    if (make_buffer(&a.ubo, sizeof(VolfogUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;
    memset(a.ubo.map, 0, sizeof(VolfogUbo));
    const VkDeviceSize polar_bytes = sizeof(float) * 8 * VOLFOG_POLAR_BINS * RENDERER_VOLFOG_MAX;   // two vec4 per bin
    if (make_buffer(&a.polar, polar_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return false;
    memset(a.polar.map, 0, (size_t)polar_bytes);
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    if (!ssao_pass(VK_FORMAT_B8G8R8A8_UNORM, s.samples, true, &a.comp_pass)) return false;
    a.comp_fb = ssao_fb(a.comp_pass, ms ? s.msaa.view : s.color.view, s.ow, s.oh);
    if (!a.comp_fb) return false;
    // 0 depth, 1 wall map, 2 uniform block, 3 polar maps, 4 injection out, 5 history, 6 injection
    // in, 7 integral out, 8 integral in.
    const VkDescriptorType types[9] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    VkDescriptorSetLayoutBinding lb[9];
    for (uint32_t i = 0; i < 9; i++) lb[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 9;
    li.pBindings = lb;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &a.set_layout) != VK_SUCCESS) return false;
    const VkDescriptorPoolSize ps[4] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10},
                                        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2;
    pci.poolSizeCount = 4;
    pci.pPoolSizes = ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &a.pool) != VK_SUCCESS) return false;
    const VkDescriptorSetLayout layouts[2] = {a.set_layout, a.set_layout};
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = a.pool;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, a.set) != VK_SUCCESS) return false;
    const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL;
    for (int k = 0; k < 2; k++) {
        const VkDescriptorImageInfo ii[9] = {{s.nearest_sampler, s.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                             {s.nearest_sampler, s.walls.img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                             {},
                                             {},
                                             {VK_NULL_HANDLE, a.vol[k].view, G},
                                             {s.clamp_sampler, a.vol[k ^ 1].view, G},
                                             {s.nearest_sampler, a.vol[k].view, G},
                                             {VK_NULL_HANDLE, a.integ.view, G},
                                             {s.clamp_sampler, a.integ.view, G}};
        const VkDescriptorBufferInfo bi[2] = {{a.ubo.buf, 0, sizeof(VolfogUbo)}, {a.polar.buf, 0, polar_bytes}};
        VkWriteDescriptorSet wr[9]{};
        for (uint32_t i = 0; i < 9; i++) {
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet = a.set[k];
            wr[i].dstBinding = i;
            wr[i].descriptorCount = 1;
            wr[i].descriptorType = types[i];
            if (i == 2 || i == 3) wr[i].pBufferInfo = &bi[i - 2];
            else wr[i].pImageInfo = &ii[i];
        }
        vkUpdateDescriptorSets(s.dev, 9, wr, 0, nullptr);
    }
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &a.set_layout;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &a.layout) != VK_SUCCESS) return false;
    a.polar_cs = shader(volfog_polar_comp, sizeof volfog_polar_comp);
    a.inject_cs = shader(volfog_inject_comp, sizeof volfog_inject_comp);
    a.integ_cs = shader(volfog_integrate_comp, sizeof volfog_integrate_comp);
    a.vs = shader(glass_vert, sizeof glass_vert);
    a.fs = ms ? shader(volfog_comp_ms_frag, sizeof volfog_comp_ms_frag) : shader(volfog_comp_frag, sizeof volfog_comp_frag);
    if (!a.polar_cs || !a.inject_cs || !a.integ_cs || !a.vs || !a.fs) return false;
    a.polar_pipe = volfog_compute(a.polar_cs);
    a.inject_pipe = volfog_compute(a.inject_cs);
    a.integ_pipe = volfog_compute(a.integ_cs);
    a.comp = fullscreen_pipeline(a.vs, a.layout, a.fs, a.comp_pass, s.samples, FS_VOLFOG);
    return a.polar_pipe && a.inject_pipe && a.integ_pipe && a.comp;
}

// At renderer_init: the volumetric fog resources when RendererConfig.volfog is set, the depth
// target can be sampled and the device stores RGBA16F 3D images. A failure leaves it off.
static void volfog_init() {
    if (!cfg.volfog) return;
    if (volfog_create()) {
        s.volfog.ok = true;
        return;
    }
    volfog_free();
}

// Compute work between barriers: a memory dependency from the shader writes of `src` stages to the
// shader reads and writes of `dst` stages.
static void volfog_sync(VkPipelineStageFlags src, VkPipelineStageFlags dst) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(s.cmd, src, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

// A pass split of its own (inside the frame's main pass, as shaft_exec): the uniform block, the
// polar wall maps (with walls), the injection, the integral (compute, outside any render pass), then
// the composite on the frame's colour.
static void volfog_exec(const RVolfog &p) {
    Volfog &a = s.volfog;
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    vkCmdEndRenderPass(s.cmd);
    const uint32_t n = p.n < (uint32_t)a.g.lights ? p.n : (uint32_t)a.g.lights;
    const bool walls = p.walls && s.walls.real;
    const bool history = p.history && a.prev_ran && !a.fresh;
    const float px = p.px > 0 ? p.px : 1.0f, py = p.py > 0 ? p.py : 1.0f;
    const float nz = p.near_z > 0 ? p.near_z : VOLFOG_NEAR, fz = p.far_z > nz * 1.5f ? p.far_z : nz * 1.5f;
    VolfogUbo u{};
    const float proj[4] = {px, py, 1.0f / px, 1.0f / py}, range[4] = {nz, fz, logf(fz / nz), p.density > 0 ? p.density : 0},
                grid[4] = {(float)a.g.w, (float)a.g.h, (float)a.g.d, (float)n}, up[4] = {p.up[0], p.up[1], p.up[2], p.time},
                jit[4] = {p.jitter[0], p.jitter[1], p.jitter[2], history ? 1.0f : 0.0f},
                fog[4] = {p.fog_on ? (float)p.fog_mode : 0.0f, p.fog_start, p.fog_end, p.fog_density},
                prevp[4] = {p.prev_px > 0 ? p.prev_px : px, p.prev_py > 0 ? p.prev_py : py, 0, 0},
                depth[4] = {p.depth_a, p.depth_b, (float)s.ow, (float)s.oh};
    memcpy(u.proj, proj, sizeof proj);
    memcpy(u.range, range, sizeof range);
    memcpy(u.grid, grid, sizeof grid);
    memcpy(u.up, up, sizeof up);
    memcpy(u.jit, jit, sizeof jit);
    memcpy(u.fog, fog, sizeof fog);
    memcpy(u.prev, p.prev, sizeof u.prev);
    memcpy(u.prevp, prevp, sizeof prevp);
    memcpy(u.tw, p.to_world, sizeof u.tw);
    memcpy(u.depth, depth, sizeof depth);
    u.mist[0] = p.ground;
    u.mist[1] = p.mist > 0 ? p.mist : 0;
    u.mist[2] = p.rain > 0 ? (p.rain < 1 ? p.rain : 1) : 0;
    if (walls) {
        memcpy(u.wall, s.walls.box, 4 * sizeof(float));
        u.wall[3] = 1;
        u.wallz[0] = s.walls.box[4];
        u.wallz[1] = s.walls.box[5];
        u.wallz[2] = s.walls.box[2] > 0 ? 1.0f / s.walls.box[2] : 1.0f;
    }
    for (uint32_t i = 0; i < n; i++) {
        const RVolfogLight &l = p.l[i];
        for (int c = 0; c < 3; c++) {
            u.lpos[i][c] = l.pos[c];
            u.lcol[i][c] = l.rgb[c] > 0 ? l.rgb[c] : 0;
        }
        u.lpos[i][3] = l.radius > 1e-2f ? l.radius : 1e-2f;
        u.lcol[i][3] = (float)l.kind;
    }
    // After the previous frames' reads of the block (one queue), before this frame's.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    const VkPipelineStageFlags cf = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    vkCmdPipelineBarrier(s.cmd, cf, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    vkCmdUpdateBuffer(s.cmd, a.ubo.buf, 0, sizeof u, &u);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, cf, 0, 1, &mb, 0, nullptr, 0, nullptr);
    if (a.fresh)
        for (Image *im : {&a.vol[0], &a.vol[1], &a.integ})
            barrier(im->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    volfog_sync(cf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);   // the previous frame's reads and writes of the volumes and maps
    VkDescriptorSet set = a.set[a.parity];
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, a.layout, 0, 1, &set, 0, nullptr);
    if (walls && n) {
        vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, a.polar_pipe);
        vkCmdDispatch(s.cmd, VOLFOG_POLAR_BINS / 64, n, 1);
        volfog_sync(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    gp_mark(GP_FOG_WALLS);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, a.inject_pipe);
    vkCmdDispatch(s.cmd, (uint32_t)(a.g.w + 3) / 4, (uint32_t)(a.g.h + 3) / 4, (uint32_t)(a.g.d + 3) / 4);
    volfog_sync(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    gp_mark(GP_FOG_INJECT);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, a.integ_pipe);
    vkCmdDispatch(s.cmd, (uint32_t)(a.g.w + 7) / 8, (uint32_t)(a.g.h + 7) / 8, 1);
    volfog_sync(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    gp_mark(GP_FOG_INTEG);
    barrier(s.depth.img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 1,
            VK_IMAGE_ASPECT_DEPTH_BIT);
    barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = a.comp_pass;
    rb.framebuffer = a.comp_fb;
    rb.renderArea = {{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.comp);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.layout, 0, 1, &set, 0, nullptr);
    VkViewport v{0, 0, (float)s.ow, (float)s.oh, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    barrier(ms ? s.msaa.img : s.color.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    barrier(s.depth.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
    gp_mark(GP_FOG_COMP);
    rb.renderPass = resume_pass();
    rb.framebuffer = s.fb;
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    a.parity ^= 1;
    a.fresh = false;
    a.ran = true;
    a.executed++;
}

// ---- Rain splashes (RendererConfig.rain, renderer_rain_splash) ----

static void splash_free() {
    Splash &a = s.splash;
    for (VkPipeline p : {a.mask_pipe, a.pipe, a.flat})
        if (p) vkDestroyPipeline(s.dev, p, nullptr);
    for (VkShaderModule m : {a.vs, a.fs, a.mask_fs})
        if (m) vkDestroyShaderModule(s.dev, m, nullptr);
    for (VkPipelineLayout l : {a.layout, a.mask_layout})
        if (l) vkDestroyPipelineLayout(s.dev, l, nullptr);
    if (a.pool) vkDestroyDescriptorPool(s.dev, a.pool, nullptr);
    if (a.set_layout) vkDestroyDescriptorSetLayout(s.dev, a.set_layout, nullptr);
    for (VkFramebuffer f : {a.fb, a.mask_fb})
        if (f) vkDestroyFramebuffer(s.dev, f, nullptr);
    for (VkRenderPass p : {a.pass, a.mask_pass})
        if (p) vkDestroyRenderPass(s.dev, p, nullptr);
    free_image(&a.mask);
    a = Splash();
}

static bool splash_create() {
    Splash &a = s.splash;
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    const VkFormat fmt = VK_FORMAT_R16G16_SFLOAT;   // as the ssao images: required sampled and as a color attachment
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(s.phys, fmt, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (!s.depth_sampled || !s.chars.ok || (fp.optimalTilingFeatures & need) != need || !split_passes_ready()) return false;
    a.scale = s.ss > 1 ? s.ss : 1;
    a.mw = (s.ow + a.scale - 1) / a.scale;
    a.mh = (s.oh + a.scale - 1) / a.scale;
    if (make_image(&a.mask, fmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)a.mw,
                   (uint32_t)a.mh))
        return false;
    if (!ssao_pass(VK_FORMAT_B8G8R8A8_UNORM, s.samples, true, &a.pass) || !ssao_pass(fmt, VK_SAMPLE_COUNT_1_BIT, false, &a.mask_pass))
        return false;
    a.fb = ssao_fb(a.pass, ms ? s.msaa.view : s.color.view, s.ow, s.oh);
    a.mask_fb = ssao_fb(a.mask_pass, a.mask.view, a.mw, a.mh);
    if (!a.fb || !a.mask_fb) return false;
    // Binding 2: the character mask (read by the mask step's CHAR_MASK variant).
    VkDescriptorSetLayoutBinding lb[3] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 3;
    li.pBindings = lb;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &a.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &a.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[2] = {a.set_layout, a.set_layout};
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = a.pool;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, sets) != VK_SUCCESS) return false;
    a.set_mask = sets[0];
    a.set_comp = sets[1];
    const VkDescriptorImageInfo chars{s.nearest_sampler, s.chars.depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo ii[6] = {{s.nearest_sampler, s.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {s.nearest_sampler, s.white.im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   chars,
                                   {s.nearest_sampler, s.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {s.clamp_sampler, a.mask.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   chars};
    VkWriteDescriptorSet wr[6]{};
    for (int i = 0; i < 6; i++) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = sets[i / 3];
        wr[i].dstBinding = (uint32_t)(i % 3);
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(s.dev, 6, wr, 0, nullptr);
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SsaoPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &a.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &a.layout) != VK_SUCCESS) return false;
    a.vs = shader(glass_vert, sizeof glass_vert);
    a.fs = ms ? shader(ssao_comp_ms_frag, sizeof ssao_comp_ms_frag) : shader(ssao_comp_frag, sizeof ssao_comp_frag);
    if (!a.vs || !a.fs) return false;
    const int32_t modes[2] = {1, 2};   // ssao_comp.frag: the splash mask, its composite
    VkSpecializationMapEntry me{0, 0, sizeof(int32_t)};
    VkSpecializationInfo spec_mask{1, &me, sizeof(int32_t), &modes[0]}, spec_comp{1, &me, sizeof(int32_t), &modes[1]};
    // The mask leaves out characters (CHAR_MASK variant) and, with the cover map, covered ground
    // (RAIN_COVER: set 1 the cover's).
    if (s.cover.ok) {
        const VkDescriptorSetLayout two[2] = {a.set_layout, s.cover.set_layout};
        pli.setLayoutCount = 2;
        pli.pSetLayouts = two;
        if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &a.mask_layout) != VK_SUCCESS) return false;
        a.mask_fs = ms ? shader(splash_cover_ms_frag, sizeof splash_cover_ms_frag) : shader(splash_cover_frag, sizeof splash_cover_frag);
    } else {
        a.mask_fs = ms ? shader(splash_ms_frag, sizeof splash_ms_frag) : shader(splash_frag, sizeof splash_frag);
    }
    if (!a.mask_fs) return false;
    a.mask_pipe = fullscreen_pipeline(a.vs, a.mask_layout ? a.mask_layout : a.layout, a.mask_fs, a.mask_pass,
                                      VK_SAMPLE_COUNT_1_BIT, FS_REPLACE, false, &spec_mask);
    a.pipe = fullscreen_pipeline(a.vs, a.layout, a.fs, a.pass, s.samples, FS_LIGHTEN, false, &spec_comp);
    a.flat = fullscreen_pipeline(a.vs, a.layout, a.fs, a.pass, s.samples, FS_ADD, false, &spec_comp);
    return a.mask_pipe && a.pipe && a.flat;
}

// At renderer_init: the splash resources when RendererConfig.rain is set and the depth target can
// be sampled. A failure frees them and leaves splashes off (renderer_rain_splash_on()).
static void splash_init() {
    if (!cfg.rain) return;
    if (splash_create()) {
        s.splash.ok = true;
        return;
    }
    splash_free();
}

// Push constants of both splash steps; the camera turned into a frame where up is +z.
static SsaoPush splash_push(const RRainSplash &p) {
    float up[3] = {p.up[0], p.up[1], p.up[2]};
    float n = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (!(n > 0)) up[0] = up[1] = 0, up[2] = n = 1;
    for (float &u : up) u /= n;
    // e1, e2, up: a right-handed orthonormal frame (e1 from the world axis least along up).
    float e1[3] = {0, 0, 0};
    e1[fabsf(up[0]) < 0.6f ? 0 : 1] = 1;
    const float k = e1[0] * up[0] + e1[1] * up[1] + e1[2] * up[2];
    for (int i = 0; i < 3; i++) e1[i] -= k * up[i];
    n = sqrtf(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
    for (float &e : e1) e /= n;
    const float e2[3] = {up[1] * e1[2] - up[2] * e1[1], up[2] * e1[0] - up[0] * e1[2], up[0] * e1[1] - up[1] * e1[0]};
    const float *vec[4] = {p.eye, p.right, p.up_v, p.back};
    const float rgb[3] = {(float)(p.color >> 16 & 255) / 255.0f, (float)(p.color >> 8 & 255) / 255.0f, (float)(p.color & 255) / 255.0f};
    const float px = p.px > 0 ? p.px : 1.0f, py = p.py > 0 ? p.py : 1.0f;
    SsaoPush pc = {{p.depth_a, p.depth_b, 1.0f / px, 1.0f / py},
                   {(float)s.ow, (float)s.oh, 1.0f / s.ow, 1.0f / s.oh},
                   {p.time, p.cell > 0 ? p.cell : 1.0f, p.period > 0 ? p.period : 1.0f, p.chance < 0 ? 0.0f : p.chance > 1 ? 1.0f : p.chance},
                   {p.splash < 0 ? 0.0f : p.splash > 4 ? 4.0f : p.splash, p.ripple < 0 ? 0.0f : p.ripple > 4 ? 4.0f : p.ripple, p.fade,
                    (float)s.splash.scale},
                   {}};
    for (int v = 0; v < 4; v++) {
        const float *x = vec[v];
        pc.cam[v][0] = x[0] * e1[0] + x[1] * e1[1] + x[2] * e1[2];
        pc.cam[v][1] = x[0] * e2[0] + x[1] * e2[1] + x[2] * e2[2];
        pc.cam[v][2] = x[0] * up[0] + x[1] * up[1] + x[2] * up[2];
        pc.cam[v][3] = v < 3 ? rgb[v] : 0;
    }
    return pc;
}

// The drops' mask (outside a render pass, the depth readable), into a.mask (then shader-readable).
static void splash_mask(const RRainSplash &p) {
    Splash &a = s.splash;
    const SsaoPush pc = splash_push(p);
    // After the previous frame's composite read the mask (one queue).
    barrier(a.mask.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = a.mask_pass;
    rb.framebuffer = a.mask_fb;
    rb.renderArea = {{0, 0}, {(uint32_t)a.mw, (uint32_t)a.mh}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.mask_pipe);
    const VkPipelineLayout ml = a.mask_layout ? a.mask_layout : a.layout;
    const VkDescriptorSet sets[2] = {a.set_mask, s.cover.set};
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ml, 0, a.mask_layout ? 2 : 1, sets, 0, nullptr);
    vkCmdPushConstants(s.cmd, ml, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport v{0, 0, (float)a.mw, (float)a.mh, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    barrier(a.mask.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

// The mask composited onto the color target, lit then flat (dir[3]: the output's scale), in its own
// render pass or (in_pass) inside the ssao composite's, which has the same attachment.
static void splash_comp(const RRainSplash &p, bool in_pass) {
    Splash &a = s.splash;
    SsaoPush pc = splash_push(p);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = a.pass;
    rb.framebuffer = a.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    if (!in_pass) vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.layout, 0, 1, &a.set_comp, 0, nullptr);
    VkViewport v{0, 0, (float)s.ow, (float)s.oh, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    pc.dir[3] = 1;
    if (s.taa.scoped) pc.cam[3][3] = (float)a.scale;   // --taa-upscale: the mask's texels by its own size (its top-left holds the scene)
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.pipe);
    vkCmdPushConstants(s.cmd, a.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    if (p.flat > 0) {
        pc.dir[3] = p.flat > 1 ? 1.0f : p.flat;
        vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.flat);
        vkCmdPushConstants(s.cmd, a.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        vkCmdDraw(s.cmd, 3, 1, 0, 0);
    }
    a.executed++;
    if (in_pass) return;
    vkCmdEndRenderPass(s.cmd);
    barrier(s.samples != VK_SAMPLE_COUNT_1_BIT ? s.msaa.img : s.color.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
}

// ---- Contact shadows (RendererConfig.contact, renderer_contact) ----

static void contact_free() {
    Contact &a = s.contact;
    for (VkPipeline p : {a.mask_pipe, a.pipe})
        if (p) vkDestroyPipeline(s.dev, p, nullptr);
    for (VkShaderModule m : {a.vs, a.fs})
        if (m) vkDestroyShaderModule(s.dev, m, nullptr);
    if (a.layout) vkDestroyPipelineLayout(s.dev, a.layout, nullptr);
    if (a.pool) vkDestroyDescriptorPool(s.dev, a.pool, nullptr);
    if (a.set_layout) vkDestroyDescriptorSetLayout(s.dev, a.set_layout, nullptr);
    for (VkFramebuffer f : {a.fb, a.mask_fb})
        if (f) vkDestroyFramebuffer(s.dev, f, nullptr);
    for (VkRenderPass p : {a.pass, a.mask_pass})
        if (p) vkDestroyRenderPass(s.dev, p, nullptr);
    free_image(&a.mask);
    a = Contact();
}

static bool contact_create() {
    Contact &a = s.contact;
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    const VkFormat fmt = VK_FORMAT_R8_UNORM;   // Vulkan 1.0 requires it sampled (linear) and as a color attachment
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(s.phys, fmt, &fp);
    const VkFormatFeatureFlags need =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    VkFormatProperties mp;
    vkGetPhysicalDeviceFormatProperties(s.phys, VK_FORMAT_R16G16_UINT, &mp);   // the material map (required sampled by Vulkan 1.0)
    if (!s.depth_sampled || !s.chars.ok || (fp.optimalTilingFeatures & need) != need || !(mp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
        !split_passes_ready())
        return false;
    a.scale = 2 * (s.ss > 1 ? s.ss : 1);   // half the output size
    a.mw = (s.ow + a.scale - 1) / a.scale;
    a.mh = (s.oh + a.scale - 1) / a.scale;
    if (make_image(&a.mask, fmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)a.mw,
                   (uint32_t)a.mh))
        return false;
    if (!ssao_pass(VK_FORMAT_B8G8R8A8_UNORM, s.samples, true, &a.pass) || !ssao_pass(fmt, VK_SAMPLE_COUNT_1_BIT, false, &a.mask_pass))
        return false;
    a.fb = ssao_fb(a.pass, ms ? s.msaa.view : s.color.view, s.ow, s.oh);
    a.mask_fb = ssao_fb(a.mask_pass, a.mask.view, a.mw, a.mh);
    if (!a.fb || !a.mask_fb) return false;
    VkDescriptorSetLayoutBinding lb[3] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 3;
    li.pBindings = lb;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &a.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &a.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[2] = {a.set_layout, a.set_layout};
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = a.pool;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, sets) != VK_SUCCESS) return false;
    a.set_mask = sets[0];
    a.set_comp = sets[1];
    const VkDescriptorImageInfo depth{s.nearest_sampler, s.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo chars{s.nearest_sampler, s.chars.depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo ii[6] = {depth, {s.nearest_sampler, s.white.im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, chars,
                                   depth, {s.clamp_sampler, a.mask.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},   chars};
    VkWriteDescriptorSet wr[6]{};
    for (int i = 0; i < 6; i++) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = sets[i / 3];
        wr[i].dstBinding = (uint32_t)(i % 3);
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(s.dev, 6, wr, 0, nullptr);
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SsaoPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &a.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &a.layout) != VK_SUCCESS) return false;
    a.vs = shader(glass_vert, sizeof glass_vert);
    a.fs = ms ? shader(contact_ms_frag, sizeof contact_ms_frag) : shader(contact_frag, sizeof contact_frag);
    if (!a.vs || !a.fs) return false;
    const int32_t modes[2] = {0, 1};   // contact.frag: the mask, its composite
    VkSpecializationMapEntry me{0, 0, sizeof(int32_t)};
    VkSpecializationInfo spec_mask{1, &me, sizeof(int32_t), &modes[0]}, spec_comp{1, &me, sizeof(int32_t), &modes[1]};
    a.mask_pipe = fullscreen_pipeline(a.vs, a.layout, a.fs, a.mask_pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE, false, &spec_mask);
    a.pipe = fullscreen_pipeline(a.vs, a.layout, a.fs, a.pass, s.samples, FS_MULTIPLY, false, &spec_comp);
    return a.mask_pipe && a.pipe;
}

// At renderer_init: the contact resources when RendererConfig.contact is set and the depth target
// and the character mask can be read. A failure frees them and leaves them off (renderer_contact_on()).
static void contact_init() {
    if (!cfg.contact) return;
    if (contact_create()) {
        s.contact.ok = true;
        return;
    }
    contact_free();
}

static SsaoPush contact_push(const RContact &p) {
    float up[3] = {p.up_view[0], p.up_view[1], p.up_view[2]};
    float n = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (!(n > 0)) up[0] = up[2] = 0, up[1] = n = 1;
    const float px = p.px > 0 ? p.px : 1.0f, py = p.py > 0 ? p.py : 1.0f;
    const float radius = p.radius > 0 ? (p.radius > 4 ? 4.0f : p.radius) : 0.6f;
    SsaoPush pc = {{p.depth_a, p.depth_b, 1.0f / px, 1.0f / py},
                   {(float)s.ow, (float)s.oh, 1.0f / s.ow, 1.0f / s.oh},
                   {radius, p.strength < 0 ? 0.0f : p.strength > 2 ? 2.0f : p.strength, radius, 0},
                   {(float)s.contact.scale, (float)s.contact.mw, (float)s.contact.mh, 0},
                   {{up[0] / n, up[1] / n, up[2] / n, 0}}};
    return pc;
}

// The occlusion mask (outside a render pass, the depth readable), into a.mask (then shader-readable).
static void contact_mask(const RContact &p) {
    Contact &a = s.contact;
    const SsaoPush pc = contact_push(p);
    barrier(a.mask.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = a.mask_pass;
    rb.framebuffer = a.mask_fb;
    rb.renderArea = {{0, 0}, {(uint32_t)a.mw, (uint32_t)a.mh}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.mask_pipe);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.layout, 0, 1, &a.set_mask, 0, nullptr);
    vkCmdPushConstants(s.cmd, a.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport v{0, 0, (float)a.mw, (float)a.mh, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    barrier(a.mask.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

// The mask multiplied onto the color target, in a render pass begun here (left open: the caller
// ends it) or (in_pass) inside another composite's on the same attachment.
static void contact_comp(const RContact &p, bool in_pass) {
    Contact &a = s.contact;
    const SsaoPush pc = contact_push(p);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = a.pass;
    rb.framebuffer = a.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    if (!in_pass) vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.pipe);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.layout, 0, 1, &a.set_comp, 0, nullptr);
    vkCmdPushConstants(s.cmd, a.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport v{0, 0, (float)s.ow, (float)s.oh, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    a.executed++;
}

// ---- Wet ground (RendererConfig.rain_wet, renderer_rain_wet) ----

// The light streaks' uniform block (shaders/wet.frag Streaks, std140).
struct WetUbo {
    float k[4];     // lights, lobe width, falloff (radii), miss fallback
    float fog[4];   // rgb
    float pos_r[RENDERER_WET_LIGHTS][4];
    float col[RENDERER_WET_LIGHTS][4];
    float mat[4];    // materials: map x0, y0, 1 / cell, strength (0: none)
    float matz[4];   // z0, zstep, w, h
    float damp[4];   // damp streets: x on (1; 0 the rain's wet ground); y neon reflections' share (0: none), 0, 0
    float wall[4];   // occluded streaks: the wall map's x0, y0, 1 / cell, 1 = loaded (0: none; WallMap.box)
    float wallz[4];  // z0, zstep, the receiver's own wall
    float sssr[4];   // --reflections sssr: frame number, march budget, history valid (1), rays per texel
    float prev[3][4];   // the previous frame's camera axes (world) with its eye in [3]
    float prevp[4];     // its 1 / px, 1 / py
    float glass[4];     // stage 2: glass strength (0: none), 1 = a dry frame (only glass), 1 = the surface pass ran, 0
};

static void wet_free() {
    Wet &a = s.wet;
    for (VkPipeline p : {a.trace, a.blur, a.comp, a.hiz_p0, a.hiz_p, a.temporal, a.surf_pipe})
        if (p) vkDestroyPipeline(s.dev, p, nullptr);
    for (VkShaderModule m : {a.vs, a.fs, a.trace_fs, a.hiz_fs0, a.hiz_fs, a.surf_vs, a.surf_fs})
        if (m) vkDestroyShaderModule(s.dev, m, nullptr);
    for (VkPipelineLayout l : {a.layout, a.trace_layout, a.hiz_layout, a.surf_layout})
        if (l) vkDestroyPipelineLayout(s.dev, l, nullptr);
    for (VkDescriptorPool p : {a.pool, a.hiz_pool})
        if (p) vkDestroyDescriptorPool(s.dev, p, nullptr);
    for (VkDescriptorSetLayout l : {a.set_layout, a.hiz_set_layout})
        if (l) vkDestroyDescriptorSetLayout(s.dev, l, nullptr);
    for (VkFramebuffer f : {a.fb, a.fb_a, a.fb_b, a.fb_h[0], a.fb_h[1], a.surf_fb})
        if (f) vkDestroyFramebuffer(s.dev, f, nullptr);
    for (int i = 0; i < WET_HIZ_LEVELS; i++) {
        if (a.hiz_fb[i]) vkDestroyFramebuffer(s.dev, a.hiz_fb[i], nullptr);
        if (a.hiz_lv[i]) vkDestroyImageView(s.dev, a.hiz_lv[i], nullptr);
    }
    for (VkRenderPass p : {a.pass, a.half_pass, a.hiz_pass, a.surf_pass})
        if (p) vkDestroyRenderPass(s.dev, p, nullptr);
    free_image(&a.surf);
    free_image(&a.surf_depth);
    free_image(&a.src);
    free_image(&a.a);
    free_image(&a.b);
    free_image(&a.hiz);
    free_image(&a.h[0]);
    free_image(&a.h[1]);
    free_image(&a.mat);
    free_buffer(&a.ubo);
    a = Wet();
}

// The material map image (RG16_UINT, sampled) of w x h texels from data (w x h x 2 uint16), uploaded
// with a one-time submit; the device must be idle for the old one (wet_material_bind rebinds it).
static bool wet_material_image(Image *im, int w, int h, const uint16_t *data) {
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    if (make_image(im, VK_FORMAT_R16G16_UINT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)w,
                   (uint32_t)h))
        return false;
    Buffer staging;
    if (make_buffer(&staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
        free_buffer(&staging);
        return false;
    }
    if (data) memcpy(staging.map, data, (size_t)bytes);
    else memset(staging.map, 0, (size_t)bytes);
    VkCommandBuffer saved;
    begin_once(&saved);
    barrier(im->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(s.cmd, staging.buf, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    const bool ok = !submit_once(saved);
    free_buffer(&staging);
    return ok;
}
// The wet sets (the classic three, then with --reflections sssr the temporal and history ones), the
// ones allocated.
static int wet_sets(VkDescriptorSet out[7]) {
    Wet &a = s.wet;
    const VkDescriptorSet all[7] = {a.set_src, a.set_a, a.set_b, a.set_t[0], a.set_t[1], a.set_h[0], a.set_h[1]};
    int n = 0;
    for (VkDescriptorSet d : all)
        if (d) out[n++] = d;
    return n;
}
// Writes one image binding of every wet set.
static void wet_bind_all(uint32_t binding, const VkDescriptorImageInfo &ii) {
    VkDescriptorSet sets[7];
    const int n = wet_sets(sets);
    VkWriteDescriptorSet wr[7]{};
    for (int i = 0; i < n; i++) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = sets[i];
        wr[i].dstBinding = binding;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[i].pImageInfo = &ii;
    }
    vkUpdateDescriptorSets(s.dev, (uint32_t)n, wr, 0, nullptr);
}
// Binding 5 of the wet sets: the level's wall map (occluded streaks), the empty material map
// standing in without one (wet.frag reads it only while the uniform block says it is loaded).
static void wet_bind_walls() {
    Wet &a = s.wet;
    if (!a.set_src) return;
    const VkImageView v = s.walls.img.view ? s.walls.img.view : a.mat.view;
    if (!v) return;
    wet_bind_all(5, VkDescriptorImageInfo{s.nearest_sampler, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
}
// Binding 4 of the wet sets: the material map.
static void wet_material_bind() {
    wet_bind_all(4, VkDescriptorImageInfo{s.nearest_sampler, s.wet.mat.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
}

// --reflections sssr's per-level image view (level l of the hierarchy).
static VkImageView wet_hiz_view(uint32_t l) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = s.wet.hiz.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R32_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, l, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    VKC(vkCreateImageView, s.dev, &vi, nullptr, &v);
    return v;
}
// --reflections sssr's images and the hierarchy's passes (sssr: the march budget), or (0) the 1x1
// stand-in of bindings 6 and 7; the histories and the stand-in are cleared and left shader-readable.
static bool wet_sssr_create(int sssr) {
    Wet &a = s.wet;
    const VkFormat hf = VK_FORMAT_R32_SFLOAT, fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    const VkImageUsageFlags use = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    a.sssr = sssr;
    if (sssr) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(s.phys, hf, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((fp.optimalTilingFeatures & need) != need || (s.ow < 64 && s.oh < 64)) return false;   // seven levels need 64 texels
        a.hiz_levels = WET_HIZ_LEVELS;
        for (int l = 0, w = s.ow, h = s.oh; l < WET_HIZ_LEVELS; l++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1) a.hiz_w[l] = w, a.hiz_h[l] = h;   // the image's mips
        if (make_image(&a.hiz, hf, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)s.ow,
                       (uint32_t)s.oh, WET_HIZ_LEVELS) ||
            make_image(&a.h[0], fmt, use, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)a.rw, (uint32_t)a.rh) ||
            make_image(&a.h[1], fmt, use, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)a.rw, (uint32_t)a.rh))
            return false;
        if (!ssao_pass(hf, VK_SAMPLE_COUNT_1_BIT, false, &a.hiz_pass)) return false;
        for (int l = 0; l < WET_HIZ_LEVELS; l++) {
            a.hiz_lv[l] = wet_hiz_view((uint32_t)l);
            if (!a.hiz_lv[l]) return false;
            a.hiz_fb[l] = ssao_fb(a.hiz_pass, a.hiz_lv[l], a.hiz_w[l], a.hiz_h[l]);
            if (!a.hiz_fb[l]) return false;
        }
        a.fb_h[0] = ssao_fb(a.half_pass, a.h[0].view, a.rw, a.rh);
        a.fb_h[1] = ssao_fb(a.half_pass, a.h[1].view, a.rw, a.rh);
        if (!a.fb_h[0] || !a.fb_h[1]) return false;
    } else {
        if (make_image(&a.hiz, hf, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1)) return false;
        // Classic: the same two histories for the puddles' temporal pass.
        if (make_image(&a.h[0], fmt, use, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)a.rw, (uint32_t)a.rh) ||
            make_image(&a.h[1], fmt, use, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)a.rw, (uint32_t)a.rh))
            return false;
        a.fb_h[0] = ssao_fb(a.half_pass, a.h[0].view, a.rw, a.rh);
        a.fb_h[1] = ssao_fb(a.half_pass, a.h[1].view, a.rw, a.rh);
        if (!a.fb_h[0] || !a.fb_h[1]) return false;
    }
    VkCommandBuffer saved;
    begin_once(&saved);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (Image *im : {&a.h[0], &a.h[1], sssr ? nullptr : &a.hiz}) {
        if (!im || !im->img) continue;
        barrier(im->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdClearColorImage(s.cmd, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
        barrier(im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    if (sssr)   // every level shader-readable between frames (each pass moves its own level)
        barrier(a.hiz.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, WET_HIZ_LEVELS);
    return !submit_once(saved);
}
// The hierarchy's layout (binding 0: the depth or the level above; the wet push constants), sets,
// shaders and pipelines (vs: the full-screen triangle).
static bool wet_hiz_pipelines() {
    Wet &a = s.wet;
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    VkDescriptorSetLayoutBinding lb{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &lb;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &a.hiz_set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, WET_HIZ_LEVELS};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = WET_HIZ_LEVELS;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &a.hiz_pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[WET_HIZ_LEVELS];
    for (VkDescriptorSetLayout &l : layouts) l = a.hiz_set_layout;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = a.hiz_pool;
    ai.descriptorSetCount = WET_HIZ_LEVELS;
    ai.pSetLayouts = layouts;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, a.hiz_set) != VK_SUCCESS) return false;
    VkDescriptorImageInfo ii[WET_HIZ_LEVELS];
    VkWriteDescriptorSet wr[WET_HIZ_LEVELS]{};
    for (int l = 0; l < WET_HIZ_LEVELS; l++) {
        ii[l] = {s.nearest_sampler, l ? a.hiz_lv[l - 1] : s.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        wr[l].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[l].dstSet = a.hiz_set[l];
        wr[l].dstBinding = 0;
        wr[l].descriptorCount = 1;
        wr[l].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[l].pImageInfo = &ii[l];
    }
    vkUpdateDescriptorSets(s.dev, WET_HIZ_LEVELS, wr, 0, nullptr);
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SsaoPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &a.hiz_set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &a.hiz_layout) != VK_SUCCESS) return false;
    a.hiz_fs0 = ms ? shader(sssr_hiz_ms_frag, sizeof sssr_hiz_ms_frag) : shader(sssr_hiz_frag, sizeof sssr_hiz_frag);
    a.hiz_fs = shader(sssr_hiz_down_frag, sizeof sssr_hiz_down_frag);
    if (!a.hiz_fs0 || !a.hiz_fs) return false;
    a.hiz_p0 = fullscreen_pipeline(a.vs, a.hiz_layout, a.hiz_fs0, a.hiz_pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE);
    a.hiz_p = fullscreen_pipeline(a.vs, a.hiz_layout, a.hiz_fs, a.hiz_pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE);
    return a.hiz_p0 && a.hiz_p;
}

// --reflections sssr's surface pass (cfg.sssr_glass): its images (left shader-readable), pass,
// framebuffer, layout, shaders and pipeline (RVertex positions and texture coordinates; the main
// pass's depth clamp, no culling: the nearest material surface wins; depth tested and written).
static void wet_surf_free() {
    Wet &a = s.wet;
    if (a.surf_pipe) vkDestroyPipeline(s.dev, a.surf_pipe, nullptr);
    for (VkShaderModule m : {a.surf_vs, a.surf_fs})
        if (m) vkDestroyShaderModule(s.dev, m, nullptr);
    if (a.surf_layout) vkDestroyPipelineLayout(s.dev, a.surf_layout, nullptr);
    if (a.surf_fb) vkDestroyFramebuffer(s.dev, a.surf_fb, nullptr);
    if (a.surf_pass) vkDestroyRenderPass(s.dev, a.surf_pass, nullptr);
    free_image(&a.surf);
    free_image(&a.surf_depth);
    a.surf_pipe = VK_NULL_HANDLE;
    a.surf_vs = a.surf_fs = VK_NULL_HANDLE;
    a.surf_layout = VK_NULL_HANDLE;
    a.surf_fb = VK_NULL_HANDLE;
    a.surf_pass = VK_NULL_HANDLE;
}
static bool wet_surf_create() {
    Wet &a = s.wet;
    if (!s.set_layout || !s.chars.ok) return false;
    const VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM, df = s.chars.fmt;
    if (make_surface_image(&a.surf, cf, false, s.ow, s.oh, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true) ||
        make_surface_image(&a.surf_depth, df, true, s.ow, s.oh, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, true))
        return false;
    VkAttachmentDescription att[2]{};
    for (VkAttachmentDescription &d : att) {
        d.samples = VK_SAMPLE_COUNT_1_BIT;
        d.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        d.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        d.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        d.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    att[0].format = cf;
    att[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    att[1].format = df;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;
    // After the previous frames' reads of both; before this frame's (the wet pass).
    const VkPipelineStageFlags att_stages =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags att_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency dep[2] = {{VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, att_stages, VK_ACCESS_SHADER_READ_BIT, att_access, 0},
                                  {0, VK_SUBPASS_EXTERNAL, att_stages, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, att_access, VK_ACCESS_SHADER_READ_BIT, 0}};
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 2;
    rp.pAttachments = att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = dep;
    if (VKC(vkCreateRenderPass, s.dev, &rp, nullptr, &a.surf_pass) != VK_SUCCESS) return false;
    const VkImageView views[2] = {a.surf.view, a.surf_depth.view};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = a.surf_pass;
    fi.attachmentCount = 2;
    fi.pAttachments = views;
    fi.width = (uint32_t)s.ow;
    fi.height = (uint32_t)s.oh;
    fi.layers = 1;
    if (VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &a.surf_fb) != VK_SUCCESS) return false;
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &s.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &a.surf_layout) != VK_SUCCESS) return false;
    a.surf_vs = shader(sssr_surf_vert, sizeof sssr_surf_vert);
    a.surf_fs = shader(sssr_surf_frag, sizeof sssr_surf_frag);
    if (!a.surf_vs || !a.surf_fs) return false;
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = a.surf_vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = a.surf_fs;
    st[1].pName = "main";
    VkVertexInputBindingDescription bind{0, sizeof(RVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr[1 + RENDERER_STAGES];
    attr[0] = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RVertex, x)};
    for (uint32_t i = 0; i < RENDERER_STAGES; i++)
        attr[1 + i] = {1 + i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)(offsetof(RVertex, tex) + i * sizeof(float[4]))};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 1 + RENDERER_STAGES;
    vi.pVertexAttributeDescriptions = attr;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = s.depth_clamp ? VK_TRUE : VK_FALSE;   // as the main pass
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cbs;
    pi.pDynamicState = &dy;
    pi.layout = a.surf_layout;
    pi.renderPass = a.surf_pass;
    return VKC(vkCreateGraphicsPipelines, s.dev, s.pcache, 1, &pi, nullptr, &a.surf_pipe) == VK_SUCCESS;
}

static bool wet_create(int sssr) {
    Wet &a = s.wet;
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    const VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;   // Vulkan 1.0 requires it sampled (linear) and as a color attachment
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(s.phys, fmt, &fp);
    const VkFormatFeatureFlags need =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if (!s.depth_sampled || !s.chars.ok || (fp.optimalTilingFeatures & need) != need || !split_passes_ready()) return false;
    a.scale = (sssr && cfg.sssr_full ? 1 : 2) * (s.ss > 1 ? s.ss : 1);   // half the output size (--reflections-res full: the internal size)
    a.rw = (s.ow + a.scale - 1) / a.scale;
    a.rh = (s.oh + a.scale - 1) / a.scale;
    const VkImageUsageFlags use = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (make_image(&a.src, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                   (uint32_t)a.rw, (uint32_t)a.rh) ||
        make_image(&a.a, fmt, use, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)a.rw, (uint32_t)a.rh) ||
        make_image(&a.b, fmt, use, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)a.rw, (uint32_t)a.rh))
        return false;
    if (!ssao_pass(VK_FORMAT_B8G8R8A8_UNORM, s.samples, true, &a.pass) || !ssao_pass(fmt, VK_SAMPLE_COUNT_1_BIT, false, &a.half_pass))
        return false;
    a.fb = ssao_fb(a.pass, ms ? s.msaa.view : s.color.view, s.ow, s.oh);
    a.fb_a = ssao_fb(a.half_pass, a.a.view, a.rw, a.rh);
    a.fb_b = ssao_fb(a.half_pass, a.b.view, a.rw, a.rh);
    if (!a.fb || !a.fb_a || !a.fb_b) return false;
    if (make_buffer(&a.ubo, sizeof(WetUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;
    memset(a.ubo.map, 0, sizeof(WetUbo));
    if (!wet_material_image(&a.mat, 1, 1, nullptr)) return false;   // empty until a level's map
    if (!wet_sssr_create(sssr)) return false;
    // --reflections sssr's glass: the surface pass (not with --taa-upscale, whose scene renders at
    // another size before the resolve); without it the glass is off.
    if (sssr && cfg.sssr_glass && !(cfg.taa && taa_up_wanted(nullptr, nullptr)) && !wet_surf_create()) {
        wet_surf_free();
        printf("[renderer] wet ground: SSSR surface pass unavailable, no glass reflections\n");
    }
    // Binding 2: the character mask (wet.frag: characters stay dry); 3: the light streaks; 4: the material
    // map; 5: the wall map (occluded streaks); 6, 7: --reflections sssr's hierarchy and history; 8, 9 its
    // surface pass's normals and materials, and depth.
    VkDescriptorSetLayoutBinding lb[10] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 10;
    li.pBindings = lb;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &a.set_layout) != VK_SUCCESS) return false;
    // Sets: src, a, b; the temporal ones (reading a) and the histories' (sssr, and the classic puddles').
    const int nsets = 7;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)(9 * nsets)}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (uint32_t)nsets}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = (uint32_t)nsets;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &a.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[7];
    for (VkDescriptorSetLayout &l : layouts) l = a.set_layout;
    VkDescriptorSet sets[7]{};
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = a.pool;
    ai.descriptorSetCount = (uint32_t)nsets;
    ai.pSetLayouts = layouts;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, sets) != VK_SUCCESS) return false;
    a.set_src = sets[0];
    a.set_a = sets[1];
    a.set_b = sets[2];
    a.set_t[0] = sets[3];
    a.set_t[1] = sets[4];
    a.set_h[0] = sets[5];
    a.set_h[1] = sets[6];
    const VkImageView second[7] = {a.src.view, a.a.view, a.b.view, a.a.view, a.a.view, a.h[0].view, a.h[1].view};
    const VkImageView seventh[7] = {a.hiz.view, a.hiz.view, a.hiz.view, a.h[1].view, a.h[0].view, a.hiz.view, a.hiz.view};
    VkDescriptorImageInfo ii[7][7];
    VkWriteDescriptorSet wr[7 * 8]{};
    const VkDescriptorBufferInfo bi{a.ubo.buf, 0, sizeof(WetUbo)};
    int nw = 0;
    for (int k = 0; k < nsets; k++) {
        ii[k][0] = VkDescriptorImageInfo{s.nearest_sampler, s.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        ii[k][1] = VkDescriptorImageInfo{s.clamp_sampler, second[k], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        ii[k][2] = VkDescriptorImageInfo{s.nearest_sampler, chars_post_view(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        ii[k][3] = VkDescriptorImageInfo{s.nearest_sampler, a.hiz.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        ii[k][4] = VkDescriptorImageInfo{seventh[k] == a.hiz.view ? s.nearest_sampler : s.clamp_sampler, seventh[k], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        ii[k][5] = VkDescriptorImageInfo{s.nearest_sampler, a.surf.view ? a.surf.view : a.hiz.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        ii[k][6] = a.surf_depth.view ? VkDescriptorImageInfo{s.nearest_sampler, a.surf_depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}
                                     : VkDescriptorImageInfo{s.nearest_sampler, a.hiz.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const uint32_t binding[7] = {0, 1, 2, 6, 7, 8, 9};
        for (int j = 0; j < 7; j++, nw++) {
            wr[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[nw].dstSet = sets[k];
            wr[nw].dstBinding = binding[j];
            wr[nw].descriptorCount = 1;
            wr[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[nw].pImageInfo = &ii[k][j];
        }
        wr[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[nw].dstSet = sets[k];
        wr[nw].dstBinding = 3;
        wr[nw].descriptorCount = 1;
        wr[nw].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        wr[nw++].pBufferInfo = &bi;
    }
    vkUpdateDescriptorSets(s.dev, (uint32_t)nw, wr, 0, nullptr);
    wet_material_bind();
    wet_bind_walls();
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SsaoPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &a.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &a.layout) != VK_SUCCESS) return false;
    a.vs = shader(glass_vert, sizeof glass_vert);
    a.fs = ms ? shader(wet_ms_frag, sizeof wet_ms_frag) : shader(wet_frag, sizeof wet_frag);
    if (!a.vs || !a.fs) return false;
    // wet.frag: trace, blur, composite, temporal (MODE), with SSSR on (1) or off.
    const int32_t modes[4][2] = {{0, sssr ? 1 : 0}, {1, sssr ? 1 : 0}, {2, sssr ? 1 : 0}, {3, sssr ? 1 : 0}};
    VkSpecializationMapEntry me[2] = {{0, 0, sizeof(int32_t)}, {1, sizeof(int32_t), sizeof(int32_t)}};
    VkSpecializationInfo spec[4] = {{2, me, sizeof modes[0], modes[0]}, {2, me, sizeof modes[1], modes[1]}, {2, me, sizeof modes[2], modes[2]},
                                    {2, me, sizeof modes[3], modes[3]}};
    if (s.cover.ok) {   // the trace leaves covered ground dry: set 1 the cover's
        const VkDescriptorSetLayout two[2] = {a.set_layout, s.cover.set_layout};
        pli.setLayoutCount = 2;
        pli.pSetLayouts = two;
        if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &a.trace_layout) != VK_SUCCESS) return false;
        a.trace_fs = ms ? shader(wet_cover_ms_frag, sizeof wet_cover_ms_frag) : shader(wet_cover_frag, sizeof wet_cover_frag);
        if (!a.trace_fs) return false;
    }
    a.trace = fullscreen_pipeline(a.vs, a.trace_layout ? a.trace_layout : a.layout, a.trace_fs ? a.trace_fs : a.fs, a.half_pass,
                                  VK_SAMPLE_COUNT_1_BIT, FS_REPLACE, false, &spec[0]);
    a.blur = fullscreen_pipeline(a.vs, a.layout, a.fs, a.half_pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE, false, &spec[1]);
    a.comp = fullscreen_pipeline(a.vs, a.layout, a.fs, a.pass, s.samples, FS_PREMUL, false, &spec[2]);
    if (!a.trace || !a.blur || !a.comp) return false;
    a.temporal = fullscreen_pipeline(a.vs, a.layout, a.fs, a.half_pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE, false, &spec[3]);
    if (!a.temporal) return false;
    return !sssr || wet_hiz_pipelines();
}

// At renderer_init: the wet resources when RendererConfig.rain and rain_wet (or damp) are set and the
// depth target can be sampled. A failure frees them and leaves wet ground off (renderer_rain_wet_on()).
static void wet_init() {
    if ((!cfg.rain || !cfg.rain_wet) && !cfg.damp && !(cfg.sssr > 0 && cfg.sssr_glass)) return;
    const int sssr = cfg.sssr <= 0 ? 0 : cfg.sssr > 96 ? 96 : cfg.sssr;   // shaders/sssr.glsl SSSR_STEPS_LIMIT
    if (wet_create(sssr)) {
        s.wet.ok = true;
        if (sssr)
            printf("[renderer] wet ground: stochastic screen-space reflections (%d-step hierarchy march, %d ray%s, %dx%d%s)\n", sssr,
                   cfg.sssr_rays > 1 ? 2 : 1, cfg.sssr_rays > 1 ? "s" : "", s.wet.rw, s.wet.rh, s.wet.surf_pipe ? ", glass" : "");
        return;
    }
    wet_free();
    if (sssr && wet_create(0)) {   // without the SSSR resources: the classic reflections
        s.wet.ok = true;
        printf("[renderer] wet ground: SSSR unavailable, classic reflections\n");
        return;
    }
    wet_free();
}

// One reduced-size step: pipeline pipe reading set into image dst (framebuffer fb).
static void wet_half(Image &dst, VkFramebuffer fb, VkPipeline pipe, VkDescriptorSet set, const SsaoPush &pc) {
    Wet &a = s.wet;
    barrier(dst.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = a.half_pass;
    rb.framebuffer = fb;
    rb.renderArea = {{0, 0}, {(uint32_t)a.rw, (uint32_t)a.rh}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    const bool cover = pipe == a.trace && a.trace_layout;   // the trace with the rain cover map: set 1
    const VkPipelineLayout pl = cover ? a.trace_layout : a.layout;
    const VkDescriptorSet sets[2] = {set, s.cover.set};
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, cover ? 2 : 1, sets, 0, nullptr);
    vkCmdPushConstants(s.cmd, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport v{0, 0, (float)a.rw, (float)a.rh, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    barrier(dst.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

// --reflections sssr: the min-depth hierarchy of the depth (shader-readable), level by level: level 0
// from the depth (pc: the wet pass's push constants, its internal size), each next one from the level
// above; every level ends shader-readable again.
static void wet_hiz_build(const SsaoPush &pc) {
    Wet &a = s.wet;
    for (int l = 0; l < a.hiz_levels; l++) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = a.hiz.img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 1, 0, 1};
        vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rb.renderPass = a.hiz_pass;
        rb.framebuffer = a.hiz_fb[l];
        rb.renderArea = {{0, 0}, {(uint32_t)a.hiz_w[l], (uint32_t)a.hiz_h[l]}};
        vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, l ? a.hiz_p : a.hiz_p0);
        vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.hiz_layout, 0, 1, &a.hiz_set[l], 0, nullptr);
        vkCmdPushConstants(s.cmd, a.hiz_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        VkViewport v{0, 0, (float)a.hiz_w[l], (float)a.hiz_h[l], 0, 1};
        vkCmdSetViewport(s.cmd, 0, 1, &v);
        vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
        vkCmdDraw(s.cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(s.cmd);
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
}

// Executes the wet ground inside the frame's render pass (begun with s.split_pass or
// s.resume_pass): ends it (the color resolved into s.color, the depth stored), blits the colour to
// the reduced size, traces and blurs the reflections there and composites them in a pass on the
// frame's color target (the multisampled one with MSAA); then resumes the frame's pass.
static void wet_exec(const RRainWet &p) {
    Wet &a = s.wet;
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    vkCmdEndRenderPass(s.cmd);
    barrier(s.depth.img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 1,
            VK_IMAGE_ASPECT_DEPTH_BIT);
    // The resolved colour (TRANSFER_SRC_OPTIMAL at the end of the pass) into src.
    barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(a.src.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit bl{};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[1] = {s.ow, s.oh, 1};
    bl.dstSubresource = bl.srcSubresource;
    bl.dstOffsets[1] = {a.rw, a.rh, 1};
    vkCmdBlitImage(s.cmd, s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, a.src.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                   VK_FILTER_LINEAR);
    barrier(a.src.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    // The single-sample color (the target, or the resolve target with MSAA) back to an attachment.
    barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    float up[3] = {p.up_view[0], p.up_view[1], p.up_view[2]};
    float n = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (!(n > 0)) up[0] = up[2] = 0, up[1] = n = 1;
    const float px = p.px > 0 ? p.px : 1.0f, py = p.py > 0 ? p.py : 1.0f;
    SsaoPush pc = {{p.depth_a, p.depth_b, 1.0f / px, 1.0f / py},
                   {(float)s.ow, (float)s.oh, 1.0f / s.ow, 1.0f / s.oh},
                   {p.reflect < 0 ? 0.0f : p.reflect > 8 ? 8.0f : p.reflect, p.darken < 0 ? 0.0f : p.darken > 1 ? 1.0f : p.darken,
                    p.reach > 0 ? p.reach : 40.0f, 0},
                   {(float)a.scale, (float)a.rw, (float)a.rh, 0},
                   {{up[0] / n, up[1] / n, up[2] / n, 0}}};
    const bool streaks = p.streaks > 0, mats = p.materials > 0 && a.mat_real;
    const bool glass_lights = a.surf_pipe && p.glass > 0 && p.nlights > 0;   // --reflections sssr's glass mirrors the streaks' lights
    const bool damp = p.damp > 0 && mats;   // the damp mode needs the material map
    if (p.puddles > 0 || streaks || mats || a.sssr) {   // (sssr: its temporal pass takes pixels to the world)   // puddles: strength, time; all: the camera in the world (else as without them)
        if (p.puddles > 0) {
            pc.dir[3] = p.puddles > 1 ? 1.0f : p.puddles;
            pc.params[3] = p.time;
        }
        const float *ax[3] = {p.right, p.up_v, p.back};
        for (int v = 0; v < 3; v++) {
            for (int k = 0; k < 3; k++) pc.cam[v + 1][k] = ax[v][k];
            pc.cam[v + 1][3] = p.eye[v];
        }
    }
    // Light streaks and materials: the uniform block (outside a render pass here), also once after
    // a frame with materials so a later frame without them does not see that frame's.
    const bool neon = streaks && p.neon > 0;
    // Classic puddles: standing water is nearly a mirror, blurred little, and
    // each reduced texel holds one ray, so where it mirrors a fence or a wall its texels popped on and off
    // as the view moved. Their reflection is accumulated over frames as --reflections sssr's is (mode 3,
    // reprojected through the previous camera, clamped to this frame's neighbourhood); dry and wet ground
    // without puddles stay this frame's alone, and a frame without puddles is as before.
    const bool pud_hist = !a.sssr && p.puddles > 0;
    if (streaks || glass_lights || mats || a.mat_ubo || a.neon_ubo || a.sssr || pud_hist) {
        WetUbo u{};
        if (a.sssr || pud_hist) {   // --reflections sssr: the noise's frame, the budget, the previous frame's camera if it is one
            float jump = 0;
            for (int k = 0; k < 3; k++) {
                const float d = p.eye[k] - a.prev_cam[k][3];
                jump += d * d;
            }
            const float dots = p.right[0] * a.prev_cam[0][0] + p.right[1] * a.prev_cam[0][1] + p.right[2] * a.prev_cam[0][2] +
                               p.back[0] * a.prev_cam[2][0] + p.back[1] * a.prev_cam[2][1] + p.back[2] * a.prev_cam[2][2];
            const bool cut = jump > WET_SSSR_CUT * WET_SSSR_CUT || dots < 2 * WET_SSSR_TURN;
            u.sssr[2] = a.hist_ok && !cut ? 1.0f : 0.0f;
            if (a.sssr) {
                u.sssr[0] = (float)(a.frame % 4096u);
                u.sssr[1] = (float)a.sssr;
                u.sssr[3] = cfg.sssr_rays > 1 ? 2.0f : 1.0f;
            }
            if (a.surf_pipe && p.glass > 0) {   // stage 2: the glass, a dry frame, this frame's surfaces
                u.glass[0] = p.glass > 2 ? 2.0f : p.glass;
                u.glass[1] = p.dry ? 1.0f : 0.0f;
                u.glass[2] = a.surf_live ? 1.0f : 0.0f;
            }
            memcpy(u.prev, a.prev_cam, sizeof u.prev);
            u.prevp[0] = a.prev_p[0];
            u.prevp[1] = a.prev_p[1];
        }
        if (streaks || glass_lights) {   // light streaks: their gain, the lights (the glass's: the lights alone)
            if (streaks) pc.cam[0][3] = p.streaks > 8 ? 8.0f : p.streaks;
            const uint32_t n = p.nlights < RENDERER_WET_LIGHTS ? p.nlights : RENDERER_WET_LIGHTS;
            u.k[0] = (float)n;
            u.k[1] = p.streak_rough > 1e-3f ? p.streak_rough : 1e-3f;
            u.k[2] = p.streak_reach > 0.1f ? p.streak_reach : 0.1f;
            u.k[3] = p.fallback < 0 ? 0.0f : p.fallback > 1 ? 1.0f : p.fallback;
            for (int c = 0; c < 3; c++) u.fog[c] = p.fog_rgb[c] < 0 ? 0.0f : p.fog_rgb[c] > 1 ? 1.0f : p.fog_rgb[c];
            for (uint32_t i = 0; i < n; i++) {
                for (int c = 0; c < 3; c++) {
                    u.pos_r[i][c] = p.light[i].pos[c];
                    u.col[i][c] = p.light[i].rgb[c] > 0 ? p.light[i].rgb[c] : 0;
                }
                u.pos_r[i][3] = p.light[i].radius > 1e-3f ? p.light[i].radius : 1e-3f;
                if (neon || (glass_lights && p.neon > 0)) u.col[i][3] = p.light[i].size > 0 ? p.light[i].size : 0;   // a sign's extent (0: a point light)
            }
        }
        if (mats) {   // materials: the map's place and their strength
            memcpy(u.mat, a.mat_box, sizeof u.mat);
            memcpy(u.matz, a.mat_box + 4, sizeof u.matz);
            u.mat[3] = p.materials > 2 ? 2.0f : p.materials;
        }
        if ((streaks || glass_lights) && s.walls.ok && s.walls.real) {   // occluded streaks (and glass lights): the level's wall map
            memcpy(u.wall, s.walls.box, sizeof u.wall);
            memcpy(u.wallz, s.walls.box + 4, sizeof u.wallz);
        }
        if (damp) u.damp[0] = 1;   // damp streets: the damp mode
        if (neon) u.damp[1] = p.neon > 1 ? 1.0f : p.neon;   // neon reflections: the saturated colours' share
        a.mat_ubo = mats;
        a.neon_ubo = neon;
        // After the previous frames' reads of the block (one queue), before this frame's.
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        vkCmdUpdateBuffer(s.cmd, a.ubo.buf, 0, sizeof u, &u);
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
        vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    if (a.sssr) {   // --reflections sssr: the hierarchy, the stochastic trace, its accumulation into h[cur], the blur of that
        wet_hiz_build(pc);
        wet_half(a.a, a.fb_a, a.trace, a.set_src, pc);
        wet_half(a.h[a.cur], a.fb_h[a.cur], a.temporal, a.set_t[a.cur], pc);
        wet_half(a.b, a.fb_b, a.blur, a.set_h[a.cur], pc);
        a.cur ^= 1;
        a.frame++;
        a.hist_ok = true;
        const float *ax[3] = {p.right, p.up_v, p.back};
        for (int v = 0; v < 3; v++) {
            for (int k = 0; k < 3; k++) a.prev_cam[v][k] = ax[v][k];
            a.prev_cam[v][3] = p.eye[v];
        }
        a.prev_p[0] = pc.proj[2];
        a.prev_p[1] = pc.proj[3];
    } else if (pud_hist) {   // classic puddles: the trace, its accumulation into h[cur], the blur of that
        wet_half(a.a, a.fb_a, a.trace, a.set_src, pc);
        wet_half(a.h[a.cur], a.fb_h[a.cur], a.temporal, a.set_t[a.cur], pc);
        wet_half(a.b, a.fb_b, a.blur, a.set_h[a.cur], pc);
        a.cur ^= 1;
        a.hist_ok = true;
        const float *ax[3] = {p.right, p.up_v, p.back};
        for (int v = 0; v < 3; v++) {
            for (int k = 0; k < 3; k++) a.prev_cam[v][k] = ax[v][k];
            a.prev_cam[v][3] = p.eye[v];
        }
        a.prev_p[0] = pc.proj[2];
        a.prev_p[1] = pc.proj[3];
    } else {
        wet_half(a.a, a.fb_a, a.trace, a.set_src, pc);
        wet_half(a.b, a.fb_b, a.blur, a.set_a, pc);
        a.hist_ok = false;   // the puddles' history is stale after a frame without them
    }
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = a.pass;
    rb.framebuffer = a.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.comp);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.layout, 0, 1, &a.set_b, 0, nullptr);
    vkCmdPushConstants(s.cmd, a.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport v{0, 0, (float)s.ow, (float)s.oh, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s.cmd);
    barrier(ms ? s.msaa.img : s.color.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    a.executed++;
    barrier(s.depth.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
    rb.renderPass = resume_pass();
    rb.framebuffer = s.fb;
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
}

// ---- Rain cover map (RendererConfig.rain_cover, renderer_rain_cover) ----

// The consumers' uniform block (shaders/rain_cover.glsl, std140).
struct CoverUbo {
    float row[3][4];   // view -> map u, v, depth
    float k[4];        // on, world units per map depth unit, covered above, soft
    float inv[4];      // 1 / px, 1 / py, map size, 0
};

static void cover_free() {
    Cover &c = s.cover;
    if (c.pipe) vkDestroyPipeline(s.dev, c.pipe, nullptr);
    if (c.vs) vkDestroyShaderModule(s.dev, c.vs, nullptr);
    if (c.layout) vkDestroyPipelineLayout(s.dev, c.layout, nullptr);
    if (c.pool) vkDestroyDescriptorPool(s.dev, c.pool, nullptr);
    if (c.set_layout) vkDestroyDescriptorSetLayout(s.dev, c.set_layout, nullptr);
    if (c.fb) vkDestroyFramebuffer(s.dev, c.fb, nullptr);
    if (c.pass) vkDestroyRenderPass(s.dev, c.pass, nullptr);
    free_image(&c.map);
    free_buffer(&c.ubo);
    c = Cover();
}

static bool cover_create() {
    Cover &c = s.cover;
    const int n = RENDERER_RAIN_COVER_SIZE;
    // D16: required as a sampled depth attachment; cleared to 1.0 (nothing there) and shader-readable.
    if (make_surface_image(&c.map, VK_FORMAT_D16_UNORM, true, n, n, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, true)) return false;
    if (make_buffer(&c.ubo, sizeof(CoverUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;
    memset(c.ubo.map, 0, sizeof(CoverUbo));
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_D16_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAttachmentReference dref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.pDepthStencilAttachment = &dref;
    // After the previous frames' reads of the map; before this frame's.
    VkSubpassDependency dep[2] = {
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_SHADER_READ_BIT,
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0},
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0}};
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = dep;
    if (VKC(vkCreateRenderPass, s.dev, &rp, nullptr, &c.pass) != VK_SUCCESS) return false;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = c.pass;
    fi.attachmentCount = 1;
    fi.pAttachments = &c.map.view;
    fi.width = fi.height = (uint32_t)n;
    fi.layers = 1;
    if (VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &c.fb) != VK_SUCCESS) return false;
    // The consumers' set: the map (nearest, read with textureGather) and the uniform block.
    VkDescriptorSetLayoutBinding lb[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2;
    li.pBindings = lb;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &c.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = ps;
    if (VKC(vkCreateDescriptorPool, s.dev, &pci, nullptr, &c.pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = c.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &c.set_layout;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, &c.set) != VK_SUCCESS) return false;
    VkDescriptorImageInfo ii{s.nearest_sampler, c.map.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo bi{c.ubo.buf, 0, sizeof(CoverUbo)};
    VkWriteDescriptorSet wr[2]{};
    for (int i = 0; i < 2; i++) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = c.set;
        wr[i].dstBinding = (uint32_t)i;
        wr[i].descriptorCount = 1;
    }
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr[0].pImageInfo = &ii;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wr[1].pBufferInfo = &bi;
    vkUpdateDescriptorSets(s.dev, 2, wr, 0, nullptr);
    // The map's pipeline: positions only (the frame's vertices, RVertex stride), no fragment shader,
    // no culling (seen from above), depth clamp when the device has it (a roof above the map's top
    // still covers), depth test and write.
    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float[3][4])};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &c.layout) != VK_SUCCESS) return false;
    c.vs = shader(rain_cover_vert, sizeof rain_cover_vert);
    if (!c.vs) return false;
    VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    st.stage = VK_SHADER_STAGE_VERTEX_BIT;
    st.module = c.vs;
    st.pName = "main";
    VkVertexInputBindingDescription bind{0, sizeof(RVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RVertex, x)};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions = &attr;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport v{0, 0, (float)n, (float)n, 0, 1};
    VkRect2D all{{0, 0}, {(uint32_t)n, (uint32_t)n}};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.pViewports = &v;
    vp.scissorCount = 1;
    vp.pScissors = &all;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = s.depth_clamp ? VK_TRUE : VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 1;
    pi.pStages = &st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cbs;
    pi.layout = c.layout;
    pi.renderPass = c.pass;
    return VKC(vkCreateGraphicsPipelines, s.dev, s.pcache, 1, &pi, nullptr, &c.pipe) == VK_SUCCESS;
}

// At renderer_init, before the rain, splash and wet ground (their RAIN_COVER variants need it): the
// cover resources when RendererConfig.rain and rain_cover are set. A failure leaves cover off (and
// those passes as without it).
static void cover_init() {
    if (!cfg.rain || !cfg.rain_cover) return;
    if (cover_create()) {
        s.cover.ok = true;
        return;
    }
    cover_free();
}

// Outside a render pass, before the frame's main pass: the consumers' uniform block (on only with a
// map this frame), then the map: every frame-target draw flagged RENDERER_RAIN_COVER (and not
// RENDERER_CHARACTER: characters are no roof), depth only.
static void cover_record(VkDeviceSize index_offset) {
    Cover &c = s.cover;
    if (!c.ok) return;
    bool any = false;
    if (c.marked && !s.indices.empty())
        for (const Op &op : s.ops)
            if (op.kind == OP_DRAW && !op.target[0] && !op.target[1] && (op.draw_flags & (RENDERER_RAIN_COVER | RENDERER_CHARACTER)) == RENDERER_RAIN_COVER) {
                any = true;
                break;
            }
    CoverUbo u{};
    if (any) {
        memcpy(u.row, c.p.view_to_map, sizeof u.row);
        u.k[0] = 1;
        u.k[1] = c.p.height;
        u.k[2] = c.p.above;
        u.k[3] = c.p.soft > 1e-4f ? c.p.soft : 1e-4f;
        u.inv[0] = c.p.inv_px;
        u.inv[1] = c.p.inv_py;
        u.inv[2] = (float)RENDERER_RAIN_COVER_SIZE;
    }
    // After the previous frames' reads of the block (one queue), before this frame's.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    vkCmdUpdateBuffer(s.cmd, c.ubo.buf, 0, sizeof u, &u);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    if (!any) return;
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = c.pass;
    rb.framebuffer = c.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)RENDERER_RAIN_COVER_SIZE, (uint32_t)RENDERER_RAIN_COVER_SIZE}};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.pipe);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(s.cmd, 0, 1, &cur().geometry.buf, &zero);
    vkCmdBindIndexBuffer(s.cmd, cur().geometry.buf, index_offset, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(s.cmd, c.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof c.p.clip_to_map, c.p.clip_to_map);
    for (const Op &op : s.ops)
        if (op.kind == OP_DRAW && !op.target[0] && !op.target[1] && (op.draw_flags & (RENDERER_RAIN_COVER | RENDERER_CHARACTER)) == RENDERER_RAIN_COVER)
            vkCmdDrawIndexed(s.cmd, op.index_count, 1, op.first_index, op.vertex_offset, 0);
    vkCmdEndRenderPass(s.cmd);
    c.executed++;
}

// ---- World shadows (RendererConfig.world_shadow, renderer_world_shadow) ----

static void wshadow_free() {
    WorldShadow &c = s.wshadow;
    if (c.gate_pipe) vkDestroyPipeline(s.dev, c.gate_pipe, nullptr);
    if (c.gate_cs) vkDestroyShaderModule(s.dev, c.gate_cs, nullptr);
    if (c.gate_layout) vkDestroyPipelineLayout(s.dev, c.gate_layout, nullptr);
    if (c.gate_pool) vkDestroyDescriptorPool(s.dev, c.gate_pool, nullptr);
    if (c.gate_set_layout) vkDestroyDescriptorSetLayout(s.dev, c.gate_set_layout, nullptr);
    if (c.cut_pipe) vkDestroyPipeline(s.dev, c.cut_pipe, nullptr);
    if (c.cut_vs) vkDestroyShaderModule(s.dev, c.cut_vs, nullptr);
    if (c.cut_fs) vkDestroyShaderModule(s.dev, c.cut_fs, nullptr);
    if (c.cut_layout) vkDestroyPipelineLayout(s.dev, c.cut_layout, nullptr);
    if (c.pipe) vkDestroyPipeline(s.dev, c.pipe, nullptr);
    if (c.vs) vkDestroyShaderModule(s.dev, c.vs, nullptr);
    if (c.layout) vkDestroyPipelineLayout(s.dev, c.layout, nullptr);
    if (c.fb) vkDestroyFramebuffer(s.dev, c.fb, nullptr);
    if (c.pass) vkDestroyRenderPass(s.dev, c.pass, nullptr);
    free_image(&c.map);
    free_buffer(&c.buf);
    c = WorldShadow();
}

static bool wshadow_create(int n) {
    WorldShadow &c = s.wshadow;
    c.size = n;
    MemTag tag(RENDERER_VRAM_EFFECTS);
    // D16 (required as a sampled depth attachment; 2-3 mm steps over the map's depth): cleared to
    // 1.0 (nothing there) and shader-readable.
    if (make_surface_image(&c.map, VK_FORMAT_D16_UNORM, true, n, n, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, true)) return false;
    if (make_buffer(&c.buf, sizeof(WorldShadowGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;
    memset(c.buf.map, 0, sizeof(WorldShadowGpu));
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_D16_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAttachmentReference dref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.pDepthStencilAttachment = &dref;
    // After the previous frames' reads of the map; before this frame's.
    VkSubpassDependency dep[2] = {
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_SHADER_READ_BIT,
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0},
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0}};
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = dep;
    if (VKC(vkCreateRenderPass, s.dev, &rp, nullptr, &c.pass) != VK_SUCCESS) return false;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = c.pass;
    fi.attachmentCount = 1;
    fi.pAttachments = &c.map.view;
    fi.width = fi.height = (uint32_t)n;
    fi.layers = 1;
    if (VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &c.fb) != VK_SUCCESS) return false;
    // The map's pipeline: positions only (the frame's vertices, RVertex stride), no fragment shader,
    // no culling (both sides of a wall cast), depth clamp when the device has it (a caster nearer
    // the light than the map's top still casts), slope-scaled depth bias (no acne on surfaces at a
    // grazing angle to the light), depth test and write.
    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float[3][4])};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &c.layout) != VK_SUCCESS) return false;
    c.vs = shader(rain_cover_vert, sizeof rain_cover_vert);
    if (!c.vs) return false;
    VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    st.stage = VK_SHADER_STAGE_VERTEX_BIT;
    st.module = c.vs;
    st.pName = "main";
    VkVertexInputBindingDescription bind{0, sizeof(RVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RVertex, x)};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions = &attr;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport v{0, 0, (float)n, (float)n, 0, 1};
    VkRect2D all{{0, 0}, {(uint32_t)n, (uint32_t)n}};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.pViewports = &v;
    vp.scissorCount = 1;
    vp.pScissors = &all;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = s.depth_clamp ? VK_TRUE : VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.depthBiasEnable = VK_TRUE;
    rs.depthBiasConstantFactor = 2.0f;
    rs.depthBiasSlopeFactor = 1.5f;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 1;
    pi.pStages = &st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cbs;
    pi.layout = c.layout;
    pi.renderPass = c.pass;
    if (VKC(vkCreateGraphicsPipelines, s.dev, s.pcache, 1, &pi, nullptr, &c.pipe) != VK_SUCCESS) return false;
    // The cut-out variant: the same state, plus the stage-0 coordinates and the alpha test.
    VkPushConstantRange cpc[2] = {{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float[3][4])}, {VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(float[3][4]), 8}};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &s.set_layout;
    pli.pushConstantRangeCount = 2;
    pli.pPushConstantRanges = cpc;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &c.cut_layout) != VK_SUCCESS) return false;
    c.cut_vs = shader(world_shadow_cut_vert, sizeof world_shadow_cut_vert);
    c.cut_fs = shader(world_shadow_cut_frag, sizeof world_shadow_cut_frag);
    if (!c.cut_vs || !c.cut_fs) return false;
    VkPipelineShaderStageCreateInfo cst[2] = {st, st};
    cst[0].module = c.cut_vs;
    cst[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    cst[1].module = c.cut_fs;
    VkVertexInputAttributeDescription cattr[2] = {attr, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RVertex, tex)}};
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = cattr;
    pi.stageCount = 2;
    pi.pStages = cst;
    pi.layout = c.cut_layout;
    if (VKC(vkCreateGraphicsPipelines, s.dev, s.pcache, 1, &pi, nullptr, &c.cut_pipe) != VK_SUCCESS) return false;
    // The indoor gate: binding 0 the map (nearest), binding 1 the parameters (read and written).
    VkDescriptorSetLayoutBinding gb[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                          {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo gli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    gli.bindingCount = 2;
    gli.pBindings = gb;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &gli, nullptr, &c.gate_set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize gps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo gpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    gpi.maxSets = 1;
    gpi.poolSizeCount = 2;
    gpi.pPoolSizes = gps;
    if (VKC(vkCreateDescriptorPool, s.dev, &gpi, nullptr, &c.gate_pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo gai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    gai.descriptorPool = c.gate_pool;
    gai.descriptorSetCount = 1;
    gai.pSetLayouts = &c.gate_set_layout;
    if (VKC(vkAllocateDescriptorSets, s.dev, &gai, &c.gate_set) != VK_SUCCESS) return false;
    VkDescriptorImageInfo gii{s.nearest_sampler, c.map.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo gbi{c.buf.buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet gw[2]{};
    for (int i = 0; i < 2; i++) {
        gw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        gw[i].dstSet = c.gate_set;
        gw[i].dstBinding = (uint32_t)i;
        gw[i].descriptorCount = 1;
    }
    gw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    gw[0].pImageInfo = &gii;
    gw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    gw[1].pBufferInfo = &gbi;
    vkUpdateDescriptorSets(s.dev, 2, gw, 0, nullptr);
    VkPipelineLayoutCreateInfo gpl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    gpl.setLayoutCount = 1;
    gpl.pSetLayouts = &c.gate_set_layout;
    if (VKC(vkCreatePipelineLayout, s.dev, &gpl, nullptr, &c.gate_layout) != VK_SUCCESS) return false;
    c.gate_cs = shader(world_shadow_gate_comp, sizeof world_shadow_gate_comp);
    if (!c.gate_cs) return false;
    VkComputePipelineCreateInfo gci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    gci.stage = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    gci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    gci.stage.module = c.gate_cs;
    gci.stage.pName = "main";
    gci.layout = c.gate_layout;
    return VKC(vkCreateComputePipelines, s.dev, s.pcache, 1, &gci, nullptr, &c.gate_pipe) == VK_SUCCESS;
}

// At renderer_init: the world shadow resources when RendererConfig.world_shadow is set (clamped to
// 512-4096 and the device's largest image). A failure leaves world shadows off.
static void wshadow_init() {
    if (cfg.world_shadow <= 0) return;
    int n = cfg.world_shadow < 512 ? 512 : cfg.world_shadow > 4096 ? 4096 : cfg.world_shadow;
    if (n > (int)s.max_dim) n = (int)s.max_dim;
    if (wshadow_create(n)) {
        s.wshadow.ok = true;
        return;
    }
    wshadow_free();
}

static bool wshadow_caster(const Op &op) {
    return op.kind == OP_DRAW && !op.target[0] && !op.target[1] && (op.draw_flags & (RENDERER_WORLD_SHADOW | RENDERER_CHARACTER)) == RENDERER_WORLD_SHADOW;
}
// A cut-out caster: its alpha test (func, ref) on the stage-0 alpha; an alpha-blended draw (source
// factor SRC_ALPHA: alpha is coverage) at least half covered. Returns false for a solid caster.
static bool wshadow_cut(const Op &op, uint32_t pc[2]) {
    const uint32_t f = op.push[0], key = (uint32_t)op.pipe_key;
    const bool test = f >= 1 && f <= 7, blend = (key >> 13 & 1) && (VkBlendFactor)(key & 31) == VK_BLEND_FACTOR_SRC_ALPHA;
    if (!test && !blend) return false;
    pc[0] = test ? f : 5u;
    pc[1] = test ? op.push[1] : 127u;
    if (blend && (pc[0] == 5u || pc[0] == 7u) && pc[1] < 127u) pc[1] = 127u;
    return true;
}

// Outside a render pass, before the frame's main pass: the receivers' parameters (on only with a
// map this frame), then the map: every frame-target draw flagged RENDERER_WORLD_SHADOW (and not
// RENDERER_CHARACTER: the title's own map has the characters), depth only.
static void wshadow_record(VkDeviceSize index_offset, float sx, float sy, std::unordered_map<uint64_t, VkDescriptorSet> &sets) {
    WorldShadow &c = s.wshadow;
    if (!c.ok) return;
    const Op *first = nullptr;
    if (c.marked && !s.indices.empty())
        for (const Op &op : s.ops)
            if (wshadow_caster(op)) {
                first = &op;
                break;
            }
    WorldShadowGpu u{};
    if (first) {
        const RWorldShadow &p = c.p;
        memcpy(u.row, p.clip_to_map, sizeof u.row);
        memcpy(u.wrow, p.clip_to_world, sizeof u.wrow);
        const float vw = first->vp.w * sx, vh = first->vp.h * sy;
        u.vp[0] = first->vp.x * sx;
        u.vp[1] = first->vp.y * sy;
        u.vp[2] = vw > 0 ? 1.0f / vw : 0.0f;
        u.vp[3] = vh > 0 ? 1.0f / vh : 0.0f;
        u.k[0] = vw > 0 && vh > 0 ? 1.0f : 0.0f;
        u.k[1] = p.bias;
        u.k[2] = p.darken < 0 ? 0 : p.darken > 1 ? 1 : p.darken;
        u.k[3] = (float)c.size;
        const uint32_t nl = p.lamps < RENDERER_WORLD_SHADOW_LAMPS ? p.lamps : RENDERER_WORLD_SHADOW_LAMPS;
        memcpy(u.eye, p.eye, sizeof p.eye);
        u.eye[3] = (float)nl;
        u.fade[0] = p.fade_near;
        u.fade[1] = p.fade_far > p.fade_near + 1e-3f ? p.fade_far : p.fade_near + 1e-3f;
        u.fade[2] = p.baked_lo;
        u.fade[3] = p.baked_hi > p.baked_lo + 1e-4f ? p.baked_hi : p.baked_lo + 1e-4f;
        u.lampk[0] = p.lamp_keep < 0 ? 0 : p.lamp_keep > 1 ? 1 : p.lamp_keep;
        for (uint32_t i = 0; i < nl; i++) {
            memcpy(u.lamp[i], p.lamp[i], sizeof u.lamp[i]);
            if (!(u.lamp[i][3] > 1e-3f)) u.lamp[i][3] = 1e-3f;
        }
        memcpy(u.lrow, p.world_to_map, sizeof u.lrow);
        u.gin[0] = p.dt < 0 ? 0 : p.dt > 0.1f ? 0.1f : p.dt;
        u.gin[1] = c.had_map ? 0.0f : 1.0f;
        u.gin[2] = p.clearance;
        u.gfwd[0] = p.ahead[0];
        u.gfwd[1] = p.ahead[1];
    }
    c.had_map = first != nullptr;
    // After the previous frames' reads of the buffer (one queue), before this frame's.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    // Everything but the gate's own state (it carries over from frame to frame).
    vkCmdUpdateBuffer(s.cmd, c.buf.buf, 0, offsetof(WorldShadowGpu, gate), &u);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
    if (!first) return;
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = c.pass;
    rb.framebuffer = c.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)c.size, (uint32_t)c.size}};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.pipe);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(s.cmd, 0, 1, &cur().geometry.buf, &zero);
    vkCmdBindIndexBuffer(s.cmd, cur().geometry.buf, index_offset, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(s.cmd, c.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof c.p.clip_to_map, c.p.clip_to_map);
    uint32_t pc[2];
    c.cut = 0;
    for (const Op &op : s.ops)
        if (wshadow_caster(op)) {
            if (wshadow_cut(op, pc)) c.cut++;
            else vkCmdDrawIndexed(s.cmd, op.index_count, 1, op.first_index, op.vertex_offset, 0);
        }
    if (c.cut) {
        vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.cut_pipe);
        vkCmdPushConstants(s.cmd, c.cut_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof c.p.clip_to_map, c.p.clip_to_map);
        VkDescriptorSet bound = VK_NULL_HANDLE;
        uint32_t last[2] = {~0u, ~0u};
        for (const Op &op : s.ops) {
            if (!wshadow_caster(op) || !wshadow_cut(op, pc)) continue;
            VkDescriptorSet set = frame_set(op.tex, sets);
            if (!set) continue;
            if (set != bound) vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.cut_layout, 0, 1, &set, 0, nullptr);
            bound = set;
            if (pc[0] != last[0] || pc[1] != last[1])
                vkCmdPushConstants(s.cmd, c.cut_layout, VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(float[3][4]), sizeof pc, pc);
            last[0] = pc[0];
            last[1] = pc[1];
            vkCmdDrawIndexed(s.cmd, op.index_count, 1, op.first_index, op.vertex_offset, 0);
        }
    }
    vkCmdEndRenderPass(s.cmd);
    // The indoor gate reads the map and writes its level before the receivers read it.
    mb.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.gate_pipe);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.gate_layout, 0, 1, &c.gate_set, 0, nullptr);
    vkCmdDispatch(s.cmd, 1, 1, 1);
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    c.executed++;
}

// ---- The rain's character mask (renderer_rain_chars_on) ----

// The character mask the passes after the TAA op read (wet ground, depth of field): the output-size
// copy with --taa-upscale, else the mask itself.
static VkImageView chars_post_view() { return s.chars.depth_up.view ? s.chars.depth_up.view : s.chars.depth.view; }

static void chars_free() {
    Chars &c = s.chars;
    if (c.pipe) vkDestroyPipeline(s.dev, c.pipe, nullptr);
    if (c.vs) vkDestroyShaderModule(s.dev, c.vs, nullptr);
    if (c.layout) vkDestroyPipelineLayout(s.dev, c.layout, nullptr);
    if (c.fb) vkDestroyFramebuffer(s.dev, c.fb, nullptr);
    if (c.fb_up) vkDestroyFramebuffer(s.dev, c.fb_up, nullptr);
    if (c.pass) vkDestroyRenderPass(s.dev, c.pass, nullptr);
    free_image(&c.depth);
    free_image(&c.depth_up);
    c = Chars();
}

static bool chars_create() {
    Chars &c = s.chars;
    // The title's D24 surfaces' format (sampled and an attachment, 24 bits or better), else D16
    // (required for both): its precision only has to tell a character's surface from what is in
    // front of it.
    c.fmt = s.depth24_format != VK_FORMAT_UNDEFINED ? s.depth24_format : VK_FORMAT_D16_UNORM;
    if (make_surface_image(&c.depth, c.fmt, true, s.ow, s.oh, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, true)) return false;
    VkAttachmentDescription att{};
    att.format = c.fmt;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAttachmentReference dref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.pDepthStencilAttachment = &dref;
    // After the previous frames' reads of the mask; before this frame's.
    VkSubpassDependency dep[2] = {
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_SHADER_READ_BIT,
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0},
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0}};
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = dep;
    if (VKC(vkCreateRenderPass, s.dev, &rp, nullptr, &c.pass) != VK_SUCCESS) return false;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = c.pass;
    fi.attachmentCount = 1;
    fi.pAttachments = &c.depth.view;
    fi.width = (uint32_t)s.ow;
    fi.height = (uint32_t)s.oh;
    fi.layers = 1;
    if (VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &c.fb) != VK_SUCCESS) return false;
    if (cfg.taa && taa_up_wanted(nullptr, nullptr)) {   // --taa-upscale: the output-size copy (both images are the target's size)
        if (make_surface_image(&c.depth_up, c.fmt, true, s.ow, s.oh, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, true)) return false;
        fi.pAttachments = &c.depth_up.view;
        if (VKC(vkCreateFramebuffer, s.dev, &fi, nullptr, &c.fb_up) != VK_SUCCESS) return false;
    }
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &c.layout) != VK_SUCCESS) return false;
    c.vs = shader(char_mask_vert, sizeof char_mask_vert);
    if (!c.vs) return false;
    VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    st.stage = VK_SHADER_STAGE_VERTEX_BIT;
    st.module = c.vs;
    st.pName = "main";
    VkVertexInputBindingDescription bind{0, sizeof(RVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RVertex, x)};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions = &attr;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = s.depth_clamp ? VK_TRUE : VK_FALSE;   // as the main pass
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;   // the nearest surface wins either way
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 1;
    pi.pStages = &st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cbs;
    pi.pDynamicState = &dy;
    pi.layout = c.layout;
    pi.renderPass = c.pass;
    return VKC(vkCreateGraphicsPipelines, s.dev, s.pcache, 1, &pi, nullptr, &c.pipe) == VK_SUCCESS;
}

// At renderer_init, before the depth of field, splash and wet ground (they need it): the character
// mask when RendererConfig.rain, lamp, contact or dof is set and the depth target can be sampled.
// A failure leaves it off, and with it the splashes, wet ground and depth of field.
static void chars_init() {
    if ((!cfg.rain && !cfg.damp && !cfg.lamp && !cfg.contact && !cfg.dof && !(cfg.sssr > 0 && cfg.sssr_glass)) || !s.depth_sampled) return;
    if (chars_create()) {
        s.chars.ok = true;
        return;
    }
    chars_free();
}

// Outside a render pass, before the frame's main pass, in a frame with splashes, wet ground, lamp
// lights, contact shadows or the depth of field: the mask cleared, then every frame-target draw
// flagged RENDERER_CHARACTER, depth only, with its own viewport (as exec_op). Nothing in other
// frames.
// --taa-upscale: called twice in a frame with the TAA op, the scene's render-size mask (jittered, into
// fb) and the output-size copy (fb_up, unjittered: up = true, the target size w x h, sx / sy at it).
static void chars_record(VkDeviceSize index_offset, float sx, float sy, bool up = false, int w = 0, int h = 0) {
    Chars &c = s.chars;
    if (!c.ok || (up && !c.fb_up)) return;
    if (!up) {
        w = s.ow;
        h = s.oh;
    }
    bool used = false;
    for (const Op &op : s.ops)
        if (op.kind == OP_SPLASH || op.kind == OP_WET || op.kind == OP_LAMP || op.kind == OP_CONTACT) {
            used = true;
            break;
        }
    if (!used && !(s.dof.ok && s.dof.want)) return;
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = c.pass;
    rb.framebuffer = up ? c.fb_up : c.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    bool bound = false;
    for (const Op &op : s.ops) {
        if (op.kind != OP_DRAW || op.target[0] || op.target[1] || !(op.draw_flags & RENDERER_CHARACTER) || s.indices.empty()) continue;
        VkViewport v{op.vp.x * sx, op.vp.y * sy, op.vp.w * sx, op.vp.h * sy, op.vp.minz, op.vp.maxz};
        int x0 = (int)v.x, y0 = (int)v.y, x1 = (int)(v.x + v.width), y1 = (int)(v.y + v.height);
        x0 = x0 < 0 ? 0 : x0;
        y0 = y0 < 0 ? 0 : y0;
        x1 = x1 > w ? w : x1;
        y1 = y1 > h ? h : y1;
        if (x1 <= x0 || y1 <= y0 || v.width <= 0 || v.height <= 0) continue;
        if (!bound) {
            vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.pipe);
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(s.cmd, 0, 1, &cur().geometry.buf, &zero);
            vkCmdBindIndexBuffer(s.cmd, cur().geometry.buf, index_offset, VK_INDEX_TYPE_UINT32);
            bound = true;
        }
        VkRect2D sc{{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
        vkCmdSetViewport(s.cmd, 0, 1, &v);
        vkCmdSetScissor(s.cmd, 0, 1, &sc);
        const bool jit = !up && (size_t)(&op - s.ops.data()) < s.taa.jit_until && (op.draw_flags & RENDERER_DEPTH_TEST);   // --taa: as in the frame
        vkCmdDrawIndexed(s.cmd, op.index_count, 1, op.first_index, op.vertex_offset, jit ? taa_instance(v.width, v.height) : 0);
        c.executed++;
    }
    vkCmdEndRenderPass(s.cmd);
}

// --reflections sssr's surface pass (Wet surf): outside a render pass, before the frame's main pass,
// in a frame whose wet op (at wet_at) has glass: the images cleared, then every frame-target draw
// before the op with a glossy material specular map (renderer_set_relief_glossy, --dry-highlights),
// depth-tested, not a character, soft particle or emissive draw, with its own viewport (as exec_op)
// and the frame's --taa jitter. Nothing in other frames (surf_live false).
static void surf_record(VkDeviceSize index_offset, float sx, float sy, std::unordered_map<uint64_t, VkDescriptorSet> &sets, size_t wet_at) {
    Wet &a = s.wet;
    a.surf_live = false;
    a.surf_executed = 0;
    if (!a.surf_pipe || wet_at >= s.ops.size() || !(a.p.glass > 0) || s.indices.empty()) return;
    const int w = s.ow, h = s.oh;
    VkClearValue clear[2]{};
    clear[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = a.surf_pass;
    rb.framebuffer = a.surf_fb;
    rb.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    rb.clearValueCount = 2;
    rb.pClearValues = clear;
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    a.surf_live = true;
    const float pk[2] = {1.0f / (a.p.px > 0 ? a.p.px : 1.0f), 1.0f / (a.p.py > 0 ? a.p.py : 1.0f)};
    bool bound = false;
    VkDescriptorSet bound_set = VK_NULL_HANDLE;
    for (size_t i = 0; i < wet_at; i++) {
        const Op &op = s.ops[i];
        if (op.kind != OP_DRAW || op.target[0] || op.target[1] || !op.relief_tex[1] || (op.pipe_key & SOFT_KEY) || (op.shape & EMIT_SHAPE) ||
            (op.draw_flags & RENDERER_CHARACTER) || !(op.draw_flags & RENDERER_DEPTH_TEST))
            continue;
        const uint32_t word = (uint32_t)op.relief[2];
        if (!((word >> 2) & 16u)) continue;   // a glossy map read with its material (renderer_set_relief_glossy, --dry-highlights)
        VkViewport v{op.vp.x * sx, op.vp.y * sy, op.vp.w * sx, op.vp.h * sy, op.vp.minz, op.vp.maxz};
        int x0 = (int)v.x, y0 = (int)v.y, x1 = (int)(v.x + v.width), y1 = (int)(v.y + v.height);
        x0 = x0 < 0 ? 0 : x0;
        y0 = y0 < 0 ? 0 : y0;
        x1 = x1 > w ? w : x1;
        y1 = y1 > h ? h : y1;
        if (x1 <= x0 || y1 <= y0 || v.width <= 0 || v.height <= 0) continue;
        const uint32_t rt[RENDERER_STAGES] = {0u, op.relief_tex[1]};
        VkDescriptorSet set = frame_set(rt, sets);
        if (!set) continue;
        if (!bound) {
            vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.surf_pipe);
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(s.cmd, 0, 1, &cur().geometry.buf, &zero);
            vkCmdBindIndexBuffer(s.cmd, cur().geometry.buf, index_offset, VK_INDEX_TYPE_UINT32);
            bound = true;
        }
        if (set != bound_set) vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.surf_layout, 0, 1, &set, 0, nullptr);
        bound_set = set;
        const float pc[4] = {pk[0], pk[1], (float)(word & 3u), 0};
        vkCmdPushConstants(s.cmd, a.surf_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
        VkRect2D sc{{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
        vkCmdSetViewport(s.cmd, 0, 1, &v);
        vkCmdSetScissor(s.cmd, 0, 1, &sc);
        const bool jit = i < s.taa.jit_until;   // --taa: as in the frame
        vkCmdDrawIndexed(s.cmd, op.index_count, 1, op.first_index, op.vertex_offset, jit ? taa_instance(v.width, v.height) : 0);
        a.surf_executed++;
    }
    vkCmdEndRenderPass(s.cmd);
}

// Records the frame's commands into s.cmd (begun here) through the end of the render pass; the
// target ends in TRANSFER_SRC_OPTIMAL. Offscreen passes come first.
// Supersampling: the color target box-filtered down to the output size, 2:1 per step (a linear
// blit at exactly half the size samples between four texels: their average). Each downsample
// image ends in TRANSFER_SRC_OPTIMAL; it is overwritten after the previous frame's reads of it
// (readback copy, swapchain blit, the next step), all transfers on the one queue.
static void record_downsample(void) {
    if (s.ss == 1) return;
    VkImage src = s.color.img;
    int sw = s.ow, sh = s.oh;
    for (int k = 0; sw > s.fw; k++, sw /= 2, sh /= 2) {
        VkImage dst = s.down[k].img;
        barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit b{};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[1] = {sw, sh, 1};
        b.dstSubresource = b.srcSubresource;
        b.dstOffsets[1] = {sw / 2, sh / 2, 1};
        vkCmdBlitImage(s.cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        src = dst;
    }
}

static int record_frame(void) {
    VkDeviceSize index_offset = 0;
    float sx = (float)s.ow / s.w, sy = (float)s.oh / s.h;   // back-buffer pixels -> target pixels
    const int full_w = s.ow, full_h = s.oh;
    const float full_sx = sx, full_sy = sy;
    if (wait_slot()) return -1;   // normally done by renderer_begin
    bool taa_frame = false;
    for (const Op &op : s.ops) taa_frame |= op.kind == OP_TAA;
    s.ssao.taa_frame = taa_frame;   // --ssao-mode gtao: per-frame noise, one denoise pass
    if (taa_frame) taa_poses();   // --taa: the characters' previous poses, with the frame's jobs
    else s.taa.vel.clear();
    if (!s.indices.empty() && (upload_geometry(&index_offset) || upload_programs() || skin_upload(index_offset))) return -1;
    cur().vcheck.swap(s.skin.vcheck);   // checked when this slot's fence is waited
    cur().vcheck_index_offset = index_offset;
    s.skin.vcheck.clear();
    VKC(vkResetDescriptorPool, s.dev, cur().set_pool, 0);   // the slot's previous frame has completed (fence)
    s.glass.executed = 0;
    s.bloom.executed = 0;
    s.filmic.executed = 0;
    s.taa.executed = 0;
    s.fg.executed = 0;
    s.fg.captured = s.fg.built = s.fg.hud_taken = false;
    s.dof.executed = 0;
    s.rain.executed = 0;
    s.sky.executed = s.sky.lines_executed = 0;
    s.ssao.executed = 0;
    s.splash.executed = 0;
    if (!s.wet.executed) s.wet.hist_ok = false;   // --reflections sssr: no history after a frame without the pass
    s.wet.executed = 0;
    s.cover.executed = 0;
    s.wshadow.executed = 0;
    s.chars.executed = 0;
    s.soft.executed = 0;
    s.lamp.executed = 0;
    s.shaft.executed = 0;
    s.volfog.executed = 0;
    s.charlit.executed = 0;
    s.charlit.world_executed = 0;
    s.emit.executed = 0;
    s.relief.executed = 0;
    s.hair.executed = 0;
    s.skinlit.executed = 0;
    s.skinlit.mat_executed = 0;
    s.soft.live = s.soft.in_pass = false;
    bool glass = false;
    size_t bloom_at = s.ops.size();   // the bloom split (renderer_bloom), if any
    size_t ssao_at = s.ops.size();    // the ssao split (renderer_ssao), if any
    size_t splash_at = s.ops.size();  // the rain splashes (renderer_rain_splash), if any
    size_t wet_at = s.ops.size();     // the wet ground (renderer_rain_wet), if any
    size_t filmic_at = s.ops.size();  // the filmic pass (renderer_filmic), if any
    size_t lamp_at = s.ops.size();    // the lamp lights (renderer_lamp_lights), if any
    size_t contact_at = s.ops.size(); // the contact shadows (renderer_contact), if any
    size_t shaft_at = s.ops.size();   // the light shafts (renderer_light_shafts), if any
    size_t volfog_at = s.ops.size();  // the volumetric fog (renderer_volfog), if any
    size_t grab_at = s.ops.size();    // the history mark (renderer_prev_frame_mark), if any
    size_t taa_at = s.ops.size();     // the temporal resolve (renderer_taa), if any: the draws before it are jittered
    size_t fg_at = s.ops.size();      // the HUD-less frame and motion field (renderer_frame_gen), if any
    size_t fg_end_at = s.ops.size();  // the HUD's end (renderer_frame_gen_hud_end), if any
    s.contact.executed = 0;
    s.contact.pending = false;
    for (size_t i = 0; i < s.ops.size(); i++) {
        if (s.ops[i].kind == OP_CONTACT && contact_at == s.ops.size()) contact_at = i;
        if (s.ops[i].kind == OP_FILMIC && filmic_at == s.ops.size()) filmic_at = i;
        if (s.ops[i].kind == OP_LAMP && lamp_at == s.ops.size()) lamp_at = i;
        if (s.ops[i].kind == OP_SHAFT && shaft_at == s.ops.size()) shaft_at = i;
        if (s.ops[i].kind == OP_VOLFOG && volfog_at == s.ops.size()) volfog_at = i;
        if (s.ops[i].kind == OP_GRAB && grab_at == s.ops.size()) grab_at = i;
        if (s.ops[i].kind == OP_TAA && taa_at == s.ops.size()) taa_at = i;
        if (s.ops[i].kind == OP_FG && fg_at == s.ops.size()) fg_at = i;
        if (s.ops[i].kind == OP_FG_END && fg_end_at == s.ops.size()) fg_end_at = i;
        glass |= s.ops[i].kind == OP_GLASS;
        if (s.ops[i].kind == OP_BLOOM && bloom_at == s.ops.size()) bloom_at = i;
        if (s.ops[i].kind == OP_SSAO && ssao_at == s.ops.size()) ssao_at = i;
        if (s.ops[i].kind == OP_SPLASH && splash_at == s.ops.size()) splash_at = i;
        if (s.ops[i].kind == OP_WET && wet_at == s.ops.size()) wet_at = i;
    }
    // Soft particles (renderer_soft): from soft_at on the depth attachment is read-only, so a later
    // depth clear or depth-writing draw on the frame target cancels them for the frame. The pass
    // splits right before it (ssao, splashes) already resume with the read-only depth (soft_from).
    size_t soft_at = s.ops.size();
    for (size_t i = 0; i < s.ops.size() && soft_at == s.ops.size(); i++)
        if (s.ops[i].kind == OP_SOFT) soft_at = i;
    for (size_t i = soft_at + 1; i < s.ops.size(); i++) {
        const Op &op = s.ops[i];
        if (op.target[0] || op.target[1]) continue;
        if ((op.kind == OP_CLEAR && (op.flags & RENDERER_CLEAR_DEPTH)) || (op.kind == OP_DRAW && (op.pipe_key >> 19 & 1))) {
            soft_at = s.ops.size();
            break;
        }
    }
    size_t soft_from = soft_at;
    while (soft_at < s.ops.size() && soft_from > 0 && (s.ops[soft_from - 1].kind == OP_SSAO || s.ops[soft_from - 1].kind == OP_SPLASH ||
                                                        s.ops[soft_from - 1].kind == OP_LAMP || s.ops[soft_from - 1].kind == OP_CONTACT ||
                                                        s.ops[soft_from - 1].kind == OP_SHAFT || s.ops[soft_from - 1].kind == OP_VOLFOG))
        soft_from--;
    if (soft_at < s.ops.size()) {
        const RSoft &p = s.ops[soft_at].soft;
        s.soft.push[0] = p.depth_a;
        s.soft.push[1] = p.depth_b;
        s.soft.push[2] = 1.0f / p.distance;
        s.soft.push[3] = 0;
    }
    glass = glass && glass_ready();
    std::unordered_map<uint64_t, VkDescriptorSet> sets;
    for (const Op &op : s.ops)   // offscreen framebuffers first: creating one is a one-time submission
        if (op.target[0] || op.target[1]) offscreen_fb(op.target[0], op.target[1]);
    twins_create();   // the same for the copies self-sampling draws read
    VKC(vkResetCommandBuffer, s.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer, s.cmd, &bi);
    overlay_stamp_begin();   // --fps-overlay-items gpu
    gp_begin();              // --stats
    skin_record();   // before every pass that reads the frame's vertices
    charlit_update();
    lshadow_update();
    gp_mark(GP_PREP);
    record_offscreen(sets, index_offset);
    gp_mark(GP_TARGETS);
    cover_record(index_offset);
    gp_mark(GP_COVER);
    // --taa-upscale: everything before the TAA op renders at the scene's render
    // size, the top-left rw x rh of every frame-size image: the target size and the back-buffer
    // scale are swapped until the op (viewports, scissors, render areas and the passes' size
    // constants follow; their shaders read by gl_FragCoord), then restored for the resolve and after.
    // The half-size work passes (ssao, lamp lights, light shafts, splash mask) cover only the
    // rectangle's share of their images too (their shaders clamp to the work size they are given);
    // the contact shadows' mask keeps its size (its composite normalises by it).
    bool scoped = false;
    int work[8] = {};
    if (s.taa.up && taa_at < s.ops.size()) {
        scoped = s.taa.scoped = true;
        s.ow = s.taa.rw;
        s.oh = s.taa.rh;
        sx = (float)s.ow / s.w;
        sy = (float)s.oh / s.h;
        int *ws[8] = {&s.ssao.hw, &s.ssao.hh, &s.lamp.hw, &s.lamp.hh, &s.shaft.hw, &s.shaft.hh, &s.splash.mw, &s.splash.mh};
        const int sc[4] = {s.ssao.scale, s.lamp.scale, s.shaft.scale, s.splash.scale};
        for (int k = 0; k < 8; k++) {
            work[k] = *ws[k];
            const int n = sc[k / 2] > 0 ? ((k % 2 ? s.oh : s.ow) + sc[k / 2] - 1) / sc[k / 2] : 0;
            if (n > 0 && n < *ws[k]) *ws[k] = n;
        }
    }
    auto unscope = [&] {
        if (!scoped) return;
        scoped = s.taa.scoped = false;
        s.ow = full_w;
        s.oh = full_h;
        sx = full_sx;
        sy = full_sy;
        int *ws[8] = {&s.ssao.hw, &s.ssao.hh, &s.lamp.hw, &s.lamp.hh, &s.shaft.hw, &s.shaft.hh, &s.splash.mw, &s.splash.mh};
        for (int k = 0; k < 8; k++) *ws[k] = work[k];
    };
    wshadow_record(index_offset, sx, sy, sets);
    gp_mark(GP_WSHADOW);
    s.taa.jit_until = taa_at < s.ops.size() ? taa_at : 0;   // --taa: the character mask's draws jittered as the frame's
    if (taa_at < s.ops.size()) taa_jitter_n(s.taa.frame, s.taa.phases, s.taa.jit);
    chars_record(index_offset, sx, sy);
    chars_record(index_offset, full_sx, full_sy, true, full_w, full_h);   // --taa-upscale: the output-size copy
    gp_mark(GP_CHARS);
    surf_record(index_offset, sx, sy, sets, wet_at);   // --reflections sssr's glass
    gp_mark(GP_SURF);
    if (taa_at < s.ops.size()) {
        taa_vel_record(index_offset, sx, sy, taa_at);   // --taa: the characters' motion
    } else {
        s.taa.vel_live = false;
    }
    gp_mark(GP_TAAVEL);

    // Initial color: the base image (or black) from the upload buffer, copied into the target or,
    // on the quad path, into the base image the first draw samples.
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)s.w, (uint32_t)s.h, 1};
    if (!s.quad_base) {
        // After the previous frame's (possibly still in flight) pass writing it and copies or
        // blits reading it.
        barrier(s.color.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyBufferToImage(s.cmd, cur().upload.buf, s.color.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    } else if (s.base_pending) {
        barrier(s.base.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyBufferToImage(s.cmd, cur().upload.buf, s.base.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(s.base.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }

    // Movie planes with new data: copied from this slot's movie buffer into the persistent images,
    // after earlier frames' (possibly in-flight) passes sampling them.
    for (int i = 0; i < MOVIE_PLANES; i++) {
        if (!(s.movie_pending >> i & 1)) continue;
        VkBufferImageCopy mr{};
        mr.bufferOffset = s.movie_off[i];
        mr.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        mr.imageExtent = {s.movie_w[i], s.movie_h[i], 1};
        barrier(s.movie[i].img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyBufferToImage(s.cmd, cur().movie.buf, s.movie[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &mr);
        barrier(s.movie[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    s.movie_pending = 0;

    VkClearValue clears[3]{};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = glass || bloom_at < s.ops.size() || ssao_at < s.ops.size() || splash_at < s.ops.size() || wet_at < s.ops.size() ||
                            soft_at < s.ops.size() || filmic_at < s.ops.size() || lamp_at < s.ops.size() ||
                            contact_at < s.ops.size() || shaft_at < s.ops.size() || volfog_at < s.ops.size() || grab_at < s.ops.size() ||
                            taa_at < s.ops.size() || fg_at < s.ops.size() || fg_end_at < s.ops.size()
                        ? s.split_pass
                        : s.pass;
    rb.framebuffer = s.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    rb.clearValueCount = s.samples != VK_SAMPLE_COUNT_1_BIT ? 3 : 2;
    rb.pClearValues = clears;
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    PassState ps;
    // The contact shadows join the split of the op right after them (lamp lights, ssao or splashes).
    auto contact_take = [&]() -> const RContact * {
        if (!s.contact.pending) return nullptr;
        s.contact.pending = false;
        return &s.contact.p;
    };
    for (size_t i = 0; i < s.ops.size(); i++) {
        if (s.ops[i].target[0] || s.ops[i].target[1]) continue;
        if (i == soft_from && soft_at < s.ops.size()) s.soft.live = true;
        // A soft draw runs as one only inside the soft pass; elsewhere as without the flag.
        Op plain;
        const Op *opp = &s.ops[i];
        if (opp->kind == OP_DRAW && (opp->pipe_key & SOFT_KEY) && !s.soft.in_pass) {
            plain = *opp;
            plain.pipe_key &= ~SOFT_KEY;
            opp = &plain;
        }
        const Op &op = *opp;
        if (op.kind == OP_SOFT) {
            if (i == soft_at && !s.soft.in_pass) {
                gp_mark(GP_SCENE);
                soft_split();
                gp_mark(GP_SOFT);
                ps = PassState();   // a new render pass instance
            }
        } else if (op.kind == OP_BLOOM) {
            if (i == bloom_at) {
                gp_mark(GP_SCENE);
                bloom_exec();
                gp_mark(GP_BLOOM);
                ps = PassState();   // a new render pass instance
            }
        } else if (op.kind == OP_SSAO) {
            if (i == ssao_at) {   // the lamp lights right before it and the splashes right after it share the split
                gp_mark(GP_SCENE);
                ssao_exec(&op.ssao, splash_at < s.ops.size() && splash_at == i + 1 ? &s.splash.p : nullptr,
                          lamp_at < s.ops.size() && lamp_at + 1 == i ? &s.lamp.p : nullptr, contact_take());
                ps = PassState();   // a new render pass instance
            }
        } else if (op.kind == OP_LAMP) {
            if (i == lamp_at && ssao_at != lamp_at + 1) {   // alone, with the splashes right after it
                gp_mark(GP_SCENE);
                ssao_exec(nullptr, splash_at < s.ops.size() && splash_at == i + 1 ? &s.splash.p : nullptr, &s.lamp.p, contact_take());
                ps = PassState();
            }
        } else if (op.kind == OP_SPLASH) {
            if (i == splash_at && splash_at != ssao_at + 1 && !(lamp_at < s.ops.size() && splash_at == lamp_at + 1 && ssao_at != lamp_at + 1)) {
                gp_mark(GP_SCENE);
                ssao_exec(nullptr, &s.splash.p, nullptr, contact_take());
                ps = PassState();
            }
        } else if (op.kind == OP_CONTACT) {
            if (i == contact_at) {
                const size_t j = i + 1;
                const bool joins = j < s.ops.size() && ((s.ops[j].kind == OP_LAMP && j == lamp_at) || (s.ops[j].kind == OP_SSAO && j == ssao_at) ||
                                                        (s.ops[j].kind == OP_SPLASH && j == splash_at));
                if (joins) {
                    s.contact.pending = true;
                } else {
                    gp_mark(GP_SCENE);
                    ssao_exec(nullptr, nullptr, nullptr, &s.contact.p);
                    ps = PassState();
                }
            }
        } else if (op.kind == OP_SHAFT) {
            if (i == shaft_at) {
                gp_mark(GP_SCENE);
                shaft_exec(s.shaft.p);
                gp_mark(GP_SHAFT);
                ps = PassState();
            }
        } else if (op.kind == OP_VOLFOG) {
            if (i == volfog_at) {
                gp_mark(GP_SCENE);
                volfog_exec(s.volfog.p);
                ps = PassState();
            }
        } else if (op.kind == OP_WET) {
            if (i == wet_at) {
                gp_mark(GP_SCENE);
                wet_exec(s.wet.p);
                gp_mark(GP_WET);
                ps = PassState();
            }
        } else if (op.kind == OP_FILMIC) {
            if (i == filmic_at) {
                gp_mark(GP_SCENE);
                filmic_exec();
                gp_mark(GP_FILMIC);
                ps = PassState();   // a new render pass instance
            }
        } else if (op.kind == OP_GLASS) {
            gp_mark(GP_SCENE);
            if (glass && glass_exec(op.glass, sx, sy)) ps = PassState();
            gp_mark(GP_GLASS);
        } else if (op.kind == OP_GRAB) {
            if (i == grab_at) {
                gp_mark(GP_SCENE);
                prev_grab_exec();
                gp_mark(GP_GRAB);
                ps = PassState();   // a new render pass instance
            }
        } else if (op.kind == OP_TAA) {
            if (i == taa_at) {
                unscope();   // --taa-upscale: the resolve and what follows at the output size
                gp_mark(GP_SCENE);
                taa_exec(sx, sy);
                gp_mark(GP_TAA);
                ps = PassState();   // a new render pass instance
            }
        } else if (op.kind == OP_FG) {
            if (i == fg_at) {
                gp_mark(GP_SCENE);
                fg_capture_exec(sx, sy);
                gp_mark(GP_FG);
                ps = PassState();   // a new render pass instance
            }
        } else if (op.kind == OP_FG_END) {
            if (i == fg_end_at && s.fg.captured) {
                gp_mark(GP_SCENE);
                fg_hud_end_exec();
                gp_mark(GP_FG);
                ps = PassState();
            }
        } else if (op.kind == OP_RAIN) {
            gp_mark(GP_SCENE);
            rain_exec(s.rain.list[op.first_index]);
            gp_mark(GP_RAIN);
            ps = PassState();   // another pipeline was bound
        } else if (op.kind == OP_SKY) {
            s.taa.jitter_now = i < taa_at;   // --taa: aligned with the jittered scene
            gp_mark(GP_SCENE);
            sky_exec(s.sky.list[op.first_index]);
            gp_mark(GP_SKY);
            s.taa.jitter_now = false;
            ps = PassState();
        } else if (op.kind == OP_SKYLINE) {
            s.taa.jitter_now = i < taa_at;
            gp_mark(GP_SCENE);
            skyline_exec(op, index_offset, sx, sy);
            gp_mark(GP_SKY);
            s.taa.jitter_now = false;
            ps = PassState();
        } else {
            s.taa.jitter_now = i < taa_at && op.kind == OP_DRAW && (op.draw_flags & RENDERER_DEPTH_TEST);
            if (i < bloom_at) {   // the scene before the bloom split: with the light mask
                Op m = op;
                m.mask = true;
                if (op.kind == OP_DRAW) m.pipe_key |= mask_role(op.pipe_key) << 27;
                exec_op(m, ps, sets, index_offset, sx, sy, s.ow, s.oh);
            } else {
                exec_op(op, ps, sets, index_offset, sx, sy, s.ow, s.oh);
            }
            s.taa.jitter_now = false;
        }
    }
    vkCmdEndRenderPass(s.cmd);   // color (or the resolve target) ends in TRANSFER_SRC_OPTIMAL
    gp_mark(GP_SCENE);
    if (s.fg.captured && !s.fg.hud_taken) {   // --frame-gen: the HUD ends with the frame
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        fg_take_target(s.fg.hud_b);
        s.fg.hud_taken = true;
        gp_mark(GP_FG);
    }
    unscope();
    s.soft.live = s.soft.in_pass = false;
    record_prev_frame();
    gp_mark(GP_PREV);
    record_downsample();
    gp_mark(GP_DOWNSAMPLE);
    record_sharpen();
    gp_mark(GP_SHARPEN);
    record_fsr();     // instead of the sharpening (sharpen_init leaves it off with FSR)
    gp_mark(GP_FSR);
    record_gamma();   // last on the frame: the console applies the ramp on scanout
    gp_mark(GP_GAMMA);
    if (s.fg.captured) {   // --frame-gen: the frame between the previous one and this one
        fg_build();
        gp_mark(GP_FG);
    }
    overlay_stamp_end();
    return 0;
}

int renderer_end(uint32_t *out, int pitch_bytes) {
    if (!s.dev) return fail("renderer not initialized");
    double t_start = now_ms();
    VkDeviceSize frame_bytes = (VkDeviceSize)s.dispw * s.disph * 4;
    if (record_frame()) return -1;
    if (s.ov.ok) record_overlay_rb();   // last, on the frame read back
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)s.dispw, (uint32_t)s.disph, 1};
    vkCmdCopyImageToBuffer(s.cmd, output_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.readback.buf, 1, &region);
    VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hb.buffer = s.readback.buf;
    hb.offset = 0;
    hb.size = frame_bytes;
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
    const bool fg_rb = s.fg.built;   // --frame-gen: the generated frame too
    if (fg_rb) {
        vkCmdCopyImageToBuffer(s.cmd, s.fg.gen.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.fg.rb.buf, 1, &region);
        hb.buffer = s.fg.rb.buf;
        vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
    }
    s.fg.rb_valid = false;
    VKC(vkEndCommandBuffer, s.cmd);
    double t_submit = now_ms();

    // Synchronous: the readback is copied out below (and earlier frames still in flight complete
    // first, in queue order).
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cmd;
    const double slot_wait = s.wait_ms;
    VkResult r = submit_slot(&si);
    s.ops.clear();
    if (r != VK_SUCCESS) return fail("frame submit failed (%d)", r);
    if (wait_slot()) return -1;
    s.fg.rb_valid = fg_rb;
    if (!s.skin.jobs.empty()) {   // the debug readback of the device-computed vertices
        s.skin.debug_verts = (const RVertex *)cur().geometry.map;
        s.skin.debug_normals = s.normals.empty() ? nullptr : (const float *)((const uint8_t *)cur().geometry.map + normal_offset);
    }
    next_slot();
    double t_done = now_ms();
    const uint32_t *px = (const uint32_t *)s.readback.map;
    for (int y = 0; y < s.disph; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)out + (size_t)y * pitch_bytes);
        for (int x = 0; x < s.dispw; x++) row[x] = px[(size_t)y * s.dispw + x] & 0x00FFFFFFu;
    }
    s.timing.record_ms = (float)(t_submit - t_start);
    s.timing.gpu_ms = (float)(t_done - t_submit + slot_wait);   // with the wait for the slot at renderer_begin
    s.timing.readback_ms = (float)(now_ms() - t_done);
    s.timing.present_ms = 0;
    pipe_frame(t_start);
    return 0;
}

int renderer_can_present(void) { return s.surface != VK_NULL_HANDLE; }
const char *renderer_present_info(void) { return s.present_info; }

int renderer_present(void) {
    if (!s.dev || !s.surface) return fail("no window surface");
    double t_start = now_ms();
    if (record_frame()) return -1;
    if (cfg.drawable_size && s.swapchain) {   // follow the window (resize, fullscreen)
        int dw = 0, dh = 0;
        cfg.drawable_size(&dw, &dh);
        if ((uint32_t)dw != s.sc_extent.width || (uint32_t)dh != s.sc_extent.height) s.sc_stale = true;
    }
    // Present waits: the real frame before the last one on screen first (at most one frame's pair
    // queued). A wait that times out three times turns them off (the queue paces as before).
    if (s.fg.pw && s.swapchain && !s.sc_stale && s.fg.pw_prev) {
        const double t0 = now_ms();
        const VkResult wr = wait_for_present(s.dev, s.swapchain, s.fg.pw_prev, 100000000ull);
        s.fg.pw_t = now_ms();
        s.fg.pw_waited = s.fg.pw_t - t0 > 0.25;
        s.fg.pw_new = wr == VK_SUCCESS;
        if (wr == VK_TIMEOUT && ++s.fg.pw_timeouts >= 3) s.fg.pw = false;
        s.fg.pw_prev = 0;
    }
    double t_acquire = now_ms(), acquire_ms = 0;
    uint32_t idx = 0;
    VkResult r = VK_SUCCESS;
    for (int attempt = 0; attempt < 2; attempt++) {
        if ((s.sc_stale || !s.swapchain) && create_swapchain()) return drop_present();
        if (!s.swapchain) break;   // minimized: render without presenting
        r = VKC(vkAcquireNextImageKHR, s.dev, s.swapchain, 1000000000ull, cur().acquired, VK_NULL_HANDLE, &idx);
        if (r != VK_ERROR_OUT_OF_DATE_KHR) break;
        s.sc_stale = true;
    }
    acquire_ms = now_ms() - t_acquire;
    bool show = s.swapchain && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR);
    if (s.swapchain && !show) {
        fail("vkAcquireNextImageKHR failed (%d)", r);
        return drop_present();
    }
    if (r == VK_SUBOPTIMAL_KHR) s.sc_stale = true;
    // --frame-gen: the generated frame goes into the first image, the real one into a second image
    // presented right after it (FIFO: a refresh later). A second image that cannot be had: the real
    // frame alone, in the first.
    uint32_t idx2 = 0;
    bool two = false;
    // While paused (show 2) the real frame goes into both, so the cadence of the queue does not change.
    const bool gen_shown = s.fg.show == 1 && s.fg.built;
    if (show && (gen_shown || s.fg.show == 2) && cur().acquired2) {
        const double t2 = now_ms();
        VkResult r2 = VKC(vkAcquireNextImageKHR, s.dev, s.swapchain, 1000000000ull, cur().acquired2, VK_NULL_HANDLE, &idx2);
        acquire_ms += now_ms() - t2;
        two = r2 == VK_SUCCESS || r2 == VK_SUBOPTIMAL_KHR;
        if (r2 == VK_SUBOPTIMAL_KHR || r2 == VK_ERROR_OUT_OF_DATE_KHR) s.sc_stale = true;
    }
    if (show) record_blit(s.sc_images[idx], idx, two && gen_shown ? s.fg.gen.img : VK_NULL_HANDLE);
    if (two) record_blit(s.sc_images[idx2], idx2);
    gp_mark(GP_BLIT);
    VKC(vkEndCommandBuffer, s.cmd);
    double t_submit = now_ms();
    const VkPipelineStageFlags wait_stages[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    const VkSemaphore waits[2] = {cur().acquired, cur().acquired2};
    VkSemaphore signals[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cmd;
    if (show) {
        signals[0] = s.sc_done[idx];
        if (two) signals[1] = s.sc_done[idx2];
        si.waitSemaphoreCount = two ? 2 : 1;
        si.pWaitSemaphores = waits;
        si.pWaitDstStageMask = wait_stages;
        si.signalSemaphoreCount = two ? 2 : 1;
        si.pSignalSemaphores = signals;
    }
    const double slot_wait = s.wait_ms;   // at renderer_begin: the slot's frame frames_in_flight ago
    r = submit_slot(&si);
    s.ops.clear();
    if (r != VK_SUCCESS) return fail("frame submit failed (%d)", r);
    double t_present = now_ms();
    VkResult pr = VK_SUCCESS;
    if (show) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &s.sc_done[idx];
        pi.swapchainCount = 1;
        pi.pSwapchains = &s.swapchain;
        pi.pImageIndices = &idx;
        // Present waits: the real frame (the second present, or the only one) carries an id.
        VkPresentIdKHR pid{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
        uint64_t id = 0;
        pid.swapchainCount = 1;
        pid.pPresentIds = &id;
        if (s.fg.pw && !two) {
            id = ++s.fg.pw_id;
            pi.pNext = &pid;
        }
        pr = VKC(vkQueuePresentKHR, s.queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) s.sc_stale = true;
        if (two) {   // the real frame after the generated one; the second result decides as the first
            if (gen_shown) s.fg.presented++;
            pi.pWaitSemaphores = &s.sc_done[idx2];
            pi.pImageIndices = &idx2;
            if (s.fg.pw) {
                id = ++s.fg.pw_id;
                pi.pNext = &pid;
            }
            const VkResult pr2 = VKC(vkQueuePresentKHR, s.queue, &pi);
            if (pr2 == VK_ERROR_OUT_OF_DATE_KHR || pr2 == VK_SUBOPTIMAL_KHR) s.sc_stale = true;
            if (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR || pr == VK_ERROR_OUT_OF_DATE_KHR) pr = pr2;
        }
    }
    if (show && s.fg.pw && s.fg.pw_id) {   // the next frame waits for the one before this one
        s.fg.pw_prev = s.fg.pw_last;
        s.fg.pw_last = s.fg.pw_id;
    }
    double t_presented = now_ms();
    // One frame in flight: wait for it now, as before. More: the next renderer_begin on this slot
    // waits, frames_in_flight frames later, so recording overlaps the GPU.
    if (s.nframes == 1 && wait_slot()) return -1;
    next_slot();
    s.timing.record_ms = (float)(t_submit - t_start - acquire_ms);
    s.timing.present_ms = (float)(acquire_ms + t_presented - t_present);
    s.timing.gpu_ms = (float)(s.nframes == 1 ? now_ms() - t_presented : slot_wait);
    s.timing.readback_ms = 0;
    pipe_frame(t_start);
    if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR && pr != VK_ERROR_OUT_OF_DATE_KHR) {
        fail("vkQueuePresentKHR failed (%d)", pr);
        return drop_present();
    }
    return 0;
}

void renderer_last_timing(RendererTiming *t) { *t = s.timing; }

}  // extern "C"

// ---- --gpu-skinning ----
static void skin_free() {
    Skin &k = s.skin;
    for (SkinSlot &sl : k.slot) {
        free_buffer(&sl.buf);
        free_buffer(&sl.dev);
        if (sl.pool) vkDestroyDescriptorPool(s.dev, sl.pool, nullptr);
        if (sl.queries) vkDestroyQueryPool(s.dev, sl.queries, nullptr);
    }
    for (VkPipeline p : {k.skin, k.faces, k.normals})
        if (p) vkDestroyPipeline(s.dev, p, nullptr);
    if (k.layout) vkDestroyPipelineLayout(s.dev, k.layout, nullptr);
    if (k.set_layout) vkDestroyDescriptorSetLayout(s.dev, k.set_layout, nullptr);
    k = Skin();
}
// A device-local buffer (not mapped), for data only the device reads and writes.
static int make_device_buffer(Buffer *b, VkDeviceSize size, VkBufferUsageFlags usage) {
    MemTag tag(RENDERER_VRAM_SKIN);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (VKC(vkCreateBuffer, s.dev, &bi, nullptr, &b->buf) != VK_SUCCESS) return fail("vkCreateBuffer failed");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(s.dev, b->buf, &req);
    int type = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = find_memory(req.memoryTypeBits, 0);
    if (type < 0) return fail("no memory type for a device buffer");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)type;
    if (VKC(vkAllocateMemory, s.dev, &ai, nullptr, &b->mem) != VK_SUCCESS) return fail("vkAllocateMemory (device buffer) failed");
    vram_add(type, req.size, false, &b->vram, &b->cat);
    VKC(vkBindBufferMemory, s.dev, b->buf, b->mem, 0);
    b->size = size;
    return 0;
}
static VkPipeline skin_pipeline(const uint32_t *code, size_t bytes) {
    VkShaderModule m = shader(code, bytes);
    if (!m) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = s.skin.layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (VKC(vkCreateComputePipelines, s.dev, s.pcache, 1, &ci, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(s.dev, m, nullptr);
    return p;
}
static bool skin_create() {
    Skin &k = s.skin;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s.phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qs(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(s.phys, &nq, qs.data());
    if (s.qfamily >= nq || !(qs[s.qfamily].queueFlags & VK_QUEUE_COMPUTE_BIT)) return false;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(s.phys, &props);
    const uint32_t bits = qs[s.qfamily].timestampValidBits;
    k.timestamps = bits > 0 && props.limits.timestampPeriod > 0;
    k.ts_period = props.limits.timestampPeriod;
    k.ts_mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    VkDescriptorSetLayoutBinding b[11];
    for (uint32_t i = 0; i < 11; i++) b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 11;
    li.pBindings = b;
    if (VKC(vkCreateDescriptorSetLayout, s.dev, &li, nullptr, &k.set_layout) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &k.set_layout;
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};   // vsh_skin.comp: the first job of the dispatch
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (VKC(vkCreatePipelineLayout, s.dev, &pli, nullptr, &k.layout) != VK_SUCCESS) return false;
    k.skin = skin_pipeline(vsh_skin_comp, sizeof vsh_skin_comp);
    k.faces = skin_pipeline(skin_faces_comp, sizeof skin_faces_comp);
    k.normals = skin_pipeline(skin_normals_comp, sizeof skin_normals_comp);
    if (!k.skin || !k.faces || !k.normals) return false;
    for (SkinSlot &sl : k.slot) {
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 11};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        if (VKC(vkCreateDescriptorPool, s.dev, &pi, nullptr, &sl.pool) != VK_SUCCESS) return false;
        if (k.timestamps) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2;
            if (VKC(vkCreateQueryPool, s.dev, &qi, nullptr, &sl.queries) != VK_SUCCESS) k.timestamps = false;
        }
    }
    return true;
}
// At renderer_init with RendererConfig.gpu_skin: a failure frees everything and leaves it off
// (renderer_skin_on() 0: the caller runs the programs on the CPU).
static void skin_init() {
    if (!cfg.gpu_skin) return;
    if (skin_create()) {
        s.skin.ok = true;
        return;
    }
    skin_free();
}

// Copies the frame's jobs and inputs into the slot's buffer and writes the frame's set (the
// geometry buffer's vertices, indices and normals regions at index_offset / normal_offset).
static int skin_upload(VkDeviceSize index_offset) {
    Skin &k = s.skin;
    k.set = VK_NULL_HANDLE;
    if (!k.ok || k.jobs.empty()) return 0;
    // The jobs by size (stable: the 64-vertex chunks), so skin_record dispatches each size class on
    // its own instead of every job times the largest one's chunks (a
    // frame's 511 jobs made a grid of 31682 workgroups for 1934 with work).
    auto chunks_of = [](const RSkinJob &j) { return (j.nv + 63) / 64; };
    bool sorted = true;
    for (size_t i = 1; i < k.jobs.size() && sorted; i++) sorted = chunks_of(k.jobs[i - 1]) <= chunks_of(k.jobs[i]);
    if (!sorted) {
        std::vector<uint32_t> order(k.jobs.size());
        for (uint32_t i = 0; i < order.size(); i++) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return chunks_of(k.jobs[a]) < chunks_of(k.jobs[b]); });
        std::vector<RSkinJob> jobs(k.jobs.size());
        std::vector<uint32_t> dst(k.dst.size());
        std::vector<uint8_t> prev(k.prev.size());
        for (size_t i = 0; i < order.size(); i++) {
            jobs[i] = k.jobs[order[i]];
            dst[i] = k.dst[order[i]];
            prev[i] = k.prev[order[i]];
        }
        k.jobs.swap(jobs);
        k.dst.swap(dst);
        k.prev.swap(prev);
        k.last_job = (size_t)-1;   // recording is over for the frame
    }
    SkinSlot &sl = k.slot[s.slot];
    const VkDeviceSize len[10] = {k.jobs.size() * sizeof(RSkinJob), k.njobs.size() * sizeof(SkinNormJob), k.ins.size() * 4,
                                  k.consts.size() * 4, k.data.size(), k.adj.size() * 4, (VkDeviceSize)k.nfaces * 16, (VkDeviceSize)k.nparts * 4,
                                  (VkDeviceSize)k.nverts * sizeof(RVertex), k.ndst.empty() ? 0 : (VkDeviceSize)k.nverts * 12};
    const void *src[6] = {k.jobs.data(), k.njobs.data(), k.ins.data(), k.consts.data(), k.data.data(), k.adj.data()};
    VkDeviceSize at = 0;
    for (int i = 0; i < 10; i++) {
        if (i == 6) sl.staged = at;
        sl.off[i] = at;
        sl.len[i] = len[i] > 16 ? len[i] : 16;
        at = (at + sl.len[i] + 255) & ~(VkDeviceSize)255;
    }
    if (sl.staged > sl.buf.size) {
        free_buffer(&sl.buf);
        VkDeviceSize size = 1 << 20;
        while (size < sl.staged) size *= 2;
        if (make_buffer(&sl.buf, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, true)) return -1;   // copied in video memory
    }
    if (at > sl.dev.size) {
        free_buffer(&sl.dev);
        VkDeviceSize size = 1 << 20;
        while (size < at) size *= 2;
        if (make_device_buffer(&sl.dev, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
            return -1;
    }
    for (int i = 0; i < 6; i++)
        if (len[i]) memcpy((uint8_t *)sl.buf.map + sl.off[i], src[i], len[i]);
    VKC(vkResetDescriptorPool, s.dev, sl.pool, 0);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = sl.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &k.set_layout;
    if (VKC(vkAllocateDescriptorSets, s.dev, &ai, &k.set) != VK_SUCCESS) return fail("gpu skinning: descriptor set allocation failed");
    const Buffer &g = cur().geometry;
    VkDescriptorBufferInfo bi[11];
    for (int i = 0; i < 9; i++) bi[i] = {sl.dev.buf, sl.off[i], sl.len[i]};
    bi[9] = {g.buf, index_offset, s.indices.size() * 4};
    bi[10] = {sl.dev.buf, sl.off[9], sl.len[9]};
    VkWriteDescriptorSet w[11];
    for (uint32_t i = 0; i < 11; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = k.set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(s.dev, 11, w, 0, nullptr);
    return 0;
}

static void skin_barrier(VkAccessFlags src_access, VkPipelineStageFlags src_stage, VkAccessFlags dst_access, VkPipelineStageFlags dst_stage) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = src_access;
    mb.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(s.cmd, src_stage, dst_stage, 0, 1, &mb, 0, nullptr, 0, nullptr);
}
// The pre-pass, first in the frame's commands: the jobs, then (lit draws) the face
// normals and the vertex normals, each step after the writes of the one before; the vertex input
// and every later shader read wait for it.
static void skin_record() {
    Skin &k = s.skin;
    SkinSlot &sl = k.slot[s.slot];
    if (sl.stamped) {   // this slot's previous frame has completed (wait_slot)
        uint64_t t[2];
        if (vkGetQueryPoolResults(s.dev, sl.queries, 0, 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            k.gpu_ms = (float)((double)((t[1] - t[0]) & k.ts_mask) * k.ts_period / 1e6);
        sl.stamped = false;
    }
    k.rec_draws = (uint32_t)k.jobs.size();
    k.rec_vertices = 0;
    for (const RSkinJob &j : k.jobs) k.rec_vertices += j.nv;
    if (!k.ok || k.jobs.empty() || !k.set) return;
    if (k.timestamps) {
        vkCmdResetQueryPool(s.cmd, sl.queries, 0, 2);
        vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, sl.queries, 0);
    }
    const VkBufferCopy in{0, 0, sl.staged};   // the inputs into device memory
    vkCmdCopyBuffer(s.cmd, sl.buf.buf, sl.dev.buf, 1, &in);
    skin_barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    gp_mark(GP_SKIN_IN);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.layout, 0, 1, &k.set, 0, nullptr);
    // One dispatch per step for all jobs (workgroup y = job): the steps are latency-bound, so many
    // small dispatches in a row cost far more than one wide one.
    uint32_t chunks = 0, parts = 0, nchunks = 0;
    for (const RSkinJob &j : k.jobs) chunks = (j.nv + 63) / 64 > chunks ? (j.nv + 63) / 64 : chunks;
    for (const SkinNormJob &n : k.njobs) {
        parts = n.nparts > parts ? n.nparts : parts;
        nchunks = (n.nv + 63) / 64 > nchunks ? (n.nv + 63) / 64 : nchunks;
    }
    // The jobs are sorted by size (skin_upload): one dispatch per power-of-two size class, its
    // first job in the push constant.
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.skin);
    for (uint32_t first = 0, n = (uint32_t)k.jobs.size(); first < n;) {
        uint32_t cls = 1;
        while (cls < (k.jobs[first].nv + 63) / 64) cls *= 2;
        uint32_t last = first;
        while (last < n && (k.jobs[last].nv + 63) / 64 <= cls) last++;
        const uint32_t wide = (k.jobs[last - 1].nv + 63) / 64;
        vkCmdPushConstants(s.cmd, k.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &first);
        vkCmdDispatch(s.cmd, wide ? wide : 1, last - first, 1);
        first = last;
    }
    gp_mark(GP_SKIN);
    if (!k.njobs.empty()) {
        skin_barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.faces);
        vkCmdDispatch(s.cmd, parts, (uint32_t)k.njobs.size(), 1);
        skin_barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.normals);
        vkCmdDispatch(s.cmd, nchunks, (uint32_t)k.njobs.size(), 1);
    }
    gp_mark(GP_SKIN_NORMALS);
    // The results into the frame's vertices and normals, one region per draw.
    skin_barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    std::vector<VkBufferCopy> &rc = k.regions;
    rc.clear();
    for (size_t i = 0; i < k.jobs.size(); i++)
        if (!k.prev[i])   // --taa's previous-pose runs stay in the device buffer (taa_vel_record reads them there)
            rc.push_back({sl.off[8] + (VkDeviceSize)k.jobs[i].out_vertex * sizeof(RVertex), (VkDeviceSize)k.dst[i] * sizeof(RVertex),
                          (VkDeviceSize)k.jobs[i].nv * sizeof(RVertex)});
    for (size_t i = 0; i < k.njobs.size(); i++)
        rc.push_back({sl.off[9] + (VkDeviceSize)k.njobs[i].vfirst * 12, normal_offset + (VkDeviceSize)k.ndst[i] * 12, (VkDeviceSize)k.njobs[i].nv * 12});
    vkCmdCopyBuffer(s.cmd, sl.dev.buf, cur().geometry.buf, (uint32_t)rc.size(), rc.data());
    skin_barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    gp_mark(GP_SKIN_OUT);
    if (k.timestamps) {
        vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, sl.queries, 1);
        sl.stamped = true;
    }
}

// Appends n trivially copyable elements with one memcpy (libc++'s range insert copies them one by
// one at -O1; a frame with every world program on the GPU appends ~1.5 MB of constants).
template <typename T> static void append_pod(std::vector<T> &v, const T *p, size_t n) {
    const size_t at = v.size();
    v.resize(at + n);
    if (n) memcpy(v.data() + at, p, n * sizeof(T));
}

// --taa: the pose of a skinned draw with identity id (the n-th of that identity this frame) is kept
// for the next frame; with last frame's, its job runs again with those constants (appended after
// the frame's jobs) and the draw joins the velocity pass (taa_vel_record).
static void taa_pose(uint64_t id, const float (*consts)[4], uint32_t op_index) {
    Taa &t = s.taa;
    Skin &k = s.skin;
    const uint64_t key = id ^ (uint64_t)t.seen[id]++ * 0x9E3779B97F4A7C15ull;
    const size_t n = 192 * 4;
    t.ids_now[key] = (uint32_t)t.pose_now.size();
    append_pod(t.pose_now, &consts[0][0], n);
    const auto it = t.ids_prev.find(key);
    const Op &op = s.ops[op_index];
    if (it == t.ids_prev.end() || op.target[0] || op.target[1] || !(op.draw_flags & RENDERER_DEPTH_TEST)) return;
    t.vel.push_back({op_index, (uint32_t)k.last_job, it->second});   // the job runs again only in a frame with a TAA op (taa_poses)
}
// At record_frame, before the skinning inputs are uploaded, in a frame with a TAA op: each pending
// draw's job again with last frame's constants (output after the frame's jobs, not copied to the
// frame); VelDraw then holds the current and previous outputs.
static void taa_poses() {
    Taa &t = s.taa;
    Skin &k = s.skin;
    for (Taa::VelDraw &d : t.vel) {
        RSkinJob pj = k.jobs[d.cur];
        const uint32_t cur = pj.out_vertex;
        pj.consts = (uint32_t)(k.consts.size() / 4);
        append_pod(k.consts, &t.pose_prev[d.prev], (size_t)192 * 4);
        pj.out_vertex = k.nverts;
        k.nverts += pj.nv;
        k.jobs.push_back(pj);
        k.dst.push_back(k.dst[d.cur]);
        k.prev.push_back(1);
        d.cur = cur;
        d.prev = pj.out_vertex;
    }
}

extern "C" {
int renderer_skin_on(void) { return s.dev && s.skin.ok; }

static int32_t draw_skinned(const RViewport *vp, uint32_t flags, const RSkinJob *job, const uint32_t (*ins)[4], const float (*consts)[4],
                            const uint8_t *data, uint32_t data_bytes, const uint32_t *idx, uint32_t ni, const RSkinNormals *nrm);
void renderer_set_draw_identity(uint64_t id) { s.taa.next_id = s.taa.ok && s.skin.ok ? id : 0; }
int32_t renderer_draw_skinned(const RViewport *vp, uint32_t flags, const RSkinJob *job, const uint32_t (*ins)[4], const float (*consts)[4],
                              const uint8_t *data, uint32_t data_bytes, const uint32_t *idx, uint32_t ni, const RSkinNormals *nrm) {
    const uint64_t id = s.taa.next_id;
    s.taa.next_id = 0;
    const size_t at = s.ops.size();
    const int32_t first = draw_skinned(vp, flags, job, ins, consts, data, data_bytes, idx, ni, nrm);
    if (id && first >= 0 && s.ops.size() > at) taa_pose(id, consts, (uint32_t)(s.ops.size() - 1));
    return first;
}
static int32_t draw_skinned(const RViewport *vp, uint32_t flags, const RSkinJob *job, const uint32_t (*ins)[4], const float (*consts)[4],
                            const uint8_t *data, uint32_t data_bytes, const uint32_t *idx, uint32_t ni, const RSkinNormals *nrm) {
    Skin &k = s.skin;
    if (!k.ok || !job->nv || ni < 3) return -1;
    const bool lit = nrm && s.charlit.ok && s.charlit.marked && !s.cur_target[0] && !s.cur_target[1];
    if (lit) s.normals.resize(s.verts.size() * 3, 0.0f);   // parallel to the vertices (renderer_draw_lit)
    const size_t at = s.ops.size();
    const uint32_t first = (uint32_t)s.verts.size();
    renderer_draw(vp, flags, nullptr, job->nv, idx, ni);
    if (s.ops.size() == at) return -1;
    RSkinJob j = *job;
    auto p = k.prog_at.find(ins);
    if (p == k.prog_at.end()) {
        p = k.prog_at.emplace(ins, (uint32_t)(k.ins.size() / 4)).first;
        k.ins.insert(k.ins.end(), &ins[0][0], &ins[0][0] + (size_t)j.nins * 4);
    }
    j.prog += p->second;
    if (consts != k.last_consts) {   // the recording thread's records share one copy of repeated constants
        k.last_consts = consts;
        k.last_consts_at = (uint32_t)(k.consts.size() / 4);
        append_pod(k.consts, &consts[0][0], 192 * 4);
    }
    j.consts += k.last_consts_at;
    const uint32_t d = (uint32_t)k.data.size();
    for (int i = 0; i < 4; i++) j.sbase[i] += d;
    append_pod(k.data, data, data_bytes);
    k.data.resize((k.data.size() + 3) & ~(size_t)3);
    j.out_vertex = k.nverts;   // in the device buffer; copied to first
    k.nverts += j.nv;
    k.jobs.push_back(j);
    k.dst.push_back(first);
    k.prev.push_back(0);
    k.last_job = k.jobs.size() - 1;
    Op &op = s.ops.back();
    if (vertex_check && vertex_check_frame) k.vcheck.push_back({first, j.nv, op.first_index, op.index_count, k.draw_tag});
    if (!lit || (op.pipe_key & SOFT_KEY)) return (int32_t)first;
    s.normals.resize(s.verts.size() * 3, 0.0f);
    SkinNormJob n{};
    n.vfirst = j.out_vertex;
    n.nv = job->nv;
    n.ifirst = op.first_index;
    n.ntri = op.index_count / 3;
    n.group = (uint32_t)k.adj.size();
    k.adj.insert(k.adj.end(), nrm->group, nrm->group + job->nv);
    n.start = (uint32_t)k.adj.size();
    k.adj.insert(k.adj.end(), nrm->start, nrm->start + nrm->ng + 1);
    n.corner = (uint32_t)k.adj.size();
    k.adj.insert(k.adj.end(), nrm->corner, nrm->corner + nrm->ncorner);
    n.face = k.nfaces;
    k.nfaces += n.ntri;
    n.part = k.nparts;
    n.nparts = (n.ntri + 255) / 256;
    k.nparts += n.nparts;
    n.px = nrm->px;
    n.py = nrm->py;
    k.njobs.push_back(n);
    k.ndst.push_back(first);
    op.pipe_key |= CHARLIT_KEY;
    relief_mark(op);
    return (int32_t)first;
}

int32_t renderer_draw_skinned_world_lit(const RViewport *vp, uint32_t flags, const RSkinJob *job, const uint32_t (*ins)[4], const float (*consts)[4],
                                        const uint8_t *data, uint32_t data_bytes, const uint32_t *idx, uint32_t ni, const float *normals,
                                        uint32_t lights) {
    const bool lit = lights && normals && s.skin.ok && s.charlit.ok && s.charlit.marked && !s.cur_target[0] && !s.cur_target[1] && job->nv && ni >= 3;
    const size_t at = s.ops.size(), nn = s.normals.size();
    if (lit) {   // the host normals at the draw's vertices (renderer_draw_lit's layout)
        s.normals.resize(s.verts.size() * 3, 0.0f);
        s.normals.insert(s.normals.end(), normals, normals + (size_t)job->nv * 3);
    }
    const int32_t first = renderer_draw_skinned(vp, flags, job, ins, consts, data, data_bytes, idx, ni, nullptr);
    if (!lit) return first;
    if (s.ops.size() == at) {   // nothing drawn: no normals either
        s.normals.resize(nn);
        return first;
    }
    Op &op = s.ops.back();
    if (!(op.pipe_key & SOFT_KEY)) {
        op.pipe_key |= CHARLIT_KEY;
        op.push[3] = lights;
        relief_mark(op);
        op.shape &= ~(HAIR_SHAPE | SKIN_SHAPE);   // hair and skin are for characters
    }
    return first;
}

void renderer_set_vertex_check(RVertexCheck check) { vertex_check = check; }
void renderer_set_vertex_check_frame(int on) { vertex_check_frame = on != 0; }
void renderer_set_draw_tag(uint32_t tag) { s.skin.draw_tag = tag; }

void renderer_skin_stats(float *gpu_ms, uint32_t *draws, uint32_t *vertices) {
    *gpu_ms = s.skin.gpu_ms;
    *draws = s.skin.rec_draws;
    *vertices = s.skin.rec_vertices;
}
const RVertex *renderer_debug_vertices(void) { return s.skin.debug_verts; }
const float *renderer_debug_normals(void) { return s.skin.debug_normals; }
}
