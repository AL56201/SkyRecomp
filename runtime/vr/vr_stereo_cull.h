// Pure stereoscopic projection and conservative frustum-culling helpers.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VRStereoEyeParams {
    // OpenXR-style FOV angles in radians. Left/down are normally negative; right/up are normally positive.
    float angle_left;
    float angle_right;
    float angle_up;
    float angle_down;
    float near_z;
    float far_z;
} VRStereoEyeParams;

// Produces a row-major right-handed projection for column vectors, with forward along -Z and Vulkan depth 0..1.
// Positive near_z/far_z arguments override the values in eye_params; non-positive arguments use the struct values.
void ComputeStereoProjectionMatrix(float out_matrix[16], const VRStereoEyeParams *eye_params, float near_z,
                                   float far_z);

// view_matrix is the combined clip-from-world view-projection matrix. expansion_factor=1 extracts its exact planes;
// values above 1 conservatively widen the four lateral planes while leaving near/far unchanged. Planes face inward
// and use ax+by+cz+d >= 0 for points inside the frustum.
void ExpandViewFrustumForWideFOV(float out_frustum_planes[6][4], const float view_matrix[16],
                                 float expansion_factor);

bool TestSphereAgainstStereoFrustum(const float frustum_planes[6][4], const float center[3], float radius);

// Values assigned by the Vulkan specification. Module-prefixed names keep this header usable without Vulkan headers.
enum {
    VR_VK_FORMAT_R8G8B8A8_UNORM = 37,
    VR_VK_FORMAT_R8G8B8A8_SRGB = 43,
    VR_VK_FORMAT_B8G8R8A8_UNORM = 44,
    VR_VK_FORMAT_B8G8R8A8_SRGB = 50,
};

// Validates a direct copy between a Vulkan source image and an OpenXR swapchain VkFormat. Recognized RGBA/BGRA
// channel differences are reported through out_needs_swizzle. out_is_srgb describes the OpenXR destination. A
// recognized SRGB/UNORM transfer-function mismatch returns false because a gamma conversion is required.
bool ValidateVulkanToOpenXRFormat(uint32_t vk_format, uint32_t openxr_format, bool *out_needs_swizzle,
                                  bool *out_is_srgb);
void ConvertRGBAToBGRASwizzle(uint8_t *rgba_pixel_data, size_t pixel_count);
float LinearToSRGB(float linear_val);
float SRGBToLinear(float srgb_val);

#ifdef __cplusplus
} // extern "C"
#endif
