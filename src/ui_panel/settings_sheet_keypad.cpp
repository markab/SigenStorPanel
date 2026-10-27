// The sheet's number pad: plant addresses, coordinates, array sizes, Modbus IDs.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings_sheet_internal.h"

namespace sheet_kit {

namespace {

lv_obj_t* s_keypad = nullptr;
lv_obj_t* s_title = nullptr;
lv_obj_t* s_field = nullptr;
lv_obj_t* s_error = nullptr;
lv_obj_t* s_matrix = nullptr;
char s_text[24] = {};
KeypadMode s_mode = KeypadMode::Address;
KeypadSave s_save = nullptr;

// The keys by index: 0-8 are 1-9, then these.
constexpr uint16_t KEY_POINT = 9;
constexpr uint16_t KEY_MINUS = 12;
constexpr uint16_t KEY_SAVE = 14;

void refresh() {
  lv_label_set_text(s_field, s_text[0] != '\0' ? s_text : " ");
}

void on_key(lv_event_t* /*event*/) {
  const uint16_t id = lv_btnmatrix_get_selected_btn(s_matrix);
  if (id == LV_BTNMATRIX_BTN_NONE) {
    return;
  }
  const char* key = lv_btnmatrix_get_btn_text(s_matrix, id);
  const size_t length = strlen(s_text);
  lv_label_set_text(s_error, "");

  if (strcmp(key, "Cancel") == 0) {
    keypad_close();
    return;
  }
  if (strcmp(key, "Save") == 0) {
    char error[64] = {};
    if (s_save != nullptr && !s_save(s_text, error, sizeof(error))) {
      lv_label_set_text(s_error, error[0] != '\0' ? error : "Not a valid value");
      return;
    }
    keypad_close();
    return;
  }
  if (strcmp(key, "Del") == 0) {
    if (length > 0) {
      s_text[length - 1] = '\0';
    }
  } else if (strcmp(key, "-") == 0) {
    // A sign, so it toggles at the front rather than typing into the middle.
    if (s_text[0] == '-') {
      memmove(s_text, s_text + 1, length);
    } else if (length + 1 < sizeof(s_text)) {
      memmove(s_text + 1, s_text, length + 1);
      s_text[0] = '-';
    }
  } else if (length + 1 < sizeof(s_text)) {
    s_text[length] = key[0];
    s_text[length + 1] = '\0';
  }
  refresh();
}

}  // namespace

void keypad_build(lv_obj_t* parent) {
  s_keypad = modal_backdrop(parent);

  lv_obj_t* box = card(s_keypad, 440, LV_SIZE_CONTENT);
  lv_obj_center(box);
  lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(box, lv_color_hex(COLOUR_CONTROL), LV_PART_MAIN);
  s_title = caption(box, "");
  s_field = text(box, FONT_BIG, PUCK_COLOUR_TEXT, "");
  lv_obj_set_width(s_field, LV_PCT(100));
  lv_obj_set_style_border_side(s_field, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_field, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_field, lv_color_hex(COLOUR_ACCENT), LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(s_field, 6, LV_PART_MAIN);
  s_error = text(box, FONT_HINT, PUCK_COLOUR_ALARM, "");

  // Not const: LVGL 8 takes the map as `const char* []`.
  static const char* KEYS[] = {"1", "2", "3", "\n", "4", "5",      "6",    "\n", "7", "8",
                               "9", "\n", ".", "0", "Del", "\n", "-", "Cancel", "Save", ""};
  s_matrix = lv_btnmatrix_create(box);
  lv_btnmatrix_set_map(s_matrix, KEYS);
  lv_obj_set_size(s_matrix, LV_PCT(100), 360);
  lv_obj_set_style_bg_opa(s_matrix, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_matrix, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_matrix, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_gap(s_matrix, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_matrix, lv_color_hex(COLOUR_CONTROL), LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(s_matrix, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_radius(s_matrix, 12, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(s_matrix, 0, LV_PART_ITEMS);
  lv_obj_set_style_border_width(s_matrix, 0, LV_PART_ITEMS);
  lv_obj_set_style_text_font(s_matrix, FONT_ROW, LV_PART_ITEMS);
  lv_obj_set_style_text_color(s_matrix, lv_color_hex(PUCK_COLOUR_TEXT), LV_PART_ITEMS);
  lv_btnmatrix_set_btn_ctrl(s_matrix, KEY_SAVE, LV_BTNMATRIX_CTRL_CHECKED);
  const lv_style_selector_t checked =
      static_cast<lv_style_selector_t>(LV_PART_ITEMS) | LV_STATE_CHECKED;
  lv_obj_set_style_bg_color(s_matrix, lv_color_hex(COLOUR_ACCENT), checked);
  keep_gestures(s_matrix);
  lv_obj_add_event_cb(s_matrix, on_key, LV_EVENT_VALUE_CHANGED, nullptr);
}

void keypad_open(const char* title, const char* initial, KeypadMode mode, KeypadSave save) {
  s_mode = mode;
  s_save = save;
  snprintf(s_text, sizeof(s_text), "%s", initial != nullptr ? initial : "");
  lv_label_set_text(s_title, title);
  lv_label_set_text(s_error, "");
  // Only the keys the value can use.
  const bool point = mode != KeypadMode::Whole;
  const bool minus = mode == KeypadMode::Signed;
  if (point) {
    lv_btnmatrix_clear_btn_ctrl(s_matrix, KEY_POINT, LV_BTNMATRIX_CTRL_DISABLED);
  } else {
    lv_btnmatrix_set_btn_ctrl(s_matrix, KEY_POINT, LV_BTNMATRIX_CTRL_DISABLED);
  }
  if (minus) {
    lv_btnmatrix_clear_btn_ctrl(s_matrix, KEY_MINUS, LV_BTNMATRIX_CTRL_DISABLED);
  } else {
    lv_btnmatrix_set_btn_ctrl(s_matrix, KEY_MINUS, LV_BTNMATRIX_CTRL_DISABLED);
  }
  refresh();
  lv_obj_move_foreground(s_keypad);
  lv_obj_clear_flag(s_keypad, LV_OBJ_FLAG_HIDDEN);
}

void keypad_close() {
  if (s_keypad != nullptr) {
    lv_obj_add_flag(s_keypad, LV_OBJ_FLAG_HIDDEN);
  }
}

bool keypad_is_open() {
  return s_keypad != nullptr && !lv_obj_has_flag(s_keypad, LV_OBJ_FLAG_HIDDEN);
}

bool valid_ipv4(const char* value) {
  int parts = 0;
  int digits = 0;
  int number = 0;
  for (const char* c = value;; ++c) {
    if (*c >= '0' && *c <= '9') {
      number = number * 10 + (*c - '0');
      if (++digits > 3 || number > 255) {
        return false;
      }
    } else if (*c == '.' || *c == '\0') {
      if (digits == 0) {
        return false;
      }
      ++parts;
      digits = 0;
      number = 0;
      if (*c == '\0') {
        break;
      }
    } else {
      return false;
    }
  }
  return parts == 4;
}

bool parse_number(const char* value, float min, float max, float* out) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const float parsed = strtof(value, &end);
  if (end == value || *end != '\0' || parsed < min || parsed > max) {
    return false;
  }
  *out = parsed;
  return true;
}

}  // namespace sheet_kit
