// Reads an EnViOT Display server for the Environment screen, on a task of its
// own: /api/live every minute, /api/stats/day every five. Panel only.
//
// Apart from the poller (which is shared with SigenStorPuck and reads the plant):
// EnViOT is a second server, set up alongside whichever data source is chosen.

#pragma once

#include <stdint.h>

#include "enviot.h"

// Starts the task. It reads nothing until an EnViOT server is set, and picks up
// a change of server or code at its next read.
void enviot_fetch_begin();

// Copies the latest readings into `out` if they changed since `*generation`, and
// updates it. False, with `out` untouched, if nothing new.
bool enviot_fetch_take(EnvReadings* out, uint32_t* generation);

// Reads again now: after the server or code was set.
void enviot_fetch_wake();
