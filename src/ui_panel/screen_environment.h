// Screen, panel only: an EnViOT Display server's sensors, up to four cards of a
// temperature gauge with its last-24-hour range, two more readings and a battery
// bar. Built only when an EnViOT server is set (UiConfig::with_environment_screen).

#pragma once

#include <lvgl.h>

#include "enviot.h"

lv_obj_t* screen_environment_create(lv_obj_t* parent);

// `elapsed_s`: seconds since the readings arrived, added to their `now` so "8 min
// ago" keeps counting between reads.
void screen_environment_update(const EnvReadings& readings, uint32_t elapsed_s);
