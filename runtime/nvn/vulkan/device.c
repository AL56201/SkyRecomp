// Vulkan device, window, swapchain and presentation (counterpart of the window/present half of metal.m).
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "../../paths.h"
#include "../../settings.h"
#include "../../vr/vr.h"
#include "../../vr/vr_connect.h"
#include "../../vr/vr_settings.h"
#include "../../ui/menu.h"
#include "vk.h"
#include "../output_scale.h"
#include "../renderer.h"

VkG g_vk;
static SDL_Window *window;
static atomic_bool ui_ready;
static VkDebugUtilsMessengerEXT messenger;
static pthread_mutex_t pipeline_cache_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { uint32_t magic, version; uint64_t blob_size; } SkyPipelineCacheHeader;
enum { SKY_PIPELINE_CACHE_MAGIC = 0x43504B53u, SKY_PIPELINE_CACHE_VERSION = 2 };

void vk_memory_report(void) {
#ifdef _WIN32
    MEMORYSTATUSEX m = {.dwLength = sizeof m};
    if (GlobalMemoryStatusEx(&m)) LOG("[vk] memory: Windows commit available %llu MB, physical available %llu MB\n",
        (unsigned long long)(m.ullAvailPageFile >> 20), (unsigned long long)(m.ullAvailPhys >> 20));
#endif
    if (!g_vk.vma) return;
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &props);
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(g_vk.vma, budgets);
    for (uint32_t i = 0; i < props.memoryHeapCount; i++) LOG("[vk] heap %u: app allocated %llu MB, usage %llu MB, budget %llu MB\n", i,
        (unsigned long long)(budgets[i].statistics.allocationBytes >> 20),
        (unsigned long long)(budgets[i].usage >> 20), (unsigned long long)(budgets[i].budget >> 20));
}

// ================================================================== instance / device
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    static int n;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && n++ < 500) LOG("[vk] %s\n", d->pMessage);
    return VK_FALSE;
}

// LARECOMP_VK_VALIDATION=1: Khronos validation layer (MSYS2 package vulkan-validation-layers)
static void create_instance(void) {
    Uint32 n_sdl = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n_sdl);
    const char *ext[16];
    uint32_t n_ext = 0;
    for (Uint32 i = 0; i < n_sdl && n_ext < 14; i++) ext[n_ext++] = sdl_ext[i];
    {
        // SteamVR's OpenXR Vulkan bridge obtains the HMD GPU through these KHR entry points even though the
        // underlying queries are core in modern Vulkan. Keep them available for enabling VR after Play.
        ext[n_ext++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
        ext[n_ext++] = VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME;
        ext[n_ext++] = VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME;
    }
    bool validate = getenv("LARECOMP_VK_VALIDATION") != NULL;
    const bool markers = getenv("SKYRECOMP_GPU_MARKERS") && atoi(getenv("SKYRECOMP_GPU_MARKERS"));  // (profiler labels)
    if (validate || markers) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "SkyRecomp",
                             .pEngineName = "SkyRecomp", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    VkResult r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    if (r != VK_SUCCESS && validate) {
        LOG("[vk] validation layer unavailable (%d); continuing without\n", r);
        ci.enabledLayerCount = 0;
        if (!markers) ci.enabledExtensionCount--;  // (debug utils stay for the profiler markers)
        validate = false;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
    if (r != VK_SUCCESS && markers && !validate) {  // (no debug utils: no labels)
        ci.enabledExtensionCount--;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    } else if (r == VK_SUCCESS && (markers || validate)) {
        g_vk.label_begin = (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCmdBeginDebugUtilsLabelEXT");
        g_vk.label_end = (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCmdEndDebugUtilsLabelEXT");
        if (!markers) g_vk.label_begin = NULL, g_vk.label_end = NULL;  // (validation alone: no labels)
        if (g_vk.label_begin) LOG("[vk] GPU markers on (VK_EXT_debug_utils labels for profilers)\n");
    }
    if (r != VK_SUCCESS) FATAL("vkCreateInstance: %d (is a Vulkan 1.3 driver installed?)\n", r);
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        if (f) f(g_vk.inst, &mi, NULL, &messenger);
    }
}

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

// Discrete GPU first (LARECOMP_GPU=<index> picks one), needs Vulkan 1.3 and a queue that can present.
static void pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) FATAL("no Vulkan device\n");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    int want = getenv("LARECOMP_GPU") ? atoi(getenv("LARECOMP_GPU")) : -1, best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        LOG("[vk] gpu %u: %s (api %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if ((int)i == want) score = 100;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0) FATAL("no Vulkan 1.3 device\n");
    g_vk.phys = pd[best];
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) { g_vk.qfam = i; break; }
    }
    free(qp);
    if (g_vk.qfam == UINT32_MAX) FATAL("no graphics+present queue\n");
    LOG("[vk] using %s\n", g_vk.props.deviceName);
    extern void perfmon_set_gpu_hint(const char *n);
    perfmon_set_gpu_hint(g_vk.props.deviceName);  // (runtime/perfmon.c: the overlay's GPU, NVML's pick)
}

static bool g_fault_ext;
// After VK_ERROR_DEVICE_LOST: what the driver says faulted (VK_EXT_device_fault), into the log.
void vk_device_fault_report(void) {
    static int once;
    if (once++ || !g_fault_ext) return;
    PFN_vkGetDeviceFaultInfoEXT get = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetDeviceFaultInfoEXT");
    if (!get) return;
    VkDeviceFaultCountsEXT c = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (get(g_vk.dev, &c, NULL) != VK_SUCCESS) { LOG("[vk] device fault: no info\n"); return; }
    VkDeviceFaultAddressInfoEXT addr[16];
    VkDeviceFaultVendorInfoEXT vend[16];
    if (c.addressInfoCount > 16) c.addressInfoCount = 16;
    if (c.vendorInfoCount > 16) c.vendorInfoCount = 16;
    c.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = vend};
    if (get(g_vk.dev, &c, &info) != VK_SUCCESS && 0) return;
    LOG("[vk] device fault: %s\n", info.description);
    static const char *const kind[] = {"none", "read invalid", "write invalid", "execute invalid", "ip unknown", "ip invalid", "ip valid"};
    for (uint32_t i = 0; i < c.addressInfoCount; i++)
        LOG("[vk]   address %s %#llx (precision %#llx)\n", addr[i].addressType < 7 ? kind[addr[i].addressType] : "?",
            (unsigned long long)addr[i].reportedAddress, (unsigned long long)addr[i].addressPrecision);
    for (uint32_t i = 0; i < c.vendorInfoCount; i++)
        LOG("[vk]   vendor: %s (code %#llx data %#llx)\n", vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
            (unsigned long long)vend[i].vendorFaultData);
}

void vk_pipeline_cache_save(void);
static void vk_pipeline_cache_save_(void);

// Saves the pipeline cache every minute while it grows, not only at exit, so a crash or a killed process keeps the
// session's pipelines (arg: the size loaded at startup)
static int pcache_saver(void *arg) {
    size_t last = *(size_t *)arg;
    for (;;) {
        SDL_Delay(60000);
        size_t sz = 0;
        if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) == VK_SUCCESS && sz > last) {
            vk_pipeline_cache_save();
            last = sz;
        }
    }
    return 0;
}

