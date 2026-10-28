// OpenXR entry points used by vr.c (instance, session, frames), loaded from the runtime. Include only where
// SKYRECOMP_HAS_OPENXR is set.
#pragma once

#define XR_USE_GRAPHICS_API_VULKAN
#define XR_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define XR_INSTANCE_FUNCTIONS(X)                                                                                    \
    X(xrGetSystem) X(xrGetVulkanGraphicsRequirements2KHR) X(xrGetVulkanGraphicsDevice2KHR) X(xrCreateSession)      \
    X(xrDestroySession) X(xrBeginSession) X(xrEndSession) X(xrPollEvent) X(xrCreateReferenceSpace)               \
    X(xrDestroySpace) X(xrLocateSpace) X(xrLocateViews) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame)              \
    X(xrEnumerateViewConfigurationViews) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain)                        \
    X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage)        \
    X(xrReleaseSwapchainImage) X(xrDestroyInstance)

#define XR_DECLARE_EXTERN(name) extern PFN_##name p_##name;
XR_INSTANCE_FUNCTIONS(XR_DECLARE_EXTERN)
#undef XR_DECLARE_EXTERN
