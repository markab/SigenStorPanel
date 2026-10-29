// The board profile: every pin and tunable for the target board.
//
// Adapted from SigenStorPuck (see UPSTREAM.md); not drift-tracked.
//
// This file selects one hardware profile at compile time and includes it.
// Nothing else in the tree carries a hardware constant, and nothing else
// includes a board_*.h profile directly — they all include "board_config.h"
// (put on the include path by -I src). Adding a board is a new profile header
// plus a case here; no downstream include changes. See CLAUDE.md.

#pragma once

#include <lvgl.h>
#include <stdint.h>

// --------------------------------------------------------------- firmware ---

// Shared across every board variant: one repo, one version. package_release.sh
// greps this file for this line, so keep it here in board_config.h.
#define PUCK_FW_VERSION "0.1.2"

// -------------------------------------------------------- profile selection ---

// One board today. The dispatch stays so a second panel (or the CrowPanel V1.0,
// whose WiFi wiring differs) is a new profile plus a case here.
#include "board_crowpanel_101.h"  // Elecrow CrowPanel Advanced 10.1" ESP32-P4 (1024x600)
