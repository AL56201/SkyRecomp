// Optional OpenXR / SteamVR output (runtime/vr.c). The desktop renderer never depends on an XR runtime: every entry
// point is a no-op unless VR was enabled before launch and OpenXR started.
//
// Frame model (all calls on the renderer thread):
//   vr_frame_begin()          first command list of a frame: xrWaitFrame + xrBeginFrame + xrLocateViews. Both eyes
//                             of the frame use this one pose sample.
//   vr_stereo_active()        true while a begun frame has valid eye views: the renderer records every command
//                             list twice (eye 0, then eye 1 into eye-local render targets).
//   vr_patch_scene_ubo()      per draw: the eye's version of the game's scene camera.
//   vr_record_eye_copies()    at present: acquires the stereo swapchain image, records both eye blits.
//   vr_frame_end()            after the queue submission holding the blits: release + xrEndFrame.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// ---- lifecycle
bool vr_requested(void);  // settings "vr" (or SKYRECOMP_VR=0/1); decides the Vulkan extensions asked for
// After the Vulkan instance and device exist. Vulkan handles are opaque to keep Vulkan out of this header.
bool vr_vulkan_init(void *instance, void *physical_device, void *device, uint32_t queue_family, uint32_t queue_index);
void vr_shutdown(void);
bool vr_available(void);
bool vr_running(void);  // the OpenXR session is running
// Game render-target scale that matches OpenXR's recommended per-eye size and the user's VR multiplier.
bool vr_render_scale(float *sx, float *sy);
bool vr_eye_extent(uint32_t *w, uint32_t *h);  // the headset's eye image size (false before the session)
uint32_t vr_hidden_mesh(int eye, const float **xy);  // the headset's hidden triangles (view-space tangents), 0: none
bool vr_eye_fov(int eye, float fov[4]);  // this frame's eye FOV angles (left, right, up, down; radians)
float vr_resolution_factor(void);  // current effective multiplier (after adaptive scaling)
const char *vr_status(void);
const char *vr_debug_stage(void);  // the OpenXR call in progress (freeze diagnostics)
// The window was closed: any thread. GPU waits and the OpenXR teardown are skipped so the process exits at once.
void TriggerFastVRExit(void);
bool IsVRExitRequested(void);

// ---- per frame
void vr_poll(void);  // session events
bool vr_take_window_focus_request(void);  // one-shot: SteamVR focused the session, raise the desktop window

bool vr_frame_begin(void);  // idempotent within a frame; true when the frame renders in stereo
bool vr_stereo_active(void);
void vr_recenter(void);  // the current head pose becomes the reference (and the 1st-person anchor faces Link again)
bool vr_patch_scene_ubo(uint32_t eye, float out[28], const float *in, uint64_t size);
bool vr_patch_scene_fragment_ubo(uint32_t eye, float out[28], const float *in, uint64_t size);
// left / right: VkImage in VK_IMAGE_LAYOUT_GENERAL, sw x sh. right may be NULL (no eye-1 image this frame): the left
// image then goes to both eyes. source_format: their VkFormat (decides how colour is carried over). False: nothing
// recorded.
bool vr_record_eye_copies(void *command_buffer, void *left_image, void *right_image, uint32_t sw, uint32_t sh,
                          bool flip_y, uint32_t source_format, uint32_t left_layer, uint32_t right_layer);  // (layers: of each image)
// The game's 2D layer (exec.c HUD texture, VkImage in VK_IMAGE_LAYOUT_GENERAL, w x h) onto the HUD panel's swapchain,
// recorded after vr_record_eye_copies. opaque: full-screen 2D (the panel shown alone, not blended).
// For the next vr_record_eye_copies. freeze: full-screen 2D (no 3D scene): show the last 3D frame, at its own poses.
// frame_is_3d: this frame drew a 3D scene; its eyes become that "last 3D frame".
void vr_freeze_world(bool freeze, bool frame_is_3d);
bool vr_record_hud_copy(void *command_buffer, void *hud_image, uint32_t w, uint32_t h, bool flip_y, bool opaque);
// Latest OpenXR grip poses (vr_grip.c) for the wrist HUD: {qx, qy, qz, qw, px, py, pz} in the local space.
void vr_set_grip_poses(bool left_tracked, const float left_pose[7], bool right_tracked, const float right_pose[7]);
void vr_frame_end(void);
extern float g_vr_gpu_ms;  // the last measured GPU frame time in ms (exec.c), for dynamic resolution
