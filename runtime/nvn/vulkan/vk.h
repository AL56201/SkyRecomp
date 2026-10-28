// Vulkan backend for the NVN layer (Windows; macOS uses runtime/nvn/metal.m + renderer.m).
// Files: device.c (instance, window, swapchain, present, screenshots, ImGui hooks), textures.c (images, uploads,
// views), exec.c (command execution: passes, draws, clears, copies, compute), shaders.c (GLSL -> SPIR-V, programs,
// pipelines). Mirrors the Metal backend's behaviour; differences are noted where they happen.
//
// Synchronisation model: every image lives in VK_IMAGE_LAYOUT_GENERAL, and a full memory barrier separates
// render passes, copies and dispatches (Metal tracks these hazards itself). Submissions are numbered; a timeline
// semaphore signals each number, which is what NVN fences (renderer_fence_seq / renderer_wait_seq) wait for.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#if __has_include(<vma/vk_mem_alloc.h>)
#include <vma/vk_mem_alloc.h>
#else
#include <vk_mem_alloc.h>  // (Debian / Ubuntu libvulkan-memory-allocator-dev)
#endif

#include "../nvn.h"
#include "../maxwell/shader.h"

void vk_device_fault_report(void);
void vk_memory_report(void);
#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { if (r_ == VK_ERROR_DEVICE_LOST) vk_device_fault_report(); \
    if (r_ == VK_ERROR_OUT_OF_DEVICE_MEMORY || r_ == VK_ERROR_OUT_OF_HOST_MEMORY) vk_memory_report(); \
    FATAL("%s failed: %d (%s:%d)\n", #x, r_, __FILE__, __LINE__); } } while (0)

typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    VmaAllocator vma;
    VkPhysicalDeviceProperties props;
    VkPipelineCache pcache;
    VkSemaphore timeline;      // value = last completed submission number
    VkCommandPool pool;        // render thread only
    bool custom_border, divisor, host_import, logic_op;
    // VR fixed foveated shading (VK_KHR_fragment_shading_rate, attachment rates): texel size of the rate image and
    // the coarsest rates the device supports (exec.c vrs_*)
    bool vrs;
    bool depth_bounds;
    bool multiview;  // (VR: VK_KHR_multiview, both eyes rendered by one pass)
    // GPU markers for profilers (Radeon GPU Profiler, RenderDoc): SKYRECOMP_GPU_MARKERS=1 (VK_EXT_debug_utils labels)
    PFN_vkCmdBeginDebugUtilsLabelEXT label_begin;
    PFN_vkCmdEndDebugUtilsLabelEXT label_end;  // (the depth bounds test: VR hidden-area rejection, exec.c begin_pass)
    uint32_t vrs_texel_w, vrs_texel_h;
    uint16_t vrs_rates;  // bit (log2 w) * 3 + log2 h of each supported rate up to 4x4
    // window
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat sc_format;
    VkExtent2D sc_extent;
    uint32_t sc_count;
    VkImage sc_images[8];
    VkImageView sc_views[8];
} VkG;
extern VkG g_vk;

// ---- submissions (exec.c)
// Work is recorded into the current command buffer; uploads go into a separate one that runs first.
VkCommandBuffer vk_cmd(void);         // current command buffer (outside any rendering unless exec.c began one)
VkCommandBuffer vk_upload_cmd(void);  // upload command buffer of the pending submission
uint64_t vk_pending_seq(void);        // number the pending submission will get
uint64_t vk_completed_seq(void);
void vk_wait_seq(uint64_t seq);
void vk_full_barrier(VkCommandBuffer cb);
void vk_label_begin(VkCommandBuffer cb, const char *name);  // profiler markers (SKYRECOMP_GPU_MARKERS=1)
void vk_label_end(VkCommandBuffer cb);
void vk_end_pass(void);
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra);  // submit + one more cb (present)
void vk_vr_gpu_frame_end(void);  // mark the most recently submitted VR work as the end of one measured frame
// VkPipelineCache is externally synchronized by Vulkan. All pipeline creation
// and cache serialization must go through these wrappers because shader builds
// may run concurrently on worker threads.
VkResult vk_create_graphics_pipelines(uint32_t count, const VkGraphicsPipelineCreateInfo *info, VkPipeline *pipelines);
VkResult vk_create_compute_pipelines(uint32_t count, const VkComputePipelineCreateInfo *info, VkPipeline *pipelines);

