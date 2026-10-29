// Which of the panel's sounds a run of readings calls for.
//
// This repo's own; SigenStorPuck has no speaker. Deliberately free of Arduino
// headers so the sim can check it (src/sim/panel_selftest.cpp). The speaker
// itself is src/device/sound.cpp.

#pragma once

#include <stdint.h>

#include "snapshot.h"

// The sounds. Each has a bit in the saved mask (Settings::sound_mask), so the
// values are stored: append only, never renumber.
enum class SoundCue : uint8_t {
  Boot = 0,
  Tap = 1,
  BatteryFull = 2,
  BatteryReserve = 3,
  EvStart = 4,
  EvStop = 5,
  Preview = 6,  // the volume slider's blip; always allowed
};

constexpr uint8_t sound_bit(SoundCue cue) {
  return static_cast<uint8_t>(1u << static_cast<uint8_t>(cue));
}

// Which switch on the Sound page each cue answers to. EV start and stop share
// one, as the battery's two do not: full is good news, reserve a warning.
constexpr uint8_t sound_switch_bit(SoundCue cue) {
  return cue == SoundCue::EvStop ? sound_bit(SoundCue::EvStart) : sound_bit(cue);
}

// Battery full at this level, and not again until it has dropped below the
// re-arm level: a battery sitting at 99-100% would otherwise chime all day.
static constexpr float SOUND_FULL_PCT = 99.5f;
static constexpr float SOUND_FULL_REARM_PCT = 97.0f;
// Reserve re-arms this far above the set level, for the same reason.
static constexpr float SOUND_RESERVE_REARM_MARGIN_PCT = 3.0f;

// An EV is charging once it has drawn at least START for START_MS, and has
// stopped once it has drawn under STOP for STOP_MS. Chargers ramp, pause and
// balance, so a single reading either way proves nothing.
static constexpr float SOUND_EV_START_KW = 1.0f;
static constexpr float SOUND_EV_STOP_KW = 0.2f;
static constexpr uint32_t SOUND_EV_START_MS = 30000;
static constexpr uint32_t SOUND_EV_STOP_MS = 60000;

class SoundEvents {
 public:
  // Feeds one fresh reading. Returns the cues it fired, as sound_bit()s. The
  // first reading after start-up fires nothing: it only learns where things
  // stand, so a panel that boots beside a full battery stays quiet.
  uint8_t update(const Snapshot& reading, uint8_t reserve_pct, uint32_t now_ms);

 private:
  bool seen_soc_ = false;
  bool full_armed_ = false;
  bool reserve_armed_ = false;

  bool seen_ev_ = false;
  bool ev_charging_ = false;
  bool ev_pending_ = false;      // drawing the other way since ev_since_ms_
  uint32_t ev_since_ms_ = 0;
};
