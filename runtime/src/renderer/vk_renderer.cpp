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
// run synchronously in renderer_texture_create through a one-time command buffer and fence. BC1-3 blocks are uploaded as is when the device samples them; otherwise the caller
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
#include <vulkan/vulkan.h>
#include "../platform.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unordered_map>
#include <vector>
#include "geom_frag.h"   // SPIR-V generated at build time by glslangValidator (--vn)
#include "geom_vert.h"
#include "glass_blur_frag.h"
#include "glass_comp_frag.h"
#include "glass_vert.h"
#include "cas_frag.h"
#include "bloom_down_frag.h"
#include "bloom_up_frag.h"
#include "bloom_comp_frag.h"
#include "filmic_frag.h"
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
#include "wet_frag.h"
#include "wet_ms_frag.h"
#include "rain_cover_vert.h"
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
#include "contact_frag.h"
#include "contact_ms_frag.h"
#include "geom_soft_frag.h"
#include "geom_soft_ms_frag.h"

#define VK_GLOBAL_FNS(X) X(vkCreateInstance)
#define VK_INSTANCE_FNS(X)                                                                         \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)            \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties)             \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceFeatures) X(vkCreateDevice) X(vkGetDeviceProcAddr)
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
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCreateSemaphore) X(vkDestroySemaphore)
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

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void *map = nullptr;
    VkDeviceSize size = 0;
};

struct Image {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

enum OpKind { OP_CLEAR, OP_DRAW, OP_GLASS, OP_BLOOM, OP_RAIN, OP_SSAO, OP_SPLASH, OP_WET, OP_SOFT, OP_FILMIC, OP_SKY, OP_SKYLINE, OP_LAMP, OP_CONTACT };
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
    uint32_t shape;          // combiner program structure (index into State::shapes)
    uint32_t blend_color;    // ARGB8888 constant color
    RGlass glass;            // OP_GLASS
    RSsao ssao;              // OP_SSAO
    RSoft soft;              // OP_SOFT
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
};

// Offscreen surface (renderer_surface): handle = SURFACE_BIT | (index + 1).
const uint32_t SURFACE_BIT = 0x8000;
// Handles of the movie planes (renderer_movie_plane): above MAX_TEXTURES, without SURFACE_BIT.
const uint32_t MOVIE_TEXTURE = 0x7FF0;
const int MOVIE_PLANES = 3;
bool is_movie(uint32_t handle) { return handle >= MOVIE_TEXTURE && handle < MOVIE_TEXTURE + MOVIE_PLANES; }
const uint32_t MAX_SURFACES = 64;
struct Surface {
    Image im;
    uint32_t kind = RENDERER_SURFACE_COLOR;
    int gw = 0, gh = 0;   // guest size (viewports)
    int w = 0, h = 0;     // image size
};
// Framebuffer of an offscreen pass: the color and depth surfaces, or per-size dummies for a
// missing one (kept in their attachment layout).
struct OffscreenFb {
    VkFramebuffer fb = VK_NULL_HANDLE;
    Image dummy_color, dummy_depth;
    uint32_t pass_class = 1;   // 1 = D16 depth, 2 = 24-bit depth
    int w = 0, h = 0, gw = 0, gh = 0;
};

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
struct Filmic {
    bool ok = false;
    RFilmic p{};   // lut / lut_size cleared after the upload
    int lut_size = 0;
    Image src, lut;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set_glow = VK_NULL_HANDLE, set_plain = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    bool marked = false;     // renderer_filmic called in the frame being recorded
    uint32_t frame = 0;      // grain seed
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
// Descriptor sets: binding 0 the depth target, binding 1 src, a or b.
struct Wet {
    bool ok = false;
    int rw = 0, rh = 0, scale = 2;
    Image src, a, b;
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
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    uint32_t executed = 0;   // character draws in the last frame's mask
};

// Frames in flight (RendererConfig.frames_in_flight): what a submitted frame reads until its fence
// signals. A slot is reused only after renderer_begin (or record_frame) has waited for that fence,
// so its buffers are rewritten, grown and its descriptor pool reset without waiting for the device.
const int MAX_FRAMES_IN_FLIGHT = 3;
struct FrameSlot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;                      // submitted, fence not waited yet
    VkSemaphore acquired = VK_NULL_HANDLE;     // swapchain image acquire (direct present)
    VkDescriptorPool set_pool = VK_NULL_HANDLE;   // the frame's descriptor sets
    Buffer upload;    // base image upload (w * h * 4 bytes)
    Buffer geometry;  // the frame's vertices, then its indices (grown on demand)
    Buffer programs;  // the frame's RPixelShader records (grown on demand)
    Buffer movie;     // the frame's movie planes (renderer_movie_plane; grown on demand)
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
    int fw = 0, fh = 0;    // output size (renderer_output_size, the readback and present source)
    int ss = 1;            // supersampling factor in use (1, 2, 4)
    Image down[2];         // supersampling: the 2:1 downsample chain (the last one is s.fw x s.fh)
    VkSampler nearest_sampler = VK_NULL_HANDLE;   // the base quad when supersampled at the back-buffer size
    bool base_nearest = false;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    float aniso = 1.0f;
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
    std::vector<RVertex> verts;
    std::vector<uint32_t> indices;
    RendererTiming timing{};
    Glass glass;
    Sharpen sharp;
    Bloom bloom;
    Filmic filmic;
    Rain rain;
    Sky sky;
    Ssao ssao;
    Splash splash;
    Wet wet;
    Cover cover;
    Chars chars;
    Soft soft;
    Lamp lamp;
    Contact contact;
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

// Host-visible, coherent buffer, mapped. cached: prefer a host-cached memory type, for buffers the
// CPU reads (the frame readback): reading uncached, write-combined memory costs about 25 ms per
// 640x480 frame on an NVIDIA GPU.
int make_buffer(Buffer *b, VkDeviceSize size, VkBufferUsageFlags usage, bool cached = false) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(s.dev, &bi, nullptr, &b->buf) != VK_SUCCESS) return fail("vkCreateBuffer failed");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(s.dev, b->buf, &req);
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int type = cached ? find_memory(req.memoryTypeBits, host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT) : -1;
    if (type < 0) type = find_memory(req.memoryTypeBits, host);
    if (type < 0) return fail("no host-visible coherent memory type");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(s.dev, &ai, nullptr, &b->mem) != VK_SUCCESS) return fail("vkAllocateMemory (buffer) failed");
    vkBindBufferMemory(s.dev, b->buf, b->mem, 0);
    if (vkMapMemory(s.dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) != VK_SUCCESS) return fail("vkMapMemory failed");
    b->size = size;
    return 0;
}

void free_buffer(Buffer *b) {
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
    if (vkCreateImage(s.dev, &ii, nullptr, &im->img) != VK_SUCCESS) return fail("vkCreateImage failed (format %d)", fmt);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(s.dev, im->img, &req);
    int type = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = find_memory(req.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)type;
    if (bytes) *bytes = req.size;
    if (type < 0 || vkAllocateMemory(s.dev, &ai, nullptr, &im->mem) != VK_SUCCESS) return fail("vkAllocateMemory (image) failed");
    vkBindImageMemory(s.dev, im->img, im->mem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im->img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, levels, 0, 1};
    if (vkCreateImageView(s.dev, &vi, nullptr, &im->view) != VK_SUCCESS) return fail("vkCreateImageView failed");
    return 0;
}