static void create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    const char *ext[16];
    uint32_t n_ext = 0;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    g_vk.custom_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (g_vk.custom_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    g_vk.divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    if (g_vk.divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    g_vk.host_import = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);  // guest heap as a GPU buffer (exec.c)
    if (g_vk.host_import) ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    bool fault = has_ext(ep, ne, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);  // device-lost diagnostics (vk_device_fault_report)
    if (fault) ext[n_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    {
        // Keep the optional OpenXR sharing entry points available even for a desktop launch, so VR can be enabled
        // later from the menu without recreating the Vulkan device. SteamVR loads the KHR entry points by name even
        // where Vulkan has promoted the operations to core.
        const char *xr_ext[] = {VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
                                VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
                                VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
#ifdef _WIN32
                                "VK_KHR_external_memory_win32",
#endif
                                VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
#ifdef _WIN32
                                "VK_KHR_external_semaphore_win32",
#endif
        };
        for (uint32_t i = 0; i < sizeof xr_ext / sizeof *xr_ext; i++) {
            if (has_ext(ep, ne, xr_ext[i])) ext[n_ext++] = xr_ext[i];
            else if (vr_requested()) LOG("[vr] Vulkan device extension unavailable: %s\n", xr_ext[i]);
        }
    }
    // VR: fixed foveated shading (exec.c) - coarser shading toward the lenses' blurry edges, by a rate image
    // Off by default (VR Options > Foveated Rendering, live; SKYRECOMP_VR_FOVEATION=on / <inner>,<outer> / 0
    // overrides): measured in Skyloft it saved ~0.4 ms of ~10 at 1.5x - the game's eye passes are bound by geometry
    // and bandwidth, not pixel shading. The extension is on with VR so the setting can change while playing.
    // Enable the capability whenever the device supports it, even if VR or
    // foveation starts disabled. Both VR Options switches can then turn it on
    // without recreating the Vulkan device.
    const bool vrs_ext = has_ext(ep, ne, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR vrs_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    if (vrs_ext) {
        VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &vrs_have};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &f2);
        VkPhysicalDeviceFragmentShadingRatePropertiesKHR pr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pr};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR rates =
            (PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR)vkGetInstanceProcAddr(g_vk.inst, "vkGetPhysicalDeviceFragmentShadingRatesKHR");
        VkPhysicalDeviceFragmentShadingRateKHR r[16];
        uint32_t nr = 16;
        for (uint32_t i = 0; i < 16; i++) r[i] = (VkPhysicalDeviceFragmentShadingRateKHR){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR};
        if (rates && rates(g_vk.phys, &nr, r) >= 0) {
            if (nr > 16) nr = 16;
            for (uint32_t i = 0; i < nr; i++) {
                const uint32_t w = r[i].fragmentSize.width, h = r[i].fragmentSize.height;
                if (w <= 4 && h <= 4 && (r[i].sampleCounts & VK_SAMPLE_COUNT_1_BIT))
                    g_vk.vrs_rates |= (uint16_t)(1u << ((w == 4 ? 2 : w == 2 ? 1 : 0) * 3 + (h == 4 ? 2 : h == 2 ? 1 : 0)));
            }
        }
        g_vk.vrs_texel_w = pr.maxFragmentShadingRateAttachmentTexelSize.width;
        g_vk.vrs_texel_h = pr.maxFragmentShadingRateAttachmentTexelSize.height;
        g_vk.vrs = vrs_have.attachmentFragmentShadingRate && g_vk.vrs_texel_w && g_vk.vrs_texel_h;
        LOG("[vr] foveated shading: %s (attachment rates %d, texel %ux%u..%ux%u, rates mask %#x)\n", g_vk.vrs ? "on" : "unavailable",
            (int)vrs_have.attachmentFragmentShadingRate, pr.minFragmentShadingRateAttachmentTexelSize.width,
            pr.minFragmentShadingRateAttachmentTexelSize.height, g_vk.vrs_texel_w, g_vk.vrs_texel_h, g_vk.vrs_rates);
        if (g_vk.vrs) ext[n_ext++] = VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME;
    }
    g_fault_ext = fault;
    free(ep);

    // What the device has of the features below. Asking for one it lacks fails vkCreateDevice with
    // VK_ERROR_FEATURE_NOT_PRESENT (-8: Intel Iris Xe / UHD, Adreno X1-85; #46, #59, #60), so the optional ones are
    // asked for only where present; dynamicRendering, synchronization2 and timelineSemaphore are needed.
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT hdiv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT hcbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .pNext = g_vk.divisor ? &hdiv : NULL};
    VkPhysicalDeviceVulkan13Features h13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .pNext = g_vk.custom_border ? (void *)&hcbc : g_vk.divisor ? (void *)&hdiv : NULL};
    VkPhysicalDeviceVulkan12Features h12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &h13};
    VkPhysicalDeviceVulkan11Features h11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &h12};
    VkPhysicalDeviceFaultFeaturesEXT hff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .pNext = &h11};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = fault ? (void *)&hff : (void *)&h11};
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
    g_vk.divisor = g_vk.divisor && hdiv.vertexAttributeInstanceRateDivisor;
    g_vk.custom_border = g_vk.custom_border && hcbc.customBorderColors && hcbc.customBorderColorWithoutFormat;
    g_vk.mirror_clamp = h12.samplerMirrorClampToEdge;  // (else mirrored repeat: textures.c)
    if (!h13.dynamicRendering || !h13.synchronization2 || !h12.timelineSemaphore)
        LOG("[vk] warning: no dynamicRendering / synchronization2 / timelineSemaphore (Vulkan 1.3): this GPU can't run it\n");
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        .vertexAttributeInstanceRateDivisor = VK_TRUE, .vertexAttributeInstanceRateZeroDivisor = VK_FALSE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = g_vk.divisor ? &div : NULL};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE,
                                            .shaderDemoteToHelperInvocation = h13.shaderDemoteToHelperInvocation,
                                            .shaderTerminateInvocation = h13.shaderTerminateInvocation,
                                            .maintenance4 = h13.maintenance4,
                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = VK_TRUE, .shaderInt8 = h12.shaderInt8, .samplerMirrorClampToEdge = h12.samplerMirrorClampToEdge,
                                            .uniformAndStorageBuffer8BitAccess = h12.uniformAndStorageBuffer8BitAccess, .storageBuffer8BitAccess = h12.storageBuffer8BitAccess,
                                            .scalarBlockLayout = h12.scalarBlockLayout, .pNext = &f13};
    g_vk.multiview = h11.multiview;  // (VR: both eyes in one pass, exec.c)
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = h11.storageBuffer16BitAccess, .multiview = g_vk.multiview, .pNext = &f12};
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR vrs_on = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
                                                              .attachmentFragmentShadingRate = VK_TRUE, .pNext = &f11};
    void *chain = g_vk.vrs ? (void *)&vrs_on : (void *)&f11;
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = hff.deviceFault,
                                           .pNext = chain};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = fault ? (void *)&ff : chain};
    VkPhysicalDeviceFeatures *b = &f.features, *h = &have.features;
    b->independentBlend = h->independentBlend;
    b->dualSrcBlend = h->dualSrcBlend;
    g_vk.logic_op = b->logicOp = h->logicOp;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    b->textureCompressionBC = h->textureCompressionBC;
    b->shaderInt64 = h->shaderInt64;
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->sampleRateShading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    b->shaderClipDistance = h->shaderClipDistance;
    g_vk.depth_bounds = b->depthBounds = h->depthBounds;  // (VR hidden area: exec.c)
    if (!h->shaderInt64) LOG("[vk] warning: no shaderInt64 (64-bit address math in shaders)\n");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);

    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.dev, .instance = g_vk.inst,
                                 .vulkanApiVersion = VK_API_VERSION_1_3};
    VK_CHECK(vmaCreateAllocator(&ai, &g_vk.vma));

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.timeline));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool));

    // pipeline cache: cache/vk_pipelines.bin (written by vk_pipeline_cache_save).
    // Our header invalidates driver blobs when renderer pipeline semantics
    // change even if the driver's UUID remains the same.
    size_t sz = 0;
    void *data = NULL;
    FILE *fp = fopen("cache/vk_pipelines.bin", "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long end = ftell(fp);
        sz = end > 0 && end <= 64 * 1024 * 1024 ? (size_t)end : 0;
        fseek(fp, 0, SEEK_SET);
        data = sz ? malloc(sz) : NULL;
        if (!data || fread(data, 1, sz, fp) != sz) sz = 0;
        fclose(fp);
        // Reject stale/corrupt blobs before passing them to the driver. The
        // Vulkan header identifies the exact vendor, device and driver UUID.
        if (sz < sizeof(SkyPipelineCacheHeader) + sizeof(VkPipelineCacheHeaderVersionOne)) sz = 0;
        if (sz) {
            const SkyPipelineCacheHeader *app = data;
            const uint8_t *blob = (const uint8_t *)data + sizeof *app;
            const size_t blob_size = sz - sizeof *app;
            const VkPipelineCacheHeaderVersionOne *h = (const VkPipelineCacheHeaderVersionOne *)blob;
            if (app->magic != SKY_PIPELINE_CACHE_MAGIC || app->version != SKY_PIPELINE_CACHE_VERSION ||
                app->blob_size != blob_size || h->headerSize < sizeof *h || h->headerSize > blob_size ||
                h->headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
                h->vendorID != g_vk.props.vendorID || h->deviceID != g_vk.props.deviceID ||
                memcmp(h->pipelineCacheUUID, g_vk.props.pipelineCacheUUID, VK_UUID_SIZE)) sz = 0;
            else { memmove(data, blob, blob_size); sz = blob_size; }
        }
    }
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz,
                                    .pInitialData = data};
    if (vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache) != VK_SUCCESS) {
        sz = 0;
        pc.initialDataSize = 0;
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache));
    }
    LOG("[vk] pipeline cache: %s (%zu bytes)\n", sz ? "loaded" : "new", sz);
    free(data);
    static size_t loaded;
    loaded = sz;
    SDL_DetachThread(SDL_CreateThread(pcache_saver, "pcache", &loaded));
}

void vk_pipeline_cache_save(void) {
    pthread_mutex_lock(&pipeline_cache_lock);
    size_t sz = 0;
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) != VK_SUCCESS || !sz) {
        pthread_mutex_unlock(&pipeline_cache_lock);
        return;
    }
    void *data = malloc(sz);
    if (!data) {
        pthread_mutex_unlock(&pipeline_cache_lock);
        return;
    }
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, data) == VK_SUCCESS) {
        mkdir("cache", 0755);
        FILE *f = fopen("cache/vk_pipelines.bin.tmp", "wb");
        if (f) {
            const SkyPipelineCacheHeader h = {SKY_PIPELINE_CACHE_MAGIC, SKY_PIPELINE_CACHE_VERSION, sz};
            bool ok = fwrite(&h, 1, sizeof h, f) == sizeof h && fwrite(data, 1, sz, f) == sz;
            fclose(f);
            if (ok) { remove("cache/vk_pipelines.bin"); rename("cache/vk_pipelines.bin.tmp", "cache/vk_pipelines.bin"); }
        }
    }
    free(data);
    pthread_mutex_unlock(&pipeline_cache_lock);
}

VkResult vk_create_graphics_pipelines(uint32_t count, const VkGraphicsPipelineCreateInfo *info, VkPipeline *pipelines) {
    pthread_mutex_lock(&pipeline_cache_lock);
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, count, info, NULL, pipelines);
    pthread_mutex_unlock(&pipeline_cache_lock);
    return r;
}

