// Panel-only UI calls, beside ui.h rather than in it: ui.h is shared with
// SigenStorPuck, where these settings apply at boot.

#pragma once

#include <stdint.h>

// Which screens the auto-cycle steps through, as PuckScreen bits. Applies at once:
// it only filters the cycle, unlike which screens exist, which needs a restart.
void ui_set_rotate_mask(uint8_t mask);