void free_image(Image *im) {
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
    if (want_surface) {
        ci.enabledExtensionCount = cfg.instance_ext_count;
        ci.ppEnabledExtensionNames = cfg.instance_ext;
    }
    VkResult r = vkCreateInstance(&ci, nullptr, &s.inst);
    if (r != VK_SUCCESS && want_surface) {   // the window system's extensions are missing: offscreen
        snprintf(s.present_info, sizeof s.present_info, "instance without the window extensions (%d)", r);
        want_surface = false;
        ci.enabledExtensionCount = 0;
        r = vkCreateInstance(&ci, nullptr, &s.inst);
    }
    if (r != VK_SUCCESS) return fail("vkCreateInstance failed (%d)", r);
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
int drop_present() {
    if (s.dev) vkDeviceWaitIdle(s.dev);
    for (VkSemaphore sem : s.sc_done) vkDestroySemaphore(s.dev, sem, nullptr);
    s.sc_done.clear();
    s.sc_images.clear();
    for (FrameSlot &f : s.frames) {
        if (f.acquired) vkDestroySemaphore(s.dev, f.acquired, nullptr);
        f.acquired = VK_NULL_HANDLE;
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
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s.phys, s.surface, &caps) != VK_SUCCESS) return fail("cannot query the surface");
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {   // the swapchain decides: the window's drawable size
        int w = s.fw, h = s.fh;
        if (cfg.drawable_size) cfg.drawable_size(&w, &h);
        ext.width = (uint32_t)(w > 0 ? w : 0);
        ext.height = (uint32_t)(h > 0 ? h : 0);
        if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
        if (ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    vkDeviceWaitIdle(s.dev);
    if (!ext.width || !ext.height) {
        s.sc_stale = true;
        if (s.swapchain) vkDestroySwapchainKHR(s.dev, s.swapchain, nullptr);
        s.swapchain = VK_NULL_HANDLE;
        return 0;
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return fail("swapchain images cannot be transfer targets");
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s.phys, s.surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(s.phys, s.surface, &n, formats.data());
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
    vkGetPhysicalDeviceSurfacePresentModesKHR(s.phys, s.surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(s.phys, s.surface, &n, modes.data());
    VkPresentModeKHR want = cfg.present_mode == RENDERER_PRESENT_FIFO        ? VK_PRESENT_MODE_FIFO_KHR
                            : cfg.present_mode == RENDERER_PRESENT_IMMEDIATE ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                                             : VK_PRESENT_MODE_MAILBOX_KHR;
    s.sc_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR m : modes)
        if (m == want) s.sc_mode = m;
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = s.surface;
    ci.minImageCount = count;
    ci.imageFormat = pick->format;
    ci.imageColorSpace = pick->colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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
    VkResult r = vkCreateSwapchainKHR(s.dev, &ci, nullptr, &sc);
    if (s.swapchain) vkDestroySwapchainKHR(s.dev, s.swapchain, nullptr);
    s.swapchain = VK_NULL_HANDLE;
    if (r != VK_SUCCESS) return fail("vkCreateSwapchainKHR failed (%d)", r);
    s.swapchain = sc;
    s.sc_extent = ext;
    s.sc_format = pick->format;
    vkGetSwapchainImagesKHR(s.dev, sc, &n, nullptr);
    s.sc_images.resize(n);
    vkGetSwapchainImagesKHR(s.dev, sc, &n, s.sc_images.data());
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    while (s.sc_done.size() < n) {
        VkSemaphore sem;
        if (vkCreateSemaphore(s.dev, &sci, nullptr, &sem) != VK_SUCCESS) return fail("vkCreateSemaphore failed");
        s.sc_done.push_back(sem);
    }
    for (int i = 0; i < s.nframes; i++)
        if (!s.frames[i].acquired && vkCreateSemaphore(s.dev, &sci, nullptr, &s.frames[i].acquired) != VK_SUCCESS)
            return fail("vkCreateSemaphore failed");
    const char *mode = s.sc_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : s.sc_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo";
    snprintf(s.present_info, sizeof s.present_info, "swapchain %ux%u %s %s, %u images", ext.width, ext.height, format_name(s.sc_format), mode, n);
    return 0;
}

// Whether a device offers VK_KHR_swapchain.
bool has_swapchain_ext(VkPhysicalDevice d) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(d, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> e(n);
    vkEnumerateDeviceExtensionProperties(d, nullptr, &n, e.data());
    for (const VkExtensionProperties &x : e)
        if (!strcmp(x.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) return true;
    return false;
}

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
    const char *swapchain_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (s.surface) {
        di.enabledExtensionCount = 1;
        di.ppEnabledExtensionNames = &swapchain_ext;
    }
    VkResult r = vkCreateDevice(s.phys, &di, nullptr, &s.dev);
    if (r != VK_SUCCESS) return fail("vkCreateDevice failed (%d)", r);
#define LOAD_DEVICE(fn) if (!(fn = (PFN_##fn)vkGetDeviceProcAddr(s.dev, #fn))) return fail("missing " #fn);
    VK_DEVICE_FNS(LOAD_DEVICE)
#define LOAD_DEVICE_OPTIONAL(fn) fn = (PFN_##fn)vkGetDeviceProcAddr(s.dev, #fn);
    VK_SWAPCHAIN_FNS(LOAD_DEVICE_OPTIONAL)
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
    if (vkCreateRenderPass(s.dev, &rp, nullptr, out) != VK_SUCCESS) return fail("vkCreateRenderPass failed");
    return 0;
}

int create_targets() {
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
    s.ss = cfg.supersample >= 4 ? 4 : cfg.supersample >= 2 ? 2 : 1;
    while (s.ss > 1 && ((uint32_t)(s.fw * s.ss) > s.max_dim || (uint32_t)(s.fh * s.ss) > s.max_dim)) s.ss /= 2;
    s.ow = s.fw * s.ss;
    s.oh = s.fh * s.ss;
    s.base_nearest = s.ss > 1 && s.fw == s.w && s.fh == s.h;
    for (int k = 0, dw = s.ow / 2, dh = s.oh / 2; dw >= s.fw && s.ss > 1; k++, dw /= 2, dh /= 2)
        if (make_image(&s.down[k], VK_FORMAT_B8G8R8A8_UNORM,   // sampled: make_image creates a view
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)dw, (uint32_t)dh))
            return -1;
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    s.quad_base = ms || s.ow != s.w || s.oh != s.h;
    s.depth_sampled = false;
    if (cfg.ssao || cfg.rain || cfg.soft || cfg.lamp || cfg.contact) {   // ssao, lamp lights, rain splashes and soft particles read the depth target: the format sampled, at this sample count
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
                       (cfg.sharpen > 0 || cfg.bloom || cfg.ssao || cfg.lamp ? VK_IMAGE_USAGE_SAMPLED_BIT : 0),   // sharpening, bloom, ssao and lamp lights (light mask) sample it
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
    if (vkCreateFramebuffer(s.dev, &fi, nullptr, &s.fb) != VK_SUCCESS) return fail("vkCreateFramebuffer failed");
    return 0;
}

// The command pool, one command buffer, fence and upload buffer per frame slot, and the one-time
// command buffer and fence.
int create_commands() {
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = s.qfamily;
    if (vkCreateCommandPool(s.dev, &pi, nullptr, &s.pool) != VK_SUCCESS) return fail("vkCreateCommandPool failed");
    s.nframes = configured_frames();
    s.slot = 0;
    VkCommandBuffer cmds[MAX_FRAMES_IN_FLIGHT + 1];
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = s.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = (uint32_t)s.nframes + 1;
    if (vkAllocateCommandBuffers(s.dev, &ai, cmds) != VK_SUCCESS) return fail("vkAllocateCommandBuffers failed");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (int i = 0; i < s.nframes; i++) {
        FrameSlot &f = s.frames[i];
        f.cmd = cmds[i];
        if (vkCreateFence(s.dev, &fci, nullptr, &f.fence) != VK_SUCCESS) return fail("vkCreateFence failed");
        if (make_buffer(&f.upload, (VkDeviceSize)s.w * s.h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return -1;
    }
    s.cmd = s.frames[0].cmd;
    s.once_cmd = cmds[s.nframes];
    if (vkCreateFence(s.dev, &fci, nullptr, &s.once_fence) != VK_SUCCESS) return fail("vkCreateFence failed");
    return make_buffer(&s.readback, (VkDeviceSize)s.fw * s.fh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
}

// Waits until the current slot's previous frame has completed (no-op when it has been waited).
int wait_slot() {
    FrameSlot &f = cur();
    if (!f.pending) return 0;
    double t = now_ms();
    VkResult r = vkWaitForFences(s.dev, 1, &f.fence, VK_TRUE, 5000000000ull);
    s.wait_ms += now_ms() - t;
    if (r != VK_SUCCESS) return fail("frame wait failed (%d)", r);
    f.pending = false;
    return 0;
}

// Submits the current slot's command buffer (ended by the caller) with its fence; the slot is
// pending until wait_slot. The next frame records into the next slot.
VkResult submit_slot(const VkSubmitInfo *si) {
    FrameSlot &f = cur();
    vkResetFences(s.dev, 1, &f.fence);
    VkResult r = vkQueueSubmit(s.queue, 1, si, f.fence);
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
    vkCreateShaderModule(s.dev, &ci, nullptr, &m);
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
    si.anisotropyEnable = s.aniso > 1.0f ? VK_TRUE : VK_FALSE;
    si.maxAnisotropy = s.aniso;
    if (vkCreateSampler(s.dev, &si, nullptr, &s.sampler) != VK_SUCCESS) return fail("vkCreateSampler failed");
    // The base image quad: clamped (no wrap-around at the edges), no mips or anisotropy.
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.anisotropyEnable = VK_FALSE;
    si.maxAnisotropy = 1.0f;
    si.maxLod = 0.0f;
    if (vkCreateSampler(s.dev, &si, nullptr, &s.clamp_sampler) != VK_SUCCESS) return fail("vkCreateSampler failed");
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (vkCreateSampler(s.dev, &si, nullptr, &s.nearest_sampler) != VK_SUCCESS) return fail("vkCreateSampler failed");
    VkDescriptorSetLayoutBinding b[RENDERER_STAGES + 1];
    for (uint32_t i = 0; i <= RENDERER_STAGES; i++)
        b[i] = {i, i < RENDERER_STAGES ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = RENDERER_STAGES + 1;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(s.dev, &li, nullptr, &s.set_layout) != VK_SUCCESS) return fail("vkCreateDescriptorSetLayout failed");
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_FRAME_SETS * RENDERER_STAGES},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAME_SETS}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = MAX_FRAME_SETS;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    for (int i = 0; i < s.nframes; i++)
        if (vkCreateDescriptorPool(s.dev, &pi, nullptr, &s.frames[i].set_pool) != VK_SUCCESS) return fail("vkCreateDescriptorPool failed");
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
    uint32_t m = st.color_mask;   // NV2A B 1<<0, G 1<<8, R 1<<16 -> VK R 1, G 2, B 4 (alpha never written)
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

// Geometry pipeline for a key: RVertex input (clip-space position, B8G8R8A8 diffuse and specular,
// four texture coordinates), triangle lists, dynamic viewport, scissor and blend constants, set 0
// (stage textures, combiner programs) and the alpha test plus program index as push constants.
// Returns VK_NULL_HANDLE on failure.
VkPipeline get_pipeline(uint32_t key, uint32_t shape) {
    const uint64_t full = (uint64_t)shape << 32 | key;
    auto it = s.pipelines.find(full);
    if (it != s.pipelines.end()) return it->second;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (!s.layout) {
        VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &s.set_layout;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &pc;
        if (vkCreatePipelineLayout(s.dev, &li, nullptr, &s.layout) != VK_SUCCESS) {
            fail("vkCreatePipelineLayout failed");
            return VK_NULL_HANDLE;
        }
    }
    const uint32_t pass_class = key >> 25 & 3;
    if (pass_class && !s.off_pass[pass_class - 1]) {
        fail("no offscreen render pass");
        return VK_NULL_HANDLE;
    }
    if (!s.vs) s.vs = shader(geom_vert, sizeof geom_vert);
    if (!s.fs) s.fs = shader(geom_frag, sizeof geom_frag);
    const bool soft = key & SOFT_KEY;
    if (!s.vs || !s.fs || (soft && !s.soft.fs)) {
        fail("vkCreateShaderModule failed");
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = s.vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = soft ? s.soft.fs : s.fs;
    stages[1].pName = "main";
    // The shape's words, then MASK_CLEAR (constant 37, bloom light mask: see below) and
    // SHADOW_RANGE (constant 38, a float; read only with the shadow word's bit 13); soft particles:
    // SOFT_RGB (constant 39: the fade scales the colour too unless the source factor is SRC_ALPHA).
    VkSpecializationMapEntry spec_map[SHAPE_WORDS + 3];
    for (uint32_t i = 0; i < SHAPE_WORDS + 3; i++) spec_map[i] = {i, i * 4, 4};
    uint32_t spec_data[SHAPE_WORDS + 3];
    memcpy(spec_data, s.shapes[shape].w, sizeof(Shape));
    const uint32_t role = key >> 27 & 7;
    spec_data[SHAPE_WORDS] = role == MASK_CLEAR;
    const float range = cfg.shadow_range > 0 ? cfg.shadow_range : 0.0f;
    memcpy(&spec_data[SHAPE_WORDS + 1], &range, 4);
    spec_data[SHAPE_WORDS + 2] = (key & 31) != VK_BLEND_FACTOR_SRC_ALPHA;
    VkSpecializationInfo spec{soft ? SHAPE_WORDS + 3 : SHAPE_WORDS + 2, spec_map, soft ? sizeof spec_data : sizeof spec_data - 4, spec_data};
    stages[1].pSpecializationInfo = &spec;
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
    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 3;
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
    pi.layout = soft ? s.soft.layout : s.layout;
    pi.renderPass = pass_class ? s.off_pass[pass_class - 1] : s.pass;
    VkResult r = vkCreateGraphicsPipelines(s.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &pipe);
    if (r != VK_SUCCESS) {
        fail("vkCreateGraphicsPipelines failed (%d)", r);
        pipe = VK_NULL_HANDLE;
    }
    s.pipelines[full] = pipe;
    return pipe;
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
int upload_geometry(VkDeviceSize *index_offset) {
    Buffer &g = cur().geometry;
    VkDeviceSize vbytes = s.verts.size() * sizeof(RVertex), ibytes = s.indices.size() * 4;
    *index_offset = (vbytes + 255) & ~(VkDeviceSize)255;
    VkDeviceSize need = *index_offset + ibytes;
    if (need > g.size) {
        free_buffer(&g);
        VkDeviceSize size = 1 << 20;
        while (size < need) size *= 2;
        if (make_buffer(&g, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return -1;
    }
    memcpy(g.map, s.verts.data(), vbytes);
    memcpy((uint8_t *)g.map + *index_offset, s.indices.data(), ibytes);
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

// One-time submission (texture upload, new surface image): begin_once makes the one-time command
// buffer the recording one (barrier() records into s.cmd) and begins it; submit_once runs it,
// waits for it and restores the frame's command buffer. The work touches only new images and
// frames submitted later see it through its final barrier (queue submission order); its fence
// also covers every earlier submission, so a frame that creates textures still waits for the
// frames in flight (new textures are rare outside loading screens).
void begin_once(VkCommandBuffer *saved) {
    *saved = s.cmd;
    s.cmd = s.once_cmd;
    vkResetCommandBuffer(s.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cmd, &bi);
}
int submit_once(VkCommandBuffer saved) {
    vkEndCommandBuffer(s.cmd);
    const VkCommandBuffer once = s.cmd;
    s.cmd = saved;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &once;
    vkResetFences(s.dev, 1, &s.once_fence);
    VkResult r = vkQueueSubmit(s.queue, 1, &si, s.once_fence);
    if (r == VK_SUCCESS) r = vkWaitForFences(s.dev, 1, &s.once_fence, VK_TRUE, 5000000000ull);
    return r == VK_SUCCESS ? 0 : fail("submit/wait failed (%d)", r);
}

VkFormat texture_format(uint32_t f) {
    switch (f) {
    case RENDERER_TEX_BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
    case RENDERER_TEX_BC1: return s.bc ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : VK_FORMAT_UNDEFINED;
    case RENDERER_TEX_BC2: return s.bc ? VK_FORMAT_BC2_UNORM_BLOCK : VK_FORMAT_UNDEFINED;
    case RENDERER_TEX_BC3: return s.bc ? VK_FORMAT_BC3_UNORM_BLOCK : VK_FORMAT_UNDEFINED;
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
    if (vkCreateImageView(s.dev, &vi, nullptr, &im->view) != VK_SUCCESS) return fail("vkCreateImageView (L8) failed");
    return 0;
}

// Creates the image and uploads every level through a temporary staging buffer.
int make_texture(Texture *t, uint32_t f, uint32_t w, uint32_t h, uint32_t levels, const void *data) {
    VkFormat fmt = texture_format(f);
    if (fmt == VK_FORMAT_UNDEFINED || !w || !h || !levels) return fail("unsupported texture format %u (%ux%u, %u levels)", f, w, h, levels);
    VkDeviceSize total = 0;
    for (uint32_t l = 0; l < levels; l++) total += level_size(f, w >> l ? w >> l : 1, h >> l ? h >> l : 1);
    if (make_image(&t->im, fmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, w, h, levels,
                   &t->bytes))
        return -1;
    if (f == RENDERER_TEX_L8 && l8_view(&t->im, levels)) return -1;
    Buffer up;
    if (make_buffer(&up, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return -1;
    memcpy(up.map, data, total);
    std::vector<VkBufferImageCopy> regions(levels);
    VkDeviceSize off = 0;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        regions[l] = VkBufferImageCopy{};
        regions[l].bufferOffset = off;
        regions[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
        regions[l].imageExtent = {lw, lh, 1};
        off += level_size(f, lw, lh);
    }
    VkCommandBuffer saved;
    begin_once(&saved);
    barrier(t->im.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, levels);
    vkCmdCopyBufferToImage(s.cmd, up.buf, t->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, regions.data());
    barrier(t->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, levels);
    int r = submit_once(saved);
    free_buffer(&up);
    return r ? -1 : 0;
}

// Drops every cached texture (and the white one, which is recreated).
void drop_textures() {
    vkDeviceWaitIdle(s.dev);
    for (Texture &t : s.textures) free_image(&t.im);
    s.textures.clear();
    s.by_key.clear();
    s.texture_bytes = 0;
    free_image(&s.white.im);
}

// The frame at the output size before sharpening: the color target, or the last downsample image
// when supersampled.
Image &frame_image() { return s.ss > 1 ? s.down[s.ss == 4 ? 1 : 0] : s.color; }

// The finished frame (TRANSFER_SRC_OPTIMAL after record_frame): the sharpened image, else the frame.
VkImage output_image() { return s.sharp.ok ? s.sharp.out.img : frame_image().img; }

// Blits the finished target (TRANSFER_SRC_OPTIMAL) into a swapchain image: black borders, the
// largest rectangle of the target's aspect, linear filtering; the image ends ready to present.
void record_blit(VkImage dst) {
    barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue black{};
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(s.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const int64_t dw = s.sc_extent.width, dh = s.sc_extent.height;
    int64_t w = dw, h = dh;
    if (dw * s.fh >= dh * s.fw) w = (dh * s.fw * 2 + s.fh) / (2 * s.fh);   // pillarbox
    else h = (dw * s.fh * 2 + s.fw) / (2 * s.fw);                          // letterbox
    VkImageBlit b{};
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {s.fw, s.fh, 1};
    b.dstSubresource = b.srcSubresource;
    b.dstOffsets[0] = {(int32_t)((dw - w) / 2), (int32_t)((dh - h) / 2), 0};
    b.dstOffsets[1] = {(int32_t)((dw - w) / 2 + w), (int32_t)((dh - h) / 2 + h), 1};
    vkCmdBlitImage(s.cmd, output_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                   VK_FILTER_LINEAR);
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
    if (vkAllocateDescriptorSets(s.dev, &ai, &set) != VK_SUCCESS) return sets[key] = VK_NULL_HANDLE;
    VkDescriptorImageInfo ii[RENDERER_STAGES];
    VkWriteDescriptorSet w[RENDERER_STAGES + 1];
    for (int i = 0; i < RENDERER_STAGES; i++) {
        const bool surf = tex[i] != BASE_TEXTURE && (tex[i] & SURFACE_BIT);
        const Texture &t = tex[i] && tex[i] != BASE_TEXTURE && !surf && !is_movie(tex[i]) ? s.textures[tex[i] - 1] : s.white;
        ii[i] = {s.sampler, t.im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        if (is_movie(tex[i])) ii[i] = {s.clamp_sampler, s.movie[tex[i] - MOVIE_TEXTURE].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        if (tex[i] == BASE_TEXTURE) ii[i] = {s.base_nearest ? s.nearest_sampler : s.clamp_sampler, s.base.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        if (surf) {
            const Surface &sf = s.surfaces[(tex[i] & ~SURFACE_BIT) - 1];
            ii[i] = {s.border_sampler, sf.im.view,
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
    vkUpdateDescriptorSets(s.dev, RENDERER_STAGES + 1, w, 0, nullptr);
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
        if (vkCreateRenderPass(s.dev, &rp, nullptr, &s.off_pass[c]) != VK_SUCCESS) return fail("vkCreateRenderPass (offscreen) failed");
    }
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    if (vkCreateSampler(s.dev, &si, nullptr, &s.border_sampler) != VK_SUCCESS) return fail("vkCreateSampler (border) failed");
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
    if (vkCreateFramebuffer(s.dev, &fi, nullptr, &f.fb) != VK_SUCCESS) {
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
// 12 soft filtering (RendererConfig.shadow_soft), 13 shadow range (RendererConfig.shadow_range);
// 0 when no stage samples a depth surface (the function does not split pipelines then).
uint32_t shadow_word(const uint32_t tex[RENDERER_STAGES], uint32_t func) {
    uint32_t w = 0;
    for (int i = 0; i < RENDERER_STAGES; i++) {
        const Surface *sf = tex[i] != BASE_TEXTURE ? surface_of(tex[i]) : nullptr;
        if (!sf || sf->kind == RENDERER_SURFACE_COLOR) continue;
        w |= 1u << (4 + i);
        if (sf->kind == RENDERER_SURFACE_DEPTH24) w |= 1u << (8 + i);
    }
    return w ? w | (func & 7u) | (cfg.shadow_soft ? 1u << 12 : 0u) | (cfg.shadow_range > 0 ? 1u << 13 : 0u) : 0;
}
}  // namespace

static void glass_free();
static void sharpen_init();
static void sharpen_free();
static void bloom_init();
static void bloom_free();
static void filmic_init();
static void filmic_free();
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

extern "C" {

const char *renderer_error(void) { return s.error; }
const char *renderer_device_name(void) { return s.name; }

void renderer_configure(const RendererConfig *c) { cfg = *c; }
void renderer_output_size(int *w, int *h) {
    *w = s.fw;
    *h = s.fh;
}
int renderer_supersample(void) { return s.dev ? s.ss : 1; }
int renderer_sharpen(void) { return s.dev && s.sharp.ok ? s.sharp.amount : 0; }
int renderer_samples(void) { return (int)s.samples; }
int renderer_frames_in_flight(void) { return s.dev ? s.nframes : configured_frames(); }
float renderer_anisotropy(void) { return s.aniso; }

int renderer_init(int w, int h) {
    if (s.dev) return 0;
    if (w <= 0 || h <= 0) return fail("bad target size %dx%d", w, h);
    s.w = w;
    s.h = h;
    s.fw = s.ow = cfg.out_w > 0 ? cfg.out_w : w;   // s.ow / s.oh: times the supersampling factor in create_targets
    s.fh = s.oh = cfg.out_h > 0 ? cfg.out_h : h;
    if (load_loader() || create_instance() || pick_device() || create_device() || create_targets() || create_commands() ||
        create_texture_state() || create_offscreen_passes() || create_default_pipelines() || make_white()) {
        char why[sizeof s.error];
        snprintf(why, sizeof why, "%s", s.error);
        renderer_shutdown();
        snprintf(s.error, sizeof s.error, "%s", why);
        return -1;
    }
    sharpen_init();   // optional: without it the frame is presented as is
    bloom_init();     // optional too: without it renderer_bloom is ignored
    filmic_init();    // likewise renderer_filmic (after bloom: it reads a bloom level)
    cover_init();     // likewise renderer_rain_cover (before the rain, splash and wet ground that read it)
    chars_init();     // the rain's character mask (before the splash and wet ground that read it)
    rain_init();      // likewise renderer_rain
    sky_init();       // and renderer_sky / renderer_skyline
    ssao_init();      // and renderer_ssao
    splash_init();    // and renderer_rain_splash
    wet_init();       // and renderer_rain_wet
    soft_init();      // and renderer_soft
    lamp_init();      // and renderer_lamp_lights (after chars_init: it reads the character mask)
    contact_init();   // and renderer_contact (likewise)
    if (s.surface && create_swapchain()) drop_present();
    return 0;
}

void renderer_shutdown(void) {
    if (s.surface) drop_present();
    if (s.dev) {
        vkDeviceWaitIdle(s.dev);
        for (FrameSlot &f : s.frames) {
            free_buffer(&f.upload);
            free_buffer(&f.geometry);
            free_buffer(&f.programs);
            free_buffer(&f.movie);
            if (f.set_pool) vkDestroyDescriptorPool(s.dev, f.set_pool, nullptr);
            if (f.fence) vkDestroyFence(s.dev, f.fence, nullptr);
        }
        if (s.once_fence) vkDestroyFence(s.dev, s.once_fence, nullptr);
        free_buffer(&s.readback);
        for (int i = 0; i < MOVIE_PLANES; i++) free_image(&s.movie[i]);
        drop_textures();
        free_surfaces();
        glass_free();
        sharpen_free();
        filmic_free();
        bloom_free();
        rain_free();
        sky_free();
        ssao_free();
        splash_free();
        wet_free();
        cover_free();
        chars_free();
        soft_free();
        lamp_free();
        contact_free();
        if (s.split_pass) vkDestroyRenderPass(s.dev, s.split_pass, nullptr);
        if (s.resume_pass) vkDestroyRenderPass(s.dev, s.resume_pass, nullptr);
        for (auto &p : s.pipelines)
            if (p.second) vkDestroyPipeline(s.dev, p.second, nullptr);
        if (s.vs) vkDestroyShaderModule(s.dev, s.vs, nullptr);
        if (s.fs) vkDestroyShaderModule(s.dev, s.fs, nullptr);
        if (s.layout) vkDestroyPipelineLayout(s.dev, s.layout, nullptr);
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
    s.ops.clear();
    s.verts.clear();
    s.indices.clear();
    s.bloom.marked = false;
    s.filmic.marked = false;
    s.ssao.marked = false;
    s.soft.marked = false;
    s.lamp.marked = false;
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
    wait_slot();
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
    }
    const uint32_t shadow = shadow_word(s.cur_tex, s.cur_ps.shadow_func);
    op.shape = shadow ? shape_index(s.cur_ps, shadow) : s.cur_shape;
    op.blend_color = s.cur_state.blend_color;
    s.verts.insert(s.verts.end(), v, v + nv);
    s.indices.insert(s.indices.end(), idx, idx + op.index_count);
    s.ops.push_back(op);
}

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
    s.verts.insert(s.verts.end(), v, v + nv);
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
    s.wet.marked = true;
    s.wet.p = *p;
    Op op{};
    op.kind = OP_WET;
    s.ops.push_back(op);
}
int renderer_rain_wet_on(void) { return s.dev && s.wet.ok; }
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
    if (!s.filmic.ok || s.filmic.marked || s.cur_target[0] || s.cur_target[1]) return;
    s.filmic.marked = true;
    Op op{};
    op.kind = OP_FILMIC;
    s.ops.push_back(op);
}
int renderer_filmic_on(void) { return s.dev && s.filmic.ok; }
uint32_t renderer_filmic_count(void) { return s.filmic.executed; }

int renderer_has_bc(void) { return s.bc; }
uint32_t renderer_texture_count(void) { return (uint32_t)s.textures.size(); }

void renderer_set_texture_budget(uint64_t bytes) { texture_budget = bytes ? bytes : DEFAULT_TEXTURE_BYTES; }

uint32_t renderer_texture_find(uint64_t key) {
    auto it = s.by_key.find(key);
    return it == s.by_key.end() ? 0 : it->second;
}

uint32_t renderer_texture_create(uint64_t key, uint32_t format, uint32_t w, uint32_t h, uint32_t levels, const void *data) {
    if (!s.dev || s.textures.size() >= MAX_TEXTURES) return 0;
    if (uint32_t found = renderer_texture_find(key)) return found;
    Texture t;
    if (make_texture(&t, format, w, h, levels, data)) {
        free_image(&t.im);
        if (!texture_failures++ || texture_failures % 100 == 0)
            printf("[renderer] cannot create a %ux%u texture (%u failures, %u textures, %llu MB): dropping the texture cache\n", w,
                   h, texture_failures, (uint32_t)s.textures.size(), (unsigned long long)(s.texture_bytes >> 20));
        texture_drop_requested = true;
        return 0;
    }
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
            vkDeviceWaitIdle(s.dev);
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

void renderer_set_texture(uint32_t handle) { renderer_set_stage_texture(0, handle); }
void renderer_set_stage_texture(uint32_t stage, uint32_t handle) {
    if (stage < RENDERER_STAGES)
        s.cur_tex[stage] = handle <= s.textures.size() || surface_of(handle) || (is_movie(handle) && s.movie[handle - MOVIE_TEXTURE].view) ? handle : 0;
}

uint32_t renderer_surface(uint64_t key, uint32_t kind, int w, int h) {
    if (!s.dev || w <= 0 || h <= 0 || kind > RENDERER_SURFACE_DEPTH24) return 0;
    const uint64_t k = key ^ ((uint64_t)kind << 62) ^ ((uint64_t)(uint32_t)w << 40) ^ ((uint64_t)(uint32_t)h << 24);
    auto it = s.surface_by_key.find(k);
    if (it != s.surface_by_key.end()) return it->second;
    if (s.surfaces.size() >= MAX_SURFACES) {
        fail("too many offscreen surfaces");
        return 0;
    }
    // Render scale (the internal height over the back-buffer height, on both axes) times
    // RendererConfig.shadow_scale.
    const int scale = cfg.shadow_scale > 1 ? (cfg.shadow_scale < 4 ? cfg.shadow_scale : 4) : 1;
    const double f = (double)s.oh / s.h * scale;
    Surface sf;
    sf.kind = kind;
    sf.gw = w;
    sf.gh = h;
    sf.w = (int)(w * f + 0.5);
    sf.h = (int)(h * f + 0.5);
    if (sf.w > (int)s.max_dim) sf.w = (int)s.max_dim;
    if (sf.h > (int)s.max_dim) sf.h = (int)s.max_dim;
    const bool depth = kind != RENDERER_SURFACE_COLOR;
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
    bool bound_buffers = false;
};
static void exec_op(const Op &op, PassState &ps, std::unordered_map<uint64_t, VkDescriptorSet> &sets, VkDeviceSize index_offset,
                    float sx, float sy, int tw, int th) {
    if (op.kind == OP_DRAW) {
        VkPipeline pipe = get_pipeline(op.pipe_key, op.shape);
        if (!pipe) return;
        if (!ps.bound_buffers) {
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(s.cmd, 0, 1, &cur().geometry.buf, &zero);
            vkCmdBindIndexBuffer(s.cmd, cur().geometry.buf, index_offset, VK_INDEX_TYPE_UINT32);
            ps.bound_buffers = true;
        }
        if (ps.bound != pipe) vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
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
        const VkPipelineLayout layout = soft ? s.soft.layout : s.layout;
        if (layout != ps.bound_layout) {
            ps.bound_set = VK_NULL_HANDLE;
            ps.bound_layout = layout;
            if (soft) vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &s.soft.set, 0, nullptr);
        }
        if (set != ps.bound_set) vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        ps.bound_set = set;
        if (soft) {
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
        vkCmdDrawIndexed(s.cmd, op.index_count, 1, op.first_index, op.vertex_offset, 0);
        if (soft) s.soft.executed++;
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

// The frame's offscreen operations, in recorded order: consecutive operations on the same target
// share one render pass instance.
static void record_offscreen(std::unordered_map<uint64_t, VkDescriptorSet> &sets, VkDeviceSize index_offset) {
    const Op *open = nullptr;
    OffscreenFb *fb = nullptr;
    PassState ps;
    for (const Op &op : s.ops) {
        if ((!op.target[0] && !op.target[1]) || op.kind == OP_GLASS) continue;
        if (open && open->target[0] == op.target[0] && open->target[1] == op.target[1]) {
            if (fb) exec_op(op, ps, sets, index_offset, (float)fb->w / fb->gw, (float)fb->h / fb->gh, fb->w, fb->h);
            continue;
        }
        if (open && fb) {
            vkCmdEndRenderPass(s.cmd);
            surface_barriers(*open, false);
        }
        open = &op;
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
// dst and FS_PREMUL (wet ground) adds src over dst x (1 - src alpha), all writing rgb only, like
// the geometry pipelines.
enum { FS_REPLACE, FS_COVER, FS_ADD, FS_SCREEN, FS_MULTIPLY, FS_LIGHTEN, FS_PREMUL };
static VkPipeline fullscreen_pipeline(VkShaderModule vs, VkPipelineLayout layout, VkShaderModule fs, VkRenderPass pass,
                                      VkSampleCountFlagBits samples, int blend, bool depth_test = false,
                                      const VkSpecializationInfo *spec = nullptr) {
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
    ds.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;   // rain slices: tested, never written
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cb{};
    cb.blendEnable = blend ? VK_TRUE : VK_FALSE;
    cb.srcColorBlendFactor = cb.srcAlphaBlendFactor = blend == FS_ADD || blend == FS_PREMUL ? VK_BLEND_FACTOR_ONE
                                                      : blend == FS_SCREEN   ? VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR
                                                      : blend == FS_MULTIPLY || blend == FS_LIGHTEN ? VK_BLEND_FACTOR_DST_COLOR
                                                                                                    : VK_BLEND_FACTOR_SRC_ALPHA;
    cb.dstColorBlendFactor = cb.dstAlphaBlendFactor = blend == FS_ADD || blend == FS_SCREEN || blend == FS_LIGHTEN ? VK_BLEND_FACTOR_ONE
                                                      : blend == FS_MULTIPLY                 ? VK_BLEND_FACTOR_ZERO
                                                                                             : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cb.colorBlendOp = cb.alphaBlendOp = VK_BLEND_OP_ADD;
    cb.colorWriteMask = blend ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT : 15;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
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
    if (vkCreateGraphicsPipelines(s.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
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
    if (vkCreateRenderPass(s.dev, &rp, nullptr, &g.blur_pass) != VK_SUCCESS) return fail("glass: vkCreateRenderPass failed"), false;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = g.blur_pass;
    fi.attachmentCount = 1;
    fi.width = ow;
    fi.height = oh;
    fi.layers = 1;
    fi.pAttachments = &g.a.view;
    if (vkCreateFramebuffer(s.dev, &fi, nullptr, &g.fb_a) != VK_SUCCESS) return false;
    fi.pAttachments = &g.b.view;
    if (vkCreateFramebuffer(s.dev, &fi, nullptr, &g.fb_b) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;
    if (vkCreateDescriptorSetLayout(s.dev, &li, nullptr, &g.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(s.dev, &pci, nullptr, &g.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[2] = {g.set_layout, g.set_layout};
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g.pool;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(s.dev, &ai, sets) != VK_SUCCESS) return false;
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
    if (vkCreatePipelineLayout(s.dev, &pli, nullptr, &g.layout) != VK_SUCCESS) return false;
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
    if (make_image(&h.out, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
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
    if (vkCreateRenderPass(s.dev, &rp, nullptr, &h.pass) != VK_SUCCESS) return false;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = h.pass;
    fi.attachmentCount = 1;
    fi.pAttachments = &h.out.view;
    fi.width = fw;
    fi.height = fh;
    fi.layers = 1;
    if (vkCreateFramebuffer(s.dev, &fi, nullptr, &h.fb) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;
    if (vkCreateDescriptorSetLayout(s.dev, &li, nullptr, &h.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(s.dev, &pci, nullptr, &h.pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = h.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &h.set_layout;
    if (vkAllocateDescriptorSets(s.dev, &ai, &h.set) != VK_SUCCESS) return false;
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
    if (vkCreatePipelineLayout(s.dev, &pli, nullptr, &h.layout) != VK_SUCCESS) return false;
    h.vs = shader(glass_vert, sizeof glass_vert);
    h.fs = shader(cas_frag, sizeof cas_frag);
    if (!h.vs || !h.fs) return false;
    h.pipe = fullscreen_pipeline(h.vs, h.layout, h.fs, h.pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE);
    return h.pipe != VK_NULL_HANDLE;
}

// At renderer_init: the sharpening resources when RendererConfig.sharpen > 0. A failure frees them
// and leaves sharpening off (logged by the caller through renderer_sharpen()).
static void sharpen_init() {
    if (cfg.sharpen <= 0) return;
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
    return vkCreateRenderPass(s.dev, &rp, nullptr, out) == VK_SUCCESS;
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
        if (vkCreateFramebuffer(s.dev, &fi, nullptr, &b.fb[i]) != VK_SUCCESS) return false;
    }
    VkDescriptorSetLayoutBinding lb{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &lb;
    if (vkCreateDescriptorSetLayout(s.dev, &li, nullptr, &b.set_layout) != VK_SUCCESS) return false;
    const uint32_t nsets = (uint32_t)b.levels + 1;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nsets};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = nsets;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(s.dev, &pci, nullptr, &b.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[MAX_BLOOM_LEVELS + 1];
    VkDescriptorSet sets[MAX_BLOOM_LEVELS + 1];
    for (uint32_t i = 0; i < nsets; i++) layouts[i] = b.set_layout;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = b.pool;
    ai.descriptorSetCount = nsets;
    ai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(s.dev, &ai, sets) != VK_SUCCESS) return false;
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
    if (vkCreatePipelineLayout(s.dev, &pli, nullptr, &b.layout) != VK_SUCCESS) return false;
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
};

static void filmic_free() {
    Filmic &f = s.filmic;
    if (f.pipe) vkDestroyPipeline(s.dev, f.pipe, nullptr);
    for (VkShaderModule m : {f.vs, f.fs})
        if (m) vkDestroyShaderModule(s.dev, m, nullptr);
    if (f.layout) vkDestroyPipelineLayout(s.dev, f.layout, nullptr);
    if (f.pool) vkDestroyDescriptorPool(s.dev, f.pool, nullptr);
    if (f.set_layout) vkDestroyDescriptorSetLayout(s.dev, f.set_layout, nullptr);
    free_image(&f.src);
    free_image(&f.lut);
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
    if (vkCreateImage(s.dev, &ii, nullptr, &f.lut.img) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(s.dev, f.lut.img, &req);
    int type = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = find_memory(req.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)type;
    if (type < 0 || vkAllocateMemory(s.dev, &ai, nullptr, &f.lut.mem) != VK_SUCCESS) return false;
    vkBindImageMemory(s.dev, f.lut.img, f.lut.mem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = f.lut.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(s.dev, &vi, nullptr, &f.lut.view) != VK_SUCCESS) return false;
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

static bool filmic_create() {
    Filmic &f = s.filmic;
    const RFilmic &p = *cfg.filmic;
    if (!p.lut || p.lut_size < 2 || p.lut_size > 64 || !split_passes_ready()) return false;
    f.p = p;
    f.p.lut = nullptr;
    f.lut_size = p.lut_size;
    if (!filmic_lut(p.lut, p.lut_size)) return false;
    if (make_image(&f.src, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                   (uint32_t)s.ow, (uint32_t)s.oh))
        return false;
    VkDescriptorSetLayoutBinding lb[3];
    for (uint32_t i = 0; i < 3; i++) lb[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 3;
    li.pBindings = lb;
    if (vkCreateDescriptorSetLayout(s.dev, &li, nullptr, &f.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(s.dev, &pci, nullptr, &f.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[2] = {f.set_layout, f.set_layout};
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = f.pool;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(s.dev, &ai, sets) != VK_SUCCESS) return false;
    f.set_glow = sets[0];
    f.set_plain = sets[1];
    // The halation glow: bloom level 1 (sum of the coarser levels after the upsample chain).
    const bool glow = s.bloom.ok && s.bloom.levels > 1;
    VkDescriptorImageInfo ii[6];
    VkWriteDescriptorSet wr[6]{};
    for (int i = 0; i < 6; i++) {
        const int bind = i % 3;
        const VkImageView v = bind == 0 ? f.src.view : bind == 1 ? f.lut.view : i < 3 && glow ? s.bloom.lv[1].view : f.src.view;
        ii[i] = {s.clamp_sampler, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = sets[i / 3];
        wr[i].dstBinding = (uint32_t)bind;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(s.dev, 6, wr, 0, nullptr);
    if (!glow) f.set_glow = f.set_plain;
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FilmicPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &f.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(s.dev, &pli, nullptr, &f.layout) != VK_SUCCESS) return false;
    f.vs = shader(glass_vert, sizeof glass_vert);
    f.fs = shader(filmic_frag, sizeof filmic_frag);
    if (!f.vs || !f.fs) return false;
    f.pipe = fullscreen_pipeline(f.vs, f.layout, f.fs, s.pass, s.samples, FS_REPLACE);
    return f.pipe != VK_NULL_HANDLE;
}

// At renderer_init: the filmic resources when RendererConfig.filmic is set. A failure frees them
// and leaves the look off (renderer_filmic_on()).
static void filmic_init() {
    if (!cfg.filmic) return;
    if (filmic_create()) {
        s.filmic.ok = true;
        return;
    }
    filmic_free();
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
    barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = resume_pass();
    rb.framebuffer = s.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
    vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    const bool glow = s.bloom.executed > 0 && f.set_glow != f.set_plain;
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, f.pipe);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, f.layout, 0, 1, glow ? &f.set_glow : &f.set_plain, 0, nullptr);
    const RFilmic &p = f.p;
    FilmicPush pc = {{1.0f / s.ow, 1.0f / s.oh, p.exposure, p.white},
                     {p.blend, p.hdr, p.lut_mix, glow ? p.halation : 0.0f},
                     {p.grain, p.vignette, (float)(f.frame++ % 4096u), (float)f.lut_size},
                     {p.norm, (float)s.ow / s.oh, 0, 0}};
    vkCmdPushConstants(s.cmd, f.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport v{0, 0, (float)s.ow, (float)s.oh, 0, 1};
    vkCmdSetViewport(s.cmd, 0, 1, &v);
    vkCmdSetScissor(s.cmd, 0, 1, &rb.renderArea);
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
    f.executed++;
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
    if (vkCreatePipelineLayout(s.dev, &pli, nullptr, &r.layout) == VK_SUCCESS) {
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
    if (vkCreateGraphicsPipelines(s.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
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
    if (vkCreatePipelineLayout(s.dev, &pli, nullptr, &k.layout) == VK_SUCCESS &&
        vkCreatePipelineLayout(s.dev, &lli, nullptr, &k.line_layout) == VK_SUCCESS) {
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
    vkCmdDraw(s.cmd, 3, 1, 0, 0);
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
    vkCmdDrawIndexed(s.cmd, op.index_count, 1, op.first_index, op.vertex_offset, 0);
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
    return vkCreateRenderPass(s.dev, &rp, nullptr, out) == VK_SUCCESS;
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
    vkCreateFramebuffer(s.dev, &fi, nullptr, &fb);
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
    if (vkCreateDescriptorSetLayout(s.dev, &li, nullptr, &a.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * nb};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(s.dev, &pci, nullptr, &a.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[2] = {a.set_layout, a.set_layout};
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = a.pool;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(s.dev, &ai, sets) != VK_SUCCESS) return false;
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
    if (vkCreatePipelineLayout(s.dev, &pli, nullptr, &a.layout) != VK_SUCCESS) return false;
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
    if (vkCreateDescriptorSetLayout(s.dev, &li, nullptr, &f.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(s.dev, &pi, nullptr, &f.pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = f.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &f.set_layout;
    if (vkAllocateDescriptorSets(s.dev, &ai, &f.set) != VK_SUCCESS) return false;
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
    if (vkCreatePipelineLayout(s.dev, &pli, nullptr, &f.layout) != VK_SUCCESS) return false;
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
    vkCmdEndRenderPass(s.cmd);
    // The pass's outgoing dependency covers its depth writes for fragment shader reads.
    barrier(s.depth.img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 1,
            VK_IMAGE_ASPECT_DEPTH_BIT);
    // The single-sample color (the target, or the resolve target with MSAA) back to an attachment;
    // with the light mask first readable (its alpha), until the reads are done (color_attach).
    // The lamp lights read the light mask in their light pass (outside the composite) whatever the
    // sample count.
    const bool mask = (ao && a.mask) || lamp;
    if (mask)
        barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    else
        barrier(s.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    auto color_attach = [] {
        barrier(s.color.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    };
    if (sp) splash_mask(*sp);
    if (lamp) lamp_light(*lamp);
    if (cs) contact_mask(*cs);
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
        ssao_half(a.a, a.fb_a, a.ao, a.set_b, pc);   // set_b: binding 1 (image b) is not read by this pass
        pc.dir[0] = 1;
        ssao_half(a.b, a.fb_b, a.blur, a.set_a, pc);
        pc.dir[0] = 0;
        pc.dir[1] = 1;
        ssao_half(a.a, a.fb_a, a.blur, a.set_b, pc);
        if (mask && !ms_read) color_attach();   // the composite draws into s.color (or does not read it)
        rb.renderPass = a.comp_pass;
        rb.framebuffer = a.comp_fb;
        vkCmdBeginRenderPass(s.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        if (lamp) lamp_comp(*lamp);   // lit first, then occluded (the same pass: one load and store)
        if (cs) contact_comp(*cs, true);
        vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.comp);
        vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.layout, 0, 1, &a.set_a, 0, nullptr);
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
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
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
    VkDescriptorSetLayoutBinding lb[5];
    for (uint32_t i = 0; i < 5; i++)
        lb[i] = {i, i == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 5;
    li.pBindings = lb;
    if (vkCreateDescriptorSetLayout(s.dev, &li, nullptr, &a.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = ps;
    if (vkCreateDescriptorPool(s.dev, &pci, nullptr, &a.pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = a.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &a.set_layout;
    if (vkAllocateDescriptorSets(s.dev, &ai, &a.set) != VK_SUCCESS) return false;
    // Every read is a texelFetch: the nearest sampler (clamped) for all of them.
    const VkDescriptorImageInfo ii[4] = {
        {s.nearest_sampler, s.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {s.nearest_sampler, a.img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        a.chars ? VkDescriptorImageInfo{s.nearest_sampler, s.chars.depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}
                : VkDescriptorImageInfo{s.nearest_sampler, s.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},   // placeholder, never read
        {s.nearest_sampler, s.color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    const VkDescriptorBufferInfo bi{a.ubo.buf, 0, sizeof(LampUbo)};
    VkWriteDescriptorSet wr[5]{};
    for (uint32_t i = 0; i < 5; i++) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = a.set;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = i == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        if (i == 4) wr[i].pBufferInfo = &bi;
        else wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(s.dev, 5, wr, 0, nullptr);
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SsaoPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &a.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(s.dev, &pli, nullptr, &a.layout) != VK_SUCCESS) return false;
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
                    {(float)p.n, p.char_gain > 1 ? p.char_gain : 1.0f, LAMP_FALLOFF_K, LAMP_WRAP},
                    {0, 0, (float)a.hw, (float)a.hh},
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
    if (vkCreateDescriptorSetLayout(s.dev, &li, nullptr, &a.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(s.dev, &pci, nullptr, &a.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[2] = {a.set_layout, a.set_layout};
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = a.pool;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(s.dev, &ai, sets) != VK_SUCCESS) return false;
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
    if (vkCreatePipelineLayout(s.dev, &pli, nullptr, &a.layout) != VK_SUCCESS) return false;
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
        if (vkCreatePipelineLayout(s.dev, &pli, nullptr, &a.mask_layout) != VK_SUCCESS) return false;
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
    if (!s.depth_sampled || !s.chars.ok || (fp.optimalTilingFeatures & need) != need || !split_passes_ready()) return false;
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
    if (vkCreateDescriptorSetLayout(s.dev, &li, nullptr, &a.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(s.dev, &pci, nullptr, &a.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[2] = {a.set_layout, a.set_layout};
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = a.pool;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(s.dev, &ai, sets) != VK_SUCCESS) return false;
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
    if (vkCreatePipelineLayout(s.dev, &pli, nullptr, &a.layout) != VK_SUCCESS) return false;
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

static void wet_free() {
    Wet &a = s.wet;
    for (VkPipeline p : {a.trace, a.blur, a.comp})
        if (p) vkDestroyPipeline(s.dev, p, nullptr);
    for (VkShaderModule m : {a.vs, a.fs, a.trace_fs})
        if (m) vkDestroyShaderModule(s.dev, m, nullptr);
    for (VkPipelineLayout l : {a.layout, a.trace_layout})
        if (l) vkDestroyPipelineLayout(s.dev, l, nullptr);
    if (a.pool) vkDestroyDescriptorPool(s.dev, a.pool, nullptr);
    if (a.set_layout) vkDestroyDescriptorSetLayout(s.dev, a.set_layout, nullptr);
    for (VkFramebuffer f : {a.fb, a.fb_a, a.fb_b})
        if (f) vkDestroyFramebuffer(s.dev, f, nullptr);
    for (VkRenderPass p : {a.pass, a.half_pass})
        if (p) vkDestroyRenderPass(s.dev, p, nullptr);
    free_image(&a.src);
    free_image(&a.a);
    free_image(&a.b);
    a = Wet();
}

static bool wet_create() {
    Wet &a = s.wet;
    const bool ms = s.samples != VK_SAMPLE_COUNT_1_BIT;
    const VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;   // Vulkan 1.0 requires it sampled (linear) and as a color attachment
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(s.phys, fmt, &fp);
    const VkFormatFeatureFlags need =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if (!s.depth_sampled || !s.chars.ok || (fp.optimalTilingFeatures & need) != need || !split_passes_ready()) return false;
    a.scale = 2 * (s.ss > 1 ? s.ss : 1);   // half the output size
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
    // Binding 2: the character mask (wet.frag: characters stay dry).
    VkDescriptorSetLayoutBinding lb[3] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 3;
    li.pBindings = lb;
    if (vkCreateDescriptorSetLayout(s.dev, &li, nullptr, &a.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 9};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 3;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(s.dev, &pci, nullptr, &a.pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[3] = {a.set_layout, a.set_layout, a.set_layout};
    VkDescriptorSet sets[3];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = a.pool;
    ai.descriptorSetCount = 3;
    ai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(s.dev, &ai, sets) != VK_SUCCESS) return false;
    a.set_src = sets[0];
    a.set_a = sets[1];
    a.set_b = sets[2];
    const VkImageView second[3] = {a.src.view, a.a.view, a.b.view};
    VkDescriptorImageInfo ii[9];
    VkWriteDescriptorSet wr[9]{};
    for (int i = 0; i < 9; i++) {
        ii[i] = i % 3 == 1   ? VkDescriptorImageInfo{s.clamp_sampler, second[i / 3], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                : i % 3 == 2 ? VkDescriptorImageInfo{s.nearest_sampler, s.chars.depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}
                             : VkDescriptorImageInfo{s.nearest_sampler, s.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = sets[i / 3];
        wr[i].dstBinding = (uint32_t)(i % 3);
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(s.dev, 9, wr, 0, nullptr);
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SsaoPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &a.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(s.dev, &pli, nullptr, &a.layout) != VK_SUCCESS) return false;
    a.vs = shader(glass_vert, sizeof glass_vert);
    a.fs = ms ? shader(wet_ms_frag, sizeof wet_ms_frag) : shader(wet_frag, sizeof wet_frag);
    if (!a.vs || !a.fs) return false;
    const int32_t modes[3] = {0, 1, 2};   // wet.frag: trace, blur, composite
    VkSpecializationMapEntry me{0, 0, sizeof(int32_t)};
    VkSpecializationInfo spec[3] = {{1, &me, sizeof(int32_t), &modes[0]}, {1, &me, sizeof(int32_t), &modes[1]}, {1, &me, sizeof(int32_t), &modes[2]}};
    if (s.cover.ok) {   // the trace leaves covered ground dry: set 1 the cover's
        const VkDescriptorSetLayout two[2] = {a.set_layout, s.cover.set_layout};
        pli.setLayoutCount = 2;
        pli.pSetLayouts = two;
        if (vkCreatePipelineLayout(s.dev, &pli, nullptr, &a.trace_layout) != VK_SUCCESS) return false;
        a.trace_fs = ms ? shader(wet_cover_ms_frag, sizeof wet_cover_ms_frag) : shader(wet_cover_frag, sizeof wet_cover_frag);
        if (!a.trace_fs) return false;
    }
    a.trace = fullscreen_pipeline(a.vs, a.trace_layout ? a.trace_layout : a.layout, a.trace_fs ? a.trace_fs : a.fs, a.half_pass,
                                  VK_SAMPLE_COUNT_1_BIT, FS_REPLACE, false, &spec[0]);
    a.blur = fullscreen_pipeline(a.vs, a.layout, a.fs, a.half_pass, VK_SAMPLE_COUNT_1_BIT, FS_REPLACE, false, &spec[1]);
    a.comp = fullscreen_pipeline(a.vs, a.layout, a.fs, a.pass, s.samples, FS_PREMUL, false, &spec[2]);
    return a.trace && a.blur && a.comp;
}

// At renderer_init: the wet resources when RendererConfig.rain and rain_wet are set and the depth
// target can be sampled. A failure frees them and leaves wet ground off (renderer_rain_wet_on()).
static void wet_init() {
    if (!cfg.rain || !cfg.rain_wet) return;
    if (wet_create()) {
        s.wet.ok = true;
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
    if (p.puddles > 0) {   // puddles: strength, time and the camera in the world (else as without them)
        pc.dir[3] = p.puddles > 1 ? 1.0f : p.puddles;
        pc.params[3] = p.time;
        const float *ax[3] = {p.right, p.up_v, p.back};
        for (int v = 0; v < 3; v++) {
            for (int k = 0; k < 3; k++) pc.cam[v + 1][k] = ax[v][k];
            pc.cam[v + 1][3] = p.eye[v];
        }
    }
    wet_half(a.a, a.fb_a, a.trace, a.set_src, pc);
    wet_half(a.b, a.fb_b, a.blur, a.set_a, pc);
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
    if (vkCreateRenderPass(s.dev, &rp, nullptr, &c.pass) != VK_SUCCESS) return false;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = c.pass;
    fi.attachmentCount = 1;
    fi.pAttachments = &c.map.view;
    fi.width = fi.height = (uint32_t)n;
    fi.layers = 1;
    if (vkCreateFramebuffer(s.dev, &fi, nullptr, &c.fb) != VK_SUCCESS) return false;
    // The consumers' set: the map (nearest, read with textureGather) and the uniform block.
    VkDescriptorSetLayoutBinding lb[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2;
    li.pBindings = lb;
    if (vkCreateDescriptorSetLayout(s.dev, &li, nullptr, &c.set_layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = ps;
    if (vkCreateDescriptorPool(s.dev, &pci, nullptr, &c.pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = c.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &c.set_layout;
    if (vkAllocateDescriptorSets(s.dev, &ai, &c.set) != VK_SUCCESS) return false;
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
    if (vkCreatePipelineLayout(s.dev, &pli, nullptr, &c.layout) != VK_SUCCESS) return false;
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
    return vkCreateGraphicsPipelines(s.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &c.pipe) == VK_SUCCESS;
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

// ---- The rain's character mask (renderer_rain_chars_on) ----

static void chars_free() {
    Chars &c = s.chars;
    if (c.pipe) vkDestroyPipeline(s.dev, c.pipe, nullptr);
    if (c.vs) vkDestroyShaderModule(s.dev, c.vs, nullptr);
    if (c.layout) vkDestroyPipelineLayout(s.dev, c.layout, nullptr);
    if (c.fb) vkDestroyFramebuffer(s.dev, c.fb, nullptr);
    if (c.pass) vkDestroyRenderPass(s.dev, c.pass, nullptr);
    free_image(&c.depth);
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
    if (vkCreateRenderPass(s.dev, &rp, nullptr, &c.pass) != VK_SUCCESS) return false;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = c.pass;
    fi.attachmentCount = 1;
    fi.pAttachments = &c.depth.view;
    fi.width = (uint32_t)s.ow;
    fi.height = (uint32_t)s.oh;
    fi.layers = 1;
    if (vkCreateFramebuffer(s.dev, &fi, nullptr, &c.fb) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (vkCreatePipelineLayout(s.dev, &pli, nullptr, &c.layout) != VK_SUCCESS) return false;
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
    return vkCreateGraphicsPipelines(s.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &c.pipe) == VK_SUCCESS;
}

// At renderer_init, before the splash and wet ground (they need it): the character mask when
// RendererConfig.rain is set and the depth target can be sampled. A failure leaves it off, and
// with it the splashes and wet ground.
static void chars_init() {
    if ((!cfg.rain && !cfg.lamp && !cfg.contact) || !s.depth_sampled) return;
    if (chars_create()) {
        s.chars.ok = true;
        return;
    }
    chars_free();
}

// Outside a render pass, before the frame's main pass, in a frame with splashes or wet ground: the
// mask cleared, then every frame-target draw flagged RENDERER_CHARACTER, depth only, with its own
// viewport (as exec_op). Nothing in other frames (rain off: never).
static void chars_record(VkDeviceSize index_offset, float sx, float sy) {
    Chars &c = s.chars;
    if (!c.ok) return;
    bool used = false;
    for (const Op &op : s.ops)
        if (op.kind == OP_SPLASH || op.kind == OP_WET || op.kind == OP_LAMP || op.kind == OP_CONTACT) {
            used = true;
            break;
        }
    if (!used) return;
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = c.pass;
    rb.framebuffer = c.fb;
    rb.renderArea = {{0, 0}, {(uint32_t)s.ow, (uint32_t)s.oh}};
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
        x1 = x1 > s.ow ? s.ow : x1;
        y1 = y1 > s.oh ? s.oh : y1;
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
        vkCmdDrawIndexed(s.cmd, op.index_count, 1, op.first_index, op.vertex_offset, 0);
        c.executed++;
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
    const float sx = (float)s.ow / s.w, sy = (float)s.oh / s.h;   // back-buffer pixels -> target pixels
    if (wait_slot()) return -1;   // normally done by renderer_begin
    if (!s.indices.empty() && (upload_geometry(&index_offset) || upload_programs())) return -1;
    vkResetDescriptorPool(s.dev, cur().set_pool, 0);   // the slot's previous frame has completed (fence)
    s.glass.executed = 0;
    s.bloom.executed = 0;
    s.filmic.executed = 0;
    s.rain.executed = 0;
    s.sky.executed = s.sky.lines_executed = 0;
    s.ssao.executed = 0;
    s.splash.executed = 0;
    s.wet.executed = 0;
    s.cover.executed = 0;
    s.chars.executed = 0;
    s.soft.executed = 0;
    s.lamp.executed = 0;
    s.soft.live = s.soft.in_pass = false;
    bool glass = false;
    size_t bloom_at = s.ops.size();   // the bloom split (renderer_bloom), if any
    size_t ssao_at = s.ops.size();    // the ssao split (renderer_ssao), if any
    size_t splash_at = s.ops.size();  // the rain splashes (renderer_rain_splash), if any
    size_t wet_at = s.ops.size();     // the wet ground (renderer_rain_wet), if any
    size_t filmic_at = s.ops.size();  // the filmic pass (renderer_filmic), if any
    size_t lamp_at = s.ops.size();    // the lamp lights (renderer_lamp_lights), if any
    size_t contact_at = s.ops.size(); // the contact shadows (renderer_contact), if any
    s.contact.executed = 0;
    s.contact.pending = false;
    for (size_t i = 0; i < s.ops.size(); i++) {
        if (s.ops[i].kind == OP_CONTACT && contact_at == s.ops.size()) contact_at = i;
        if (s.ops[i].kind == OP_FILMIC && filmic_at == s.ops.size()) filmic_at = i;
        if (s.ops[i].kind == OP_LAMP && lamp_at == s.ops.size()) lamp_at = i;
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
                                                        s.ops[soft_from - 1].kind == OP_LAMP || s.ops[soft_from - 1].kind == OP_CONTACT))
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
    vkResetCommandBuffer(s.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cmd, &bi);
    record_offscreen(sets, index_offset);
    cover_record(index_offset);
    chars_record(index_offset, sx, sy);

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
                            contact_at < s.ops.size()
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
                soft_split();
                ps = PassState();   // a new render pass instance
            }
        } else if (op.kind == OP_BLOOM) {
            if (i == bloom_at) {
                bloom_exec();
                ps = PassState();   // a new render pass instance
            }
        } else if (op.kind == OP_SSAO) {
            if (i == ssao_at) {   // the lamp lights right before it and the splashes right after it share the split
                ssao_exec(&op.ssao, splash_at < s.ops.size() && splash_at == i + 1 ? &s.splash.p : nullptr,
                          lamp_at < s.ops.size() && lamp_at + 1 == i ? &s.lamp.p : nullptr, contact_take());
                ps = PassState();   // a new render pass instance
            }
        } else if (op.kind == OP_LAMP) {
            if (i == lamp_at && ssao_at != lamp_at + 1) {   // alone, with the splashes right after it
                ssao_exec(nullptr, splash_at < s.ops.size() && splash_at == i + 1 ? &s.splash.p : nullptr, &s.lamp.p, contact_take());
                ps = PassState();
            }
        } else if (op.kind == OP_SPLASH) {
            if (i == splash_at && splash_at != ssao_at + 1 && !(lamp_at < s.ops.size() && splash_at == lamp_at + 1 && ssao_at != lamp_at + 1)) {
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
                    ssao_exec(nullptr, nullptr, nullptr, &s.contact.p);
                    ps = PassState();
                }
            }
        } else if (op.kind == OP_WET) {
            if (i == wet_at) {
                wet_exec(s.wet.p);
                ps = PassState();
            }
        } else if (op.kind == OP_FILMIC) {
            if (i == filmic_at) {
                filmic_exec();
                ps = PassState();   // a new render pass instance
            }
        } else if (op.kind == OP_GLASS) {
            if (glass && glass_exec(op.glass, sx, sy)) ps = PassState();
        } else if (op.kind == OP_RAIN) {
            rain_exec(s.rain.list[op.first_index]);
            ps = PassState();   // another pipeline was bound
        } else if (op.kind == OP_SKY) {
            sky_exec(s.sky.list[op.first_index]);
            ps = PassState();
        } else if (op.kind == OP_SKYLINE) {
            skyline_exec(op, index_offset, sx, sy);
            ps = PassState();
        } else if (i < bloom_at) {   // the scene before the bloom split: with the light mask
            Op m = op;
            m.mask = true;
            if (op.kind == OP_DRAW) m.pipe_key |= mask_role(op.pipe_key) << 27;
            exec_op(m, ps, sets, index_offset, sx, sy, s.ow, s.oh);
        } else {
            exec_op(op, ps, sets, index_offset, sx, sy, s.ow, s.oh);
        }
    }
    vkCmdEndRenderPass(s.cmd);   // color (or the resolve target) ends in TRANSFER_SRC_OPTIMAL
    s.soft.live = s.soft.in_pass = false;
    record_downsample();
    record_sharpen();
    return 0;
}

int renderer_end(uint32_t *out, int pitch_bytes) {
    if (!s.dev) return fail("renderer not initialized");
    double t_start = now_ms();
    VkDeviceSize frame_bytes = (VkDeviceSize)s.fw * s.fh * 4;
    if (record_frame()) return -1;
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)s.fw, (uint32_t)s.fh, 1};
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
    vkEndCommandBuffer(s.cmd);
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
    next_slot();
    double t_done = now_ms();
    const uint32_t *px = (const uint32_t *)s.readback.map;
    for (int y = 0; y < s.fh; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)out + (size_t)y * pitch_bytes);
        for (int x = 0; x < s.fw; x++) row[x] = px[(size_t)y * s.fw + x] & 0x00FFFFFFu;
    }
    s.timing.record_ms = (float)(t_submit - t_start);
    s.timing.gpu_ms = (float)(t_done - t_submit + slot_wait);   // with the wait for the slot at renderer_begin
    s.timing.readback_ms = (float)(now_ms() - t_done);
    s.timing.present_ms = 0;
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
    double t_acquire = now_ms(), acquire_ms = 0;
    uint32_t idx = 0;
    VkResult r = VK_SUCCESS;
    for (int attempt = 0; attempt < 2; attempt++) {
        if ((s.sc_stale || !s.swapchain) && create_swapchain()) return drop_present();
        if (!s.swapchain) break;   // minimized: render without presenting
        r = vkAcquireNextImageKHR(s.dev, s.swapchain, 1000000000ull, cur().acquired, VK_NULL_HANDLE, &idx);
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
    if (show) record_blit(s.sc_images[idx]);
    vkEndCommandBuffer(s.cmd);
    double t_submit = now_ms();
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cmd;
    if (show) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &cur().acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &s.sc_done[idx];
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
        pr = vkQueuePresentKHR(s.queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) s.sc_stale = true;
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
    if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR && pr != VK_ERROR_OUT_OF_DATE_KHR) {
        fail("vkQueuePresentKHR failed (%d)", pr);
        return drop_present();
    }
    return 0;
}

void renderer_last_timing(RendererTiming *t) { *t = s.timing; }

}  // extern "C"
