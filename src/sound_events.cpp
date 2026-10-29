#include "sound_events.h"

uint8_t SoundEvents::update(const Snapshot& reading, uint8_t reserve_pct, uint32_t now_ms) {
  uint8_t fired = 0;

  // Unknown is not empty: a reading without a state of charge changes nothing.
  const MaybeFloat& soc = reading.battery.soc_pct;
  if (soc.known) {
    const float reserve = static_cast<float>(reserve_pct);
    if (!seen_soc_) {
      seen_soc_ = true;
      full_armed_ = soc.value < SOUND_FULL_PCT;
      reserve_armed_ = soc.value > reserve;
    } else {
      if (full_armed_ && soc.value >= SOUND_FULL_PCT) {
        fired |= sound_bit(SoundCue::BatteryFull);
        full_armed_ = false;
      } else if (!full_armed_ && soc.value < SOUND_FULL_REARM_PCT) {
        full_armed_ = true;
      }
      // 0% would mean a battery already flat; nothing to warn about there.
      if (reserve_pct > 0) {
        if (reserve_armed_ && soc.value <= reserve) {
          fired |= sound_bit(SoundCue::BatteryReserve);
          reserve_armed_ = false;
        } else if (!reserve_armed_ && soc.value >= reserve + SOUND_RESERVE_REARM_MARGIN_PCT) {
          reserve_armed_ = true;
        }
      }
    }
  }

  const MaybeFloat& ev = reading.power.ev;
  if (ev.known) {
    if (!seen_ev_) {
      seen_ev_ = true;
      ev_charging_ = ev.value >= SOUND_EV_START_KW;
      ev_pending_ = false;
    } else {
      const bool other_way =
          ev_charging_ ? ev.value < SOUND_EV_STOP_KW : ev.value >= SOUND_EV_START_KW;
      if (!other_way) {
        ev_pending_ = false;
      } else if (!ev_pending_) {
        ev_pending_ = true;
        ev_since_ms_ = now_ms;
      } else if (now_ms - ev_since_ms_ >= (ev_charging_ ? SOUND_EV_STOP_MS : SOUND_EV_START_MS)) {
        ev_charging_ = !ev_charging_;
        ev_pending_ = false;
        fired |= sound_bit(ev_charging_ ? SoundCue::EvStart : SoundCue::EvStop);
      }
    }
  }

  return fired;
}