VkResult vk_create_compute_pipelines(uint32_t count, const VkComputePipelineCreateInfo *info, VkPipeline *pipelines) {
    pthread_mutex_lock(&pipeline_cache_lock);
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, count, info, NULL, pipelines);
    pthread_mutex_unlock(&pipeline_cache_lock);
    return r;
}

// ================================================================== swapchain
static VkPresentModeKHR present_mode(void) {
    // MAILBOX by default: the game paces itself (vi.c's emulated 60 Hz vsync), and with FIFO every submission
    // queued behind the one waiting for a window image, so the game's fence waits followed the presentation engine:
    // a throttled / captured / busy compositor slowed the whole game (4-21 fps with the GPU mostly idle; mailbox:
    // 60). No tearing. LARECOMP_VK_PRESENT=fifo|immediate|mailbox to choose.
    const char *e = getenv("LARECOMP_VK_PRESENT");
    VkPresentModeKHR want = !e || !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) return want;
    // VR: the window is only the desktop mirror and must never wait for the monitor. With FIFO (AMD on Windows has
    // no MAILBOX) a 60 Hz monitor held the headset's frames to 60 Hz once its queue filled up, so SteamVR saw late
    // frames and throttled the headset between 72 and 48 Hz (~57 fps); IMMEDIATE (tearing on the mirror only):
    // a steady 72 on a 144 Hz Index.
    if (vr_requested() && !e)
        for (uint32_t i = 0; i < n; i++) if (m[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) return VK_PRESENT_MODE_IMMEDIATE_KHR;
    return VK_PRESENT_MODE_FIFO_KHR;
}

static void create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps));
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
    if (!ext.width || !ext.height) return;  // minimised
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    // scanout shows the bytes the game wrote (see the raw views in the present pass): a UNORM swapchain like Metal's
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    const VkPresentModeKHR mode = present_mode();
    // FIFO/IMMEDIATE use the smallest practical two-image chain to minimize
    // queued latency. MAILBOX needs one additional image so rendering does not
    // stall while the compositor owns the displayed image.
    uint32_t count = mode == VK_PRESENT_MODE_MAILBOX_KHR ? caps.minImageCount + 1 : 2;
    if (count < caps.minImageCount) count = caps.minImageCount;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    VkSwapchainKHR old = g_vk.swapchain;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        // Presentation renders directly into the image; transfer source is
        // needed only for screenshots. Nothing copies into a swapchain image.
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = mode, .clipped = VK_TRUE,
        .oldSwapchain = old};
    VkSwapchainKHR next = VK_NULL_HANDLE;
    VkResult result = vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &next);
    // A fullscreen transition can invalidate the surface between querying it and creating the swapchain.
    // Passing oldSwapchain retires it even when creation fails, so retry with a fresh chain next frame.
    if (result != VK_SUCCESS && result != VK_ERROR_OUT_OF_DATE_KHR && result != VK_ERROR_INITIALIZATION_FAILED)
        VK_CHECK(result);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    g_vk.swapchain = result == VK_SUCCESS ? next : VK_NULL_HANDLE;
    g_vk.sc_count = 0;
    if (result != VK_SUCCESS) {
        LOG("[vk] surface changing (%d); retrying swapchain next frame\n", result);
        return;
    }
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
    g_vk.sc_count = 8;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);
    LOG("[vk] present mode %s, %u swapchain images\n",
        mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo",
        g_vk.sc_count);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
    }
}

static void recreate_swapchain(void) {
    vkDeviceWaitIdle(g_vk.dev);
    create_swapchain();
}

// ================================================================== frames (start screen and game)
// Acquire semaphores rotate per frame in flight; a slot is reused once the submission that waited on it is done.
#define FRAMES 3
static VkSemaphore acquire_sem[FRAMES], done_sem[8];
static uint64_t slot_seq[FRAMES];
static int slot;

static VkSemaphore binary_semaphore(void) {
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

typedef struct { uint32_t image; VkSemaphore acquired; bool ok; } Frame;
static Frame cur_frame;

// Acquire the next window image and begin rendering into it (cleared to `clear`). Returns false when there is
// nothing to draw into (minimised window).
// nowait (the VR desktop mirror): never block the caller - not on the GPU finishing this slot's earlier frame, not
// on the window having an image free (Windows can hold one back, e.g. while a capture or streaming app reads the
// window). The mirror then skips this refresh; the headset frame goes on at its own rate.
static bool frame_begin(const float clear[4], bool nowait) {
    cur_frame.ok = false;
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (!w || !h) return false;  // minimised
    if (!g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height) recreate_swapchain();
    if (!g_vk.swapchain) return false;
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] frame begin %dx%d (swapchain %ux%u)\n", w, h, g_vk.sc_extent.width, g_vk.sc_extent.height);
    if (nowait && slot_seq[(slot + 1) % FRAMES] && vk_completed_seq() < slot_seq[(slot + 1) % FRAMES]) return false;
    slot = (slot + 1) % FRAMES;
    if (!acquire_sem[slot]) acquire_sem[slot] = binary_semaphore();
    vk_wait_seq(slot_seq[slot]);
    VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, nowait ? 0 : UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    if (nowait && (r == VK_NOT_READY || r == VK_TIMEOUT)) return false;  // (no image free now: the semaphore is untouched)
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        if (!g_vk.swapchain) return false;
        r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LOG("[vk] acquire failed: %d\n", r); return false; }
    cur_frame.acquired = acquire_sem[slot];
    slot_seq[slot] = vk_pending_seq();
    VkCommandBuffer cb = vk_cmd();
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue.color.float32 = {clear[0], clear[1], clear[2], clear[3]}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    cur_frame.ok = true;
    return true;
}
bool vk_frame_begin(const float clear[4]) { return frame_begin(clear, false); }
static bool vk_frame_begin_nowait(const float clear[4]) { return frame_begin(clear, true); }

// Passes of their own between vk_frame_begin and vk_frame_end (exec.c post passes): end the window image's
// rendering, and begin it again keeping what was drawn.
void vk_frame_suspend(void) {
    if (cur_frame.ok) vkCmdEndRendering(vk_cmd());
}
void vk_frame_resume(void) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
}

VkExtent2D vk_frame_extent(void) { return g_vk.sc_extent; }
void *vk_frame_cmd(void) { return (void *)vk_cmd(); }

static void save_window_image(VkCommandBuffer cb, VkSpan *out) {
    uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
    *out = vk_readback((uint64_t)w * h * 4);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rg = {.bufferOffset = out->off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->buf, 1, &rg);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

// End rendering into the window image and present it. shot_path: also save the window image as PNG.
void vk_frame_end(const char *shot_path) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    vkCmdEndRendering(cb);
    VkSpan shot = {0};
    if (shot_path) save_window_image(cb, &shot);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    uint32_t img = cur_frame.image;
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    uint64_t seq = vk_pending_seq();
    vk_submit_extra(cur_frame.acquired, done_sem[img], VK_NULL_HANDLE);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done_sem[img],
                           .swapchainCount = 1, .pSwapchains = &g_vk.swapchain, .pImageIndices = &img};
    VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) recreate_swapchain();
    cur_frame.ok = false;
    if (shot_path) {
        vk_wait_seq(seq);
        uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
        uint8_t *px = shot.ptr;
        bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        for (uint64_t i = 0; bgr && i < (uint64_t)w * h; i++) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) px[i * 4 + 3] = 255;
        vk_save_png(shot_path, px, w, h);
    }
}

void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)rgba, (int)w * 4);
    if (!s || !SDL_SavePNG(s, path)) LOG("[vk] saving %s failed: %s\n", path, SDL_GetError());
    else LOG("[vk] saved %s (%ux%u)\n", path, w, h);
    SDL_DestroySurface(s);
}

// ================================================================== game presentation (metal.m backend_present)
// Draws the game's image into the window with the widescreen crop, then the overlay menu.
extern void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]);
void renderer_submit(void);
void renderer_set_scale(float sx, float sy);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);

static uint64_t frames, total_frames, g_frame_counter;
uint64_t renderer_present_count(void) { return total_frames; }
uint64_t vk_frame_counter(void) { return g_frame_counter; }
static float g_shot_crop[4] = {0, 0, 1, 1};
static double last_fps_time;
static NvnTexture *g_last_presented;
static atomic_int g_vr_enabled_request = -1;

// Called by the settings UI, which may be on the game thread while asynchronous replay presents on another one.
// The transition is performed only after a complete frame has been submitted and presented.
void vk_vr_request_enabled(bool enabled) { atomic_store(&g_vr_enabled_request, enabled ? 1 : 0); }

static void vr_apply_enabled_request(void) {
    static VrConnectState connect;
    static uint64_t next_poll;
    const int requested = atomic_exchange(&g_vr_enabled_request, -1);
    const uint64_t now = SDL_GetTicks();
    if (!g_vk.dev) return;
    if (requested >= 0) { connect = (VrConnectState){0}; next_poll = 0; }
    const bool enabled = vr_game_active() && vr_requested();
    if (enabled && now < next_poll) return;
    next_poll = now + 1000;
    const bool ready = enabled && vr_runtime_ready();
    unsigned action = vr_connect_tick(&connect, now, enabled, vr_available(), ready);
    if (action & VR_CONNECT_START) vr_runtime_start();
    if (action & VR_CONNECT_STOP) {
        vkDeviceWaitIdle(g_vk.dev);
        vr_shutdown();
        LOG("[vr] continuing in desktop mode\n");
    }
    if (action & VR_CONNECT_TRY) {
        vkDeviceWaitIdle(g_vk.dev);
        if (!vr_vulkan_init((void *)g_vk.inst, (void *)g_vk.phys, (void *)g_vk.dev, g_vk.qfam, 0))
            LOG("[vr] not ready yet; desktop play continues, retrying later\n");
    }
}

