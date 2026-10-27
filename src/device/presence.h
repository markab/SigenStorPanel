// An optional mmWave presence sensor (Waveshare HMMD) on the UART1 header.
//
// This repo's own. The bytes are parsed by src/presence_parse.cpp, which the sim
// checks; this file only owns the port and the clock. With no sensor fitted
// nothing ever arrives, `connected` stays false, and nothing acts on it.

#pragma once

#include <stdint.h>

struct PresenceStatus {
  // A reading arrived within PUCK_PRESENCE_LOST_MS.
  bool connected = false;
  bool present = false;
  // millis() of the last reading that saw someone; boot until then, so an empty
  // room is timed from power-on rather than treated as empty for ever.
  uint32_t last_present_ms = 0;
  bool has_distance = false;
  uint16_t distance = 0;
};

void presence_begin();

// Drains the UART. Call from loop().
void presence_loop();

PresenceStatus presence_status();
