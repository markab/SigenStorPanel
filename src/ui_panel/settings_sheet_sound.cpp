// The settings sheet's Sound page: volume, which sounds play, and the battery
// reserve level the reserve sound plays at.
//
// Every change saves at once, as the Display page's do, and is heard as it is
// made: the volume plays a blip on release, and a sound switched on plays itself.

#include <stdio.h>

#include "settings_sheet_internal.h"

namespace sheet_kit {

namespace {

// The volume moves in fives; finer than that cannot be heard.
constexpr int32_t VOLUME_STEP = 5;
// The reserve level, 5% to 50% in fives. A Sigenergy reserve is set in the same
// steps in its app.
constexpr int32_t RESERVE_STEP = 5;
constexpr int32_t RESERVE_MIN = 5;
constexpr int32_t RESERVE_MAX = 50;

SheetSound s_sound;

lv_obj_t* s_volume = nullptr;
lv_obj_t* s_volume_value = nullptr;
lv_obj_t* s_full = nullptr;
lv_obj_t* s_reserve = nullptr;
lv_obj_t* s_reserve_level = nullptr;
lv_obj_t* s_reserve_value = nullptr;
lv_obj_t* s_boot = nullptr;
lv_obj_t* s_taps = nullptr;
lv_obj_t* s_ev = nullptr;

struct Switch {
  lv_obj_t** toggle;
  SoundCue cue;
};
const Switch SWITCHES[] = {
    {&s_full, SoundCue::BatteryFull}, {&s_reserve, SoundCue::BatteryReserve},
    {&s_boot, SoundCue::Boot},        {&s_taps, SoundCue::Tap},
    {&s_ev, SoundCue::EvStart},
};

void refresh() {
  char line[16];
  if (!lv_obj_has_state(s_volume, LV_STATE_PRESSED)) {
    lv_slider_set_value(s_volume, s_sound.volume / VOLUME_STEP, LV_ANIM_OFF);
  }
  snprintf(line, sizeof(line), s_sound.volume == 0 ? "Off" : "%u%%", s_sound.volume);
  lv_label_set_text(s_volume_value, line);

  for (const Switch& entry : SWITCHES) {
    set_checked(*entry.toggle, (s_sound.on & sound_bit(entry.cue)) != 0);
  }

  if (!lv_obj_has_state(s_reserve_level, LV_STATE_PRESSED)) {
    lv_slider_set_value(s_reserve_level, s_sound.reserve_pct / RESERVE_STEP, LV_ANIM_OFF);
  }
  snprintf(line, sizeof(line), "%u%%", s_sound.reserve_pct);
  lv_label_set_text(s_reserve_value, line);
  set_disabled(s_reserve_level, (s_sound.on & sound_bit(SoundCue::BatteryReserve)) == 0);
}

void save(SoundCue preview) {
  if (sheet_actions().set_sound != nullptr) {
    sheet_actions().set_sound(s_sound);
  }
  if (sheet_actions().preview_sound != nullptr) {
    sheet_actions().preview_sound(preview);
  }
}

void on_volume(lv_event_t* event) {
  s_sound.volume = static_cast<uint8_t>(lv_slider_get_value(s_volume) * VOLUME_STEP);
  if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
    save(SoundCue::Preview);
  }
  refresh();
}

void on_reserve_level(lv_event_t* event) {
  s_sound.reserve_pct = static_cast<uint8_t>(lv_slider_get_value(s_reserve_level) * RESERVE_STEP);
  if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
    save(SoundCue::BatteryReserve);
  }
  refresh();
}

void on_switch(lv_event_t* event) {
  const auto* entry = static_cast<const Switch*>(lv_event_get_user_data(event));
  const bool on = lv_obj_has_state(*entry->toggle, LV_STATE_CHECKED);
  if (on) {
    s_sound.on |= sound_bit(entry->cue);
  } else {
    s_sound.on &= static_cast<uint8_t>(~sound_bit(entry->cue));
  }
  refresh();
  if (sheet_actions().set_sound != nullptr) {
    sheet_actions().set_sound(s_sound);
  }
  // Heard as it is switched on, so you know what you have chosen.
  if (on && sheet_actions().preview_sound != nullptr) {
    sheet_actions().preview_sound(entry->cue);
  }
}

lv_obj_t* switch_for(lv_obj_t* parent, const char* name, lv_obj_t** toggle) {
  *toggle = labelled_toggle(parent, name);
  for (const Switch& entry : SWITCHES) {
    if (entry.toggle == toggle) {
      lv_obj_add_event_cb(*toggle, on_switch, LV_EVENT_VALUE_CHANGED,
                          const_cast<Switch*>(&entry));
    }
  }
  return *toggle;
}

}  // namespace

void sound_build_page(lv_obj_t* tile) {
  lv_obj_t* body = page_body(tile);

  lv_obj_t* left = card(body, 560, CARD_H);
  caption(left, "SPEAKER");
  s_volume = labelled_slider(left, "Volume", 0, 100 / VOLUME_STEP, &s_volume_value);
  lv_obj_add_event_cb(s_volume, on_volume, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(s_volume, on_volume, LV_EVENT_RELEASED, nullptr);
  divider(left);
  caption(left, "BATTERY");
  switch_for(left, "Full", &s_full);
  switch_for(left, "Down to its reserve", &s_reserve);
  s_reserve_level = labelled_slider(left, "Reserve", RESERVE_MIN / RESERVE_STEP,
                                    RESERVE_MAX / RESERVE_STEP, &s_reserve_value);
  lv_obj_set_style_opa(s_reserve_level, LV_OPA_40, LV_STATE_DISABLED);
  lv_obj_add_event_cb(s_reserve_level, on_reserve_level, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(s_reserve_level, on_reserve_level, LV_EVENT_RELEASED, nullptr);
  hint(left, "Set it to match the reserve in the Sigen app.");

  lv_obj_t* right = card(body, 360, CARD_H);
  caption(right, "PANEL");
  switch_for(right, "When it starts", &s_boot);
  switch_for(right, "Taps", &s_taps);
  divider(right);
  caption(right, "CAR CHARGING");
  switch_for(right, "Starts and stops", &s_ev);
  spacer(right);
  hint(right, "Silent in the overnight hours, and while the room sensor sees nobody.");

  refresh();
}

void sound_set(const SheetSound& sound) {
  s_sound = sound;
  if (s_volume != nullptr) {
    refresh();
  }
}

}  // namespace sheet_kit
