#include <assert.h>
#include <stdio.h>
#include "../runtime/hle/motion_calibration.h"

int main(void) {
    unsigned state = 0, prompt_opens = 0, prompt_closes = 0, sound_events = 0, calibrated = 0, cleaned_up = 0;
    for (unsigned sample = 0; sample < 50; sample++) {
        state = motion_calibration_background_state(state);
        // This models prompt transitions only. The direct gyro reset has a separate sound (0x13bd);
        // this policy test cannot establish audio silence or uninterrupted game animation.
        switch (state) {
        case 0: prompt_opens++; sound_events++; state = 1; break;
        case 1: state = 2; break;
        case 2: if (sample == 45) { calibrated++; state = 3; } break;
        case 3: prompt_closes++; sound_events++; state = 4; break;
        case 4: state = 5; break;
        case 5: cleaned_up++; state = 0; break;
        }
        if (cleaned_up) break;
    }
    assert(!prompt_opens && !prompt_closes && !sound_events);
    assert(calibrated == 1 && cleaned_up == 1 && state == 0);
    assert(motion_calibration_background_state(1) == 2);  // already entering a prompt
    assert(motion_calibration_background_state(4) == 5);  // already leaving a prompt
    assert(motion_calibration_background_state(9) == 9);  // don't rewrite unknown/error states
    puts("calibration policy: prompt transitions skipped; calibration and cleanup preserved (audio/stalls not tested)");
}
