// VR controller input: one OpenXR action set whose actions are a virtual pair of Joy-Cons, suggested bindings for
// each supported controller, and the per-frame sample handed to the Joy-Con translator
// (runtime/nvn/vulkan/JoyConTranslator.h). Players can rebind every action in SteamVR's controller bindings.
//
// Default layout, Valve Index (primary):
//   left   thumbstick: stick (move), click: L-stick | trigger: ZL (target) | grip: L | A: Minus | B: Plus
//          trackpad press: d-pad (direction on the pad)
//   right  thumbstick: stick | trigger: ZR | grip: R | A: A | B: B | trackpad press: X
//   Pointing (aim pose) of each hand is that Joy-Con's orientation: swing, thrust, point.
#include "vr.h"

#if !defined(__APPLE__) && !defined(SKYRECOMP_NO_OPENXR) && __has_include(<openxr/openxr.h>) && \
    __has_include(<openxr/openxr_platform.h>)

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "vr_xr.h"
#include "vr_internal.h"
#include "nvn/vulkan/JoyConTranslator.h"

enum {
    ACT_LEFT_STICK, ACT_RIGHT_STICK, ACT_DPAD,                      // vector2
    ACT_ZL, ACT_ZR, ACT_L, ACT_R,                                   // float
    ACT_A, ACT_B, ACT_X, ACT_PLUS, ACT_MINUS, ACT_LSTICK, ACT_RSTICK, ACT_DPAD_PRESS,  // boolean
    ACT_LEFT_AIM, ACT_RIGHT_AIM,                                    // pose used by the Joy-Con motion emulation
    ACT_LEFT_GRIP, ACT_RIGHT_GRIP,                                  // grip poses: the wrist HUD
    ACT_COUNT
};

