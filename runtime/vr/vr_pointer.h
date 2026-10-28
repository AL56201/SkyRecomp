// Controller-ray interaction with the virtual VR HUD surface.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VRPointerState {
    bool active;
    float ray_origin[3];
    float ray_direction[3];
    float ui_space_x;
    float ui_space_y;
    float distance_to_quad;
    bool intersects;
} VRPointerState;

// Matrices are row-major and multiplied by column vectors, matching vr_hud.h. The controller points along its local
// -Z axis. The HUD occupies local x/y coordinates -1..1; +Y maps toward UI-space Y=0 (the top of the UI).
bool ComputeVRPointerIntersection(VRPointerState *out_pointer, const float controller_pose[16],
                                  const float hud_quad_matrix[16]);

// Produces two xyz vertices: ray origin and either the HUD hit or max_distance along the ray. A renderer may draw
// this as a line or expand it into a camera-facing strip/cylinder. Returns false when the pointer is inactive.
bool ComputeVRPointerBeamSegment(float out_vertices[6], const VRPointerState *pointer, float max_distance);

#ifdef __cplusplus
} // extern "C"
#endif
