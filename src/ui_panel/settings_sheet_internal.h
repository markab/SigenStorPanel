// Internal to the settings sheet: the look shared by its pages, the widgets
// they are built from, and what one page's file needs from another's. Not for
// use outside src/ui_panel/settings_sheet*.cpp.

#pragma once

#include <lvgl.h>
#include <stddef.h>
#include <stdint.h>

#include "board_config.h"
#include "settings_sheet.h"
#include "theme.h"
#include "ui.h"

// Read across a room is the data screens' job. These are read at arm's length,
// with a finger on the glass, so one size larger than the theme's body text.
#define FONT_CAPTION (&puck_font_18)
#define FONT_HINT (&puck_font_18)
#define FONT_TEXT (&puck_font_20)
#define FONT_ROW (&puck_font_24)
#define FONT_BIG (&puck_font_32)

namespace sheet_kit {

// ------------------------------------------------------------------ layout ---

inline constexpr lv_coord_t SHEET_W = PUCK_LCD_WIDTH;
inline constexpr lv_coord_t SHEET_H = PUCK_LCD_HEIGHT;
inline constexpr lv_coord_t HEADER_H = 84;
inline constexpr lv_coord_t PAGE_PAD_X = 40;
inline constexpr lv_coord_t PAGE_PAD_TOP = 8;
inline constexpr lv_coord_t CARD_GAP = 24;
inline constexpr lv_coord_t CARD_H = 420;
inline constexpr lv_coord_t CARD_PAD = 24;
inline constexpr lv_coord_t RESTART_BAR_H = 52;

// A shade off black, so the sheet reads as something laid over the screens
// rather than as another screen. Cards a step lighter again.
inline constexpr uint32_t COLOUR_SHEET = 0x0E0E10;
inline constexpr uint32_t COLOUR_CARD = 0x1C1C1E;
inline constexpr uint32_t COLOUR_CONTROL = 0x3A3A3C;
// The grid's blue doubles as the one accent for anything you can touch. The data
// screens never put grid blue on a control, so it cannot be mistaken for data.
inline constexpr uint32_t COLOUR_ACCENT = PUCK_COLOUR_GRID;

inline constexpr uint32_t OPEN_MS = 220;
// Left open and untouched, the sheet gets out of the way. A wall display parked
// on its settings is not doing its job.
inline constexpr uint32_t IDLE_CLOSE_MS = 60000;
// How long a "tap again to confirm" button stays armed.
inline constexpr uint32_t CONFIRM_MS = 3000;

// ----------------------------------------------------------------- widgets ---

lv_obj_t* plain(lv_obj_t* parent);
lv_obj_t* text(lv_obj_t* parent, const lv_font_t* font, uint32_t colour, const char* value);
lv_obj_t* caption(lv_obj_t* parent, const char* value);
lv_obj_t* hint(lv_obj_t* parent, const char* value);
lv_obj_t* card(lv_obj_t* parent, lv_coord_t width, lv_coord_t height);
lv_obj_t* row(lv_obj_t* parent);
lv_obj_t* spacer(lv_obj_t* parent);
lv_obj_t* divider(lv_obj_t* parent);
void keep_gestures(lv_obj_t* obj);
lv_obj_t* slider(lv_obj_t* parent, int32_t min, int32_t max);
lv_obj_t* toggle(lv_obj_t* parent);
void set_checked(lv_obj_t* obj, bool on);
void set_disabled(lv_obj_t* obj, bool disabled);
void set_hidden(lv_obj_t* obj, bool hidden);
lv_obj_t* button(lv_obj_t* parent, const char* label, uint32_t background, uint32_t foreground);
void button_set_text(lv_obj_t* obj, const char* label);
lv_obj_t* signal_bars(lv_obj_t* parent);
int bars_for(int8_t rssi);
const char* signal_word(int8_t rssi);
void set_bars(lv_obj_t* bars, int8_t rssi, bool present);
// A full-sheet dimmed layer for a modal to sit on, hidden.
lv_obj_t* modal_backdrop(lv_obj_t* parent);

void format_bytes(char* out, size_t size, uint32_t bytes);
void format_duration(char* out, size_t size, uint32_t seconds);
void format_step(char* out, size_t size, uint32_t seconds);
void format_clock(char* out, size_t size, uint16_t minutes);
int to_percent(uint8_t level);
uint8_t from_percent(int32_t percent);
const char* screen_name(PuckScreen screen);

template <size_t N>
int nearest_step(const uint32_t (&steps)[N], uint32_t value) {
  int best = 0;
  for (size_t i = 0; i < N; ++i) {
    if (steps[i] <= value) {
      best = static_cast<int>(i);
    }
  }
  return best;
}

// ------------------------------------------------------------------ keypad ---

enum class KeypadMode : uint8_t {
  Address,  // digits and points: an IPv4 address
  Decimal,  // a number that may have a fraction
  Signed,   // the same, and may be negative
  Whole,    // digits only
};

// Called on Save. Return false, with a reason in `error`, to keep the pad up.
using KeypadSave = bool (*)(const char* text, char* error, size_t error_size);

void keypad_build(lv_obj_t* parent);
void keypad_open(const char* title, const char* initial, KeypadMode mode, KeypadSave save);
void keypad_close();
bool keypad_is_open();

bool valid_ipv4(const char* value);
// A whole string that is a number within [min, max].
bool parse_number(const char* value, float min, float max, float* out);

// --------------------------------------------- between the sheet's own files ---

// settings_sheet.cpp: what the owner handed settings_sheet_create().
const SheetActions& sheet_actions();

// settings_sheet_plant.cpp: the Solar page and the Modbus device editor.
void plant_build_page(lv_obj_t* tile);
void plant_build_modals(lv_obj_t* root);
void plant_set_solar(const SheetSolar& solar);
void plant_set_data(const SheetData& data);
void plant_open_devices();
void plant_open_array(int index);
bool plant_modal_open();
void plant_close_modals();
// "2 inverters, 1 charger", for the Data page.
const char* plant_devices_summary(const SheetData& data, char* out, size_t size);

// ----------------------------------------------------------------- confirm ---

void disarm();
// True on the second tap. The first only arms the button.
bool confirmed(lv_obj_t* obj, const char* armed_text);

}  // namespace sheet_kit
