// Fetches Home Assistant's sensor list for the settings sheet's picker
// (src/ha_catalog.h), on a task of its own.
//
// Not on the UI thread: a request to Home Assistant can take seconds, and
// network work there froze the screen once (CLAUDE.md). Not on the poll task
// either: poller.cpp is shared with SigenStorPuck, and this is the panel's alone.
// The task exists only while a fetch runs.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ha_catalog.h"

enum class HaCatalogState : uint8_t {
  Idle,     // never asked for
  Loading,
  Ready,
  Failed,
};

// Starts a fetch with the stored Home Assistant address and token, unless one
// is already running.
void ha_catalog_request();

// Where the fetch has got to, and on failure why, in a few words.
HaCatalogState ha_catalog_state(char* message, size_t size);

// Bumped each time a fetch finishes, so a reader can tell a new list from the one
// it already has.
uint32_t ha_catalog_generation();

// Copies up to `max` sensors into `out`, returning how many it copied.
size_t ha_catalog_copy(HaCandidate* out, size_t max);