static const struct { const char *name, *label; XrActionType type; uint32_t button; } action_defs[ACT_COUNT] = {
    [ACT_LEFT_STICK] = {"left_stick", "Left stick (move)", XR_ACTION_TYPE_VECTOR2F_INPUT, 0},
    [ACT_RIGHT_STICK] = {"right_stick", "Right stick", XR_ACTION_TYPE_VECTOR2F_INPUT, 0},
    [ACT_DPAD] = {"dpad", "D-pad direction", XR_ACTION_TYPE_VECTOR2F_INPUT, 0},
    [ACT_ZL] = {"zl", "ZL (target)", XR_ACTION_TYPE_FLOAT_INPUT, 0},
    [ACT_ZR] = {"zr", "ZR", XR_ACTION_TYPE_FLOAT_INPUT, 0},
    [ACT_L] = {"l", "L", XR_ACTION_TYPE_FLOAT_INPUT, 0},
    [ACT_R] = {"r", "R", XR_ACTION_TYPE_FLOAT_INPUT, 0},
    [ACT_A] = {"a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT, SKY_JC_A},
    [ACT_B] = {"b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT, SKY_JC_B},
    [ACT_X] = {"x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT, SKY_JC_X},
    [ACT_PLUS] = {"plus", "Plus", XR_ACTION_TYPE_BOOLEAN_INPUT, SKY_JC_PLUS},
    [ACT_MINUS] = {"minus", "Minus", XR_ACTION_TYPE_BOOLEAN_INPUT, SKY_JC_MINUS},
    [ACT_LSTICK] = {"left_stick_click", "Left stick click", XR_ACTION_TYPE_BOOLEAN_INPUT, SKY_JC_LSTICK},
    [ACT_RSTICK] = {"right_stick_click", "Right stick click", XR_ACTION_TYPE_BOOLEAN_INPUT, SKY_JC_RSTICK},
    [ACT_DPAD_PRESS] = {"dpad_press", "D-pad press", XR_ACTION_TYPE_BOOLEAN_INPUT, 0},
    [ACT_LEFT_AIM] = {"left_joycon", "Joy-Con (L) pose", XR_ACTION_TYPE_POSE_INPUT, 0},
    [ACT_RIGHT_AIM] = {"right_joycon", "Joy-Con (R) pose", XR_ACTION_TYPE_POSE_INPUT, 0},
    [ACT_LEFT_GRIP] = {"left_hand", "Left hand grip pose", XR_ACTION_TYPE_POSE_INPUT, 0},
    [ACT_RIGHT_GRIP] = {"right_hand", "Right hand grip pose", XR_ACTION_TYPE_POSE_INPUT, 0},
};

typedef struct { int action; const char *path; } Binding;
#define L(p) "/user/hand/left/input/" p
#define R(p) "/user/hand/right/input/" p

static const Binding index_bindings[] = {
    {ACT_LEFT_STICK, L("thumbstick")}, {ACT_LSTICK, L("thumbstick/click")}, {ACT_ZL, L("trigger/value")},
    {ACT_L, L("squeeze/value")}, {ACT_MINUS, L("a/click")}, {ACT_PLUS, L("b/click")}, {ACT_DPAD, L("trackpad")},
    {ACT_DPAD_PRESS, L("trackpad/force")}, {ACT_LEFT_AIM, L("aim/pose")}, {ACT_LEFT_GRIP, L("grip/pose")},
    {ACT_RIGHT_STICK, R("thumbstick")}, {ACT_ZR, R("trigger/value")},
    {ACT_R, R("squeeze/value")}, {ACT_A, R("a/click")}, {ACT_B, R("b/click")}, {ACT_X, R("trackpad/force")},
    {ACT_RIGHT_AIM, R("aim/pose")}, {ACT_RIGHT_GRIP, R("grip/pose")},
};
static const Binding touch_bindings[] = {
    {ACT_LEFT_STICK, L("thumbstick")}, {ACT_LSTICK, L("thumbstick/click")}, {ACT_ZL, L("trigger/value")},
    {ACT_L, L("squeeze/value")}, {ACT_MINUS, L("x/click")}, {ACT_PLUS, L("y/click")}, {ACT_LEFT_AIM, L("aim/pose")},
    {ACT_LEFT_GRIP, L("grip/pose")},
    {ACT_RIGHT_STICK, R("thumbstick")}, {ACT_ZR, R("trigger/value")},
    {ACT_R, R("squeeze/value")}, {ACT_A, R("a/click")}, {ACT_B, R("b/click")}, {ACT_RIGHT_AIM, R("aim/pose")},
    {ACT_RIGHT_GRIP, R("grip/pose")},
};
static const Binding vive_bindings[] = {
    {ACT_LEFT_STICK, L("trackpad")}, {ACT_LSTICK, L("trackpad/click")}, {ACT_ZL, L("trigger/value")},
    {ACT_L, L("squeeze/click")}, {ACT_PLUS, L("menu/click")}, {ACT_LEFT_AIM, L("aim/pose")}, {ACT_LEFT_GRIP, L("grip/pose")},
    {ACT_RIGHT_STICK, R("trackpad")}, {ACT_A, R("trackpad/click")}, {ACT_ZR, R("trigger/value")},
    {ACT_R, R("squeeze/click")}, {ACT_RIGHT_AIM, R("aim/pose")}, {ACT_RIGHT_GRIP, R("grip/pose")},
};
static const Binding wmr_bindings[] = {
    {ACT_LEFT_STICK, L("thumbstick")}, {ACT_LSTICK, L("thumbstick/click")}, {ACT_ZL, L("trigger/value")},
    {ACT_L, L("squeeze/click")}, {ACT_PLUS, L("menu/click")}, {ACT_DPAD, L("trackpad")},
    {ACT_DPAD_PRESS, L("trackpad/click")}, {ACT_LEFT_AIM, L("aim/pose")}, {ACT_LEFT_GRIP, L("grip/pose")},
    {ACT_RIGHT_STICK, R("thumbstick")}, {ACT_ZR, R("trigger/value")},
    {ACT_R, R("squeeze/click")}, {ACT_MINUS, R("menu/click")}, {ACT_A, R("trackpad/click")},
    {ACT_RIGHT_AIM, R("aim/pose")}, {ACT_RIGHT_GRIP, R("grip/pose")},
};
static const Binding simple_bindings[] = {
    {ACT_ZL, L("select/click")}, {ACT_PLUS, L("menu/click")}, {ACT_LEFT_AIM, L("aim/pose")}, {ACT_LEFT_GRIP, L("grip/pose")},
    {ACT_A, R("select/click")}, {ACT_MINUS, R("menu/click")}, {ACT_RIGHT_AIM, R("aim/pose")}, {ACT_RIGHT_GRIP, R("grip/pose")},
};
#undef L
#undef R

static const struct { const char *profile; const Binding *bindings; uint32_t count; } profiles[] = {
    {"/interaction_profiles/valve/index_controller", index_bindings, sizeof index_bindings / sizeof *index_bindings},
    {"/interaction_profiles/oculus/touch_controller", touch_bindings, sizeof touch_bindings / sizeof *touch_bindings},
    {"/interaction_profiles/htc/vive_controller", vive_bindings, sizeof vive_bindings / sizeof *vive_bindings},
    {"/interaction_profiles/microsoft/motion_controller", wmr_bindings, sizeof wmr_bindings / sizeof *wmr_bindings},
    {"/interaction_profiles/khr/simple_controller", simple_bindings, sizeof simple_bindings / sizeof *simple_bindings},
};

static XrSession session = XR_NULL_HANDLE;
static XrActionSet action_set = XR_NULL_HANDLE;
static XrAction actions[ACT_COUNT];
static XrSpace aim_space[2], grip_space[2];

static bool ok(XrResult r, const char *what) {
    if (XR_SUCCEEDED(r)) return true;
    LOG("[vr] %s failed (%d)\n", what, (int)r);
    return false;
}

static bool suggest(XrInstance instance, uint32_t p) {
    XrActionSuggestedBinding b[32];
    uint32_t n = 0;
    for (uint32_t i = 0; i < profiles[p].count && n < 32; i++) {
        if (!ok(p_xrStringToPath(instance, profiles[p].bindings[i].path, &b[n].binding), profiles[p].bindings[i].path))
            return false;
        b[n++].action = actions[profiles[p].bindings[i].action];
    }
    XrPath profile;
    if (!ok(p_xrStringToPath(instance, profiles[p].profile, &profile), profiles[p].profile)) return false;
    XrInteractionProfileSuggestedBinding sb = {.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
                                               .interactionProfile = profile, .countSuggestedBindings = n,
                                               .suggestedBindings = b};
    return ok(p_xrSuggestInteractionProfileBindings(instance, &sb), profiles[p].profile);
}

bool vr_input_create(XrInstance instance, XrSession s) {
    XrActionSetCreateInfo si = {.type = XR_TYPE_ACTION_SET_CREATE_INFO};
    snprintf(si.actionSetName, sizeof si.actionSetName, "joycons");
    snprintf(si.localizedActionSetName, sizeof si.localizedActionSetName, "Joy-Cons (Skyward Sword)");
    if (!ok(p_xrCreateActionSet(instance, &si, &action_set), "xrCreateActionSet")) return false;
    for (int i = 0; i < ACT_COUNT; i++) {
        XrActionCreateInfo ai = {.type = XR_TYPE_ACTION_CREATE_INFO, .actionType = action_defs[i].type};
        snprintf(ai.actionName, sizeof ai.actionName, "%s", action_defs[i].name);
        snprintf(ai.localizedActionName, sizeof ai.localizedActionName, "%s", action_defs[i].label);
        if (!ok(p_xrCreateAction(action_set, &ai, &actions[i]), action_defs[i].name)) return false;
    }
    int suggested = 0;
    for (uint32_t p = 0; p < sizeof profiles / sizeof *profiles; p++) suggested += suggest(instance, p);
    if (!suggested) return false;
    XrSessionActionSetsAttachInfo at = {.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = 1,
                                        .actionSets = &action_set};
    if (!ok(p_xrAttachSessionActionSets(s, &at), "xrAttachSessionActionSets")) return false;
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo as = {.type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
                                      .poseInActionSpace.orientation.w = 1};
        as.action = actions[h ? ACT_RIGHT_AIM : ACT_LEFT_AIM];
        if (!ok(p_xrCreateActionSpace(s, &as, &aim_space[h]), "xrCreateActionSpace(aim)")) return false;
        as.action = actions[h ? ACT_RIGHT_GRIP : ACT_LEFT_GRIP];
        if (!ok(p_xrCreateActionSpace(s, &as, &grip_space[h]), "xrCreateActionSpace(grip)")) return false;
    }
    session = s;
    LOG("[vr] controller actions ready (%d of %d controller profiles bound)\n", suggested,
        (int)(sizeof profiles / sizeof *profiles));
    return true;
}

void vr_input_destroy(void) {
    sky_joycon_reset();
    for (int h = 0; h < 2; h++) {
        if (aim_space[h] && p_xrDestroySpace) p_xrDestroySpace(aim_space[h]);
        if (grip_space[h] && p_xrDestroySpace) p_xrDestroySpace(grip_space[h]);
        aim_space[h] = grip_space[h] = XR_NULL_HANDLE;
    }
    if (action_set && p_xrDestroyActionSet) p_xrDestroyActionSet(action_set);  // (its actions with it)
    action_set = XR_NULL_HANDLE;
    memset(actions, 0, sizeof actions);
    session = XR_NULL_HANDLE;
}

static bool get_bool(int a) {
    XrActionStateGetInfo gi = {.type = XR_TYPE_ACTION_STATE_GET_INFO, .action = actions[a]};
    XrActionStateBoolean st = {.type = XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(p_xrGetActionStateBoolean(session, &gi, &st)) && st.isActive && st.currentState;
}

static float get_float(int a) {
    XrActionStateGetInfo gi = {.type = XR_TYPE_ACTION_STATE_GET_INFO, .action = actions[a]};
    XrActionStateFloat st = {.type = XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(p_xrGetActionStateFloat(session, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
}

static XrVector2f get_vec2(int a) {
    XrActionStateGetInfo gi = {.type = XR_TYPE_ACTION_STATE_GET_INFO, .action = actions[a]};
    XrActionStateVector2f st = {.type = XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(p_xrGetActionStateVector2f(session, &gi, &st)) && st.isActive) return st.currentState;
    return (XrVector2f){0, 0};
}

static void locate_hand(XrSpace hand, XrSpace space, XrTime time, SkyVrHand *out) {
    XrSpaceVelocity vel = {.type = XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation loc = {.type = XR_TYPE_SPACE_LOCATION, .next = &vel};
    memset(out, 0, sizeof *out);
    if (!hand || XR_FAILED(p_xrLocateSpace(hand, space, time, &loc))) return;
    const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((loc.locationFlags & valid) != valid) return;
    out->tracked = true;
    out->orientation[0] = loc.pose.orientation.x; out->orientation[1] = loc.pose.orientation.y;
    out->orientation[2] = loc.pose.orientation.z; out->orientation[3] = loc.pose.orientation.w;
    out->position[0] = loc.pose.position.x; out->position[1] = loc.pose.position.y; out->position[2] = loc.pose.position.z;
    const XrSpaceVelocityFlags both = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
    out->velocity_valid = (vel.velocityFlags & both) == both;
    out->linear_velocity[0] = vel.linearVelocity.x; out->linear_velocity[1] = vel.linearVelocity.y;
    out->linear_velocity[2] = vel.linearVelocity.z;
    out->angular_velocity[0] = vel.angularVelocity.x; out->angular_velocity[1] = vel.angularVelocity.y;
    out->angular_velocity[2] = vel.angularVelocity.z;
}

void vr_input_sync(XrSpace space, XrTime time, const float reference[7]) {
    SkyVrControllers s = {.reference = {0, 0, 0, 1}};
    if (session && space && time > 0) {
        XrActiveActionSet active = {.actionSet = action_set};
        XrActionsSyncInfo sync = {.type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 1, .activeActionSets = &active};
        s.active = XR_SUCCEEDED(p_xrSyncActions(session, &sync));
    }
    if (s.active) {
        s.time_ns = time;
        if (reference) memcpy(s.reference, reference, sizeof s.reference);
        for (int a = ACT_A; a <= ACT_RSTICK; a++)
            if (get_bool(a)) s.buttons |= action_defs[a].button;
        if (get_bool(ACT_DPAD_PRESS)) {  // the pressed trackpad's direction as one d-pad button
            const XrVector2f d = get_vec2(ACT_DPAD);
            if (d.x * d.x + d.y * d.y > 0.09f)
                s.buttons |= fabsf(d.x) > fabsf(d.y) ? (d.x > 0 ? SKY_JC_RIGHT : SKY_JC_LEFT)
                                                     : (d.y > 0 ? SKY_JC_UP : SKY_JC_DOWN);
        }
        s.zl = get_float(ACT_ZL);
        s.zr = get_float(ACT_ZR);
        s.l = get_float(ACT_L);
        s.r = get_float(ACT_R);
        const XrVector2f ls = get_vec2(ACT_LEFT_STICK), rs = get_vec2(ACT_RIGHT_STICK);
        s.stick[0][0] = ls.x; s.stick[0][1] = ls.y;
        s.stick[1][0] = rs.x; s.stick[1][1] = rs.y;
        for (int h = 0; h < 2; h++) locate_hand(aim_space[h], space, time, &s.hand[h]);
    }
    {   // OpenXR actions bypass SDL, while the ImGui menu normally receives its Back/B action through SDL.
        // Bridge the rising edge explicitly so the menu can always be dismissed from inside the headset.
        static bool b_was_down;
        const bool b_down = (s.buttons & SKY_JC_B) != 0;
        extern bool menu_is_open(void);
        extern void menu_vr_close(void);
        if (b_down && !b_was_down && menu_is_open()) menu_vr_close();
        b_was_down = b_down;
    }
    vr_pointer_set_aim(s.active && s.hand[1].tracked, s.hand[1].orientation, s.hand[1].position);  // (menu pointer)
    // The game's motion sensors follow the aim poses (where each controller points); the grip poses (the controller
    // in the palm) place the wrist HUD.
    SkyVrHand grip[2] = {0};
    if (s.active)
        for (int h = 0; h < 2; h++) locate_hand(grip_space[h], space, time, &grip[h]);
    sky_joycon_update(&s);
    // In Auto mode, meaningful OpenXR use selects motion controls. Merely enabling VR never prevents using an SDL
    // gamepad in the headset; whichever source the player uses most recently wins.
    static bool motion_ready;
    const bool ready = s.active && s.hand[0].tracked && s.hand[1].tracked;
    float speed = 0.0f;
    for (int h = 0; h < 2; h++)
        if (s.hand[h].velocity_valid)
            for (int i = 0; i < 3; i++) speed += fabsf(s.hand[h].linear_velocity[i]) + .12f * fabsf(s.hand[h].angular_velocity[i]);
    const bool used = s.buttons || s.zl > .2f || s.zr > .2f || s.l > .2f || s.r > .2f ||
                      fabsf(s.stick[0][0]) > .25f || fabsf(s.stick[0][1]) > .25f ||
                      fabsf(s.stick[1][0]) > .25f || fabsf(s.stick[1][1]) > .25f || speed > .35f;
    if (ready && (!motion_ready || used)) {
        extern void motion_select_input(bool joycon);
        motion_select_input(true);
    }
    motion_ready = ready;
    float pose[2][7];
    for (int h = 0; h < 2; h++) {
        memcpy(pose[h], grip[h].orientation, 4 * sizeof(float));
        memcpy(pose[h] + 4, grip[h].position, 3 * sizeof(float));
    }
    vr_set_grip_poses(s.active && grip[0].tracked, pose[0], s.active && grip[1].tracked, pose[1]);

}

#endif