const float *vk_shot_crop(void) { return g_shot_crop; }

static int g_window_crop[4];
void backend_set_window_crop(const int crop[4]) {
    if (crop) memcpy(g_window_crop, crop, sizeof g_window_crop);
}

void backend_present(NvnTexture *t);
static bool g_representing;
static bool g_cinema;
static bool g_black_backdrop;
static bool g_blank;      // this frame is a full-screen colour fade: show it in the eyes, not on the panel
static bool g_had_scene;
static bool g_fade;  // (this frame drew a full-screen 2D quad - a fade: in the eyes; the panel never opaque)  // this frame drew 3D: never put a full-screen copy on the panel
static bool g_had_hud;    // this frame routed actual 2D-layer draws into the HUD texture
static float g_vr_crop[4] = {0, 0, 1, 1};  // the eyes' part of the game image; the mirror shows the same crop
void backend_represent(void) {
    extern bool nvn_async_on(void), nvn_async_is_replay_thread(void);
    extern void nvn_async_represent(void);
    if (nvn_async_on() && !nvn_async_is_replay_thread()) { nvn_async_represent(); return; }
    g_representing = true;  // (VR: the frozen world - the last 3D frame stays put in the world, the head moves freely)
    if (g_last_presented) backend_present(g_last_presented);
    else SDL_Delay(16);
    g_representing = false;
}

// Each presented frame, the hooks that act on the game (scripted input, warps, tests, the save editor): on the
// game thread - with the asynchronous replay (nvn/async.c) when the present is queued.
void present_game_hooks(uint64_t frame) {
    {   // (test) LARECOMP_FRAME_RATE_AT=<frame>:<60|120|144>: the frame rate setting from that frame on (scripted
        // routes are recorded by frame number at 60 FPS)
        static long at = -2, rate;
        if (at == -2 && (!getenv("LARECOMP_FRAME_RATE_AT") || sscanf(getenv("LARECOMP_FRAME_RATE_AT"), "%ld:%ld", &at, &rate) != 2)) at = -1;
        if (at >= 0 && (long)frame == at) { g_settings.frame_rate = (int)rate; LOG("[test] frame rate %ld from frame %ld\n", rate, at); }
    }
    extern void input_frame(uint64_t frame);
    input_frame(frame);
    {   // (test) SKYRECOMP_KBD_AT=<frame>: the name entry dialog opens at that frame (as the game's swkbd would)
        static long at = -1;
        if (at < 0) at = getenv("SKYRECOMP_KBD_AT") ? atol(getenv("SKYRECOMP_KBD_AT")) : 0;
        if (at && (long)frame == at) {
            extern int menu_keyboard_run(const char *initial, int max_len, char *out, int cap);
            char out[64];
            LOG("[test] name entry -> %d (%s)\n", menu_keyboard_run("Link", 8, out, sizeof out), out);
        }
    }
    static long slow_ms = -1;
    if (slow_ms < 0) slow_ms = getenv("LARECOMP_SLOW_FRAME_MS") ? atol(getenv("LARECOMP_SLOW_FRAME_MS")) : 0;
    static long slow_until = -1;  // LARECOMP_SLOW_FRAME_UNTIL=<frame>: the stall ends there (frame pacing recovery tests)
    if (slow_until < 0) slow_until = getenv("LARECOMP_SLOW_FRAME_UNTIL") ? atol(getenv("LARECOMP_SLOW_FRAME_UNTIL")) : 0;
    if (slow_ms > 0 && frame > 2600 && (!slow_until || (long)frame < slow_until)) SDL_Delay((Uint32)slow_ms);
    extern void fnprof_frame(uint64_t frame);
    fnprof_frame(frame);
    extern void warp_frame(uint64_t frame);
    warp_frame(frame);
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (LARECOMP_REBOOT_AT)
    reboot_test_frame(frame);
    extern void save_editor_frame(uint64_t frame);
    save_editor_frame(frame);
}

static char g_post_shot[64];  // (eye shots) eye 0 after vk_vr_eye_post, saved this frame

// SKYRECOMP_VR_PACE_TRACE=1: the VR present's CPU time by part (ms per frame, every 300 frames)
enum { PT_POLL, PT_HEAD, PT_FREEZE, PT_POST, PT_EYECOPY, PT_HUD, PT_MIRROR_ACQ, PT_MIRROR_DRAW, PT_MENU, PT_MIRROR_END,
       PT_SUBMIT, PT_ENDFRAME, PT_TAIL, PT_N };
static const char *const pt_name[PT_N] = {"vr_poll", "head", "freeze", "eye post", "eye copies (+acquire/wait)",
                                          "HUD quad (+acquire)", "mirror acquire", "mirror draw", "menu",
                                          "mirror submit+present", "submit", "xrEndFrame", "after present"};
static uint64_t pt_ns[PT_N], pt_last, pt_frames;
static inline void pt_mark(int part) {
    const uint64_t now = SDL_GetTicksNS();
    if (pt_last) pt_ns[part] += now - pt_last;
    pt_last = now;
}
static void pt_report(void) {
    static int trace = -1;
    if (trace < 0) trace = getenv("SKYRECOMP_VR_PACE_TRACE") != NULL;
    if (!trace || ++pt_frames < 300) return;
    char line[600];
    int n = snprintf(line, sizeof line, "[vr] present CPU ms/frame:");
    double total = 0;
    for (int i = 0; i < PT_N; i++) {
        total += pt_ns[i] / 1e6 / pt_frames;
        if (pt_ns[i] / 1e6 / pt_frames >= 0.005)
            n += snprintf(line + n, sizeof line - n, " %s %.3f,", pt_name[i], pt_ns[i] / 1e6 / pt_frames);
    }
    LOG("%s total %.3f\n", line, total);
    memset(pt_ns, 0, sizeof pt_ns);
    pt_frames = 0;
}

extern bool menu_on_vr_panel(void);  // (ui/menu.mm)
static bool g_vr_menu_on_panel;
static TexBackend *g_vr_menu_img;  // (this frame: the menu panel's image, for the mirror)  // (this frame: the settings menu was drawn onto the VR HUD panel)
static TexBackend *g_vr_panel_img;  // (this frame: the panel's image - the HUD texture, or it with the menu)

