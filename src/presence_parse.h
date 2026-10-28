// Reading the Waveshare HMMD mmWave presence sensor's UART output.
//
// This repo's own; SigenStorPuck has no sensor. Deliberately free of Arduino
// headers so the sim can check it (src/sim/panel_selftest.cpp).
//
// The sensor (a Silicon Micro mmWave SoC behind a PY32 MCU) talks 115200 8N1. Out
// of the box it is in "normal" mode, which sends text lines: "ON" or "OFF" for
// presence, with a "Range <n>" line alongside while someone is there. It can be
// switched to "report" mode, which sends binary frames instead:
//
//   F4 F3 F2 F1 | length (2, LE) | presence (1) | distance (2, LE) |
//   16 gate energies (2 each) | F8 F7 F6 F5
//
// Both are understood, so a sensor someone has reconfigured with Waveshare's
// tool still works. The panel never sends it a command: the write commands are
// not documented, and none are needed to read presence. Source:
// https://www.waveshare.com/wiki/HMMD_mmWave_Sensor (read 2026-10-27).

#pragma once

#include <stddef.h>
#include <stdint.h>

struct PresenceReading {
  bool present = false;
  // Distance to the target in the sensor's own unit (cm in report mode, the
  // "Range" figure in normal mode), when it gave one.
  bool has_distance = false;
  uint16_t distance = 0;
};

class PresenceParser {
 public:
  // Feeds one byte. True when it completed a reading, written to `out`.
  bool feed(uint8_t byte, PresenceReading* out);

 private:
  bool finish_line(PresenceReading* out);

  // Normal mode: one text line at a time.
  char line_[24] = {};
  size_t line_length_ = 0;
  bool line_overflow_ = false;
  bool present_ = false;
  bool has_distance_ = false;
  uint16_t distance_ = 0;

  // Report mode: a frame at a time.
  enum class Frame : uint8_t { Hunting, Length, Body };
  Frame frame_ = Frame::Hunting;
  uint8_t header_matched_ = 0;
  uint8_t body_[48] = {};
  size_t frame_length_ = 0;
  size_t frame_received_ = 0;
  uint8_t length_bytes_[2] = {};
  uint8_t length_received_ = 0;
};
