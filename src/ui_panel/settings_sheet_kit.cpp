#include <stdio.h>
#include <string.h>

#include "settings_sheet_internal.h"

namespace sheet_kit {

// "Tap again to confirm": one button armed at a time.
lv_obj_t* s_armed = nullptr;
char s_armed_idle_text[32] = {};
lv_color_t s_armed_idle_colour;
lv_timer_t* s_armed_timer = nullptr;

// A confirmed button showing that the tap landed, until what it started takes
// over the screen. One at a time.
lv_obj_t* s_acked = nullptr;
char s_acked_idle_text[32] = {};
lv_color_t s_acked_idle_colour;
lv_color_t s_acked_idle_text_colour;
lv_timer_t* s_acked_timer = nullptr;

// ----------------------------------------------------------- small helpers ---

lv_obj_t* plain(lv_obj_t* parent) {
  lv_obj_t* obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  return obj;
}

lv_obj_t* text(lv_obj_t* parent, const lv_font_t* font, uint32_t colour, const char* value) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(colour), LV_PART_MAIN);
  lv_label_set_text(label, value);
  return label;
}

// The same small spaced capitals the data screens use for their headings.
lv_obj_t* caption(lv_obj_t* parent, const char* value) {
  lv_obj_t* label = text(parent, FONT_CAPTION, PUCK_COLOUR_MUTED, value);
  lv_obj_set_style_text_letter_space(label, 2, LV_PART_MAIN);
  return label;
}

lv_obj_t* hint(lv_obj_t* parent, const char* value) {
  lv_obj_t* label = text(parent, FONT_HINT, PUCK_COLOUR_MUTED, value);
  lv_obj_set_width(label, LV_PCT(100));
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  return label;
}

