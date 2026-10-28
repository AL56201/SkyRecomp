// VR motion controllers -> a pair of Joy-Cons, as Skyward Sword HD's motion controls read them (runtime/hle/motion.c).
//
// Each hand becomes one Joy-Con (left hand: Joy-Con (L), right hand: Joy-Con (R)) held the way the game expects:
// the controller's pointing direction is the Joy-Con's +y axis. The game then recognises swings, thrusts, shield
// bashes and pointing itself, from physically consistent sensor data:
//   direction       the Joy-Con's axes in the player's frame (x right, y forward, z up), as rows; forward is the
//                   direction the player faced when VR tracking started (the game camera's forward)
//   gyro            angular velocity in the Joy-Con's own axes, right-handed, revolutions per second
//   accel           acceleration in g as the Joy-Con reports it: (0, 0, -1) lying flat at rest (gravity's
//                   direction), linear acceleration entering with the opposite sign
//   angle           the integrated gyro, revolutions
// Measured against the game (2026-10-25): turning the right Joy-Con about its z axis moves the pointer
// horizontally, about x vertically; the pointer follows `direction`, not the integrated gyro.
//
// Buttons come in already mapped to Joy-Con buttons (runtime/vr_input.c names its actions after them); analog
// triggers and grips become ZL / ZR / L / R with hysteresis here.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Npad button bits (nn::hid::NpadButton), the same in runtime/hle/input.c.
enum {
    SKY_JC_A = 1u << 0, SKY_JC_B = 1u << 1, SKY_JC_X = 1u << 2, SKY_JC_Y = 1u << 3,
    SKY_JC_LSTICK = 1u << 4, SKY_JC_RSTICK = 1u << 5, SKY_JC_L = 1u << 6, SKY_JC_R = 1u << 7,
    SKY_JC_ZL = 1u << 8, SKY_JC_ZR = 1u << 9, SKY_JC_PLUS = 1u << 10, SKY_JC_MINUS = 1u << 11,
    SKY_JC_LEFT = 1u << 12, SKY_JC_UP = 1u << 13, SKY_JC_RIGHT = 1u << 14, SKY_JC_DOWN = 1u << 15,
};

typedef struct SkyVrHand {
    bool tracked;               // orientation valid this frame
    bool velocity_valid;        // linear and angular velocity valid
    float orientation[4];       // x, y, z, w: the controller's pointing pose (OpenXR aim pose) in local space
    float position[3];          // metres
    float linear_velocity[3];   // m/s, local space
    float angular_velocity[3];  // rad/s, local space
} SkyVrHand;

typedef struct SkyVrControllers {
    bool active;          // the actions synced this frame
    int64_t time_ns;      // the sample's display time
    float reference[4];   // head orientation the game camera stands for (only its yaw is used); {0,0,0,1}: none
    SkyVrHand hand[2];    // 0 left, 1 right
    uint32_t buttons;     // SKY_JC_* from digital actions
    float zl, zr, l, r;   // analog trigger / grip values 0..1
    float stick[2][2];    // left, right: x, y in -1..1
} SkyVrControllers;

// nn::hid::SixAxisSensorState's payload, see above.
typedef struct SkyJoyConSensor {
    float accel[3], gyro[3], angle[3], direction[9];
    int64_t delta_ns;
} SkyJoyConSensor;

typedef struct SkyJoyConState {
    uint32_t buttons;     // SKY_JC_*
    float stick[2][2];    // left, right
} SkyJoyConState;

// Renderer thread: one sample per frame. The getters may run on any thread (the game's).
void sky_joycon_update(const SkyVrControllers *sample);
bool sky_joycon_state(SkyJoyConState *out);                  // false: no VR controllers
bool sky_joycon_sensor(int device, SkyJoyConSensor *out);   // device 0 left, 1 right; false: not tracked yet
void sky_joycon_reset(void);

#ifdef __cplusplus
}

#include <array>

namespace skyrecomp::vr {

class JoyConTranslator final {
public:
    using Vec3 = std::array<float, 3>;
    using Quat = std::array<float, 4>;  // x, y, z, w
    using Mat3 = std::array<float, 9>;  // row-major

    static constexpr float TriggerPress = 0.55f, TriggerRelease = 0.45f;
    static constexpr float GripPress = 0.75f, GripRelease = 0.60f;
    static constexpr float StickDeadzone = 0.12f;
    static constexpr float MaxAccelG = 8.0f;     // Joy-Con accelerometer range
    static constexpr float MaxGyroRps = 5.5f;    // Joy-Con gyroscope range (~2000 deg/s)
    static constexpr float AccelSmoothing = 0.5f;  // exponential smoothing of the differentiated velocity
    static constexpr float RestAngularSpeed = 0.08f; // rad/s
    static constexpr float RestLinearSpeed = 0.04f;  // m/s
    static constexpr float RestSettleSeconds = 0.40f;

    void update(const SkyVrControllers &sample) noexcept;
    void reset() noexcept;
    [[nodiscard]] const SkyJoyConState &state() const noexcept { return state_; }
    [[nodiscard]] bool sensor(int device, SkyJoyConSensor &out) const noexcept;

    // The pure parts, for the tests.
    // Yaw-only rotation of a head orientation (OpenXR: +y up, -z forward).
    [[nodiscard]] static Quat yawOnly(const Quat &head) noexcept;
    // `direction` rows for a controller orientation, relative to the reference yaw.
    [[nodiscard]] static Mat3 direction(const Quat &controller, const Quat &referenceYaw) noexcept;
    // A local-space vector (angular velocity, acceleration) in the Joy-Con's own axes.
    [[nodiscard]] static Vec3 toJoyCon(const Vec3 &local, const Quat &controller) noexcept;

private:
    struct Hand {
        bool valid{};
        bool haveVelocity{};
        Vec3 velocity{};      // local space, previous sample
        Vec3 accel{};         // local space, smoothed
        Vec3 angle{};         // revolutions
        Vec3 gyroBias{};      // stationary OpenXR angular-velocity bias, local space
        float restTime{};
        SkyJoyConSensor sensor{};
    };
    static float deadzone(float v) noexcept;
    void updateHand(int index, const SkyVrHand &in, const Quat &referenceYaw, float dt) noexcept;

    Hand hands_[2]{};
    SkyJoyConState state_{};
    bool zl_{}, zr_{}, l_{}, r_{};
    int64_t lastTime_{};
};

}  // namespace skyrecomp::vr
#endif