// Transient GPU-visible memory that lives until the submission it was allocated for completes.
typedef struct { VkBuffer buf; uint64_t off; void *ptr; } VkSpan;
VkSpan vk_transient(uint64_t size, uint64_t align);
VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align);
VkSpan vk_readback(uint64_t size);  // host-cached GPU -> CPU staging (read right after the copy completes)
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout);

// ---- textures (textures.c)
typedef struct TexBackend {
    VkImage image;
    VmaAllocation mem;
    VkFormat format;
    VkImageViewType base_type;
    uint32_t width, height, depth, layers, levels, samples;
    VkImageAspectFlags aspect;
    bool gpu_owned, uploaded, dynamic, custom, custom_enabled;
    const void *custom_wait;  // (textures.c) its pack replacement, being decoded in the background (custom_tex.c)
    uint64_t data_hash, sample_hash, checked_frame, full_frame;
    uint32_t wgen;  // page write tracking generation at the last upload check (textures.c)
    uint32_t unchanged_frames;
    bool scalable, presented;
    uint64_t touched_frame, frame_cleared;
    uint32_t level_gen;
    float sx, sy;
    void *pending_rgba;  // host-produced pixels (video) waiting to be uploaded on the render thread
    uint32_t pending_w, pending_h, pending_stride;
    uint8_t astc_bw, astc_bh;  // ASTC without GPU support: decoded to RGBA8 on upload (runtime/nvn/astc.c)
    bool astc_srgb;
    int tp;              // runtime/nvn/texpatch.c: 0 not checked yet, -1 none, else known UI texture + 1
    uint32_t tp_gen;     // texpatch_generation() it was last uploaded with
    int reg;  // slot in textures.c's live list + 1 (a reboot destroys them all)
    struct TexBackend *vr_eye;  // VR eye 1's own image of a GPU-written texture (textures.c), NULL until then
    // VR: eye-sized render targets hold both eyes as two layers of one image (multiview). layer0: the first image
    // layer this backend owns (1 for the eye-1 backend); layer_alias: the image belongs to the base texture;
    // eye_layer: this base image has the spare layer 1 for its eye.
    uint32_t layer0;
    bool layer_alias, eye_layer;
} TexBackend;

// The backend image (uploads CPU contents unless for_render). While VR eye 1 is recording, a texture the GPU
// writes resolves to its eye-1 image.
TexBackend *vk_tex(NvnTexture *t, bool for_render);
void vk_vr_set_eye(uint32_t eye);  // 0 or 1: which eye exec.c is recording
void vk_vr_set_stereo(bool on);    // a stereo list is being recorded: GPU-written textures get their eye-1 image
uint32_t vk_vr_eye(void);
VkImage vk_vr_eye_image(NvnTexture *t, uint32_t eye);  // t's image for an eye (eye 1: NULL until rendered)
void vk_vr_list_begin(void);        // begin conservative dependency tracking for one stereo command list
bool vk_vr_list_shareable(void);    // true when eye 0 used no existing eye-specific rendered input
void vk_vr_list_share_outputs(void);  // copy eye-0 outputs to eye 1 instead of replaying an independent list
VkFormat vk_format(uint32_t nvn_format);
bool vk_format_alpha_one(uint32_t nvn_format);
// View of t (optionally through an NVN texture view) for rendering or sampling.
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype);
VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect);
VkImageView vk_dummy_view(int textype);
VkSampler vk_sampler(NvnSamplerDesc *d);
VkSampler vk_default_sampler(bool linear);
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect);
// Destruction once every submission that may use the object has completed.
void vk_defer_image(VkImage img, VmaAllocation mem);
void vk_defer_view(VkImageView v);
void vk_views_forget(TexBackend *b);  // drop (deferred) the cached views of an image being replaced
uint64_t vk_frame_counter(void);

// ---- shaders / pipelines (shaders.c)
VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name);  // NULL on failure (logged)
VkShaderModule vk_builtin_shader(const char *name);  // internal shaders (clear, copy, present)

// ---- device / window (device.c)
void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
