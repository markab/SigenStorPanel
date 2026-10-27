// The panel's additions to updater.h, which is kept verbatim with SigenStorPuck.
// Tracked in UPSTREAM.md, "Waiting on upstream": if the Puck takes these too,
// they move into updater.h and this file goes.

#pragma once

#include <stdint.h>

// How far an install has got, 0-100, or -1 when none is running or the size is
// not known yet. Written by the poll task, read by the UI; a stale read is one
// step behind, which a progress bar does not mind.
int8_t updater_progress_percent();