void backend_present(NvnTexture *t) {
    pt_last = SDL_GetTicksNS();
    vr_poll();
    pt_mark(PT_POLL);
    static uint64_t pres_count;
    if (++pres_count % 60 == 1) LOG("[vk] present frame %llu\n", (unsigned long long)pres_count);
    g_last_presented = t;
    extern void vk_hud_note_presented(NvnTexture *t);  // (exec.c: the VR HUD layer's "this is the screen")
    vk_hud_note_presented(t);
    TexBackend *b = vk_tex(t, true);
    b->presented = true;
    vk_end_pass();
    uint64_t t0 = SDL_GetTicksNS();
    if (vr_running()) {
        // Both eyes were recorded list by list (exec.c exec_list_stereo); here they go to the OpenXR swapchain's two
        // array layers, recorded into the same submission as the desktop mirror. Game images stay in
        // VK_IMAGE_LAYOUT_GENERAL throughout (vk.h).
        vr_frame_begin();  // (no-op unless this frame executed no command list)
        pt_mark(PT_HEAD);
        {   // (test) SKYRECOMP_VR_EYE_SHOT=<frame>: both eyes' presented images -> build/vr_eye0.png, build/vr_eye1.png.
            // A list (<frame>,<frame>,...) saves build/vr_eye0_<frame>.png and build/vr_eye1_<frame>.png for each.
            static long at[64];
            static int n = -1;
            static bool list;
            if (n < 0) {
                n = 0;
                const char *e = getenv("SKYRECOMP_VR_EYE_SHOT");
                list = e && strchr(e, ',');
                while (e && *e && n < 64) {
                    at[n++] = strtol(e, (char **)&e, 10);
                    while (*e == ',' || *e == ' ') e++;
                }
            }
            static long every = -1;  // SKYRECOMP_VR_EYE_SHOT_EVERY=<frames>: eye 0 -> build/vr_live_<frame>.png
            if (every < 0) every = getenv("SKYRECOMP_VR_EYE_SHOT_EVERY") ? atol(getenv("SKYRECOMP_VR_EYE_SHOT_EVERY")) : 0;
            if (every > 0 && total_frames % (uint64_t)every == 0) {
                char p[64];
                snprintf(p, sizeof p, "build/vr_live_%06llu.png", (unsigned long long)total_frames);
                renderer_save_texture_tex(b, p);
            }
            for (int i = 0; i < n; i++) {
                if (total_frames != (uint64_t)at[i]) continue;
                char p0[64] = "build/vr_eye0.png", p1[64] = "build/vr_eye1.png";
                if (list) {
                    snprintf(p0, sizeof p0, "build/vr_eye0_%ld.png", at[i]);
                    snprintf(p1, sizeof p1, "build/vr_eye1_%ld.png", at[i]);
                }
                renderer_save_texture_tex(b, p0);
                if (b->vr_eye) renderer_save_texture_tex(b->vr_eye, p1);
                snprintf(g_post_shot, sizeof g_post_shot, list ? "build/vr_post0_%ld.png" : "build/vr_post0.png", at[i]);
                extern TexBackend *vk_hud_backend(void);  // (exec.c: the 2D layer, SKYRECOMP_VR_HUD_LAYER)
                if (vk_hud_backend()) {
                    char ph[64];
                    snprintf(ph, sizeof ph, list ? "build/vr_hud_%ld.png" : "build/vr_hud.png", at[i]);
                    extern void vk_save_texture_raw(TexBackend *b, const char *path);
                    vk_save_texture_raw(vk_hud_backend(), ph);  // (raw bytes: its alpha is the point)
                }
                LOG("[vr] eye shots at frame %ld: eye 1 image %s\n", at[i], b->vr_eye ? "saved" : "missing");
            }
        }
        // Scene targets carry the present generation that produced them.  Consult it before vk_hud_present: the
        // async replay boolean can arrive one present late, which used to copy the void cutscene into the HUD first
        // and then show the same scene both stereoscopically and as a flat floating panel.
        const uint64_t present_gen = renderer_present_count();
        const bool target_had_scene = b->scene_frame && b->scene_frame <= present_gen + 1 &&
                                      b->scene_frame + 1 >= present_gen;
        extern void vk_hud_present(NvnTexture *screen);  // (exec.c: full-screen 2D frames -> the HUD texture)
        if (!target_had_scene) vk_hud_present(t);
        {   // full-screen 2D (the map): the eyes keep the last 3D frame
            extern bool vk_hud_layer_on(void), vk_hud_opaque(void), vk_hud_frame_had_scene(void), vk_hud_frame_had_content(void);
            const bool replay_had_scene = vk_hud_frame_had_scene();  // always consume/reset the async marker
            const bool had_scene = target_had_scene || replay_had_scene;
            g_had_scene = had_scene;
            extern bool vk_hud_frame_had_fullscreen_quad(void);
            g_fade = vk_hud_frame_had_fullscreen_quad();
            g_had_hud = vk_hud_frame_had_content();
            extern bool vk_hud_frame_blank(void);  // (exec.c: no scene, and no 2D but full-screen colour - a fade)
            // A fade may cover a live 3D scene (the Skyview void transition does exactly that).  It still owns the
            // whole view and must suppress the floating panel; scene presence must not cancel the fade.
            g_blank = vk_hud_frame_blank();
            {   // a prerendered cutscene (hle/movie.c) on its own screen: the panel, larger and still, in the dark
                extern bool movie_playing(void);
                extern void vr_cinema(bool on);
                g_cinema = vk_hud_layer_on() && movie_playing() && !had_scene;
                vr_cinema(g_cinema);
                extern bool vk_hud_black_backdrop(NvnTexture *screen), vr_world_menu_active(void);
                extern void vr_black_background(bool on);
                g_black_backdrop = vk_hud_layer_on() && !had_scene && !vr_world_menu_active() && vk_hud_black_backdrop(t);
                vr_black_background(g_black_backdrop);
                if (getenv("SKYRECOMP_VR_FADE_TRACE") && present_gen % 60 == 0)
                    LOG("[vr-backdrop] frame %llu scene %d target %d replay %d menu %d opaque %d black %d selected %d\n",
                        (unsigned long long)present_gen, had_scene, target_had_scene, replay_had_scene,
                        vr_world_menu_active(), vk_hud_opaque(), vk_hud_black_backdrop(t), g_black_backdrop);
            }
            if (g_representing) vr_freeze_world(true, false);  // (the game waiting: its last frame, held in the world)
            else vr_freeze_world(vk_hud_layer_on() && vk_hud_opaque() && !had_scene && !g_blank && !g_fade, had_scene);  // (world mode: live)
        }
        pt_mark(PT_FREEZE);
        TexBackend *mirror_src = b;  // (the desktop mirror shows eye 0 as the headset gets it: no second post chain)
        {   // each eye: anti-aliased and filtered to the headset's eye size (exec.c vk_vr_eye_post), then copied
            extern TexBackend *vk_vr_eye_post(int eye, TexBackend *b, uint32_t ow, uint32_t oh);
            uint32_t ew = 0, eh = 0;
            TexBackend *e1 = vk_vr_eye_image(t, 1) ? b->vr_eye : NULL, *p0 = b, *p1 = e1;
            if (vr_eye_extent(&ew, &eh)) {
                p0 = vk_vr_eye_post(0, b, ew, eh);
                mirror_src = p0;
                if (e1) p1 = vk_vr_eye_post(1, e1, ew, eh);
            }
            pt_mark(PT_POST);
            if (g_post_shot[0]) {  // (SKYRECOMP_VR_EYE_SHOT: eye 0 as the headset gets it)
                if (p0 != b) renderer_save_texture_tex(p0, g_post_shot);
                g_post_shot[0] = 0;
            }
            {   // the game's window crop (its dynamic resolution's smaller frame): the eyes show that part, as the desktop
                extern void vr_set_eye_crop(const float crop[4]);
                float c[4] = {0, 0, 1, 1};
                if (g_window_crop[2] > 0 && g_window_crop[3] > 0 && t->width > 0 && t->height > 0)
                    c[0] = (float)g_window_crop[0] / t->width, c[1] = (float)g_window_crop[1] / t->height,
                    c[2] = (float)g_window_crop[2] / t->width, c[3] = (float)g_window_crop[3] / t->height;
                vr_set_eye_crop(c);
                memcpy(g_vr_crop, c, sizeof g_vr_crop);
            }
            vk_label_begin(vk_cmd(), "[VR] Eye copies");
            vr_record_eye_copies((void *)vk_cmd(), (void *)p0->image, p1 ? (void *)p1->image : NULL, p0->width, p0->height,
                                 renderer_origin_lower_left(), (uint32_t)p0->format, p0->layer0, p1 ? p1->layer0 : p0->layer0);
            vk_label_end(vk_cmd());
            pt_mark(PT_EYECOPY);
        }
        {   // the 2D layer (exec.c HUD texture) onto the HUD panel
            extern bool vk_hud_layer_on(void), vk_hud_opaque(void);
            extern TexBackend *vk_hud_backend(void);
            extern bool vr_world_menu_active(void);
            // (a blank frame - a fade - fills the eyes as it is: no panel)
            // The HUD image persists between frames.  Submit it only when this present actually routed or copied
            // panel content; otherwise black fades can reveal a stale, flat copy of the preceding 3D scene.
            // A retained full-screen screen (file select/title) must also disappear as soon as live 3D resumes.
            // Merely making it non-opaque is insufficient: those textures contain alpha 1 over every pixel.
            const bool panel_allowed = !g_had_scene || !vk_hud_opaque() || vr_world_menu_active();
            TexBackend *hb = vk_hud_layer_on() && !g_blank && g_had_hud && panel_allowed
                                 ? vk_hud_backend() : NULL;
            {   // (test) LARECOMP_MENU_AT=<frame>: the settings menu opens at that frame (as on the desktop below)
                static long menu_at = -1;
                if (menu_at < 0) menu_at = getenv("LARECOMP_MENU_AT") ? atol(getenv("LARECOMP_MENU_AT")) : 0;
                if (menu_at && (long)total_frames + 1 == menu_at) { extern void menu_test_open(void); menu_test_open(); }
            }
            g_vr_menu_on_panel = false;
            extern bool menu_vr_passive(void);
            extern void vr_menu_panel_frame(void);
            // Closing a menu must clear its placement even on frames with no UI to draw. Otherwise the next
            // dialog (notably name entry) can stay at an old world position, outside the current view.
            vr_menu_panel_frame();
            if (menu_on_vr_panel() || menu_vr_passive()) {  // (the settings menu / name entry / overlays: exec.c, vr.c)
                extern TexBackend *vk_vr_menu_panel(void);
                extern bool vr_record_menu_copy(void *cb, void *image, uint32_t w, uint32_t h);
                TexBackend *m = vk_vr_menu_panel();
                // Diagnostic: capture the actual VR UI image, including the software keyboard.
                static unsigned menu_shot_frames;
                const char *menu_shot = getenv("SKYRECOMP_VR_MENU_SHOT");
                if (menu_shot && menu_on_vr_panel() && ++menu_shot_frames == 3)
                    renderer_save_texture_tex(m, menu_shot);
                g_vr_menu_img = m;
                g_vr_menu_on_panel = vr_record_menu_copy((void *)vk_cmd(), (void *)m->image, m->width, m->height);
            }
            g_vr_panel_img = hb;
            if (hb) {
                vk_label_begin(vk_cmd(), "[VR] HUD Quad");
                vr_record_hud_copy((void *)vk_cmd(), (void *)hb->image, hb->width, hb->height, renderer_origin_lower_left(),
                               vk_hud_opaque() && !g_had_scene && !g_fade);
                vk_label_end(vk_cmd());
            }
            pt_mark(PT_HUD);
        }
        TexBackend *view_fade_mirror = NULL;
        {   // Always record the final fade, including frames where the throttled desktop mirror is skipped.
            extern bool vk_take_view_fade(float rgba[4]);
            extern TexBackend *vk_view_fade_mirror(const float rgba[4]);
            static float last_fade[4];
            float rgba[4]={0};
            if (g_representing) memcpy(rgba,last_fade,sizeof rgba);
            else {
                vk_take_view_fade(rgba);
                memcpy(last_fade,rgba,sizeof rgba);
            }
            if (rgba[3]>0) {
                vr_record_view_fade((void *)vk_cmd(), rgba);
                view_fade_mirror = vk_view_fade_mirror(rgba);
            }
        }
        // Desktop mirror: the left eye, with the menu.
        static const float mirror_black[4] = {0, 0, 0, 1};
        // Keep the settings usable: opening the menu temporarily restores the mirror even when it is disabled.
        // The mirror is a preview for the people around: refreshed at most Mirror Refresh Rate times a second
        // (g_settings.vr_mirror_hz, default 60; SKYRECOMP_VR_MIRROR_HZ supplies its initial value), not at the headset's rate. Each
        // mirror frame cost ~0.3 ms of this thread and doubled the time xrEndFrame then took (~0.55 -> ~1.1 ms): at
        // 144 Hz, 1.0x went from a steady 144 to ~137 fps with it on.
        static bool mirror_env_read;
        static uint64_t mirror_next;
        if (!mirror_env_read) {
            mirror_env_read = true;
            const char *e = getenv("SKYRECOMP_VR_MIRROR_HZ");
            if (e) {
                g_settings.vr_mirror_hz = (int)atof(e);
                LOG("[vr] initial mirror rate from SKYRECOMP_VR_MIRROR_HZ (VR Options remains live)\n");
            }
        }
        const double mirror_hz = g_settings.vr_mirror_hz;
        const uint64_t now_ns = SDL_GetTicksNS();
        const bool mirror_due = menu_on_vr_panel() || mirror_hz <= 0 || now_ns >= mirror_next;
        const bool mirror = (g_vr_config.desktop_mirror || menu_on_vr_panel()) && mirror_due && vk_frame_begin_nowait(mirror_black);
        if (mirror && mirror_hz > 0) {
            const uint64_t period = (uint64_t)(1e9 / mirror_hz);
            mirror_next = now_ns + period - period / 8 > mirror_next + period ? now_ns + period - period / 8 : mirror_next + period;
            // (never more than a period ahead: frames forced while a menu is up - every headset frame - must not push
            // it further and further out, or the mirror stays frozen that long after the menu closes)
            if (mirror_next > now_ns + period) mirror_next = now_ns + period;
        }
        pt_mark(PT_MIRROR_ACQ);
        {   // SKYRECOMP_VR_PACE_TRACE: the mirror's actual refresh, every 2 s
            static int trace = -1;
            static uint64_t t0_ns, n_mirror, n_frames;
            if (trace < 0) trace = getenv("SKYRECOMP_VR_PACE_TRACE") != NULL;
            if (trace) {
                n_frames++, n_mirror += mirror;
                if (!t0_ns) t0_ns = now_ns;
                if (now_ns - t0_ns >= 2000000000ull) {
                    const double sec = (now_ns - t0_ns) / 1e9;
                    LOG("[vr] mirror: %.1f per second (setting %.0f, headset frames %.1f per second, menu %s)\n",
                        n_mirror / sec, mirror_hz, n_frames / sec, menu_is_open() ? "open" : "closed");
                    t0_ns = now_ns, n_mirror = n_frames = 0;
                }
            }
        }
        if (mirror) {
            VkExtent2D extent = vk_frame_extent();
            float pv[8] = {renderer_origin_lower_left() ? 1.0f : 0.0f, 0, 0, 0, g_vr_crop[0], g_vr_crop[1], g_vr_crop[2], g_vr_crop[3]};
            // Exactly what the headset is given: this frame's composition layers (vr.c), the same images in the same
            // order, as the left eye sees them - no mirror-only drawing to drift from the headset.
            VrMirrorLayer ml[9];
            const int nml = vr_mirror_layers(ml, 9);
            if (nml > 0) {
                // The eye image's full width across the window, top and bottom cropped (no stretch): every layer's
                // clip y scaled by the same factor, so the panels stay where the eye image has them.
                const float k = ml[0].height && extent.height
                                    ? ((float)extent.width / extent.height) / ((float)ml[0].width / ml[0].height) : 1.0f;
                for (int i = 0; i < nml; i++) {
                    if (ml[i].full) {
                        memset(ml[i].mvp, 0, sizeof ml[i].mvp);
                        ml[i].mvp[0] = 1, ml[i].mvp[5] = -1, ml[i].mvp[10] = 1, ml[i].mvp[15] = 1;
                        ml[i].full = false;
                    }
                    for (int c = 0; c < 4; c++) ml[i].mvp[c * 4 + 1] *= k;
                }
                extern void vk_present_layer(VkCommandBuffer cb, const VrMirrorLayer *l);
                vk_frame_suspend();
                vr_mirror_barriers((void *)vk_cmd(), ml, nml, true);
                vk_frame_resume();
                for (int i = 0; i < nml; i++) vk_present_layer(vk_cmd(), &ml[i]);
                vk_frame_suspend();
                vr_mirror_barriers((void *)vk_cmd(), ml, nml, false);
                vk_frame_resume();
            }
            if (nml <= 0 && !g_cinema && !g_black_backdrop) vk_present_draw(vk_cmd(), mirror_src, 0, 0, (float)extent.width, (float)extent.height, pv);
            if (nml <= 0) {   // (no headset frame to copy: the HUD panel's 2D layer over it, 16:9 in the middle)
                extern bool vk_hud_layer_on(void), vk_hud_opaque(void);
                extern TexBackend *vk_hud_backend(void);
                extern bool vr_world_menu_active(void);
                extern void vk_present_overlay(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh,
                                               const float pv[8], bool opaque);
                const bool panel_allowed = !g_had_scene || !vk_hud_opaque() || vr_world_menu_active();
                TexBackend *hb = vk_hud_layer_on() && !g_blank && g_had_hud && panel_allowed
                                     ? vk_hud_backend() : NULL;
                extern bool vr_mirror_hud_mvp(float out[16]);
                extern void vk_present_hud_quad(VkCommandBuffer cb, TexBackend *b, const float mvp[16], bool flip, bool opaque);
                float mvp[16];
                if (hb && vr_mirror_hud_mvp(mvp)) {  // (where the left eye sees the panel)
                    vk_present_hud_quad(vk_cmd(), hb, mvp, renderer_origin_lower_left(), vk_hud_opaque() && !g_had_scene && !g_fade);
                } else if (hb) {
                    const float ww = (float)extent.width, wh = (float)extent.height;
                    const float w = fminf(ww, wh * 16.0f / 9.0f), h = w * 9.0f / 16.0f;
                    vk_present_overlay(vk_cmd(), hb, (ww - w) / 2, (wh - h) / 2, w, h, pv, vk_hud_opaque());
                }
                extern bool vr_mirror_menu_mvp(float out[16]);  // (the settings menu's own panel, as the left eye sees it)
                if (g_vr_menu_on_panel && g_vr_menu_img && vr_mirror_menu_mvp(mvp))
                    vk_present_hud_quad(vk_cmd(), g_vr_menu_img, mvp, false, false);
            }
            pt_mark(PT_MIRROR_DRAW);
            if (!g_vr_menu_on_panel) menu_render((void *)vk_cmd(), extent.width, extent.height);  // (else: on the panel)
            if (nml <= 0 && view_fade_mirror) {
                extern void vk_present_overlay(VkCommandBuffer cb, TexBackend *b, float x, float y, float w, float h, const float pv[8], bool opaque);
                const float full[8]={0,0,0,0,0,0,1,1};
                vk_present_overlay(vk_cmd(),view_fade_mirror,0,0,(float)extent.width,(float)extent.height,full,false);
            }
            pt_mark(PT_MENU);
            extern void vk_hud_discard_after_present(void);
            vk_hud_discard_after_present();
            const char *mirror_shot = getenv("LARECOMP_WINDOW_SHOT");
            vk_frame_end(mirror_shot && (long)total_frames + 1 == atol(mirror_shot) ? "build/window.png" : NULL);
            pt_mark(PT_MIRROR_END);
        } else {
            extern void vk_hud_discard_after_present(void);
            vk_hud_discard_after_present();
            renderer_submit();
            pt_mark(PT_SUBMIT);
        }
        vk_vr_gpu_frame_end();
        vr_frame_end();  // (after the submission: releases the eye image, ends the OpenXR frame)
        pt_mark(PT_ENDFRAME);
        goto present_complete;
    }
    static const float black[4] = {0, 0, 0, 1};
    bool have = vk_frame_begin(black);
    uint64_t nd = SDL_GetTicksNS() - t0;
    if (nd > 30000000) {
        static int warned;
        if (warned++ < 20) LOG("[vk] acquire blocked %llums\n", (unsigned long long)(nd / 1000000));
    }
    if (have) {
        double dw = g_vk.sc_extent.width, dh = g_vk.sc_extent.height;
        extern void widescreen_frame_end(uint64_t frame);
        widescreen_frame_end(total_frames);
        extern void widescreen_set_factor(float f);
        extern bool widescreen_allowed(void);
        // render scale + placement in the window: runtime/nvn/output_scale.c (Video tab "Internal resolution");
        // no GPU timer here (-1): dynamic resolution goes by the frame time
        OutScale os;
        output_scale(dw, dh, widescreen_allowed(), SDL_GetTicksNS() / 1e9, -1, &os);
        double sx = os.sx, sy = os.sy, vx = os.vx, vy = os.vy, vw = os.vw, vh = os.vh, shape = os.shape;
        // interiors / dungeons: their 16:9 centre, or the part inside a room wider than one screen
        extern void widescreen_present_crop(uint64_t, double, float *, double *, double *, double *, double *);
        float crop[4];  // uv origin and size
        widescreen_present_crop(total_frames, shape, crop, &vx, &vy, &vw, &vh);
        if (g_window_crop[2] > 0 && g_window_crop[3] > 0 && t->width > 0 && t->height > 0) {
            float u0 = (float)g_window_crop[0] / t->width;
            float v0 = (float)g_window_crop[1] / t->height;
            float uw = (float)g_window_crop[2] / t->width;
            float vh = (float)g_window_crop[3] / t->height;
            crop[0] = u0 + crop[0] * uw;
            crop[1] = v0 + crop[1] * vh;
            crop[2] *= uw;
            crop[3] *= vh;
        }
        widescreen_set_factor((float)shape);
        renderer_set_scale((float)sx, (float)sy);
        VkCommandBuffer cb = vk_cmd();
        float pv[8] = {renderer_origin_lower_left() ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
        memcpy(g_shot_crop, crop, sizeof crop);
        vk_present_draw(cb, b, (float)vx, (float)vy, (float)vw, (float)vh, pv);
        static long menu_at = -1;
        if (menu_at < 0) menu_at = getenv("LARECOMP_MENU_AT") ? atol(getenv("LARECOMP_MENU_AT")) : 0;
        if (menu_at && (long)total_frames + 1 == menu_at) { extern void menu_test_open(void); menu_test_open(); }
        menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
        static long win_shot = -1;
        if (win_shot < 0) win_shot = getenv("LARECOMP_WINDOW_SHOT") ? atol(getenv("LARECOMP_WINDOW_SHOT")) : 0;
        bool shot = win_shot && (long)total_frames + 1 == win_shot;
        extern char g_window_shot_request[256];
        vk_frame_end(g_window_shot_request[0] ? g_window_shot_request : shot ? "build/window.png" : NULL);
        g_window_shot_request[0] = 0;
        if (shot && getenv("LARECOMP_EXIT_AFTER_SHOT")) { fflush(stderr); _exit(0); }
    } else {
        renderer_submit();
    }
present_complete:
    ;
    const bool pt_vr = vr_running();
    extern void vk_prof_frame(uint64_t frame, uint64_t present_ns);
    vk_prof_frame(total_frames, SDL_GetTicksNS() - t0);
    frames++;
    total_frames++;
    g_frame_counter++;
    {   // Optional low-overhead timing capture; no screenshots/readbacks in frame-stall measurements.
        static FILE *trace;
        static bool opened;
        if (!opened) {
            opened = true;
            const char *path = getenv("SKYRECOMP_FRAME_TRACE");
            if (path) trace = fopen(path, "w");
        }
        if (trace) {
            fprintf(trace, "%llu,%llu\n", (unsigned long long)total_frames, (unsigned long long)SDL_GetTicksNS());
            if (total_frames % 120 == 0) fflush(trace);
        }
    }
    extern void vk_pace_frame(void);
    vk_pace_frame();  // (exec.c: the game's 30 fps mode when frames don't fit 60)
    extern void perfmon_frame(void);
    extern void perfmon_set_gpu_mem(float game_mb, float total_mb);
    perfmon_frame();  // (runtime/perfmon.c: the performance overlay's frame times)
    if (total_frames % 30 == 0 && g_vk.vma) {  // the game's device-local allocations (VMA's count), and those heaps' size
        VmaBudget b[VK_MAX_MEMORY_HEAPS];
        const VkPhysicalDeviceMemoryProperties *mp;
        vmaGetHeapBudgets(g_vk.vma, b);
        vmaGetMemoryProperties(g_vk.vma, &mp);
        uint64_t used = 0, size = 0;
        for (uint32_t i = 0; i < mp->memoryHeapCount; i++)
            if (mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) used += b[i].usage, size += mp->memoryHeaps[i].size;
        perfmon_set_gpu_mem(used / 1048576.0f, size / 1048576.0f);
    }
    extern bool nvn_async_on(void);
    if (!nvn_async_on()) present_game_hooks(total_frames);  // (async replay: run by the game thread, nvn/async.c)
    extern void vk_level_change_check(void);
    vk_level_change_check();
    vr_apply_enabled_request();
    if (pt_vr) { pt_mark(PT_TAIL); pt_report(); }
    static long shot_every = -1;
    if (shot_every < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_EVERY"); shot_every = e ? atol(e) : 0; }
    extern bool g_debug_frame;
    static long debug_at = -1;
    if (debug_at < 0) { const char *e = getenv("LARECOMP_DEBUG_FRAME"); debug_at = e ? atol(e) : 0; }
    extern volatile int g_debug_frame_request;
    if (g_debug_frame_request) { g_debug_frame_request = 0; debug_at = (long)total_frames + 1; }
    // LARECOMP_DEBUG_FRAMES=<n> (with LARECOMP_DEBUG_FRAME / F9): dump n consecutive frames (build/pass_<frame>_NNN.png)
    static long debug_n = -1;
    if (debug_n < 0) { const char *e = getenv("LARECOMP_DEBUG_FRAMES"); debug_n = e && atol(e) > 1 ? atol(e) : 1; }
    g_debug_frame = debug_at && total_frames >= (uint64_t)debug_at && total_frames < (uint64_t)debug_at + (uint64_t)debug_n;
    {   // LARECOMP_GPUPROF_AT=<frame>[,<frame>...]: GPU time per pass of those frames ("[gpuprof]" in the log)
        extern bool g_gpuprof_frame;
        extern void vk_gpuprof_report(void);
        if (g_gpuprof_frame) { g_gpuprof_frame = false; vk_gpuprof_report(); }
        const char *e = getenv("LARECOMP_GPUPROF_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            if (at == (long)total_frames) { g_gpuprof_frame = true; break; }
            if (*p == ',') p++; else break;
        }
        // One automatic capture after startup when the measured application GPU work is clearly over a 90 Hz
        // budget. It profiles the following frame and intentionally runs only once: reporting waits for that frame
        // so repeating it would itself disturb pacing. Opt-in (a hitch while it reports): SKYRECOMP_VR_AUTO_GPUPROF=1.
        static bool auto_profiled;
        static int auto_on = -1;
        if (auto_on < 0) auto_on = getenv("SKYRECOMP_VR_AUTO_GPUPROF") && atoi(getenv("SKYRECOMP_VR_AUTO_GPUPROF")) != 0;
        if (auto_on && !auto_profiled && vr_running() && total_frames > 120) {
            const float gpu_ms = g_vr_gpu_ms;
            if (gpu_ms >= 12.0f) {
                auto_profiled = true;
                g_gpuprof_frame = true;
                LOG("[gpuprof] automatic VR capture armed after %.2f ms GPU frame\n", gpu_ms);
            }
        }
    }
    if (g_debug_frame) LOG("===== debug frame %llu =====\n", (unsigned long long)total_frames);
    {   // LARECOMP_SHOT_AT=<frame>,<frame>,... (ascending): the game image at those frames -> build/frames/frame_N.png
        static const char *next;
        static int init;
        if (!init) { init = 1; next = getenv("LARECOMP_SHOT_AT"); if (next) mkdir("build/frames", 0755); }
        while (next && *next) {
            long at = strtol(next, (char **)&next, 10);
            if ((long)total_frames < at) { while (next > getenv("LARECOMP_SHOT_AT") && next[-1] != ',') next--; break; }
            if ((long)total_frames == at) {
                char path[256];
                snprintf(path, sizeof path, "build/frames/frame_%05llu.png", (unsigned long long)total_frames);
                renderer_save_texture_tex(b, path);
            }
            if (*next == ',') next++;
        }
    }
    static long shot_from = -1;
    if (shot_from < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_FROM"); shot_from = e ? atol(e) : 0; }
    if (shot_every && total_frames % shot_every == 0 && (long)total_frames >= shot_from) {
        char path[256];
        if (shot_from) mkdir("build/frames", 0755);
        snprintf(path, sizeof path, shot_from ? "build/frames/frame_%05llu.png" : "build/frame_%05llu.png",
                 (unsigned long long)total_frames);
        renderer_save_texture_tex(b, path);
        // a series (SCREENSHOT_FROM) runs on until LARECOMP_WINDOW_SHOT exits
        if (getenv("LARECOMP_EXIT_AFTER_SHOT") && !shot_from) { fflush(stderr); _exit(0); }
    }
    double now = SDL_GetTicks() / 1000.0;
    static double fps_iv = -1, prev_present, max_gap;
    if (fps_iv < 0) fps_iv = getenv("LARECOMP_FPS_INTERVAL") ? atof(getenv("LARECOMP_FPS_INTERVAL")) : 2.0;
    if (prev_present > 0 && now - prev_present > max_gap) max_gap = now - prev_present;
    prev_present = now;
    if (now - last_fps_time >= fps_iv) {
        extern const char *game_level_name(void);
        if (last_fps_time > 0)
            LOG("[vk] t=%.1f frame %llu %.1f fps max %.0fms level %s (%s)\n", now, (unsigned long long)total_frames,
                frames / (now - last_fps_time), max_gap * 1000.0, game_level_name(), renderer_stats());
        max_gap = 0;
        frames = 0;
        last_fps_time = now;
    }
}

// ================================================================== UI thread
void vk_exec_init(void);
void vk_shaders_init(void);

// exit() from any thread: let the GPU finish, keep the pipeline cache, then end the process without the DLL
// teardown (driver / validation-layer threads can stall it for minutes while the game's threads still run).
static void quick_exit_handler(void) {
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] exit: waiting for the GPU\n");
    if (g_vk.dev) {
        // Window closed (runtime/vr/vr.h TriggerFastVRExit): no wait for the GPU and no OpenXR teardown (the
        // process ends; the runtime and driver reclaim both). The pipeline cache is still saved: it is what keeps
        // the next launch from recompiling every shader.
        if (!IsVRExitRequested()) {
            vkDeviceWaitIdle(g_vk.dev);
            vr_shutdown();
        }
        if (trace) LOG("[vk] exit: saving the pipeline cache\n");
        vk_pipeline_cache_save();
    }
    if (trace) LOG("[vk] exit: terminating\n");
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
}

#ifdef _WIN32
static void ensure_reshade_config(void) {
    const char *preset = "HenrikoMagnifico_Ultra_NO_MOTION_BLUR_v8.ini";
    FILE *f = fopen("ReShade.ini", "r");
    if (!f) {
        FILE *w = fopen("ReShade.ini", "w");
        if (w) {
            fprintf(w,
                "[GENERAL]\n"
                "EffectSearchPaths=.\\reshade-shaders\\Shaders\\**\n"
                "TextureSearchPaths=.\\reshade-shaders\\Textures\\**\n"
                "PresetPath=.\\%s\n"
                "PerformanceMode=1\n"
                "SkipLoadingDisabledEffects=1\n"
                "PreprocessorDefinitions=RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=1000.0,RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=1,RESHADE_DEPTH_INPUT_IS_REVERSED=1,RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=0\n"
                "\n[INPUT]\n"
                "KeyOverlay=36,0,0,0\n"
                "KeyEffects=123,0,0,0\n"
                "\n[OVERLAY]\n"
                "TutorialProgress=4\n"
                "ShowFPS=0\n", preset);
            fclose(w);
        }
    } else {
        fclose(f);
    }
}
#endif

void ui_window_init(void) {
    if (window) return;
    LOG("[vk] initializing window\n");
    atexit(quick_exit_handler);
#ifdef _WIN32
    ensure_reshade_config();
    char manifest[1100], module[1100];
    settings_load();
    const char *reshade = getenv("SKYRECOMP_RESHADE");
    bool use_reshade = g_settings.reshade && (!reshade || strcmp(reshade, "0"));
    if (!use_reshade) {
        setenv("DISABLE_VK_LAYER_reshade_1", "1", 1);
        setenv("VK_LOADER_LAYERS_DISABLE", "~all~,*reshade*,VK_LAYER_reshade", 1);
        unsetenv("VK_INSTANCE_LAYERS");
        unsetenv("VK_LAYER_PATH");
        unsetenv("VK_ADD_LAYER_PATH");
        LOG("[reshade] disabled by settings\n");
    } else {
        unsetenv("DISABLE_VK_LAYER_reshade_1");
        unsetenv("VK_LOADER_LAYERS_DISABLE");
    }
    if (use_reshade &&
        rt_resource("reshade/ReShade64.json", "reshade/ReShade64.json", manifest, sizeof manifest) &&
        rt_resource("reshade/ReShade64.dll", "reshade/ReShade64.dll", module, sizeof module)) {
        char *end = strrchr(manifest, '/'), *back = strrchr(manifest, '\\');
        if (back && (!end || back > end)) end = back;
        if (end) {
            *end = 0;
            char paths[4096];
            const char *key = getenv("VK_LAYER_PATH") ? "VK_LAYER_PATH" : "VK_ADD_LAYER_PATH";
            const char *prior = getenv(key);
            if (snprintf(paths, sizeof paths, "%s%s%s", manifest, prior && *prior ? ";" : "", prior ? prior : "") < sizeof paths) {
                setenv(key, paths, 1);
                const char *layers = getenv("VK_INSTANCE_LAYERS");
                if (!layers || !strstr(layers, "VK_LAYER_reshade")) {
                    if (snprintf(paths, sizeof paths, "%s%sVK_LAYER_reshade", layers ? layers : "", layers && *layers ? ";" : "") < sizeof paths)
                        setenv("VK_INSTANCE_LAYERS", paths, 1);
                }
                LOG("[vk] local ReShade layer enabled\n");
            }
        }
    }
#endif
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    settings_load();
    LOG("[vk] initializing SDL video and input\n");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) FATAL("SDL_Init: %s\n", SDL_GetError());
    LOG("[vk] SDL ready\n");
    int win_w = 1280, win_h = 720;
    if (getenv("LARECOMP_WINDOW")) sscanf(getenv("LARECOMP_WINDOW"), "%dx%d", &win_w, &win_h);
    SDL_WindowFlags wflags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (getenv("LARECOMP_HIDDEN")) wflags |= SDL_WINDOW_HIDDEN;
    {   // scripted test runs (user input ignored: runtime/hle/input.c input_user_blocked) open without taking the
        // keyboard focus from whatever the player is doing
        const char *v = getenv("LARECOMP_NO_USER_INPUT");
        bool scripted = v ? atoi(v) != 0 : (getenv("LARECOMP_INPUT") || getenv("LARECOMP_DOORJUMP_TEST") || getenv("LARECOMP_PLACE") ||
                                            getenv("LARECOMP_HOST_INPUT") || getenv("LARECOMP_BIND_TEST"));
        if (scripted) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    }
    window = SDL_CreateWindow("Sky Recomp", win_w, win_h, wflags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    LOG("[vk] SDL window created at %llums\n", (unsigned long long)SDL_GetTicks());
    rt_window_icon(window);
    LOG("[vk] window icon ready at %llums\n", (unsigned long long)SDL_GetTicks());
    // Create Vulkan before requesting fullscreen; finish the transition before the game starts presenting.
    create_instance();
    extern void vk_reshade_init(void);
    vk_reshade_init();
    if (!SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface)) FATAL("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
    pick_device();
    create_device();
    LOG("[vk] device ready\n");
    create_swapchain();
    LOG("[vk] swapchain ready\n");
    vk_exec_init();
    LOG("[vk] execution ready\n");
    vk_shaders_init();
    LOG("[vk] shaders ready\n");
    // OpenXR starts after Play, once the runtime is ready (vr_apply_enabled_request).
    if (g_settings.fullscreen && !getenv("LARECOMP_WINDOW") && !getenv("LARECOMP_HIDDEN")) {
        if (!SDL_SetWindowFullscreen(window, true) || !SDL_SyncWindow(window))
            LOG("[vk] fullscreen transition: %s\n", SDL_GetError());
    }
}

SDL_Window *ui_window(void) { return window; }

void backend_init(void) {
    while (!atomic_load(&ui_ready)) SDL_Delay(1);
    LOG("[vk] %s, swapchain %ux%u\n", g_vk.props.deviceName, g_vk.sc_extent.width, g_vk.sc_extent.height);
}

// ---- in-place reboot (runtime/main.c teardown; main thread, guest threads stopped)
void backend_reset(void) {
    extern void nvn_async_stop(void);
    nvn_async_stop();  // (the replay thread finishes what was queued and ends)
    if (!g_vk.dev) return;
    vkDeviceWaitIdle(g_vk.dev);
    extern void vk_textures_reset(void);
    extern void vk_exec_reset(void);
    vr_shutdown();
    vk_textures_reset();
    vk_vr_request_enabled(vr_requested());  // a subsequent Play gets one fresh automatic launch attempt
    vk_exec_reset();
    g_last_presented = NULL;
}
void backend_guest_memory_reset(void) {}  // (exec.c re-imports the heap on first use)
void backend_clear_window(void) {
    static const float black[4] = {0, 0, 0, 1};
    if (g_vk.dev && vk_frame_begin(black)) vk_frame_end(NULL);
}

int ui_run(void) {
    ui_window_init();
    atomic_store(&ui_ready, true);
    uint64_t last_dump = SDL_GetTicksNS();
    for (;;) {
        if (rt_reboot_requested()) { input_mouse_update(window, false); return rt_reboot_requested(); }
        if (getenv("LARECOMP_DUMP_THREADS")) {
            uint64_t now = SDL_GetTicksNS();
            if (now - last_dump >= 3000000000ull) {
                last_dump = now;
                extern void kernel_dump_threads(void);
                kernel_dump_threads();
            }
        }
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 16)) {
            if (rt_reboot_requested()) break;
            if (e.type == SDL_EVENT_QUIT) {
                LOG("window closed\n");
                TriggerFastVRExit();
                exit(0);
            }
            extern bool input_user_blocked(const SDL_Event *e);
            if (input_user_blocked(&e)) continue;  // scripted test runs: the real keyboard / mouse / pads do nothing (as metal.m)
            bool consumed = menu_handle_event(&e);
            input_mouse_update(window, true);
            if (consumed) continue;
            input_handle_event(&e);
        }
        menu_apply_window_requests(window);
        // SteamVR takes the desktop focus when the session starts: give keyboard / gamepad input back to the game
        // window once (here: SDL window calls belong to this thread).
        if (vr_take_window_focus_request() && !getenv("LARECOMP_HIDDEN")) SDL_RaiseWindow(window);
    }
}
