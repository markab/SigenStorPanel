// Panel-only UI calls, beside ui.h rather than in it: ui.h is shared with
// SigenStorPuck, where these settings apply at boot.

#pragma once

#include <stdint.h>

#include "enviot.h"

// Which screens the auto-cycle steps through, as PuckScreen bits. Applies at once:
// it only filters the cycle, unlike which screens exist, which needs a restart.
void ui_set_rotate_mask(uint16_t mask);  // PuckScreenMask

// What the on-screen day arrows do, given -1 for back and +1 for forward. The
// owner supplies it so a tap does what a button press does: on the device that
// includes waking the poll task to fetch the day (buttons_step_day()).
using UiDayStep = void (*)(int delta);
void ui_set_day_step(UiDayStep step);

// The EnViOT Display readings for the Environment screen, whenever a read
// finishes, good or not (EnvReadings::status). Ignored when the screen was not
// built (UiConfig::with_environment_screen).
void ui_update_environment(const EnvReadings& readings);
