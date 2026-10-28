// Home Assistant's sensors, as a list to choose from on the panel.
//
// Which sensor supplies each role (PV power, battery SOC, today's import and the
// rest, HA_ENTITIES in home_assistant.h) used to be typed as entity IDs on the
// web page. On the panel they are picked from a list instead, and the list comes
// from Home Assistant itself: HA_CATALOG_TEMPLATE is rendered by its
// /api/template endpoint into one line per sensor, filtered by Home Assistant to
// the units the panel can read, so the response is a few KB rather than the whole
// of /api/states.
//
// Lives outside src/device/ on purpose, like presence_parse.cpp: only the request
// is Arduino-only, and the parse and the per-role filter are what want testing.

#pragma once

#include <stddef.h>

#include "home_assistant.h"

// The most sensors kept. The template stops at the same number, so a large Home
// Assistant does not send more than the panel will hold.
static constexpr size_t HA_CATALOG_MAX = 300;

struct HaCandidate {
  char entity_id[HA_ENTITY_ID_MAX] = {};
  char name[48] = {};   // Home Assistant's friendly name, cut to fit
  char unit[8] = {};    // as Home Assistant gives it; empty for an on/off sensor
  char state[16] = {};  // the current value, so the list shows which one is live
};

// The Jinja template for /api/template. One line per sensor:
//   entity_id <TAB> unit <TAB> friendly name <TAB> state <LF>
extern const char HA_CATALOG_TEMPLATE[];

// Fills `out` from a rendered template, returning how many it holds. Lines that
// are malformed, or whose entity ID is not one, are skipped.
size_t ha_catalog_parse(const char* text, size_t length, HaCandidate* out, size_t max);

// Whether a sensor can supply a role of this kind: its unit is one the panel
// converts (power in W or kW, energy in Wh or kWh, and so on), or for an on/off
// role, it has no unit at all.
bool ha_catalog_suits(HaValueKind kind, const HaCandidate& candidate);
