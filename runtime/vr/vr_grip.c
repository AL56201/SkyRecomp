// The VR controllers' grip poses, for the wrist HUD (vr.c). Pose actions only: no buttons or sticks are read; the
// game is played with a gamepad. A runtime without controllers leaves the wrist HUD off (the floating panel stays).
#include "vr.h"

#if !defined(__APPLE__) && !defined(SKYRECOMP_NO_OPENXR) && __has_include(<openxr/openxr.h>) && \
    __has_include(<openxr/openxr_platform.h>)
#include <stdio.h>
#include <string.h>

#include "hle.h"
#include "vr_xr.h"

static XrSession session;
static XrActionSet action_set;
static XrAction grip_action[2];
static XrSpace grip_space[2];

static bool ok(XrResult r, const char *what) {
    if (XR_SUCCEEDED(r)) return true;
    LOG("[vr] grip poses: %s failed (%d)\n", what, (int)r);
    return false;
}

static const char *const profiles[] = {
    "/interaction_profiles/valve/index_controller", "/interaction_profiles/oculus/touch_controller",
    "/interaction_profiles/htc/vive_controller", "/interaction_profiles/microsoft/motion_controller",
    "/interaction_profiles/khr/simple_controller",
};

bool vr_grip_create(XrInstance instance, XrSession s) {
    XrActionSetCreateInfo si = {.type = XR_TYPE_ACTION_SET_CREATE_INFO};
    snprintf(si.actionSetName, sizeof si.actionSetName, "hands");
    snprintf(si.localizedActionSetName, sizeof si.localizedActionSetName, "Hands");
    if (!ok(p_xrCreateActionSet(instance, &si, &action_set), "xrCreateActionSet")) return false;
    for (int h = 0; h < 2; h++) {
        XrActionCreateInfo ai = {.type = XR_TYPE_ACTION_CREATE_INFO, .actionType = XR_ACTION_TYPE_POSE_INPUT};
        snprintf(ai.actionName, sizeof ai.actionName, h ? "right_hand" : "left_hand");
        snprintf(ai.localizedActionName, sizeof ai.localizedActionName, h ? "Right hand grip pose" : "Left hand grip pose");
        if (!ok(p_xrCreateAction(action_set, &ai, &grip_action[h]), "xrCreateAction")) return false;
    }
    XrPath path[2];
    if (!ok(p_xrStringToPath(instance, "/user/hand/left/input/grip/pose", &path[0]), "xrStringToPath") ||
        !ok(p_xrStringToPath(instance, "/user/hand/right/input/grip/pose", &path[1]), "xrStringToPath"))
        return false;
    const XrActionSuggestedBinding b[2] = {{grip_action[0], path[0]}, {grip_action[1], path[1]}};
    int suggested = 0;
    for (size_t p = 0; p < sizeof profiles / sizeof *profiles; p++) {
        XrPath profile;
        if (XR_FAILED(p_xrStringToPath(instance, profiles[p], &profile))) continue;
        XrInteractionProfileSuggestedBinding sb = {.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
                                                   .interactionProfile = profile, .countSuggestedBindings = 2,
                                                   .suggestedBindings = b};
        suggested += XR_SUCCEEDED(p_xrSuggestInteractionProfileBindings(instance, &sb));
    }
    XrSessionActionSetsAttachInfo at = {.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = 1,
                                        .actionSets = &action_set};
    if (!ok(p_xrAttachSessionActionSets(s, &at), "xrAttachSessionActionSets")) return false;
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo as = {.type = XR_TYPE_ACTION_SPACE_CREATE_INFO, .action = grip_action[h],
                                      .poseInActionSpace.orientation.w = 1};
        if (!ok(p_xrCreateActionSpace(s, &as, &grip_space[h]), "xrCreateActionSpace")) return false;
    }
    session = s;
    LOG("[vr] grip poses ready (%d controller profiles bound)\n", suggested);
    return true;
}

void vr_grip_sync(XrSpace space, XrTime time) {
    float pose[2][7] = {{0}};
    bool tracked[2] = {false, false};
    if (session && space && time > 0) {
        XrActiveActionSet active = {.actionSet = action_set};
        XrActionsSyncInfo sync = {.type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 1, .activeActionSets = &active};
        if (XR_SUCCEEDED(p_xrSyncActions(session, &sync))) {
            const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            for (int h = 0; h < 2; h++) {
                XrSpaceLocation loc = {.type = XR_TYPE_SPACE_LOCATION};
                if (!grip_space[h] || XR_FAILED(p_xrLocateSpace(grip_space[h], space, time, &loc)) ||
                    (loc.locationFlags & valid) != valid)
                    continue;
                tracked[h] = true;
                const float p[7] = {loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z,
                                    loc.pose.orientation.w, loc.pose.position.x, loc.pose.position.y, loc.pose.position.z};
                memcpy(pose[h], p, sizeof p);
            }
        }
    }
    vr_set_grip_poses(tracked[0], pose[0], tracked[1], pose[1]);
}

void vr_grip_destroy(void) {
    for (int h = 0; h < 2; h++) {
        if (grip_space[h] && p_xrDestroySpace) p_xrDestroySpace(grip_space[h]);
        grip_space[h] = XR_NULL_HANDLE;
        grip_action[h] = XR_NULL_HANDLE;
    }
    if (action_set && p_xrDestroyActionSet) p_xrDestroyActionSet(action_set);
    action_set = XR_NULL_HANDLE;
    session = XR_NULL_HANDLE;
}
#endif