lv_obj_t* card(lv_obj_t* parent, lv_coord_t width, lv_coord_t height) {
  lv_obj_t* obj = plain(parent);
  lv_obj_set_size(obj, width, height);
  lv_obj_set_style_bg_color(obj, lv_color_hex(COLOUR_CARD), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(obj, 20, LV_PART_MAIN);
  lv_obj_set_style_pad_all(obj, CARD_PAD, LV_PART_MAIN);
  lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(obj, 12, LV_PART_MAIN);
  return obj;
}

// A label on the left, a value or control on the right.
lv_obj_t* row(lv_obj_t* parent) {
  lv_obj_t* obj = plain(parent);
  lv_obj_set_size(obj, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  return obj;
}

lv_obj_t* spacer(lv_obj_t* parent) {
  lv_obj_t* obj = plain(parent);
  lv_obj_set_width(obj, LV_PCT(100));
  lv_obj_set_flex_grow(obj, 1);
  return obj;
}

lv_obj_t* divider(lv_obj_t* parent) {
  lv_obj_t* obj = plain(parent);
  lv_obj_set_size(obj, LV_PCT(100), 1);
  lv_obj_set_style_bg_color(obj, lv_color_hex(PUCK_COLOUR_TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
  return obj;
}

// Sliders and switches are dragged, and a drag that wanders 50 px vertically
// would otherwise read as the swipe that closes the sheet.
void keep_gestures(lv_obj_t* obj) {
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

lv_obj_t* slider(lv_obj_t* parent, int32_t min, int32_t max) {
  lv_obj_t* obj = lv_slider_create(parent);
  lv_obj_set_size(obj, LV_PCT(100), 10);
  lv_slider_set_range(obj, min, max);
  lv_obj_set_style_bg_color(obj, lv_color_hex(COLOUR_CONTROL), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(obj, lv_color_hex(COLOUR_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(obj, lv_color_hex(PUCK_COLOUR_TEXT), LV_PART_KNOB);
  lv_obj_set_style_pad_all(obj, 10, LV_PART_KNOB);
  lv_obj_set_style_shadow_width(obj, 0, LV_PART_KNOB);
  // A taller hit area than a 10 px bar gives a finger.
  lv_obj_set_ext_click_area(obj, 18);
  keep_gestures(obj);
  return obj;
}

lv_obj_t* toggle(lv_obj_t* parent) {
  lv_obj_t* obj = lv_switch_create(parent);
  lv_obj_set_size(obj, 64, 34);
  lv_obj_set_style_bg_color(obj, lv_color_hex(COLOUR_CONTROL), LV_PART_MAIN);
  lv_obj_set_style_bg_color(obj, lv_color_hex(COLOUR_ACCENT),
                            static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(obj, lv_color_hex(PUCK_COLOUR_TEXT), LV_PART_KNOB);
  lv_obj_set_style_opa(obj, LV_OPA_40, LV_STATE_DISABLED);
  keep_gestures(obj);
  return obj;
}

void set_checked(lv_obj_t* obj, bool on) {
  if (on) {
    lv_obj_add_state(obj, LV_STATE_CHECKED);
  } else {
    lv_obj_clear_state(obj, LV_STATE_CHECKED);
  }
}

void set_disabled(lv_obj_t* obj, bool disabled) {
  if (disabled) {
    lv_obj_add_state(obj, LV_STATE_DISABLED);
  } else {
    lv_obj_clear_state(obj, LV_STATE_DISABLED);
  }
}

void set_hidden(lv_obj_t* obj, bool hidden) {
  if (hidden) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
  }
}

lv_obj_t* button(lv_obj_t* parent, const char* label, uint32_t background, uint32_t foreground) {
  lv_obj_t* obj = lv_btn_create(parent);
  lv_obj_set_height(obj, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(obj, lv_color_hex(background), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(obj, 14, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(obj, 22, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(obj, 12, LV_PART_MAIN);
  lv_obj_set_style_opa(obj, LV_OPA_40, LV_STATE_DISABLED);
  lv_obj_t* inner = text(obj, FONT_TEXT, foreground, label);
  lv_obj_center(inner);
  return obj;
}

void button_set_text(lv_obj_t* obj, const char* label) {
  lv_label_set_text(lv_obj_get_child(obj, 0), label);
}

// Four bars, lit by signal strength. Drawn rather than taken from a symbol
// font: the Montserrat subsets carry no icons.
lv_obj_t* signal_bars(lv_obj_t* parent) {
  lv_obj_t* obj = plain(parent);
  lv_obj_set_size(obj, 30, 22);
  for (int i = 0; i < 4; ++i) {
    lv_obj_t* bar = plain(obj);
    const lv_coord_t height = 7 + i * 5;
    lv_obj_set_size(bar, 5, height);
    lv_obj_set_pos(bar, i * 8, 22 - height);
    lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
  }
  return obj;
}

int bars_for(int8_t rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

const char* signal_word(int8_t rssi) {
  switch (bars_for(rssi)) {
    case 4:
      return "Excellent";
    case 3:
      return "Good";
    case 2:
      return "Fair";
    default:
      return "Weak";
  }
}

void set_bars(lv_obj_t* bars, int8_t rssi, bool present) {
  const int lit = present ? bars_for(rssi) : 0;
  for (int i = 0; i < 4; ++i) {
    lv_obj_set_style_bg_color(lv_obj_get_child(bars, i),
                              lv_color_hex(i < lit ? PUCK_COLOUR_TEXT : COLOUR_CONTROL),
                              LV_PART_MAIN);
  }
}

void format_bytes(char* out, size_t size, uint32_t bytes) {
  if (bytes >= 1024u * 1024u) {
    snprintf(out, size, "%.1f MB", bytes / (1024.0 * 1024.0));
  } else {
    snprintf(out, size, "%u KB", static_cast<unsigned>(bytes / 1024u));
  }
}

void format_duration(char* out, size_t size, uint32_t seconds) {
  if (seconds < 60) {
    snprintf(out, size, "%u s", static_cast<unsigned>(seconds));
  } else if (seconds < 3600) {
    snprintf(out, size, "%u min", static_cast<unsigned>(seconds / 60));
  } else if (seconds < 86400) {
    snprintf(out, size, "%u h %u min", static_cast<unsigned>(seconds / 3600),
             static_cast<unsigned>(seconds % 3600 / 60));
  } else {
    snprintf(out, size, "%u d %u h", static_cast<unsigned>(seconds / 86400),
             static_cast<unsigned>(seconds % 86400 / 3600));
  }
}

void format_step(char* out, size_t size, uint32_t seconds) {
  if (seconds == 0) {
    snprintf(out, size, "Never");
  } else if (seconds < 60) {
    snprintf(out, size, "%u s", static_cast<unsigned>(seconds));
  } else {
    snprintf(out, size, "%u min", static_cast<unsigned>(seconds / 60));
  }
}

void format_clock(char* out, size_t size, uint16_t minutes) {
  snprintf(out, size, "%02u:%02u", static_cast<unsigned>(minutes / 60 % 24),
           static_cast<unsigned>(minutes % 60));
}


// Brightness is stored 0..255 and shown as a percentage.
int to_percent(uint8_t level) {
  return (level * 100 + 127) / 255;
}

uint8_t from_percent(int32_t percent) {
  return static_cast<uint8_t>((percent * 255 + 50) / 100);
}

const char* screen_name(PuckScreen screen) {
  switch (screen) {
    case PUCK_SCREEN_POWER:
      return "Live power";
    case PUCK_SCREEN_BATTERY:
      return "Battery";
    case PUCK_SCREEN_SOLAR:
      return "Solar";
    case PUCK_SCREEN_LOAD:
      return "Home use";
    case PUCK_SCREEN_GRID:
      return "Grid";
    case PUCK_SCREEN_FLOWS:
      return "Energy flows";
    case PUCK_SCREEN_COST:
      return "Cost";
    default:
      return "";
  }
}

// ----------------------------------------------------------------- confirm ---

void disarm() {
  if (s_armed != nullptr) {
    button_set_text(s_armed, s_armed_idle_text);
    lv_obj_set_style_bg_color(s_armed, s_armed_idle_colour, LV_PART_MAIN);
    s_armed = nullptr;
  }
  if (s_armed_timer != nullptr) {
    lv_timer_del(s_armed_timer);
    s_armed_timer = nullptr;
  }
}

void unacknowledge(lv_obj_t* obj) {
  if (s_acked == nullptr || (obj != nullptr && obj != s_acked)) {
    return;
  }
  button_set_text(s_acked, s_acked_idle_text);
  lv_obj_set_style_bg_color(s_acked, s_acked_idle_colour, LV_PART_MAIN);
  lv_obj_set_style_text_color(lv_obj_get_child(s_acked, 0), s_acked_idle_text_colour,
                              LV_PART_MAIN);
  lv_obj_add_flag(s_acked, LV_OBJ_FLAG_CLICKABLE);
  s_acked = nullptr;
  if (s_acked_timer != nullptr) {
    lv_timer_del(s_acked_timer);
    s_acked_timer = nullptr;
  }
}

// True on the second tap. The first only arms the button: forgetting a network
// or restarting is not something a stray touch on a wall panel should do.
//
// With `done_text`, the second tap turns the button green and says so, rather
// than putting it straight back as it was. Install and Restart need that: what
// they start begins a moment later, on another task, and a button that reverts
// on the tap reads as a tap that did nothing. Only for buttons that outlive the
// tap (not a list row the tap removes).
bool confirmed(lv_obj_t* obj, const char* armed_text, const char* done_text) {
  if (s_armed == obj) {
    if (done_text == nullptr) {
      disarm();
      return true;
    }
    unacknowledge(nullptr);
    s_acked = obj;
    snprintf(s_acked_idle_text, sizeof(s_acked_idle_text), "%s", s_armed_idle_text);
    s_acked_idle_colour = s_armed_idle_colour;
    s_armed = nullptr;
    disarm();  // only the timer is left to clear
    lv_obj_t* label = lv_obj_get_child(obj, 0);
    s_acked_idle_text_colour = lv_obj_get_style_text_color(label, LV_PART_MAIN);
    button_set_text(obj, done_text);
    lv_obj_set_style_bg_color(obj, lv_color_hex(COLOUR_CONFIRMED), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    s_acked_timer = lv_timer_create([](lv_timer_t*) { unacknowledge(nullptr); }, ACKNOWLEDGE_MS,
                                    nullptr);
    lv_timer_set_repeat_count(s_acked_timer, 1);
    return true;
  }
  disarm();
  s_armed = obj;
  snprintf(s_armed_idle_text, sizeof(s_armed_idle_text), "%s",
           lv_label_get_text(lv_obj_get_child(obj, 0)));
  s_armed_idle_colour = lv_obj_get_style_bg_color(obj, LV_PART_MAIN);
  button_set_text(obj, armed_text);
  lv_obj_set_style_bg_color(obj, lv_color_hex(PUCK_COLOUR_ALARM), LV_PART_MAIN);
  s_armed_timer = lv_timer_create([](lv_timer_t*) { disarm(); }, CONFIRM_MS, nullptr);
  lv_timer_set_repeat_count(s_armed_timer, 1);
  return false;
}

lv_obj_t* modal_backdrop(lv_obj_t* parent) {
  lv_obj_t* backdrop = plain(parent);
  lv_obj_set_size(backdrop, SHEET_W, SHEET_H);
  lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);  // swallow taps behind it
  lv_obj_set_style_bg_color(backdrop, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(backdrop, LV_OPA_80, LV_PART_MAIN);
  lv_obj_add_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
  return backdrop;
}

}  // namespace sheet_kit
