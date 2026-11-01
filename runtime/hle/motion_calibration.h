#pragma once
#include <stdint.h>

// main+0xdd2a68: 0 opens SYS_GYRO_00, 1 waits for its animation, 2 calibrates, 3 closes the prompt,
// 4 waits for its animation, 5 restores game state. VR never requests the message or its UI sounds.
static inline uint32_t motion_calibration_background_state(uint32_t state) {
    if (state <= 1) return 2;
    if (state == 3 || state == 4) return 5;
    return state;
}
