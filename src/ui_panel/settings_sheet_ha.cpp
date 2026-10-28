// The settings sheet's Home Assistant sensors: which sensor supplies each role,
// picked from the list Home Assistant itself gives (src/ha_catalog.h) rather than
// typed as entity IDs.
//
// Both lists are an lv_table, one object holding every row as text, not a widget
// per row: Home Assistant can offer a few hundred sensors, and an object each
// would take a large bite of the LVGL pool for a list that is open for seconds.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings_sheet_internal.h"

namespace sheet_kit {

namespace {

constexpr lv_coord_t MODAL_W = 800;
constexpr lv_coord_t LIST_H = 330;
// The picker's columns: a sensor's name and ID, then its value. The role list's:
// a short role name, then the sensor chosen for it, which is the longer one.
constexpr lv_coord_t PICKER_NAME_W = 520;
constexpr lv_coord_t PICKER_VALUE_W = 230;
constexpr lv_coord_t ROLE_NAME_W = 300;
constexpr lv_coord_t ROLE_VALUE_W = 450;
// The most rows the picker draws. A role's unit narrows the list a long way, so
// this is reached only by a Home Assistant with an unusual number of meters.
constexpr size_t PICKER_MAX = 150;

SheetHaSensors s_sensors;
SheetHaList s_list;

lv_obj_t* s_roles_modal = nullptr;
lv_obj_t* s_roles_table = nullptr;
lv_obj_t* s_roles_status = nullptr;

lv_obj_t* s_picker_modal = nullptr;
lv_obj_t* s_picker_title = nullptr;
lv_obj_t* s_picker_status = nullptr;
lv_obj_t* s_picker_table = nullptr;
int s_picker_role = 0;
// Picker row -> index into s_list.items; row 0 is "None".
uint16_t s_picker_rows[PICKER_MAX + 1] = {};
size_t s_picker_count = 0;

const HaCandidate* find(const char* entity_id) {
  if (s_list.state != SheetHaListState::Ready || entity_id[0] == '\0') {
    return nullptr;
  }
  for (size_t i = 0; i < s_list.count; ++i) {
    if (strcmp(s_list.items[i].entity_id, entity_id) == 0) {
      return &s_list.items[i];
    }
  }
  return nullptr;
}

// "Loading...", the failure, or nothing once the list is in.
void status_text(lv_obj_t* label) {
  switch (s_list.state) {
    case SheetHaListState::Idle:
    case SheetHaListState::Loading:
      lv_label_set_text(label, "Getting the sensor list from Home Assistant...");
      lv_obj_set_style_text_color(label, lv_color_hex(PUCK_COLOUR_MUTED), LV_PART_MAIN);
      break;
    case SheetHaListState::Failed:
      lv_label_set_text(label, s_list.message[0] != '\0' ? s_list.message
                                                         : "Could not get the sensor list");
      lv_obj_set_style_text_color(label, lv_color_hex(PUCK_COLOUR_ALARM), LV_PART_MAIN);
      break;
    case SheetHaListState::Ready:
      lv_label_set_text(label, s_list.message);
      lv_obj_set_style_text_color(label, lv_color_hex(PUCK_COLOUR_MUTED), LV_PART_MAIN);
      break;
  }
}

void style_table(lv_obj_t* table, lv_coord_t first_w, lv_coord_t second_w) {
  lv_obj_set_size(table, LV_PCT(100), LIST_H);
  lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(table, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(table, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(table, lv_color_hex(COLOUR_CARD), LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(table, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(table, lv_color_hex(COLOUR_CONTROL),
                            static_cast<lv_style_selector_t>(LV_PART_ITEMS) | LV_STATE_PRESSED);
  lv_obj_set_style_text_font(table, FONT_TEXT, LV_PART_ITEMS);
  lv_obj_set_style_text_color(table, lv_color_hex(PUCK_COLOUR_TEXT), LV_PART_ITEMS);
  lv_obj_set_style_border_color(table, lv_color_hex(COLOUR_CONTROL), LV_PART_ITEMS);
  lv_obj_set_style_border_width(table, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_side(table, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
  lv_obj_set_style_pad_ver(table, 12, LV_PART_ITEMS);
  lv_obj_set_style_pad_hor(table, 10, LV_PART_ITEMS);
  lv_table_set_col_cnt(table, 2);
  lv_table_set_col_width(table, 0, first_w);
  lv_table_set_col_width(table, 1, second_w);
  // A finger dragging the list must scroll it, not page the sheet.
  keep_gestures(table);
}

lv_obj_t* buttons_row(lv_obj_t* parent) {
  lv_obj_t* buttons = plain(parent);
  lv_obj_set_size(buttons, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(buttons, 10, LV_PART_MAIN);
  return buttons;
}

void refresh_roles() {
  if (s_roles_table == nullptr) {
    return;
  }
  status_text(s_roles_status);
  lv_table_set_row_cnt(s_roles_table, HA_ENTITY_COUNT);
  for (size_t i = 0; i < HA_ENTITY_COUNT; ++i) {
    lv_table_set_cell_value(s_roles_table, i, 0, HA_ENTITIES[i].label);
    const char* chosen = s_sensors.entity[i];
    const HaCandidate* candidate = find(chosen);
    lv_table_set_cell_value(s_roles_table, i, 1,
                            chosen[0] == '\0'        ? "Not read"
                            : candidate != nullptr && candidate->name[0] != '\0'
                                ? candidate->name
                                : chosen);
  }
}

int by_name(const void* a, const void* b) {
  const HaCandidate* items = s_list.items;
  const HaCandidate& left = items[*static_cast<const uint16_t*>(a)];
  const HaCandidate& right = items[*static_cast<const uint16_t*>(b)];
  return strcasecmp(left.name[0] != '\0' ? left.name : left.entity_id,
                    right.name[0] != '\0' ? right.name : right.entity_id);
}

void refresh_picker() {
  if (s_picker_table == nullptr) {
    return;
  }
  const HaEntityDescriptor& role = HA_ENTITIES[s_picker_role];
  lv_label_set_text(s_picker_title, role.label);
  status_text(s_picker_status);

  // The sensors in a unit this role can use, by name.
  s_picker_count = 0;
  if (s_list.state == SheetHaListState::Ready) {
    for (size_t i = 0; i < s_list.count && s_picker_count < PICKER_MAX; ++i) {
      if (ha_catalog_suits(role.kind, s_list.items[i])) {
        s_picker_rows[1 + s_picker_count++] = static_cast<uint16_t>(i);
      }
    }
    qsort(&s_picker_rows[1], s_picker_count, sizeof(uint16_t), by_name);
    if (s_picker_count == 0) {
      lv_label_set_text(s_picker_status,
                        "Home Assistant has no sensor in a unit this can use");
    }
  }

  lv_table_set_row_cnt(s_picker_table, 1 + s_picker_count);
  lv_table_set_cell_value(s_picker_table, 0, 0, "None: do not read this");
  lv_table_set_cell_value(s_picker_table, 0, 1,
                          s_sensors.entity[s_picker_role][0] == '\0' ? "chosen" : "");
  for (size_t row = 1; row <= s_picker_count; ++row) {
    const HaCandidate& candidate = s_list.items[s_picker_rows[row]];
    lv_table_set_cell_value_fmt(s_picker_table, row, 0, "%s\n%s",
                                candidate.name[0] != '\0' ? candidate.name : candidate.entity_id,
                                candidate.entity_id);
    const bool chosen = strcmp(candidate.entity_id, s_sensors.entity[s_picker_role]) == 0;
    lv_table_set_cell_value_fmt(s_picker_table, row, 1, "%s %s%s", candidate.state,
                                candidate.unit, chosen ? "\nchosen" : "");
  }
  lv_obj_scroll_to_y(s_picker_table, 0, LV_ANIM_OFF);
}

void build_roles_modal(lv_obj_t* root) {
  s_roles_modal = modal_backdrop(root);
  lv_obj_t* box = card(s_roles_modal, MODAL_W, LV_SIZE_CONTENT);
  lv_obj_center(box);
  lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(box, lv_color_hex(COLOUR_CONTROL), LV_PART_MAIN);
  caption(box, "HOME ASSISTANT SENSORS");
  s_roles_status = hint(box, "");
  s_roles_table = lv_table_create(box);
  style_table(s_roles_table, ROLE_NAME_W, ROLE_VALUE_W);
  lv_obj_add_event_cb(
      s_roles_table,
      [](lv_event_t*) {
        uint16_t row = 0;
        uint16_t column = 0;
        lv_table_get_selected_cell(s_roles_table, &row, &column);
        if (row < HA_ENTITY_COUNT) {
          ha_open_picker(row);
        }
      },
      LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_t* buttons = buttons_row(box);
  lv_obj_t* reload = button(buttons, "Reload list", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(
      reload,
      [](lv_event_t*) {
        if (sheet_actions().load_ha_list != nullptr) {
          sheet_actions().load_ha_list();
        }
      },
      LV_EVENT_CLICKED, nullptr);
  lv_obj_t* done = button(buttons, "Done", COLOUR_ACCENT, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(
      done, [](lv_event_t*) { lv_obj_add_flag(s_roles_modal, LV_OBJ_FLAG_HIDDEN); },
      LV_EVENT_CLICKED, nullptr);
}

void build_picker_modal(lv_obj_t* root) {
  s_picker_modal = modal_backdrop(root);
  lv_obj_t* box = card(s_picker_modal, MODAL_W, LV_SIZE_CONTENT);
  lv_obj_center(box);
  lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(box, lv_color_hex(COLOUR_CONTROL), LV_PART_MAIN);
  s_picker_title = text(box, FONT_ROW, PUCK_COLOUR_TEXT, "");
  s_picker_status = hint(box, "");
  s_picker_table = lv_table_create(box);
  style_table(s_picker_table, PICKER_NAME_W, PICKER_VALUE_W);
  lv_obj_add_event_cb(
      s_picker_table,
      [](lv_event_t*) {
        uint16_t row = 0;
        uint16_t column = 0;
        lv_table_get_selected_cell(s_picker_table, &row, &column);
        if (row > s_picker_count) {
          return;
        }
        const char* entity = row == 0 ? "" : s_list.items[s_picker_rows[row]].entity_id;
        snprintf(s_sensors.entity[s_picker_role], HA_ENTITY_ID_MAX, "%s", entity);
        if (sheet_actions().set_ha_sensor != nullptr) {
          sheet_actions().set_ha_sensor(s_picker_role, entity);
        }
        lv_obj_add_flag(s_picker_modal, LV_OBJ_FLAG_HIDDEN);
        refresh_roles();
      },
      LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_t* buttons = buttons_row(box);
  lv_obj_t* cancel = button(buttons, "Cancel", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(
      cancel, [](lv_event_t*) { lv_obj_add_flag(s_picker_modal, LV_OBJ_FLAG_HIDDEN); },
      LV_EVENT_CLICKED, nullptr);
}

}  // namespace

void ha_build_modals(lv_obj_t* root) {
  build_roles_modal(root);
  build_picker_modal(root);
}

void ha_set_sensors(const SheetHaSensors& sensors) {
  s_sensors = sensors;
  refresh_roles();
}

void ha_set_list(const SheetHaList& list) {
  s_list = list;
  refresh_roles();
  if (s_picker_modal != nullptr && !lv_obj_has_flag(s_picker_modal, LV_OBJ_FLAG_HIDDEN)) {
    refresh_picker();
  }
}

void ha_open_sensors() {
  // Fetched each time the list is opened unless it is already in: sensors come
  // and go in Home Assistant, and a stale list is a confusing one.
  if (s_list.state != SheetHaListState::Ready && s_list.state != SheetHaListState::Loading &&
      sheet_actions().load_ha_list != nullptr) {
    sheet_actions().load_ha_list();
  }
  refresh_roles();
  lv_obj_scroll_to_y(s_roles_table, 0, LV_ANIM_OFF);
  lv_obj_move_foreground(s_roles_modal);
  lv_obj_clear_flag(s_roles_modal, LV_OBJ_FLAG_HIDDEN);
}

void ha_open_picker(int role) {
  if (role < 0 || role >= static_cast<int>(HA_ENTITY_COUNT)) {
    return;
  }
  s_picker_role = role;
  refresh_picker();
  lv_obj_move_foreground(s_picker_modal);
  lv_obj_clear_flag(s_picker_modal, LV_OBJ_FLAG_HIDDEN);
}

bool ha_modal_open() {
  return (s_roles_modal != nullptr && !lv_obj_has_flag(s_roles_modal, LV_OBJ_FLAG_HIDDEN)) ||
         (s_picker_modal != nullptr && !lv_obj_has_flag(s_picker_modal, LV_OBJ_FLAG_HIDDEN));
}

void ha_close_modals() {
  if (s_roles_modal != nullptr) {
    lv_obj_add_flag(s_roles_modal, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_picker_modal != nullptr) {
    lv_obj_add_flag(s_picker_modal, LV_OBJ_FLAG_HIDDEN);
  }
}

const char* ha_sensors_summary(char* out, size_t size) {
  size_t chosen = 0;
  for (size_t i = 0; i < HA_ENTITY_COUNT; ++i) {
    if (s_sensors.entity[i][0] != '\0') {
      ++chosen;
    }
  }
  snprintf(out, size, "%u of %u chosen", static_cast<unsigned>(chosen),
           static_cast<unsigned>(HA_ENTITY_COUNT));
  return out;
}

}  // namespace sheet_kit
