// The settings sheet's plant pages: the Solar page (the panel's own forecast, for
// the sources with no server to supply one) and the Modbus device editor, which
// the Data page opens. Until these, both were web-page only.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "settings_sheet_internal.h"

namespace sheet_kit {

namespace {

constexpr float LATITUDE_LIMIT = 90.0f;
constexpr float LONGITUDE_LIMIT = 180.0f;
constexpr float MAX_ARRAY_KWP = 100.0f;
constexpr float MAX_INVERTER_KW = 100.0f;
// System efficiency, as the slider shows it: what is left after losses.
constexpr int EFFICIENCY_MIN_PCT = 60;
constexpr int EFFICIENCY_MAX_PCT = 100;
constexpr int TILT_STEP = 5;
constexpr int FACING_STEP = 15;
constexpr uint8_t PLANT_ADDRESS_MAX = 247;
constexpr uint8_t DEVICE_ID_MAX = 246;

SheetSolar s_solar;
DataSource s_source = DataSource::Server;
uint8_t s_plant_address = 247;
SheetModbusDevice s_devices[SHEET_MAX_MODBUS];

// Solar page
lv_obj_t* s_server_note = nullptr;
lv_obj_t* s_left = nullptr;
lv_obj_t* s_right = nullptr;
lv_obj_t* s_choice_line = nullptr;
lv_obj_t* s_choice = nullptr;
lv_obj_t* s_how = nullptr;
lv_obj_t* s_settings = nullptr;  // everything the panel's own forecast uses
lv_obj_t* s_latitude = nullptr;
lv_obj_t* s_longitude = nullptr;
lv_obj_t* s_efficiency = nullptr;
lv_obj_t* s_efficiency_value = nullptr;
lv_obj_t* s_inverter = nullptr;
lv_obj_t* s_array_name[SHEET_MAX_ARRAYS] = {};
lv_obj_t* s_array_summary[SHEET_MAX_ARRAYS] = {};
lv_obj_t* s_array_button[SHEET_MAX_ARRAYS] = {};

// Array editor: works on a copy, saved as a whole.
lv_obj_t* s_array_modal = nullptr;
lv_obj_t* s_array_title = nullptr;
lv_obj_t* s_array_size = nullptr;
lv_obj_t* s_array_tilt = nullptr;
lv_obj_t* s_array_facing = nullptr;
lv_obj_t* s_array_remove = nullptr;
int s_array_index = 0;
SheetArray s_array_edit;

// Device editor: the same.
lv_obj_t* s_devices_modal = nullptr;
lv_obj_t* s_plant_button = nullptr;
lv_obj_t* s_device_id[SHEET_MAX_MODBUS] = {};
lv_obj_t* s_device_kind[SHEET_MAX_MODBUS] = {};
uint8_t s_plant_edit = 247;
SheetModbusDevice s_devices_edit[SHEET_MAX_MODBUS];
// Which device ID the keypad is editing.
int s_device_editing = 0;

const char* compass(float azimuth) {
  static const char* const POINTS[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  const int index = static_cast<int>(lroundf(azimuth / 45.0f)) % 8;
  return POINTS[index < 0 ? index + 8 : index];
}

const char* kind_name(SheetDeviceKind kind) {
  switch (kind) {
    case SheetDeviceKind::InverterWithDc:
      return "Inverter + DC charger";
    case SheetDeviceKind::AcCharger:
      return "AC charger";
    default:
      return "Inverter";
  }
}

void save_solar() {
  if (sheet_actions().set_solar != nullptr) {
    sheet_actions().set_solar(s_solar);
  }
}

// ------------------------------------------------------------------ refresh ---

void refresh_page() {
  const bool server = s_source == DataSource::Server;
  const bool home_assistant = s_source == DataSource::HomeAssistant;
  set_hidden(s_server_note, !server);
  set_hidden(s_left, server);
  set_hidden(s_right, server);
  if (server) {
    return;
  }

  set_hidden(s_choice_line, !home_assistant);
  if (home_assistant) {
    const uint16_t choice = static_cast<uint16_t>(s_solar.ha_source);
    lv_btnmatrix_set_btn_ctrl(s_choice, choice, LV_BTNMATRIX_CTRL_CHECKED);
  }
  // With Modbus the panel always calculates it; with Home Assistant only when
  // asked to. Everything below stays editable either way, just dimmed.
  const bool used = !home_assistant || s_solar.ha_source == SolarForecastSource::Puck;
  lv_label_set_text(s_how, !home_assistant ? "Calculated on the panel from the location and arrays."
                           : used         ? "Calculated on the panel from the location and arrays."
                           : s_solar.ha_source == SolarForecastSource::HomeAssistant
                               ? "Taken from Home Assistant's forecast entities."
                               : "No forecast is shown.");
  lv_obj_set_style_opa(s_settings, used ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
  lv_obj_set_style_opa(s_right, used ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);

  char value[48];
  if (s_solar.location_set) {
    snprintf(value, sizeof(value), "%.4f", s_solar.latitude);
    button_set_text(s_latitude, value);
    snprintf(value, sizeof(value), "%.4f", s_solar.longitude);
    button_set_text(s_longitude, value);
  } else {
    button_set_text(s_latitude, "Not set");
    button_set_text(s_longitude, "Not set");
  }
  const int efficiency = static_cast<int>(lroundf(s_solar.system_loss * 100.0f));
  if (!lv_obj_has_state(s_efficiency, LV_STATE_PRESSED)) {
    lv_slider_set_value(s_efficiency, efficiency, LV_ANIM_OFF);
  }
  snprintf(value, sizeof(value), "%d%%", efficiency);
  lv_label_set_text(s_efficiency_value, value);
  if (s_solar.inverter_cap_kw > 0.0f) {
    snprintf(value, sizeof(value), "%.1f kW", s_solar.inverter_cap_kw);
    button_set_text(s_inverter, value);
  } else {
    button_set_text(s_inverter, "No limit");
  }

  for (int i = 0; i < SHEET_MAX_ARRAYS; ++i) {
    const SheetArray& array = s_solar.arrays[i];
    const bool empty = array.kwp <= 0.0f;
    if (empty) {
      lv_label_set_text(s_array_summary[i], "Empty");
    } else {
      snprintf(value, sizeof(value), "%.1f kWp, %d\xC2\xB0 tilt, facing %s", array.kwp,
               static_cast<int>(lroundf(array.tilt_deg)), compass(array.azimuth_deg));
      lv_label_set_text(s_array_summary[i], value);
    }
    lv_obj_set_style_text_color(s_array_summary[i],
                                lv_color_hex(empty ? PUCK_COLOUR_MUTED : PUCK_COLOUR_TEXT),
                                LV_PART_MAIN);
    button_set_text(s_array_button[i], empty ? "Add" : "Edit");
  }
}

void refresh_array_editor() {
  char value[32];
  snprintf(value, sizeof(value), "ARRAY %d", s_array_index + 1);
  lv_label_set_text(s_array_title, value);
  if (s_array_edit.kwp > 0.0f) {
    snprintf(value, sizeof(value), "%.1f kWp", s_array_edit.kwp);
  } else {
    snprintf(value, sizeof(value), "Set size");
  }
  button_set_text(s_array_size, value);
  snprintf(value, sizeof(value), "%d\xC2\xB0", static_cast<int>(lroundf(s_array_edit.tilt_deg)));
  lv_label_set_text(s_array_tilt, value);
  snprintf(value, sizeof(value), "%d\xC2\xB0 %s", static_cast<int>(lroundf(s_array_edit.azimuth_deg)),
           compass(s_array_edit.azimuth_deg));
  lv_label_set_text(s_array_facing, value);
  set_hidden(s_array_remove, s_solar.arrays[s_array_index].kwp <= 0.0f);
}

void refresh_devices_editor() {
  char value[16];
  snprintf(value, sizeof(value), "%u", static_cast<unsigned>(s_plant_edit));
  button_set_text(s_plant_button, value);
  for (int i = 0; i < SHEET_MAX_MODBUS; ++i) {
    const SheetModbusDevice& device = s_devices_edit[i];
    if (device.slave_id == 0) {
      button_set_text(s_device_id[i], "Off");
    } else {
      snprintf(value, sizeof(value), "%u", static_cast<unsigned>(device.slave_id));
      button_set_text(s_device_id[i], value);
    }
    button_set_text(s_device_kind[i], kind_name(device.kind));
    set_disabled(s_device_kind[i], device.slave_id == 0);
  }
}

// ------------------------------------------------------------- keypad saves ---

bool save_latitude(const char* value, char* error, size_t size) {
  float parsed = 0.0f;
  if (!parse_number(value, -LATITUDE_LIMIT, LATITUDE_LIMIT, &parsed)) {
    snprintf(error, size, "Between -90 and 90");
    return false;
  }
  s_solar.latitude = parsed;
  s_solar.location_set = true;
  save_solar();
  refresh_page();
  return true;
}

bool save_longitude(const char* value, char* error, size_t size) {
  float parsed = 0.0f;
  if (!parse_number(value, -LONGITUDE_LIMIT, LONGITUDE_LIMIT, &parsed)) {
    snprintf(error, size, "Between -180 and 180 (west is negative)");
    return false;
  }
  s_solar.longitude = parsed;
  s_solar.location_set = true;
  save_solar();
  refresh_page();
  return true;
}

bool save_inverter(const char* value, char* error, size_t size) {
  float parsed = 0.0f;
  if (value[0] == '\0') {
    parsed = 0.0f;  // cleared: no limit
  } else if (!parse_number(value, 0.0f, MAX_INVERTER_KW, &parsed)) {
    snprintf(error, size, "Up to %.0f kW, or 0 for no limit", MAX_INVERTER_KW);
    return false;
  }
  s_solar.inverter_cap_kw = parsed;
  save_solar();
  refresh_page();
  return true;
}

bool save_array_size(const char* value, char* error, size_t size) {
  float parsed = 0.0f;
  if (!parse_number(value, 0.1f, MAX_ARRAY_KWP, &parsed)) {
    snprintf(error, size, "Between 0.1 and %.0f kWp", MAX_ARRAY_KWP);
    return false;
  }
  s_array_edit.kwp = parsed;
  refresh_array_editor();
  return true;
}

bool save_plant_address(const char* value, char* error, size_t size) {
  float parsed = 0.0f;
  if (!parse_number(value, 1.0f, PLANT_ADDRESS_MAX, &parsed)) {
    snprintf(error, size, "Between 1 and %u (247 unless changed)", PLANT_ADDRESS_MAX);
    return false;
  }
  s_plant_edit = static_cast<uint8_t>(parsed);
  refresh_devices_editor();
  return true;
}

bool save_device_id(const char* value, char* error, size_t size) {
  float parsed = 0.0f;
  if (value[0] == '\0' || strcmp(value, "0") == 0) {
    parsed = 0.0f;  // cleared: slot off
  } else if (!parse_number(value, 1.0f, DEVICE_ID_MAX, &parsed)) {
    snprintf(error, size, "Between 1 and %u, or 0 for none", DEVICE_ID_MAX);
    return false;
  }
  s_devices_edit[s_device_editing].slave_id = static_cast<uint8_t>(parsed);
  refresh_devices_editor();
  return true;
}

void open_number(const char* title, float value, bool has_value, int decimals, KeypadMode mode,
                 KeypadSave save) {
  char initial[24] = "";
  if (has_value) {
    snprintf(initial, sizeof(initial), "%.*f", decimals, value);
  }
  keypad_open(title, initial, mode, save);
}

// ------------------------------------------------------------------- build ---

lv_obj_t* value_row(lv_obj_t* parent, const char* name, lv_event_cb_t on_tap) {
  lv_obj_t* line = row(parent);
  text(line, FONT_TEXT, PUCK_COLOUR_TEXT, name);
  lv_obj_t* value = button(line, "", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_set_style_pad_ver(value, 8, LV_PART_MAIN);
  lv_obj_set_style_min_width(value, 150, LV_PART_MAIN);
  lv_obj_add_event_cb(value, on_tap, LV_EVENT_CLICKED, nullptr);
  return value;
}

lv_obj_t* stepper(lv_obj_t* parent, const char* name, lv_event_cb_t on_step) {
  lv_obj_t* line = row(parent);
  text(line, FONT_TEXT, PUCK_COLOUR_TEXT, name);
  lv_obj_t* controls = plain(line);
  lv_obj_set_size(controls, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(controls, 8, LV_PART_MAIN);
  lv_obj_t* minus = button(controls, "-", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_set_style_pad_hor(minus, 18, LV_PART_MAIN);
  lv_obj_add_event_cb(minus, on_step, LV_EVENT_CLICKED, reinterpret_cast<void*>(-1));
  lv_obj_t* value = text(controls, FONT_ROW, PUCK_COLOUR_TEXT, "");
  lv_obj_set_width(value, 110);
  lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_t* plus = button(controls, "+", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_set_style_pad_hor(plus, 18, LV_PART_MAIN);
  lv_obj_add_event_cb(plus, on_step, LV_EVENT_CLICKED, reinterpret_cast<void*>(1));
  return value;
}

int step_of(lv_event_t* event) {
  return static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
}

void build_array_modal(lv_obj_t* root) {
  s_array_modal = modal_backdrop(root);
  lv_obj_t* box = card(s_array_modal, 560, LV_SIZE_CONTENT);
  lv_obj_center(box);
  lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(box, lv_color_hex(COLOUR_CONTROL), LV_PART_MAIN);
  lv_obj_set_style_pad_row(box, 16, LV_PART_MAIN);
  s_array_title = caption(box, "");

  s_array_size = value_row(box, "Size", [](lv_event_t*) {
    open_number("ARRAY SIZE (kWp)", s_array_edit.kwp, s_array_edit.kwp > 0.0f, 2,
                KeypadMode::Decimal, save_array_size);
  });
  s_array_tilt = stepper(box, "Tilt", [](lv_event_t* event) {
    const int next = static_cast<int>(lroundf(s_array_edit.tilt_deg)) + step_of(event) * TILT_STEP;
    s_array_edit.tilt_deg = static_cast<float>(next < 0 ? 0 : next > 90 ? 90 : next);
    refresh_array_editor();
  });
  s_array_facing = stepper(box, "Facing", [](lv_event_t* event) {
    const int next = static_cast<int>(lroundf(s_array_edit.azimuth_deg)) + step_of(event) * FACING_STEP;
    s_array_edit.azimuth_deg = static_cast<float>((next + 360) % 360);
    refresh_array_editor();
  });
  hint(box, "Tilt is from flat. Facing is a compass bearing: 180 is due south.");

  lv_obj_t* buttons = row(box);
  s_array_remove = button(buttons, "Remove", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(
      s_array_remove,
      [](lv_event_t*) {
        if (!confirmed(s_array_remove, "Remove?")) {
          return;
        }
        s_solar.arrays[s_array_index] = SheetArray{};
        save_solar();
        refresh_page();
        lv_obj_add_flag(s_array_modal, LV_OBJ_FLAG_HIDDEN);
      },
      LV_EVENT_CLICKED, nullptr);
  lv_obj_t* end = plain(buttons);
  lv_obj_set_size(end, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(end, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(end, 10, LV_PART_MAIN);
  lv_obj_t* cancel = button(end, "Cancel", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(
      cancel, [](lv_event_t*) { lv_obj_add_flag(s_array_modal, LV_OBJ_FLAG_HIDDEN); },
      LV_EVENT_CLICKED, nullptr);
  lv_obj_t* save = button(end, "Save", COLOUR_ACCENT, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(
      save,
      [](lv_event_t*) {
        if (s_array_edit.kwp <= 0.0f) {
          return;  // nothing to save until it has a size
        }
        s_solar.arrays[s_array_index] = s_array_edit;
        save_solar();
        refresh_page();
        lv_obj_add_flag(s_array_modal, LV_OBJ_FLAG_HIDDEN);
      },
      LV_EVENT_CLICKED, nullptr);
}

void build_devices_modal(lv_obj_t* root) {
  s_devices_modal = modal_backdrop(root);
  lv_obj_t* box = card(s_devices_modal, 680, LV_SIZE_CONTENT);
  lv_obj_center(box);
  lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(box, lv_color_hex(COLOUR_CONTROL), LV_PART_MAIN);
  caption(box, "MODBUS DEVICES");
  s_plant_button = value_row(box, "Plant address", [](lv_event_t*) {
    open_number("PLANT ADDRESS", s_plant_edit, true, 0, KeypadMode::Whole, save_plant_address);
  });
  divider(box);
  for (int i = 0; i < SHEET_MAX_MODBUS; ++i) {
    lv_obj_t* line = row(box);
    char name[16];
    snprintf(name, sizeof(name), "Device %d", i + 1);
    text(line, FONT_TEXT, PUCK_COLOUR_TEXT, name);
    lv_obj_t* controls = plain(line);
    lv_obj_set_size(controls, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(controls, 10, LV_PART_MAIN);
    s_device_id[i] = button(controls, "", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
    lv_obj_set_style_min_width(s_device_id[i], 90, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_device_id[i], 8, LV_PART_MAIN);
    lv_obj_add_event_cb(
        s_device_id[i],
        [](lv_event_t* event) {
          s_device_editing = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
          const uint8_t id = s_devices_edit[s_device_editing].slave_id;
          open_number("MODBUS ID (0 FOR NONE)", id, id != 0, 0, KeypadMode::Whole, save_device_id);
        },
        LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    // Tapping steps through the three kinds.
    s_device_kind[i] = button(controls, "", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
    lv_obj_set_style_min_width(s_device_kind[i], 270, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_device_kind[i], 8, LV_PART_MAIN);
    lv_obj_add_event_cb(
        s_device_kind[i],
        [](lv_event_t* event) {
          const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
          SheetDeviceKind& kind = s_devices_edit[index].kind;
          kind = kind == SheetDeviceKind::Inverter         ? SheetDeviceKind::InverterWithDc
                 : kind == SheetDeviceKind::InverterWithDc ? SheetDeviceKind::AcCharger
                                                           : SheetDeviceKind::Inverter;
          refresh_devices_editor();
        },
        LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
  }
  hint(box, "IDs as the Sigen app lists them. The plant is 247 unless changed. Tap a kind to "
            "change it.");
  lv_obj_t* buttons = plain(box);
  lv_obj_set_size(buttons, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(buttons, 10, LV_PART_MAIN);
  lv_obj_t* cancel = button(buttons, "Cancel", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(
      cancel, [](lv_event_t*) { lv_obj_add_flag(s_devices_modal, LV_OBJ_FLAG_HIDDEN); },
      LV_EVENT_CLICKED, nullptr);
  lv_obj_t* save = button(buttons, "Save", COLOUR_ACCENT, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(
      save,
      [](lv_event_t*) {
        s_plant_address = s_plant_edit;
        memcpy(s_devices, s_devices_edit, sizeof(s_devices));
        if (sheet_actions().set_modbus_devices != nullptr) {
          sheet_actions().set_modbus_devices(s_plant_address, s_devices, SHEET_MAX_MODBUS);
        }
        lv_obj_add_flag(s_devices_modal, LV_OBJ_FLAG_HIDDEN);
      },
      LV_EVENT_CLICKED, nullptr);
}

}  // namespace

void plant_build_page(lv_obj_t* tile) {
  lv_obj_t* body = plain(tile);
  lv_obj_set_size(body, SHEET_W, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_hor(body, PAGE_PAD_X, LV_PART_MAIN);
  lv_obj_set_style_pad_top(body, PAGE_PAD_TOP, LV_PART_MAIN);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(body, CARD_GAP, LV_PART_MAIN);

  s_server_note = card(body, SHEET_W - 2 * PAGE_PAD_X, LV_SIZE_CONTENT);
  caption(s_server_note, "SOLAR FORECAST");
  text(s_server_note, FONT_ROW, PUCK_COLOUR_TEXT, "The server supplies the forecast.");
  hint(s_server_note, "SigenStor Display works it out from its own location and array settings, "
                      "so there is nothing to set here. These settings are for the Modbus and "
                      "Home Assistant sources.");

  s_left = card(body, 460, CARD_H);
  caption(s_left, "SOLAR FORECAST");
  s_choice_line = plain(s_left);
  lv_obj_set_size(s_choice_line, LV_PCT(100), LV_SIZE_CONTENT);
  // Not const: LVGL 8 takes the map as `const char* []`. Order is SolarForecastSource.
  static const char* CHOICES[] = {"Off", "This panel", "From HA", ""};
  s_choice = lv_btnmatrix_create(s_choice_line);
  lv_btnmatrix_set_map(s_choice, CHOICES);
  lv_btnmatrix_set_btn_ctrl_all(s_choice, LV_BTNMATRIX_CTRL_CHECKABLE);
  lv_btnmatrix_set_one_checked(s_choice, true);
  lv_obj_set_size(s_choice, LV_PCT(100), 54);
  lv_obj_set_style_bg_color(s_choice, lv_color_hex(COLOUR_CONTROL), LV_PART_MAIN);
  lv_obj_set_style_border_width(s_choice, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(s_choice, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_choice, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_gap(s_choice, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_choice, LV_OPA_TRANSP, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(s_choice, 0, LV_PART_ITEMS);
  lv_obj_set_style_border_width(s_choice, 0, LV_PART_ITEMS);
  lv_obj_set_style_radius(s_choice, 10, LV_PART_ITEMS);
  lv_obj_set_style_text_font(s_choice, FONT_TEXT, LV_PART_ITEMS);
  lv_obj_set_style_text_color(s_choice, lv_color_hex(PUCK_COLOUR_MUTED), LV_PART_ITEMS);
  const lv_style_selector_t checked =
      static_cast<lv_style_selector_t>(LV_PART_ITEMS) | LV_STATE_CHECKED;
  lv_obj_set_style_bg_opa(s_choice, LV_OPA_COVER, checked);
  lv_obj_set_style_bg_color(s_choice, lv_color_hex(COLOUR_ACCENT), checked);
  lv_obj_set_style_text_color(s_choice, lv_color_hex(PUCK_COLOUR_TEXT), checked);
  keep_gestures(s_choice);
  lv_obj_add_event_cb(
      s_choice,
      [](lv_event_t*) {
        const uint16_t selected = lv_btnmatrix_get_selected_btn(s_choice);
        if (selected > 2) {
          return;
        }
        s_solar.ha_source = static_cast<SolarForecastSource>(selected);
        if (sheet_actions().set_ha_forecast_source != nullptr) {
          sheet_actions().set_ha_forecast_source(s_solar.ha_source);
        }
        refresh_page();
      },
      LV_EVENT_VALUE_CHANGED, nullptr);
  s_how = hint(s_left, "");

  s_settings = plain(s_left);
  lv_obj_set_size(s_settings, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s_settings, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_settings, 10, LV_PART_MAIN);
  s_latitude = value_row(s_settings, "Latitude", [](lv_event_t*) {
    open_number("LATITUDE", s_solar.latitude, s_solar.location_set, 4, KeypadMode::Signed,
                save_latitude);
  });
  s_longitude = value_row(s_settings, "Longitude", [](lv_event_t*) {
    open_number("LONGITUDE (WEST IS NEGATIVE)", s_solar.longitude, s_solar.location_set, 4,
                KeypadMode::Signed, save_longitude);
  });
  lv_obj_t* efficiency_line = row(s_settings);
  text(efficiency_line, FONT_TEXT, PUCK_COLOUR_TEXT, "System efficiency");
  s_efficiency_value = text(efficiency_line, FONT_TEXT, PUCK_COLOUR_TEXT, "");
  s_efficiency = slider(s_settings, EFFICIENCY_MIN_PCT, EFFICIENCY_MAX_PCT);
  const auto on_efficiency = [](lv_event_t* event) {
    s_solar.system_loss = lv_slider_get_value(s_efficiency) / 100.0f;
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
      save_solar();
    }
    refresh_page();
  };
  lv_obj_add_event_cb(s_efficiency, on_efficiency, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(s_efficiency, on_efficiency, LV_EVENT_RELEASED, nullptr);
  s_inverter = value_row(s_settings, "Inverter limit", [](lv_event_t*) {
    open_number("INVERTER LIMIT (kW, 0 FOR NONE)", s_solar.inverter_cap_kw,
                s_solar.inverter_cap_kw > 0.0f, 1, KeypadMode::Decimal, save_inverter);
  });

  s_right = card(body, SHEET_W - 2 * PAGE_PAD_X - 460 - CARD_GAP, CARD_H);
  caption(s_right, "ARRAYS");
  for (int i = 0; i < SHEET_MAX_ARRAYS; ++i) {
    lv_obj_t* line = row(s_right);
    lv_obj_set_height(line, 56);
    lv_obj_t* words = plain(line);
    lv_obj_set_size(words, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(words, LV_FLEX_FLOW_COLUMN);
    char name[16];
    snprintf(name, sizeof(name), "Array %d", i + 1);
    s_array_name[i] = text(words, FONT_TEXT, PUCK_COLOUR_TEXT, name);
    s_array_summary[i] = text(words, FONT_HINT, PUCK_COLOUR_MUTED, "");
    s_array_button[i] = button(line, "", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
    lv_obj_set_style_pad_ver(s_array_button[i], 8, LV_PART_MAIN);
    lv_obj_add_event_cb(
        s_array_button[i],
        [](lv_event_t* event) {
          plant_open_array(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event))));
        },
        LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
  }
  spacer(s_right);
  hint(s_right, "One per roof face with panels on it. Size is the panels' rating in kWp.");

  refresh_page();
}

void plant_build_modals(lv_obj_t* root) {
  build_array_modal(root);
  build_devices_modal(root);
}

void plant_set_solar(const SheetSolar& solar) {
  s_solar = solar;
  refresh_page();
}

void plant_set_data(const SheetData& data) {
  s_source = data.source;
  s_plant_address = data.modbus_plant_address;
  memcpy(s_devices, data.modbus_devices, sizeof(s_devices));
  refresh_page();
}

void plant_open_devices() {
  s_plant_edit = s_plant_address;
  memcpy(s_devices_edit, s_devices, sizeof(s_devices_edit));
  refresh_devices_editor();
  lv_obj_move_foreground(s_devices_modal);
  lv_obj_clear_flag(s_devices_modal, LV_OBJ_FLAG_HIDDEN);
}

void plant_open_array(int index) {
  if (index < 0 || index >= SHEET_MAX_ARRAYS) {
    return;
  }
  s_array_index = index;
  s_array_edit = s_solar.arrays[index];
  if (s_array_edit.kwp <= 0.0f) {
    s_array_edit = SheetArray{};  // a new one starts at 35 degrees, due south
  }
  refresh_array_editor();
  lv_obj_move_foreground(s_array_modal);
  lv_obj_clear_flag(s_array_modal, LV_OBJ_FLAG_HIDDEN);
}

bool plant_modal_open() {
  return !lv_obj_has_flag(s_array_modal, LV_OBJ_FLAG_HIDDEN) ||
         !lv_obj_has_flag(s_devices_modal, LV_OBJ_FLAG_HIDDEN);
}

void plant_close_modals() {
  lv_obj_add_flag(s_array_modal, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_devices_modal, LV_OBJ_FLAG_HIDDEN);
}

const char* plant_devices_summary(const SheetData& data, char* out, size_t size) {
  int inverters = 0;
  int chargers = 0;
  for (const SheetModbusDevice& device : data.modbus_devices) {
    if (device.slave_id == 0) {
      continue;
    }
    if (device.kind == SheetDeviceKind::AcCharger) {
      ++chargers;
    } else {
      ++inverters;
    }
  }
  if (inverters == 0 && chargers == 0) {
    snprintf(out, size, "None set");
  } else if (chargers == 0) {
    snprintf(out, size, "%d inverter%s", inverters, inverters == 1 ? "" : "s");
  } else {
    snprintf(out, size, "%d inverter%s, %d charger%s", inverters, inverters == 1 ? "" : "s",
             chargers, chargers == 1 ? "" : "s");
  }
  return out;
}

}  // namespace sheet_kit
