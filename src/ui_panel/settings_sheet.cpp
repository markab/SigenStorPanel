#include "settings_sheet.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "qr_block.h"
#include "theme.h"
#include "settings_sheet_internal.h"
#include "ui.h"

namespace {

using namespace sheet_kit;

constexpr int PAGE_COUNT = static_cast<int>(SheetPage::Count);
const char* const PAGE_NAMES[PAGE_COUNT] = {"Display", "Screens", "WiFi", "Data", "Solar",
                                            "Device"};

// Steps for the sliders that pick from a list rather than a range.
constexpr uint32_t DIM_AFTER_STEPS[] = {0, 15, 30, 60, 120, 300, 600, 1800};
constexpr uint32_t CYCLE_STEPS[] = {5, 10, 15, 20, 30, 45, 60, 120, 300};
constexpr uint32_t POLL_STEPS[] = {2, 3, 5, 10, 15, 30, 60};
constexpr uint16_t TIME_STEP_MIN = 15;

// Changes that only take effect when the screens are rebuilt at boot.
enum RestartReason : uint8_t {
  RESTART_SCREENS = 1u << 0,
  RESTART_SOURCE = 1u << 1,
  RESTART_FLIP = 1u << 2,
};

// ------------------------------------------------------------------- state ---

SheetActions s_actions;
SheetDisplay s_display;
SheetScreens s_screens;
SheetWifi s_wifi;
SheetData s_data;
SheetDevice s_device;

// What the device booted with, so a change that is put back clears its
// "restart to apply" rather than leaving it standing.
bool s_have_boot_screens = false;
bool s_have_boot_data = false;
bool s_have_boot_display = false;
uint8_t s_boot_visible = 0xFF;
DataSource s_boot_source = DataSource::Server;
bool s_boot_flipped = false;

bool s_open = false;
SheetPage s_page = SheetPage::Display;

// Render statistics, from LVGL's monitor callback.
uint32_t s_frames_counting = 0;
uint32_t s_frames_last_second = 0;
uint32_t s_last_render_ms = 0;

// ----------------------------------------------------------------- widgets ---

lv_obj_t* s_root = nullptr;
lv_obj_t* s_tabs[PAGE_COUNT] = {};
lv_obj_t* s_pages = nullptr;
lv_obj_t* s_restart_bar = nullptr;
lv_obj_t* s_restart_text = nullptr;

// Display
lv_obj_t* s_bright = nullptr;
lv_obj_t* s_bright_value = nullptr;
lv_obj_t* s_dim = nullptr;
lv_obj_t* s_dim_value = nullptr;
lv_obj_t* s_dim_after = nullptr;
lv_obj_t* s_dim_after_value = nullptr;
lv_obj_t* s_off_switch = nullptr;
lv_obj_t* s_off_times = nullptr;
lv_obj_t* s_off_start = nullptr;
lv_obj_t* s_off_end = nullptr;
lv_obj_t* s_flip_switch = nullptr;

// Screens
constexpr int LISTED_SCREENS = PUCK_SCREEN_COUNT - 1;  // all but settings
PuckScreen s_listed[LISTED_SCREENS] = {};
lv_obj_t* s_screen_name[LISTED_SCREENS] = {};
lv_obj_t* s_screen_note[LISTED_SCREENS] = {};
lv_obj_t* s_screen_show[LISTED_SCREENS] = {};
lv_obj_t* s_screen_cycle[LISTED_SCREENS] = {};
lv_obj_t* s_cycle_switch = nullptr;
lv_obj_t* s_cycle_slider = nullptr;
lv_obj_t* s_cycle_value = nullptr;

// WiFi
lv_obj_t* s_wifi_ssid = nullptr;
lv_obj_t* s_wifi_bars = nullptr;
lv_obj_t* s_wifi_signal = nullptr;
lv_obj_t* s_wifi_ip = nullptr;
lv_obj_t* s_wifi_channel = nullptr;
lv_obj_t* s_saved_rows[SHEET_MAX_NETWORKS] = {};
lv_obj_t* s_saved_bars[SHEET_MAX_NETWORKS] = {};
lv_obj_t* s_saved_ssid[SHEET_MAX_NETWORKS] = {};
lv_obj_t* s_saved_state[SHEET_MAX_NETWORKS] = {};
lv_obj_t* s_saved_forget[SHEET_MAX_NETWORKS] = {};
lv_obj_t* s_saved_none = nullptr;

// Data
lv_obj_t* s_source = nullptr;
lv_obj_t* s_source_detail[3] = {};
lv_obj_t* s_server_host = nullptr;
lv_obj_t* s_server_link = nullptr;
lv_obj_t* s_server_forget = nullptr;
lv_obj_t* s_modbus_field = nullptr;
lv_obj_t* s_modbus_value = nullptr;
lv_obj_t* s_ha_host = nullptr;
lv_obj_t* s_ha_token = nullptr;
lv_obj_t* s_modbus_devices = nullptr;
lv_obj_t* s_poll_slider = nullptr;
lv_obj_t* s_poll_value = nullptr;

// Device
enum PerfRow : uint8_t {
  PERF_RENDER,
  PERF_FPS,
  PERF_INTERNAL,
  PERF_LARGEST,
  PERF_PSRAM,
  PERF_LVGL,
  PERF_UPTIME,
  PERF_WIFI,
  PERF_LAST_READ,
  PERF_FAILURES,
  PERF_COUNT,
};
const char* const PERF_NAMES[PERF_COUNT] = {
    "Last redraw",      "Frames last second", "Internal RAM free", "Largest free block",
    "PSRAM free",       "LVGL memory",        "Uptime",            "WiFi signal",
    "Last reading",     "Failures in a row",
};
lv_obj_t* s_perf[PERF_COUNT] = {};
lv_obj_t* s_firmware = nullptr;
lv_obj_t* s_update_text = nullptr;
lv_obj_t* s_check_button = nullptr;
lv_obj_t* s_install_button = nullptr;
lv_obj_t* s_restart_button = nullptr;
lv_obj_t* s_qr = nullptr;
lv_obj_t* s_web_address = nullptr;
char s_qr_url[64] = {};

// Keypad, for the Modbus plant address.

// Network picker and the password screen.
SheetScan s_scan;
SheetJoinStatus s_join;
lv_obj_t* s_picker = nullptr;
lv_obj_t* s_picker_status = nullptr;
lv_obj_t* s_picker_rows[SHEET_MAX_SCAN] = {};
lv_obj_t* s_picker_bars[SHEET_MAX_SCAN] = {};
lv_obj_t* s_picker_ssid[SHEET_MAX_SCAN] = {};
lv_obj_t* s_picker_kind[SHEET_MAX_SCAN] = {};
lv_obj_t* s_join_panel = nullptr;
lv_obj_t* s_join_title = nullptr;
lv_obj_t* s_password_line = nullptr;
lv_obj_t* s_password_field = nullptr;
lv_obj_t* s_password_show = nullptr;
lv_obj_t* s_join_status = nullptr;
lv_obj_t* s_join_button = nullptr;
lv_obj_t* s_keyboard = nullptr;
char s_join_ssid[33] = {};
bool s_join_secure = true;
// Long enough for an enrolment link or a Home Assistant token, not just a
// WiFi password: the same screen types all of them.
char s_password[256] = {};
// What the keyboard screen is for.
enum class EntryKind : uint8_t { WifiPassword, EnrolmentLink, HaAddress, HaToken };
EntryKind s_entry_kind = EntryKind::WifiPassword;
char s_entry_error[64] = {};
bool s_password_visible = false;
// The join this panel asked for, so a status left over from an earlier one is
// not mistaken for its answer.
bool s_join_requested = false;

enum class KeyboardMode : uint8_t { Lower, Upper, Digits, Symbols };
KeyboardMode s_keyboard_mode = KeyboardMode::Lower;

// The install screen: over everything, sheet open or not.
lv_obj_t* s_updating = nullptr;
lv_obj_t* s_updating_title = nullptr;
lv_obj_t* s_updating_bar = nullptr;
lv_obj_t* s_updating_percent = nullptr;

lv_obj_t* s_auto_check = nullptr;
lv_obj_t* s_device_dot = nullptr;

// ----------------------------------------------------------------- refresh ---

void refresh_restart_bar() {
  uint8_t reasons = 0;
  if (s_have_boot_screens && s_screens.visible != s_boot_visible) {
    reasons |= RESTART_SCREENS;
  }
  if (s_have_boot_data && s_data.source != s_boot_source) {
    reasons |= RESTART_SOURCE;
  }
  if (s_have_boot_display && s_display.flipped != s_boot_flipped) {
    reasons |= RESTART_FLIP;
  }
  if (reasons == 0) {
    lv_obj_add_flag(s_restart_bar, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  char line[96] = "Restart to apply:";
  const char* separator = " ";
  if (reasons & RESTART_SCREENS) {
    strncat(line, separator, sizeof(line) - strlen(line) - 1);
    strncat(line, "screens shown", sizeof(line) - strlen(line) - 1);
    separator = ", ";
  }
  if (reasons & RESTART_SOURCE) {
    strncat(line, separator, sizeof(line) - strlen(line) - 1);
    strncat(line, "data source", sizeof(line) - strlen(line) - 1);
    separator = ", ";
  }
  if (reasons & RESTART_FLIP) {
    strncat(line, separator, sizeof(line) - strlen(line) - 1);
    strncat(line, "upside down", sizeof(line) - strlen(line) - 1);
  }
  lv_label_set_text(s_restart_text, line);
  lv_obj_clear_flag(s_restart_bar, LV_OBJ_FLAG_HIDDEN);
}

void refresh_display() {
  char value[24];
  if (!lv_obj_has_state(s_bright, LV_STATE_PRESSED)) {
    lv_slider_set_value(s_bright, to_percent(s_display.brightness), LV_ANIM_OFF);
  }
  snprintf(value, sizeof(value), "%d%%", to_percent(s_display.brightness));
  lv_label_set_text(s_bright_value, value);

  if (!lv_obj_has_state(s_dim, LV_STATE_PRESSED)) {
    lv_slider_set_value(s_dim, to_percent(s_display.dim_brightness), LV_ANIM_OFF);
  }
  snprintf(value, sizeof(value), "%d%%", to_percent(s_display.dim_brightness));
  lv_label_set_text(s_dim_value, value);

  const int dim_step = nearest_step(DIM_AFTER_STEPS, s_display.dim_after_s);
  if (!lv_obj_has_state(s_dim_after, LV_STATE_PRESSED)) {
    lv_slider_set_value(s_dim_after, dim_step, LV_ANIM_OFF);
  }
  format_step(value, sizeof(value), DIM_AFTER_STEPS[dim_step]);
  lv_label_set_text(s_dim_after_value, value);
  // Dimming to a level is meaningless when it never dims.
  set_disabled(s_dim, s_display.dim_after_s == 0);

  set_checked(s_off_switch, s_display.screen_off_set);
  format_clock(value, sizeof(value), s_display.screen_off_start_min);
  lv_label_set_text(s_off_start, value);
  format_clock(value, sizeof(value), s_display.screen_off_end_min);
  lv_label_set_text(s_off_end, value);
  lv_obj_set_style_opa(s_off_times, s_display.screen_off_set ? LV_OPA_COVER : LV_OPA_40,
                       LV_PART_MAIN);

  set_checked(s_flip_switch, s_display.flipped);
  refresh_restart_bar();
}

void refresh_screens() {
  for (int i = 0; i < LISTED_SCREENS; ++i) {
    const PuckScreen screen = s_listed[i];
    const uint8_t bit = static_cast<uint8_t>(1u << screen);
    const bool available = (s_screens.available & bit) != 0;
    // The live screen is what every failure falls back to, so ui_create()
    // builds it whatever the mask says. Shown as on and fixed, not as a switch
    // that does nothing.
    const bool fixed = screen == PUCK_SCREEN_POWER;
    const bool shown = fixed || (s_screens.visible & bit) != 0;

    set_checked(s_screen_show[i], available && shown);
    set_checked(s_screen_cycle[i], available && shown && (s_screens.rotate & bit) != 0);
    set_disabled(s_screen_show[i], !available || fixed);
    set_disabled(s_screen_cycle[i], !available || !shown);
    lv_obj_set_style_text_color(s_screen_name[i],
                                lv_color_hex(available ? PUCK_COLOUR_TEXT : PUCK_COLOUR_MUTED),
                                LV_PART_MAIN);
    if (!available) {
      lv_label_set_text(s_screen_note[i], "needs the server");
    } else if (fixed) {
      lv_label_set_text(s_screen_note[i], "always shown");
    } else {
      lv_label_set_text(s_screen_note[i], "");
    }
  }

  set_checked(s_cycle_switch, s_screens.rotate_enabled);
  const int step = nearest_step(CYCLE_STEPS, s_screens.rotate_s);
  if (!lv_obj_has_state(s_cycle_slider, LV_STATE_PRESSED)) {
    lv_slider_set_value(s_cycle_slider, step, LV_ANIM_OFF);
  }
  char value[16];
  format_step(value, sizeof(value), CYCLE_STEPS[step]);
  lv_label_set_text(s_cycle_value, value);
  set_disabled(s_cycle_slider, !s_screens.rotate_enabled);
  refresh_restart_bar();
}

void refresh_wifi() {
  char line[64];
  if (s_wifi.connected) {
    lv_label_set_text(s_wifi_ssid, s_wifi.ssid);
    set_bars(s_wifi_bars, s_wifi.rssi, true);
    snprintf(line, sizeof(line), "%s, %d dBm", signal_word(s_wifi.rssi), s_wifi.rssi);
    lv_label_set_text(s_wifi_signal, line);
    lv_label_set_text(s_wifi_ip, s_wifi.ip);
    snprintf(line, sizeof(line), "%u", static_cast<unsigned>(s_wifi.channel));
    lv_label_set_text(s_wifi_channel, line);
  } else {
    lv_label_set_text(s_wifi_ssid, "Not connected");
    set_bars(s_wifi_bars, 0, false);
    lv_label_set_text(s_wifi_signal, "--");
    lv_label_set_text(s_wifi_ip, "--");
    lv_label_set_text(s_wifi_channel, "--");
  }

  for (int i = 0; i < SHEET_MAX_NETWORKS; ++i) {
    if (i >= s_wifi.saved_count) {
      lv_obj_add_flag(s_saved_rows[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    const SheetNetwork& network = s_wifi.saved[i];
    const bool current = s_wifi.connected && strcmp(network.ssid, s_wifi.ssid) == 0;
    lv_obj_clear_flag(s_saved_rows[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_saved_ssid[i], network.ssid);
    set_bars(s_saved_bars[i], network.rssi, network.in_range);
    lv_label_set_text(s_saved_state[i], current            ? "connected"
                                        : network.in_range ? "in range"
                                                           : "out of range");
    lv_obj_set_style_text_color(s_saved_state[i],
                                lv_color_hex(current ? PUCK_COLOUR_BATTERY : PUCK_COLOUR_MUTED),
                                LV_PART_MAIN);
  }
  set_hidden(s_saved_none, s_wifi.saved_count != 0);
}

void refresh_data() {
  const int index = static_cast<int>(s_data.source);
  lv_btnmatrix_set_btn_ctrl(s_source, static_cast<uint16_t>(index), LV_BTNMATRIX_CTRL_CHECKED);
  for (int i = 0; i < 3; ++i) {
    set_hidden(s_source_detail[i], i != index);
  }

  lv_label_set_text(s_server_host, s_data.server_set ? s_data.server_host : "--");
  lv_label_set_text(s_server_link, s_data.server_set ? "Set" : "Not set");
  lv_obj_set_style_text_color(s_server_link,
                              lv_color_hex(s_data.server_set ? PUCK_COLOUR_BATTERY : PUCK_COLOUR_WARN),
                              LV_PART_MAIN);
  set_disabled(s_server_forget, !s_data.server_set);

  lv_label_set_text(s_modbus_value, s_data.modbus_host[0] != '\0' ? s_data.modbus_host : "Tap to set");
  lv_obj_set_style_text_color(
      s_modbus_value,
      lv_color_hex(s_data.modbus_host[0] != '\0' ? PUCK_COLOUR_TEXT : PUCK_COLOUR_MUTED),
      LV_PART_MAIN);

  char devices[48];
  lv_label_set_text(s_modbus_devices, plant_devices_summary(s_data, devices, sizeof(devices)));

  lv_label_set_text(s_ha_host, s_data.ha_host[0] != '\0' ? s_data.ha_host : "--");
  lv_label_set_text(s_ha_token, s_data.ha_set ? "Set" : "Not set");
  lv_obj_set_style_text_color(s_ha_token,
                              lv_color_hex(s_data.ha_set ? PUCK_COLOUR_BATTERY : PUCK_COLOUR_WARN),
                              LV_PART_MAIN);

  const int step = nearest_step(POLL_STEPS, s_data.poll_s);
  if (!lv_obj_has_state(s_poll_slider, LV_STATE_PRESSED)) {
    lv_slider_set_value(s_poll_slider, step, LV_ANIM_OFF);
  }
  char value[16];
  format_step(value, sizeof(value), POLL_STEPS[step]);
  lv_label_set_text(s_poll_value, value);
  refresh_restart_bar();
}

void refresh_device() {
  char value[48];

  if (s_last_render_ms > 0 || s_frames_last_second > 0) {
    snprintf(value, sizeof(value), "%u ms", static_cast<unsigned>(s_last_render_ms));
  } else {
    snprintf(value, sizeof(value), "--");
  }
  lv_label_set_text(s_perf[PERF_RENDER], value);
  snprintf(value, sizeof(value), "%u", static_cast<unsigned>(s_frames_last_second));
  lv_label_set_text(s_perf[PERF_FPS], value);

  if (s_device.internal_free > 0) {
    format_bytes(value, sizeof(value), s_device.internal_free);
    lv_label_set_text(s_perf[PERF_INTERNAL], value);
    format_bytes(value, sizeof(value), s_device.internal_largest);
    lv_label_set_text(s_perf[PERF_LARGEST], value);
  } else {
    lv_label_set_text(s_perf[PERF_INTERNAL], "--");
    lv_label_set_text(s_perf[PERF_LARGEST], "--");
  }
  if (s_device.psram_total > 0) {
    char total[16];
    format_bytes(value, sizeof(value), s_device.psram_free);
    format_bytes(total, sizeof(total), s_device.psram_total);
    strncat(value, " of ", sizeof(value) - strlen(value) - 1);
    strncat(value, total, sizeof(value) - strlen(value) - 1);
    lv_label_set_text(s_perf[PERF_PSRAM], value);
  } else {
    lv_label_set_text(s_perf[PERF_PSRAM], "--");
  }

  // Zero when LVGL allocates from the system heap rather than its own pool.
  lv_mem_monitor_t mem;
  lv_mem_monitor(&mem);
  if (mem.total_size > 0) {
    snprintf(value, sizeof(value), "%u%% of %u KB", static_cast<unsigned>(mem.used_pct),
             static_cast<unsigned>(mem.total_size / 1024));
  } else {
    snprintf(value, sizeof(value), "system heap");
  }
  lv_label_set_text(s_perf[PERF_LVGL], value);

  format_duration(value, sizeof(value), s_device.uptime_s);
  lv_label_set_text(s_perf[PERF_UPTIME], value);

  if (s_device.wifi_connected) {
    snprintf(value, sizeof(value), "%d dBm", s_device.rssi);
  } else {
    snprintf(value, sizeof(value), "not connected");
  }
  lv_label_set_text(s_perf[PERF_WIFI], value);

  if (s_device.ever_read) {
    format_duration(value, sizeof(value), s_device.last_read_age_s);
    strncat(value, " ago", sizeof(value) - strlen(value) - 1);
  } else {
    snprintf(value, sizeof(value), "never");
  }
  lv_label_set_text(s_perf[PERF_LAST_READ], value);

  if (s_device.failures_in_a_row > 0 && s_device.last_result[0] != '\0') {
    snprintf(value, sizeof(value), "%u, %s", static_cast<unsigned>(s_device.failures_in_a_row),
             s_device.last_result);
  } else {
    snprintf(value, sizeof(value), "%u", static_cast<unsigned>(s_device.failures_in_a_row));
  }
  lv_label_set_text(s_perf[PERF_FAILURES], value);
  lv_obj_set_style_text_color(
      s_perf[PERF_FAILURES],
      lv_color_hex(s_device.failures_in_a_row > 0 ? PUCK_COLOUR_WARN : PUCK_COLOUR_TEXT),
      LV_PART_MAIN);

  snprintf(value, sizeof(value), "Version %s", s_device.firmware);
  lv_label_set_text(s_firmware, value);
  const char* update = "";
  switch (s_device.update) {
    case SheetUpdate::Idle:
      update = "Not checked yet";
      break;
    case SheetUpdate::Checking:
      update = "Checking...";
      break;
    case SheetUpdate::UpToDate:
      update = "Up to date";
      break;
    case SheetUpdate::Available:
      snprintf(value, sizeof(value), "Version %s is available", s_device.update_version);
      update = value;
      break;
    case SheetUpdate::Installing:
      update = "Installing. The panel restarts when it is done";
      break;
    case SheetUpdate::Failed:
      update = s_device.update_detail[0] != '\0' ? s_device.update_detail : "Check failed";
      break;
  }
  lv_label_set_text(s_update_text, update);
  lv_obj_set_style_text_color(
      s_update_text,
      lv_color_hex(s_device.update == SheetUpdate::Available ? PUCK_COLOUR_BATTERY
                   : s_device.update == SheetUpdate::Failed  ? PUCK_COLOUR_WARN
                                                             : PUCK_COLOUR_MUTED),
      LV_PART_MAIN);
  set_hidden(s_install_button, s_device.update != SheetUpdate::Available);
  set_hidden(s_device_dot, s_device.update != SheetUpdate::Available);
  set_checked(s_auto_check, s_device.auto_check);
  set_disabled(s_check_button, s_device.update == SheetUpdate::Checking ||
                                   s_device.update == SheetUpdate::Installing);

  char url[64];
  if (s_device.ip[0] != '\0') {
    snprintf(url, sizeof(url), "http://%s/", s_device.ip);
  } else {
    url[0] = '\0';
  }
  if (strcmp(url, s_qr_url) != 0) {
    snprintf(s_qr_url, sizeof(s_qr_url), "%s", url);
    puck_qr_block_set_url(s_qr, s_qr_url[0] != '\0' ? s_qr_url : "http://0.0.0.0/");
  }
  set_hidden(s_qr, s_qr_url[0] == '\0');
  if (s_device.ip[0] != '\0') {
    snprintf(value, sizeof(value), "%s.local\n%s", s_device.host, s_device.ip);
  } else {
    snprintf(value, sizeof(value), "Not on a network yet");
  }
  lv_label_set_text(s_web_address, value);
}

void refresh_tabs() {
  for (int i = 0; i < PAGE_COUNT; ++i) {
    const bool here = i == static_cast<int>(s_page);
    lv_obj_set_style_bg_opa(s_tabs[i], here ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(s_tabs[i], 0),
                                lv_color_hex(here ? PUCK_COLOUR_TEXT : PUCK_COLOUR_MUTED),
                                LV_PART_MAIN);
  }
}

// ------------------------------------------------------------------ events ---

void save_display() {
  if (s_actions.set_display != nullptr) {
    s_actions.set_display(s_display);
  }
}

void save_screens() {
  if (s_actions.set_screens != nullptr) {
    s_actions.set_screens(s_screens);
  }
}

void on_bright(lv_event_t* event) {
  s_display.brightness = from_percent(lv_slider_get_value(s_bright));
  if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
    save_display();
  } else if (s_actions.preview_brightness != nullptr) {
    s_actions.preview_brightness(s_display.brightness);
  }
  refresh_display();
}

// While the dim slider is held, the panel shows the dim level, so it can be
// judged on the glass. Letting go puts full brightness back and saves.
void on_dim(lv_event_t* event) {
  s_display.dim_brightness = from_percent(lv_slider_get_value(s_dim));
  if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
    if (s_actions.preview_brightness != nullptr) {
      s_actions.preview_brightness(s_display.brightness);
    }
    save_display();
  } else if (s_actions.preview_brightness != nullptr) {
    s_actions.preview_brightness(s_display.dim_brightness);
  }
  refresh_display();
}

void on_dim_after(lv_event_t* event) {
  s_display.dim_after_s = DIM_AFTER_STEPS[lv_slider_get_value(s_dim_after)];
  if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
    save_display();
  }
  refresh_display();
}

void on_off_switch(lv_event_t* /*event*/) {
  s_display.screen_off_set = lv_obj_has_state(s_off_switch, LV_STATE_CHECKED);
  // A zero-length window is refused by the store, so never offer one.
  if (s_display.screen_off_start_min == s_display.screen_off_end_min) {
    s_display.screen_off_end_min = (s_display.screen_off_start_min + 8 * 60) % 1440;
  }
  save_display();
  refresh_display();
}

// User data: 0 start-, 1 start+, 2 end-, 3 end+.
void on_time_step(lv_event_t* event) {
  const intptr_t which = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
  uint16_t& minutes =
      which < 2 ? s_display.screen_off_start_min : s_display.screen_off_end_min;
  const uint16_t other =
      which < 2 ? s_display.screen_off_end_min : s_display.screen_off_start_min;
  const int delta = (which % 2 == 0) ? -TIME_STEP_MIN : TIME_STEP_MIN;
  int next = (static_cast<int>(minutes) + delta + 1440) % 1440;
  if (next == other) {
    next = (next + delta + 1440) % 1440;
  }
  minutes = static_cast<uint16_t>(next);
  s_display.screen_off_set = true;
  save_display();
  refresh_display();
}

void on_flip(lv_event_t* /*event*/) {
  s_display.flipped = lv_obj_has_state(s_flip_switch, LV_STATE_CHECKED);
  save_display();
  refresh_display();
}

// User data: the index into s_listed.
void on_screen_show(lv_event_t* event) {
  const intptr_t i = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
  const uint8_t bit = static_cast<uint8_t>(1u << s_listed[i]);
  if (lv_obj_has_state(s_screen_show[i], LV_STATE_CHECKED)) {
    s_screens.visible |= bit;
  } else {
    s_screens.visible &= static_cast<uint8_t>(~bit);
  }
  save_screens();
  refresh_screens();
}

void on_screen_cycle(lv_event_t* event) {
  const intptr_t i = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
  const uint8_t bit = static_cast<uint8_t>(1u << s_listed[i]);
  if (lv_obj_has_state(s_screen_cycle[i], LV_STATE_CHECKED)) {
    s_screens.rotate |= bit;
  } else {
    s_screens.rotate &= static_cast<uint8_t>(~bit);
  }
  save_screens();
  refresh_screens();
}

void on_cycle_switch(lv_event_t* /*event*/) {
  s_screens.rotate_enabled = lv_obj_has_state(s_cycle_switch, LV_STATE_CHECKED);
  save_screens();
  refresh_screens();
}

void on_cycle_slider(lv_event_t* event) {
  s_screens.rotate_s = CYCLE_STEPS[lv_slider_get_value(s_cycle_slider)];
  if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
    save_screens();
  }
  refresh_screens();
}

void on_forget(lv_event_t* event) {
  const intptr_t i = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
  if (!confirmed(s_saved_forget[i], "Forget?")) {
    return;
  }
  if (s_actions.forget_network != nullptr && i < s_wifi.saved_count) {
    s_actions.forget_network(s_wifi.saved[i].ssid);
  }
}

void on_add_network(lv_event_t* /*event*/) {
  disarm();
  settings_sheet_show_picker();
}

void on_source(lv_event_t* /*event*/) {
  const uint16_t selected = lv_btnmatrix_get_selected_btn(s_source);
  if (selected > 2) {
    return;
  }
  s_data.source = static_cast<DataSource>(selected);
  if (s_actions.set_source != nullptr) {
    s_actions.set_source(s_data.source);
  }
  refresh_data();
}

void on_forget_server(lv_event_t* /*event*/) {
  if (!confirmed(s_server_forget, "Forget link?")) {
    return;
  }
  if (s_actions.forget_server != nullptr) {
    s_actions.forget_server();
  }
}

void on_poll(lv_event_t* event) {
  s_data.poll_s = POLL_STEPS[lv_slider_get_value(s_poll_slider)];
  if (lv_event_get_code(event) == LV_EVENT_RELEASED && s_actions.set_poll_interval != nullptr) {
    s_actions.set_poll_interval(s_data.poll_s);
  }
  refresh_data();
}

void on_auto_check(lv_event_t* /*event*/) {
  s_device.auto_check = lv_obj_has_state(s_auto_check, LV_STATE_CHECKED);
  if (s_actions.set_auto_check != nullptr) {
    s_actions.set_auto_check(s_device.auto_check);
  }
}

void on_check(lv_event_t* /*event*/) {
  disarm();
  if (s_actions.check_update != nullptr) {
    s_actions.check_update();
  }
}

void on_install(lv_event_t* /*event*/) {
  if (!confirmed(s_install_button, "Install now?")) {
    return;
  }
  if (s_actions.install_update != nullptr) {
    s_actions.install_update();
  }
}

void on_restart(lv_event_t* event) {
  lv_obj_t* target = lv_event_get_current_target(event);
  if (!confirmed(target, "Restart now?")) {
    return;
  }
  if (s_actions.restart != nullptr) {
    s_actions.restart();
  }
}

// ------------------------------------------------------------------ keypad ---

bool save_modbus_host(const char* value, char* error, size_t error_size) {
  if (!valid_ipv4(value)) {
    snprintf(error, error_size, "That is not an IP address");
    return false;
  }
  snprintf(s_data.modbus_host, sizeof(s_data.modbus_host), "%s", value);
  if (s_actions.set_modbus_host != nullptr) {
    s_actions.set_modbus_host(s_data.modbus_host);
  }
  refresh_data();
  return true;
}

void open_modbus_keypad() {
  keypad_open("PLANT ADDRESS", s_data.modbus_host, KeypadMode::Address, save_modbus_host);
}

// ------------------------------------------------------------- open, close ---

void set_translate(void* obj, int32_t value) {
  lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(obj), static_cast<lv_coord_t>(value),
                               LV_PART_MAIN);
}

void slide(int32_t from, int32_t to, lv_anim_ready_cb_t done) {
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, s_root);
  lv_anim_set_exec_cb(&anim, set_translate);
  lv_anim_set_values(&anim, from, to);
  lv_anim_set_time(&anim, OPEN_MS);
  lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
  if (done != nullptr) {
    lv_anim_set_ready_cb(&anim, done);
  }
  lv_anim_start(&anim);
}

bool modal_open() {
  return keypad_is_open() || plant_modal_open() ||
         !lv_obj_has_flag(s_picker, LV_OBJ_FLAG_HIDDEN) ||
         !lv_obj_has_flag(s_join_panel, LV_OBJ_FLAG_HIDDEN);
}

void on_gesture(lv_event_t* /*event*/) {
  lv_indev_t* indev = lv_indev_get_act();
  if (indev == nullptr) {
    return;
  }
  const lv_dir_t direction = lv_indev_get_gesture_dir(indev);
  if (!lv_obj_has_flag(s_updating, LV_OBJ_FLAG_HIDDEN)) {
    return;  // nothing to do while an update installs
  }
  if (!s_open && direction == LV_DIR_TOP) {
    settings_sheet_open(s_page, true);
    lv_indev_wait_release(indev);
  } else if (s_open && direction == LV_DIR_BOTTOM && !modal_open()) {
    settings_sheet_close(true);
    lv_indev_wait_release(indev);
  }
}

void on_page_changed(lv_event_t* /*event*/) {
  lv_obj_t* active = lv_tileview_get_tile_act(s_pages);
  for (int i = 0; i < PAGE_COUNT; ++i) {
    if (lv_obj_get_child(s_pages, i) == active) {
      s_page = static_cast<SheetPage>(i);
    }
  }
  disarm();
  refresh_tabs();
  if (s_page == SheetPage::Device) {
    refresh_device();
  }
}

void on_tab(lv_event_t* event) {
  const intptr_t i = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
  lv_obj_set_tile_id(s_pages, static_cast<uint32_t>(i), 0, LV_ANIM_ON);
}

void monitor_cb(lv_disp_drv_t* /*driver*/, uint32_t time_ms, uint32_t /*pixels*/) {
  ++s_frames_counting;
  s_last_render_ms = time_ms;
}

void tick(lv_timer_t* /*timer*/) {
  s_frames_last_second = s_frames_counting;
  s_frames_counting = 0;
  if (!s_open) {
    return;
  }
  // Not while picking a network: with nothing saved, that is the only thing the
  // panel can usefully show.
  if (lv_disp_get_inactive_time(nullptr) > IDLE_CLOSE_MS && !modal_open()) {
    settings_sheet_close(true);
    return;
  }
  if (s_page == SheetPage::Device) {
    refresh_device();
  }
}

// ------------------------------------------------------------------- build ---

lv_obj_t* page_body(lv_obj_t* tile) {
  lv_obj_t* body = plain(tile);
  lv_obj_set_size(body, SHEET_W, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_hor(body, PAGE_PAD_X, LV_PART_MAIN);
  lv_obj_set_style_pad_top(body, PAGE_PAD_TOP, LV_PART_MAIN);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(body, CARD_GAP, LV_PART_MAIN);
  return body;
}

// A caption above a slider, with the value at the right of the caption line.
lv_obj_t* labelled_slider(lv_obj_t* parent, const char* name, int32_t min, int32_t max,
                          lv_obj_t** value) {
  lv_obj_t* line = row(parent);
  text(line, FONT_ROW, PUCK_COLOUR_TEXT, name);
  *value = text(line, FONT_ROW, PUCK_COLOUR_TEXT, "");
  return slider(parent, min, max);
}

lv_obj_t* labelled_toggle(lv_obj_t* parent, const char* name) {
  lv_obj_t* line = row(parent);
  text(line, FONT_ROW, PUCK_COLOUR_TEXT, name);
  return toggle(line);
}

// "FROM   [-] 22:30 [+]" on one line.
lv_obj_t* time_stepper(lv_obj_t* parent, const char* name, intptr_t base, lv_obj_t** value) {
  lv_obj_t* outer = row(parent);
  caption(outer, name);

  lv_obj_t* line = plain(outer);
  lv_obj_set_size(line, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(line, 8, LV_PART_MAIN);

  lv_obj_t* minus = button(line, "-", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_set_style_pad_hor(minus, 16, LV_PART_MAIN);
  lv_obj_add_event_cb(minus, on_time_step, LV_EVENT_CLICKED, reinterpret_cast<void*>(base));
  *value = text(line, FONT_ROW, PUCK_COLOUR_TEXT, "00:00");
  lv_obj_set_width(*value, 80);
  lv_obj_set_style_text_align(*value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_t* plus = button(line, "+", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_set_style_pad_hor(plus, 16, LV_PART_MAIN);
  lv_obj_add_event_cb(plus, on_time_step, LV_EVENT_CLICKED, reinterpret_cast<void*>(base + 1));
  return outer;
}

void build_display(lv_obj_t* tile) {
  lv_obj_t* body = page_body(tile);

  lv_obj_t* left = card(body, 560, CARD_H);
  caption(left, "BRIGHTNESS");
  s_bright = labelled_slider(left, "In use", 3, 100, &s_bright_value);
  lv_obj_add_event_cb(s_bright, on_bright, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(s_bright, on_bright, LV_EVENT_RELEASED, nullptr);
  s_dim = labelled_slider(left, "Dimmed", 3, 100, &s_dim_value);
  lv_obj_set_style_opa(s_dim, LV_OPA_40, LV_STATE_DISABLED);
  lv_obj_add_event_cb(s_dim, on_dim, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(s_dim, on_dim, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(s_dim, on_dim, LV_EVENT_RELEASED, nullptr);
  s_dim_after = labelled_slider(left, "Dim after", 0,
                                static_cast<int32_t>(sizeof(DIM_AFTER_STEPS) / sizeof(DIM_AFTER_STEPS[0])) - 1,
                                &s_dim_after_value);
  lv_obj_add_event_cb(s_dim_after, on_dim_after, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(s_dim_after, on_dim_after, LV_EVENT_RELEASED, nullptr);
  spacer(left);
  hint(left, "Drag to see it on the panel. Saved when you let go. Any touch brings full "
             "brightness back.");

  lv_obj_t* right = card(body, 360, CARD_H);
  caption(right, "OVERNIGHT");
  s_off_switch = labelled_toggle(right, "Backlight off");
  lv_obj_add_event_cb(s_off_switch, on_off_switch, LV_EVENT_VALUE_CHANGED, nullptr);
  s_off_times = plain(right);
  lv_obj_set_size(s_off_times, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s_off_times, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_off_times, 8, LV_PART_MAIN);
  time_stepper(s_off_times, "FROM", 0, &s_off_start);
  time_stepper(s_off_times, "UNTIL", 2, &s_off_end);
  hint(right, "A touch lights it again for a while.");
  spacer(right);
  divider(right);
  caption(right, "MOUNTING");
  s_flip_switch = labelled_toggle(right, "Upside down");
  lv_obj_add_event_cb(s_flip_switch, on_flip, LV_EVENT_VALUE_CHANGED, nullptr);
  hint(right, "For hanging it with the cable at the top.");
}

void build_screens(lv_obj_t* tile) {
  lv_obj_t* body = page_body(tile);

  lv_obj_t* left = card(body, 600, CARD_H);
  lv_obj_set_style_pad_row(left, 4, LV_PART_MAIN);
  lv_obj_t* heading = row(left);
  caption(heading, "SCREEN");
  lv_obj_t* columns = plain(heading);
  lv_obj_set_size(columns, 180, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(columns, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(columns, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  caption(columns, "SHOW");
  caption(columns, "CYCLE");

  int listed = 0;
  for (PuckScreen screen : PUCK_SCREEN_ORDER) {
    if (screen == PUCK_SCREEN_SETTINGS || listed >= LISTED_SCREENS) {
      continue;
    }
    const int i = listed++;
    s_listed[i] = screen;
    lv_obj_t* line = row(left);
    lv_obj_set_height(line, 46);

    lv_obj_t* names = plain(line);
    lv_obj_set_size(names, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(names, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(names, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(names, 12, LV_PART_MAIN);
    s_screen_name[i] = text(names, FONT_ROW, PUCK_COLOUR_TEXT, screen_name(screen));
    s_screen_note[i] = text(names, FONT_HINT, PUCK_COLOUR_MUTED, "");

    lv_obj_t* switches = plain(line);
    lv_obj_set_size(switches, 180, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(switches, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(switches, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    s_screen_show[i] = toggle(switches);
    s_screen_cycle[i] = toggle(switches);
    lv_obj_add_event_cb(s_screen_show[i], on_screen_show, LV_EVENT_VALUE_CHANGED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(s_screen_cycle[i], on_screen_cycle, LV_EVENT_VALUE_CHANGED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(i)));
  }

  lv_obj_t* right = card(body, 320, CARD_H);
  caption(right, "AUTO-CYCLE");
  s_cycle_switch = labelled_toggle(right, "Cycle screens");
  lv_obj_add_event_cb(s_cycle_switch, on_cycle_switch, LV_EVENT_VALUE_CHANGED, nullptr);
  s_cycle_slider = labelled_slider(right, "Every", 0,
                                   static_cast<int32_t>(sizeof(CYCLE_STEPS) / sizeof(CYCLE_STEPS[0])) - 1,
                                   &s_cycle_value);
  lv_obj_set_style_opa(s_cycle_slider, LV_OPA_40, LV_STATE_DISABLED);
  lv_obj_add_event_cb(s_cycle_slider, on_cycle_slider, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(s_cycle_slider, on_cycle_slider, LV_EVENT_RELEASED, nullptr);
  hint(right, "Visits the screens marked Cycle. Waits while someone is touching the panel.");
  spacer(right);
  hint(right, "Showing or hiding a screen takes effect after a restart.");
}

void build_wifi(lv_obj_t* tile) {
  lv_obj_t* body = page_body(tile);

  lv_obj_t* left = card(body, 420, CARD_H);
  caption(left, "CONNECTED TO");
  lv_obj_t* name_line = plain(left);
  lv_obj_set_size(name_line, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(name_line, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(name_line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(name_line, 14, LV_PART_MAIN);
  s_wifi_bars = signal_bars(name_line);
  s_wifi_ssid = text(name_line, FONT_BIG, PUCK_COLOUR_TEXT, "");
  lv_label_set_long_mode(s_wifi_ssid, LV_LABEL_LONG_DOT);
  lv_obj_set_width(s_wifi_ssid, 320);
  divider(left);

  const auto detail = [left](const char* name) {
    lv_obj_t* line = row(left);
    text(line, FONT_TEXT, PUCK_COLOUR_MUTED, name);
    return text(line, FONT_TEXT, PUCK_COLOUR_TEXT, "");
  };
  s_wifi_signal = detail("Signal");
  s_wifi_ip = detail("IP address");
  s_wifi_channel = detail("Channel");
  spacer(left);
  hint(left, "With more than one saved network in range, the panel joins the strongest.");

  lv_obj_t* right = card(body, 500, CARD_H);
  caption(right, "SAVED NETWORKS");
  for (int i = 0; i < SHEET_MAX_NETWORKS; ++i) {
    lv_obj_t* line = row(right);
    lv_obj_set_height(line, 48);
    s_saved_rows[i] = line;

    lv_obj_t* names = plain(line);
    lv_obj_set_size(names, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(names, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(names, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(names, 12, LV_PART_MAIN);
    s_saved_bars[i] = signal_bars(names);
    lv_obj_t* stack = plain(names);
    lv_obj_set_size(stack, 250, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stack, LV_FLEX_FLOW_COLUMN);
    s_saved_ssid[i] = text(stack, FONT_TEXT, PUCK_COLOUR_TEXT, "");
    lv_label_set_long_mode(s_saved_ssid[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_saved_ssid[i], 250);
    s_saved_state[i] = text(stack, FONT_HINT, PUCK_COLOUR_MUTED, "");

    s_saved_forget[i] = button(line, "Forget", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
    lv_obj_set_style_pad_ver(s_saved_forget[i], 8, LV_PART_MAIN);
    lv_obj_add_event_cb(s_saved_forget[i], on_forget, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(i)));
  }
  s_saved_none = hint(right, "None yet.");
  spacer(right);
  lv_obj_t* add = button(right, "Add a network", COLOUR_ACCENT, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(add, on_add_network, LV_EVENT_CLICKED, nullptr);
  hint(right, "Pick a network and type its password here. Up to five are remembered.");
}

lv_obj_t* button_row(lv_obj_t* parent) {
  lv_obj_t* line = plain(parent);
  lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(line, 10, LV_PART_MAIN);
  return line;
}

void build_data(lv_obj_t* tile) {
  lv_obj_t* body = page_body(tile);

  lv_obj_t* left = card(body, 560, CARD_H);
  caption(left, "SOURCE");
  // Not const: LVGL 8 takes the map as `const char* []`.
  static const char* SOURCE_MAP[] = {"Server", "Modbus", "Home Assistant", ""};
  s_source = lv_btnmatrix_create(left);
  lv_btnmatrix_set_map(s_source, SOURCE_MAP);
  lv_btnmatrix_set_btn_ctrl_all(s_source, LV_BTNMATRIX_CTRL_CHECKABLE);
  lv_btnmatrix_set_one_checked(s_source, true);
  lv_obj_set_size(s_source, LV_PCT(100), 58);
  lv_obj_set_style_bg_color(s_source, lv_color_hex(COLOUR_CONTROL), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_source, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_source, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(s_source, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_source, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_gap(s_source, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_source, LV_OPA_TRANSP, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(s_source, 0, LV_PART_ITEMS);
  lv_obj_set_style_border_width(s_source, 0, LV_PART_ITEMS);
  lv_obj_set_style_radius(s_source, 10, LV_PART_ITEMS);
  lv_obj_set_style_text_font(s_source, FONT_TEXT, LV_PART_ITEMS);
  lv_obj_set_style_text_color(s_source, lv_color_hex(PUCK_COLOUR_MUTED), LV_PART_ITEMS);
  const lv_style_selector_t checked =
      static_cast<lv_style_selector_t>(LV_PART_ITEMS) | LV_STATE_CHECKED;
  lv_obj_set_style_bg_opa(s_source, LV_OPA_COVER, checked);
  lv_obj_set_style_bg_color(s_source, lv_color_hex(COLOUR_ACCENT), checked);
  lv_obj_set_style_text_color(s_source, lv_color_hex(PUCK_COLOUR_TEXT), checked);
  keep_gestures(s_source);
  lv_obj_add_event_cb(s_source, on_source, LV_EVENT_VALUE_CHANGED, nullptr);

  const auto detail_box = [left]() {
    lv_obj_t* box = plain(left);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_top(box, 8, LV_PART_MAIN);
    return box;
  };
  const auto detail = [](lv_obj_t* box, const char* name) {
    lv_obj_t* line = row(box);
    text(line, FONT_TEXT, PUCK_COLOUR_MUTED, name);
    lv_obj_t* value = text(line, FONT_TEXT, PUCK_COLOUR_TEXT, "");
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(value, 340, LV_PART_MAIN);
    return value;
  };

  lv_obj_t* server = detail_box();
  s_source_detail[0] = server;
  s_server_host = detail(server, "Server");
  s_server_link = detail(server, "Enrolment link");
  lv_obj_t* server_buttons = button_row(server);
  lv_obj_t* enter_link = button(server_buttons, "Enter link", COLOUR_ACCENT, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(
      enter_link, [](lv_event_t*) { settings_sheet_show_entry(SheetEntry::EnrolmentLink); },
      LV_EVENT_CLICKED, nullptr);
  s_server_forget = button(server_buttons, "Forget link", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(s_server_forget, on_forget_server, LV_EVENT_CLICKED, nullptr);
  hint(server, "The link from SigenStor Display's Admin page. It can also be pasted on the "
               "web settings page (Device page).");

  lv_obj_t* modbus = detail_box();
  s_source_detail[1] = modbus;
  lv_obj_t* modbus_line = row(modbus);
  text(modbus_line, FONT_TEXT, PUCK_COLOUR_MUTED, "Plant address");
  s_modbus_field = button(modbus_line, "", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_set_width(s_modbus_field, 240);
  s_modbus_value = lv_obj_get_child(s_modbus_field, 0);
  lv_obj_set_style_text_font(s_modbus_value, FONT_ROW, LV_PART_MAIN);
  lv_obj_add_event_cb(
      s_modbus_field, [](lv_event_t*) { open_modbus_keypad(); }, LV_EVENT_CLICKED,
      nullptr);
  lv_obj_t* devices_line = row(modbus);
  text(devices_line, FONT_TEXT, PUCK_COLOUR_MUTED, "Devices");
  lv_obj_t* devices_end = plain(devices_line);
  lv_obj_set_size(devices_end, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(devices_end, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(devices_end, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(devices_end, 14, LV_PART_MAIN);
  s_modbus_devices = text(devices_end, FONT_TEXT, PUCK_COLOUR_TEXT, "");
  lv_obj_t* edit_devices = button(devices_end, "Edit", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_set_style_pad_ver(edit_devices, 8, LV_PART_MAIN);
  lv_obj_add_event_cb(edit_devices, [](lv_event_t*) { plant_open_devices(); }, LV_EVENT_CLICKED,
                      nullptr);
  hint(modbus, "The gateway or inverter's IP. Allow this panel's IP for Modbus in the "
               "Sigen app.");

  lv_obj_t* ha = detail_box();
  s_source_detail[2] = ha;
  s_ha_host = detail(ha, "Home Assistant");
  s_ha_token = detail(ha, "Access token");
  lv_obj_t* ha_buttons = button_row(ha);
  lv_obj_t* set_address = button(ha_buttons, "Set address", COLOUR_ACCENT, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(
      set_address, [](lv_event_t*) { settings_sheet_show_entry(SheetEntry::HaAddress); },
      LV_EVENT_CLICKED, nullptr);
  lv_obj_t* set_token = button(ha_buttons, "Set token", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(
      set_token, [](lv_event_t*) { settings_sheet_show_entry(SheetEntry::HaToken); },
      LV_EVENT_CLICKED, nullptr);
  hint(ha, "Which entities to read is set on the web settings page (Device page).");

  lv_obj_t* right = card(body, 360, CARD_H);
  caption(right, "READING");
  s_poll_slider = labelled_slider(right, "Every", 0,
                                  static_cast<int32_t>(sizeof(POLL_STEPS) / sizeof(POLL_STEPS[0])) - 1,
                                  &s_poll_value);
  lv_obj_add_event_cb(s_poll_slider, on_poll, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(s_poll_slider, on_poll, LV_EVENT_RELEASED, nullptr);
  hint(right, "How often the live figures are read. The day's charts update every five "
              "minutes whatever this says.");
  spacer(right);
  hint(right, "A change of source takes effect after a restart.");
}

void build_device(lv_obj_t* tile) {
  lv_obj_t* body = page_body(tile);

  lv_obj_t* left = card(body, 520, CARD_H);
  lv_obj_set_style_pad_row(left, 7, LV_PART_MAIN);
  caption(left, "PERFORMANCE");
  for (int i = 0; i < PERF_COUNT; ++i) {
    lv_obj_t* line = row(left);
    text(line, FONT_TEXT, PUCK_COLOUR_MUTED, PERF_NAMES[i]);
    s_perf[i] = text(line, FONT_TEXT, PUCK_COLOUR_TEXT, "--");
  }
  spacer(left);
  s_restart_button = button(left, "Restart", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(s_restart_button, on_restart, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* column = plain(body);
  lv_obj_set_size(column, 400, CARD_H);
  lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(column, CARD_GAP / 2, LV_PART_MAIN);

  lv_obj_t* firmware = card(column, 400, LV_SIZE_CONTENT);
  caption(firmware, "FIRMWARE");
  s_firmware = text(firmware, FONT_ROW, PUCK_COLOUR_TEXT, "");
  s_update_text = hint(firmware, "");
  lv_obj_t* auto_line = row(firmware);
  text(auto_line, FONT_TEXT, PUCK_COLOUR_TEXT, "Check automatically");
  s_auto_check = toggle(auto_line);
  lv_obj_add_event_cb(s_auto_check, on_auto_check, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_t* buttons = plain(firmware);
  lv_obj_set_size(buttons, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_gap(buttons, 10, LV_PART_MAIN);
  s_check_button = button(buttons, "Check now", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(s_check_button, on_check, LV_EVENT_CLICKED, nullptr);
  s_install_button = button(buttons, "Install", COLOUR_ACCENT, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(s_install_button, on_install, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* web = card(column, 400, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(web, 1);
  lv_obj_set_flex_flow(web, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(web, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(web, 16, LV_PART_MAIN);
  s_qr = puck_qr_block_create(web, 100);
  lv_obj_t* words = plain(web);
  lv_obj_set_size(words, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(words, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(words, 8, LV_PART_MAIN);
  caption(words, "WEB SETTINGS");
  s_web_address = text(words, FONT_TEXT, COLOUR_ACCENT, "");
  lv_obj_set_style_text_line_space(s_web_address, 6, LV_PART_MAIN);
}

void build_header(lv_obj_t* parent) {
  lv_obj_t* handle = plain(parent);
  lv_obj_set_size(handle, 64, 6);
  lv_obj_set_style_radius(handle, 3, LV_PART_MAIN);
  lv_obj_set_style_bg_color(handle, lv_color_hex(COLOUR_CONTROL), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, 10);

  lv_obj_t* tabs = plain(parent);
  lv_obj_set_size(tabs, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(tabs, 6, LV_PART_MAIN);
  lv_obj_align(tabs, LV_ALIGN_TOP_MID, 0, 26);
  for (int i = 0; i < PAGE_COUNT; ++i) {
    lv_obj_t* tab = button(tabs, PAGE_NAMES[i], COLOUR_CARD, PUCK_COLOUR_MUTED);
    lv_obj_set_style_pad_ver(tab, 10, LV_PART_MAIN);
    lv_obj_set_style_text_font(lv_obj_get_child(tab, 0), FONT_ROW, LV_PART_MAIN);
    lv_obj_add_event_cb(tab, on_tab, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    s_tabs[i] = tab;
  }
  // A dot on Device when an update is waiting, so it is seen from any page.
  s_device_dot = plain(s_tabs[PAGE_COUNT - 1]);
  lv_obj_set_size(s_device_dot, 10, 10);
  lv_obj_set_style_radius(s_device_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_device_dot, lv_color_hex(PUCK_COLOUR_BATTERY), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_device_dot, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(s_device_dot, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_align(s_device_dot, LV_ALIGN_TOP_RIGHT, 12, -6);
  lv_obj_add_flag(s_device_dot, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* done = button(parent, "Done", COLOUR_CARD, COLOUR_ACCENT);
  lv_obj_set_style_text_font(lv_obj_get_child(done, 0), FONT_ROW, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(done, 10, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(done, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_align(done, LV_ALIGN_TOP_RIGHT, -PAGE_PAD_X + 16, 26);
  lv_obj_add_event_cb(done, [](lv_event_t*) { settings_sheet_close(true); }, LV_EVENT_CLICKED,
                      nullptr);
}

void build_restart_bar(lv_obj_t* parent) {
  s_restart_bar = plain(parent);
  lv_obj_set_size(s_restart_bar, SHEET_W - 2 * PAGE_PAD_X, RESTART_BAR_H);
  lv_obj_align(s_restart_bar, LV_ALIGN_BOTTOM_MID, 0, -14);
  lv_obj_set_style_bg_color(s_restart_bar, lv_color_hex(0x2A1F0A), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_restart_bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(s_restart_bar, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(s_restart_bar, 20, LV_PART_MAIN);
  lv_obj_set_flex_flow(s_restart_bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s_restart_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  s_restart_text = text(s_restart_bar, FONT_TEXT, PUCK_COLOUR_WARN, "");
  lv_obj_t* restart = button(s_restart_bar, "Restart", PUCK_COLOUR_WARN, PUCK_COLOUR_BG);
  lv_obj_set_style_pad_ver(restart, 6, LV_PART_MAIN);
  lv_obj_add_event_cb(restart, on_restart, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(s_restart_bar, LV_OBJ_FLAG_HIDDEN);
}

// ------------------------------------------------------- picker and joining ---

// Four layouts, like a phone's. Shift is one-shot: it drops back to lower case
// after the next letter.
const char* KEYS_LOWER[] = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
                            "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
                            "Shift", "z", "x", "c", "v", "b", "n", "m", "Del", "\n",
                            "123", "space", ".", ""};
const char* KEYS_UPPER[] = {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
                            "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
                            "Shift", "Z", "X", "C", "V", "B", "N", "M", "Del", "\n",
                            "123", "space", ".", ""};
// For links and tokens: the bottom row trades width of space bar for the
// characters every URL and token is full of.
const char* KEYS_LOWER_URL[] = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
                                "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
                                "Shift", "z", "x", "c", "v", "b", "n", "m", "Del", "\n",
                                "123", "/", ":", "space", ".", "-", "_", ""};
const char* KEYS_UPPER_URL[] = {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
                                "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
                                "Shift", "Z", "X", "C", "V", "B", "N", "M", "Del", "\n",
                                "123", "/", ":", "space", ".", "-", "_", ""};
const char* KEYS_DIGITS[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
                             "-", "/", ":", ";", "(", ")", "$", "&", "@", "\"", "\n",
                             "#+=", ".", ",", "?", "!", "'", "_", "Del", "\n",
                             "abc", "space", ".", ""};
const char* KEYS_SYMBOLS[] = {"[", "]", "{", "}", "#", "%", "^", "*", "+", "=", "\n",
                              "\\", "|", "~", "<", ">", "`", "$", "&", "@", "\"", "\n",
                              "123", ".", ",", "?", "!", "'", "_", "Del", "\n",
                              "abc", "space", ".", ""};

bool is_special_key(const char* key) {
  return strcmp(key, "Shift") == 0 || strcmp(key, "Del") == 0 || strcmp(key, "123") == 0 ||
         strcmp(key, "#+=") == 0 || strcmp(key, "abc") == 0;
}

void apply_keyboard_mode() {
  const bool url = s_entry_kind != EntryKind::WifiPassword;
  const char** map = s_keyboard_mode == KeyboardMode::Upper    ? (url ? KEYS_UPPER_URL : KEYS_UPPER)
                     : s_keyboard_mode == KeyboardMode::Digits ? KEYS_DIGITS
                     : s_keyboard_mode == KeyboardMode::Symbols ? KEYS_SYMBOLS
                     : url ? KEYS_LOWER_URL
                           : KEYS_LOWER;
  lv_btnmatrix_set_map(s_keyboard, map);
  uint16_t id = 0;
  for (const char** key = map; (*key)[0] != '\0'; ++key) {
    if (strcmp(*key, "\n") == 0) {
      continue;
    }
    if (strcmp(*key, "space") == 0) {
      lv_btnmatrix_set_btn_width(s_keyboard, id, url ? 4 : 6);
    } else if (is_special_key(*key)) {
      lv_btnmatrix_set_btn_width(s_keyboard, id, 2);
      lv_btnmatrix_set_btn_ctrl(s_keyboard, id, LV_BTNMATRIX_CTRL_CHECKED);
    }
    // Shift is only lit when it is on.
    if (strcmp(*key, "Shift") == 0 && s_keyboard_mode != KeyboardMode::Upper) {
      lv_btnmatrix_clear_btn_ctrl(s_keyboard, id, LV_BTNMATRIX_CTRL_CHECKED);
    }
    ++id;
  }
}

void refresh_password() {
  const size_t length = strlen(s_password);
  if (length == 0) {
    lv_label_set_text(s_password_field,
                      s_entry_kind == EntryKind::WifiPassword ? "Password" : "Type it here");
    lv_obj_set_style_text_color(s_password_field, lv_color_hex(PUCK_COLOUR_MUTED), LV_PART_MAIN);
  } else if (s_password_visible) {
    lv_label_set_text(s_password_field, s_password);
    lv_obj_set_style_text_color(s_password_field, lv_color_hex(PUCK_COLOUR_TEXT), LV_PART_MAIN);
  } else {
    char masked[sizeof(s_password)];
    memset(masked, '*', length);
    masked[length] = '\0';
    lv_label_set_text(s_password_field, masked);
    lv_obj_set_style_text_color(s_password_field, lv_color_hex(PUCK_COLOUR_TEXT), LV_PART_MAIN);
  }
  button_set_text(s_password_show, s_password_visible ? "Hide" : "Show");
}

// The same keyboard screen types a link, an address or a token. Those are long,
// so the field wraps in a smaller font instead of trailing off.
void refresh_entry() {
  static const char* const TITLES[] = {"", "Enrolment link", "Home Assistant address",
                                       "Home Assistant token"};
  static const char* const HINTS[] = {
      "",
      "The link from SigenStor Display's Admin page, starting https://",
      "For example http://homeassistant.local:8123",
      "A long-lived access token, from your profile page in Home Assistant",
  };
  const int kind = static_cast<int>(s_entry_kind);
  lv_label_set_text(s_join_title, TITLES[kind]);
  button_set_text(s_join_button, "Save");
  const bool failed = s_entry_error[0] != '\0';
  lv_label_set_text(s_join_status, failed ? s_entry_error : HINTS[kind]);
  lv_obj_set_style_text_color(s_join_status,
                              lv_color_hex(failed ? PUCK_COLOUR_ALARM : PUCK_COLOUR_MUTED),
                              LV_PART_MAIN);
  lv_obj_set_style_text_font(s_password_field, FONT_TEXT, LV_PART_MAIN);
  lv_label_set_long_mode(s_password_field, LV_LABEL_LONG_WRAP);
  set_hidden(s_password_line, false);
  set_hidden(s_keyboard, false);
  set_disabled(s_join_button, s_password[0] == '\0');
  set_disabled(s_keyboard, false);
  refresh_password();
}

void refresh_join() {
  if (s_entry_kind != EntryKind::WifiPassword) {
    refresh_entry();
    return;
  }
  lv_obj_set_style_text_font(s_password_field, FONT_BIG, LV_PART_MAIN);
  lv_label_set_long_mode(s_password_field, LV_LABEL_LONG_DOT);
  button_set_text(s_join_button, "Join");
  char title[64];
  snprintf(title, sizeof(title), "Join %s", s_join_ssid);
  lv_label_set_text(s_join_title, title);
  const bool joining = s_join_requested && s_join.state == SheetJoin::Joining;
  const bool failed = s_join_requested && s_join.state == SheetJoin::Failed;
  if (joining) {
    lv_label_set_text(s_join_status, "Joining...");
  } else if (failed) {
    lv_label_set_text(s_join_status, s_join.message[0] != '\0' ? s_join.message : "Could not join");
  } else {
    lv_label_set_text(s_join_status, s_join_secure ? "" : "No password needed");
  }
  lv_obj_set_style_text_color(s_join_status,
                              lv_color_hex(failed ? PUCK_COLOUR_ALARM : PUCK_COLOUR_MUTED),
                              LV_PART_MAIN);
  set_hidden(s_password_line, !s_join_secure);
  set_hidden(s_keyboard, !s_join_secure);
  set_disabled(s_join_button, joining || (s_join_secure && strlen(s_password) < 8));
  set_disabled(s_keyboard, joining);
  refresh_password();
}

void refresh_picker() {
  char status[32];
  if (s_scan.running) {
    snprintf(status, sizeof(status), "Scanning...");
  } else if (s_scan.count == 0) {
    snprintf(status, sizeof(status), "Nothing found");
  } else {
    snprintf(status, sizeof(status), "%u found", static_cast<unsigned>(s_scan.count));
  }
  lv_label_set_text(s_picker_status, status);
  for (int i = 0; i < SHEET_MAX_SCAN; ++i) {
    if (i >= s_scan.count) {
      lv_obj_add_flag(s_picker_rows[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    const SheetScanResult& result = s_scan.results[i];
    lv_obj_clear_flag(s_picker_rows[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_picker_ssid[i], result.ssid);
    set_bars(s_picker_bars[i], result.rssi, true);
    lv_label_set_text(s_picker_kind[i], result.saved    ? "saved"
                                        : result.secure ? "password"
                                                        : "open");
    lv_obj_set_style_text_color(
        s_picker_kind[i], lv_color_hex(result.saved ? PUCK_COLOUR_BATTERY : PUCK_COLOUR_MUTED),
        LV_PART_MAIN);
  }
}

void save_entry() {
  const SheetActions& actions = s_actions;
  bool (*save)(const char*, char*, size_t) =
      s_entry_kind == EntryKind::EnrolmentLink ? actions.set_enrolment_link
      : s_entry_kind == EntryKind::HaAddress   ? actions.set_ha_address
                                               : actions.set_ha_token;
  s_entry_error[0] = '\0';
  if (save != nullptr && !save(s_password, s_entry_error, sizeof(s_entry_error))) {
    if (s_entry_error[0] == '\0') {
      snprintf(s_entry_error, sizeof(s_entry_error), "That was not saved");
    }
    refresh_join();
    return;
  }
  // A token is a credential: do not leave it sitting in a buffer.
  memset(s_password, 0, sizeof(s_password));
  lv_obj_add_flag(s_join_panel, LV_OBJ_FLAG_HIDDEN);
}

void start_join() {
  if (s_entry_kind != EntryKind::WifiPassword) {
    save_entry();
    return;
  }
  s_join_requested = true;
  s_join.state = SheetJoin::Joining;
  s_join.message[0] = '\0';
  refresh_join();
  if (s_actions.join_network != nullptr) {
    s_actions.join_network(s_join_ssid, s_join_secure ? s_password : "");
  }
}

void on_pick(lv_event_t* event) {
  const intptr_t i = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
  if (i >= s_scan.count) {
    return;
  }
  const SheetScanResult& result = s_scan.results[i];
  // A saved network already has its password; an open one needs none.
  const bool ask = result.secure && !result.saved;
  settings_sheet_show_password(result.ssid, ask);
  if (!ask) {
    start_join();
  }
}

void on_keyboard(lv_event_t* /*event*/) {
  const uint16_t id = lv_btnmatrix_get_selected_btn(s_keyboard);
  if (id == LV_BTNMATRIX_BTN_NONE) {
    return;
  }
  const char* key = lv_btnmatrix_get_btn_text(s_keyboard, id);
  const size_t length = strlen(s_password);
  if (strcmp(key, "Shift") == 0) {
    s_keyboard_mode =
        s_keyboard_mode == KeyboardMode::Upper ? KeyboardMode::Lower : KeyboardMode::Upper;
    apply_keyboard_mode();
    return;
  }
  if (strcmp(key, "123") == 0 || strcmp(key, "#+=") == 0 || strcmp(key, "abc") == 0) {
    s_keyboard_mode = key[0] == '1'   ? KeyboardMode::Digits
                      : key[0] == '#' ? KeyboardMode::Symbols
                                      : KeyboardMode::Lower;
    apply_keyboard_mode();
    return;
  }
  if (strcmp(key, "Del") == 0) {
    if (length > 0) {
      s_password[length - 1] = '\0';
    }
  } else if (length + 1 < sizeof(s_password)) {
    s_password[length] = strcmp(key, "space") == 0 ? ' ' : key[0];
    s_password[length + 1] = '\0';
    if (s_keyboard_mode == KeyboardMode::Upper) {
      s_keyboard_mode = KeyboardMode::Lower;
      apply_keyboard_mode();
    }
  }
  // A failed attempt's message goes once the password is being changed.
  s_entry_error[0] = '\0';
  if (s_join.state == SheetJoin::Failed) {
    s_join.state = SheetJoin::Idle;
  }
  refresh_join();
}

void build_picker(lv_obj_t* parent) {
  s_picker = modal_backdrop(parent);
  lv_obj_t* box = card(s_picker, 640, 540);
  lv_obj_center(box);
  lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(box, lv_color_hex(COLOUR_CONTROL), LV_PART_MAIN);

  lv_obj_t* heading = row(box);
  caption(heading, "CHOOSE A NETWORK");
  s_picker_status = text(heading, FONT_HINT, PUCK_COLOUR_MUTED, "");

  lv_obj_t* list = plain(box);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_width(list, LV_PCT(100));
  lv_obj_set_flex_grow(list, 1);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 4, LV_PART_MAIN);
  for (int i = 0; i < SHEET_MAX_SCAN; ++i) {
    lv_obj_t* line = lv_btn_create(list);
    lv_obj_set_size(line, LV_PCT(100), 56);
    lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(line, lv_color_hex(COLOUR_CONTROL), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(line, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(line, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(line, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(line, 14, LV_PART_MAIN);
    lv_obj_add_event_cb(line, on_pick, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    s_picker_rows[i] = line;
    s_picker_bars[i] = signal_bars(line);
    s_picker_ssid[i] = text(line, FONT_ROW, PUCK_COLOUR_TEXT, "");
    lv_label_set_long_mode(s_picker_ssid[i], LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(s_picker_ssid[i], 1);
    s_picker_kind[i] = text(line, FONT_HINT, PUCK_COLOUR_MUTED, "");
  }

  lv_obj_t* buttons = row(box);
  lv_obj_t* again = button(buttons, "Scan again", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(
      again,
      [](lv_event_t*) {
        s_scan.running = true;
        refresh_picker();
        if (s_actions.start_scan != nullptr) {
          s_actions.start_scan();
        }
      },
      LV_EVENT_CLICKED, nullptr);
  lv_obj_t* cancel = button(buttons, "Cancel", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(
      cancel, [](lv_event_t*) { lv_obj_add_flag(s_picker, LV_OBJ_FLAG_HIDDEN); },
      LV_EVENT_CLICKED, nullptr);
}

void build_join_panel(lv_obj_t* parent) {
  s_join_panel = modal_backdrop(parent);
  lv_obj_t* box = card(s_join_panel, SHEET_W - 48, LV_SIZE_CONTENT);
  lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 12);
  lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(box, lv_color_hex(COLOUR_CONTROL), LV_PART_MAIN);

  lv_obj_t* heading = row(box);
  s_join_title = text(heading, FONT_BIG, PUCK_COLOUR_TEXT, "");
  lv_label_set_long_mode(s_join_title, LV_LABEL_LONG_DOT);
  lv_obj_set_style_max_width(s_join_title, 600, LV_PART_MAIN);
  lv_obj_t* actions = plain(heading);
  lv_obj_set_size(actions, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(actions, 10, LV_PART_MAIN);
  lv_obj_t* cancel = button(actions, "Cancel", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(
      cancel,
      [](lv_event_t*) {
        s_join_requested = false;
        lv_obj_add_flag(s_join_panel, LV_OBJ_FLAG_HIDDEN);
      },
      LV_EVENT_CLICKED, nullptr);
  s_join_button = button(actions, "Join", COLOUR_ACCENT, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(s_join_button, [](lv_event_t*) { start_join(); }, LV_EVENT_CLICKED,
                      nullptr);

  s_password_line = row(box);
  lv_obj_set_style_pad_column(s_password_line, 16, LV_PART_MAIN);
  s_password_field = text(s_password_line, FONT_BIG, PUCK_COLOUR_TEXT, "");
  lv_obj_set_flex_grow(s_password_field, 1);
  lv_label_set_long_mode(s_password_field, LV_LABEL_LONG_DOT);
  lv_obj_set_style_border_side(s_password_field, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_password_field, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_password_field, lv_color_hex(COLOUR_ACCENT), LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(s_password_field, 6, LV_PART_MAIN);
  s_password_show = button(s_password_line, "Show", COLOUR_CONTROL, PUCK_COLOUR_TEXT);
  lv_obj_add_event_cb(
      s_password_show,
      [](lv_event_t*) {
        s_password_visible = !s_password_visible;
        refresh_password();
      },
      LV_EVENT_CLICKED, nullptr);

  s_join_status = text(box, FONT_TEXT, PUCK_COLOUR_MUTED, "");

  s_keyboard = lv_btnmatrix_create(box);
  lv_obj_set_size(s_keyboard, LV_PCT(100), 300);
  lv_obj_set_style_bg_opa(s_keyboard, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_keyboard, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_keyboard, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_gap(s_keyboard, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(COLOUR_CONTROL), LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(s_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_radius(s_keyboard, 10, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(s_keyboard, 0, LV_PART_ITEMS);
  lv_obj_set_style_border_width(s_keyboard, 0, LV_PART_ITEMS);
  lv_obj_set_style_text_font(s_keyboard, FONT_ROW, LV_PART_ITEMS);
  lv_obj_set_style_text_color(s_keyboard, lv_color_hex(PUCK_COLOUR_TEXT), LV_PART_ITEMS);
  // Special keys a shade darker, and Shift lit when on: CHECKED marks both.
  const lv_style_selector_t checked =
      static_cast<lv_style_selector_t>(LV_PART_ITEMS) | LV_STATE_CHECKED;
  lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(PUCK_COLOUR_TRACK), checked);
  lv_obj_set_style_opa(s_keyboard, LV_OPA_40, LV_STATE_DISABLED);
  keep_gestures(s_keyboard);
  lv_obj_add_event_cb(s_keyboard, on_keyboard, LV_EVENT_VALUE_CHANGED, nullptr);
  apply_keyboard_mode();
}

void refresh_updating() {
  const bool installing = s_device.update == SheetUpdate::Installing;
  if (!installing) {
    lv_obj_add_flag(s_updating, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  char line[48];
  snprintf(line, sizeof(line), "Updating to %s", s_device.update_version);
  lv_label_set_text(s_updating_title, line);
  if (s_device.update_percent >= 0) {
    lv_bar_set_value(s_updating_bar, s_device.update_percent, LV_ANIM_OFF);
    snprintf(line, sizeof(line), "%d%%", s_device.update_percent);
  } else {
    lv_bar_set_value(s_updating_bar, 0, LV_ANIM_OFF);
    snprintf(line, sizeof(line), "Starting the download");
  }
  lv_label_set_text(s_updating_percent, line);
  if (lv_obj_has_flag(s_updating, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_move_foreground(s_updating);
    lv_obj_clear_flag(s_updating, LV_OBJ_FLAG_HIDDEN);
  }
}

void build_updating(lv_obj_t* parent) {
  s_updating = plain(parent);
  lv_obj_set_size(s_updating, SHEET_W, SHEET_H);
  lv_obj_add_flag(s_updating, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(s_updating, lv_color_hex(PUCK_COLOUR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_updating, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_flex_flow(s_updating, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_updating, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(s_updating, 24, LV_PART_MAIN);
  s_updating_title = text(s_updating, FONT_BIG, PUCK_COLOUR_TEXT, "");
  s_updating_bar = lv_bar_create(s_updating);
  lv_obj_set_size(s_updating_bar, 560, 12);
  lv_bar_set_range(s_updating_bar, 0, 100);
  lv_obj_set_style_bg_color(s_updating_bar, lv_color_hex(COLOUR_CONTROL), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_updating_bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_updating_bar, lv_color_hex(COLOUR_ACCENT), LV_PART_INDICATOR);
  s_updating_percent = text(s_updating, FONT_ROW, PUCK_COLOUR_TEXT, "");
  text(s_updating, FONT_HINT, PUCK_COLOUR_MUTED,
       "Keep the panel switched on. It restarts by itself when this is done.");
  lv_obj_add_flag(s_updating, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

void settings_sheet_create(lv_obj_t* parent, const SheetActions& actions) {
  s_actions = actions;

  s_root = plain(parent);
  lv_obj_set_size(s_root, SHEET_W, SHEET_H);
  lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);  // nothing reaches the screens below
  lv_obj_set_style_bg_color(s_root, lv_color_hex(COLOUR_SHEET), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);

  build_header(s_root);

  s_pages = lv_tileview_create(s_root);
  lv_obj_set_size(s_pages, SHEET_W, SHEET_H - HEADER_H);
  lv_obj_set_pos(s_pages, 0, HEADER_H);
  lv_obj_set_style_bg_opa(s_pages, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(s_pages, LV_SCROLLBAR_MODE_OFF);
  lv_obj_t* tiles[PAGE_COUNT];
  for (int i = 0; i < PAGE_COUNT; ++i) {
    tiles[i] = lv_tileview_add_tile(s_pages, static_cast<uint8_t>(i), 0, LV_DIR_HOR);
    lv_obj_clear_flag(tiles[i], LV_OBJ_FLAG_SCROLLABLE);
  }
  build_display(tiles[0]);
  build_screens(tiles[1]);
  build_wifi(tiles[2]);
  build_data(tiles[3]);
  plant_build_page(tiles[4]);
  build_device(tiles[5]);
  lv_obj_add_event_cb(s_pages, on_page_changed, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_set_tile_id(s_pages, 0, 0, LV_ANIM_OFF);

  build_restart_bar(s_root);
  plant_build_modals(s_root);
  keypad_build(s_root);
  build_picker(s_root);
  build_join_panel(s_root);
  build_updating(parent);

  lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(parent, on_gesture, LV_EVENT_GESTURE, nullptr);

  lv_disp_t* display = lv_disp_get_default();
  if (display != nullptr && display->driver->monitor_cb == nullptr) {
    display->driver->monitor_cb = monitor_cb;
  }
  lv_timer_create(tick, 1000, nullptr);

  refresh_tabs();
  refresh_display();
  refresh_screens();
  refresh_wifi();
  refresh_data();
  refresh_device();
  refresh_picker();
  refresh_join();
}

void settings_sheet_set_display(const SheetDisplay& display) {
  s_display = display;
  if (!s_have_boot_display) {
    s_have_boot_display = true;
    s_boot_flipped = display.flipped;
  }
  refresh_display();
}

void settings_sheet_set_screens(const SheetScreens& screens) {
  s_screens = screens;
  if (!s_have_boot_screens) {
    s_have_boot_screens = true;
    s_boot_visible = screens.visible;
  }
  refresh_screens();
}

void settings_sheet_set_wifi(const SheetWifi& wifi) {
  // Leave an armed Forget alone: the list it points into is about to be rebuilt
  // under it only if the networks themselves changed.
  const bool same_list =
      wifi.saved_count == s_wifi.saved_count &&
      memcmp(wifi.saved, s_wifi.saved, sizeof(SheetNetwork) * wifi.saved_count) == 0;
  if (!same_list) {
    disarm();
  }
  s_wifi = wifi;
  refresh_wifi();
}

void settings_sheet_set_data(const SheetData& data) {
  s_data = data;
  plant_set_data(data);
  if (!s_have_boot_data) {
    s_have_boot_data = true;
    s_boot_source = data.source;
  }
  refresh_data();
}

void settings_sheet_set_device(const SheetDevice& device) {
  s_device = device;
  if (s_open && s_page == SheetPage::Device) {
    refresh_device();
  }
  // The dot on the Device tab and the install screen matter with the sheet shut.
  set_hidden(s_device_dot, s_device.update != SheetUpdate::Available);
  refresh_updating();
}

void settings_sheet_open(SheetPage page, bool animate) {
  s_page = page;
  lv_obj_set_tile_id(s_pages, static_cast<uint32_t>(page), 0, LV_ANIM_OFF);
  refresh_tabs();
  refresh_device();
  lv_obj_move_foreground(s_root);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
  s_open = true;
  if (animate) {
    slide(SHEET_H, 0, nullptr);
  } else {
    lv_anim_del(s_root, set_translate);
    set_translate(s_root, 0);
  }
}

void settings_sheet_close(bool animate) {
  if (!s_open) {
    return;
  }
  s_open = false;
  disarm();
  keypad_close();
  plant_close_modals();
  lv_obj_add_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_join_panel, LV_OBJ_FLAG_HIDDEN);
  s_join_requested = false;
  // Whatever a slider was previewing, the panel goes back to its own level.
  if (s_actions.preview_brightness != nullptr) {
    s_actions.preview_brightness(s_display.brightness);
  }
  if (animate) {
    slide(0, SHEET_H, [](lv_anim_t*) { lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN); });
  } else {
    lv_anim_del(s_root, set_translate);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
  }
}

bool settings_sheet_is_open() {
  return s_open;
}

SheetPage settings_sheet_page() {
  return s_page;
}

void settings_sheet_show_keypad(bool show) {
  if (show) {
    open_modbus_keypad();
  } else {
    keypad_close();
  }
}

void settings_sheet_set_scan(const SheetScan& scan) {
  s_scan = scan;
  refresh_picker();
}

void settings_sheet_set_join(const SheetJoinStatus& join) {
  s_join = join;
  if (s_join_requested && join.state == SheetJoin::Joined) {
    // Done: back to the WiFi page, which now shows the new network.
    s_join_requested = false;
    s_password[0] = '\0';
    lv_obj_add_flag(s_join_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
  }
  refresh_join();
}

void settings_sheet_show_picker() {
  if (!s_open) {
    settings_sheet_open(SheetPage::Wifi, true);
  } else {
    lv_obj_set_tile_id(s_pages, static_cast<uint32_t>(SheetPage::Wifi), 0, LV_ANIM_OFF);
  }
  s_scan.running = true;
  refresh_picker();
  lv_obj_move_foreground(s_picker);
  lv_obj_clear_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
  if (s_actions.start_scan != nullptr) {
    s_actions.start_scan();
  }
}

void settings_sheet_show_password(const char* ssid, bool secure) {
  s_entry_kind = EntryKind::WifiPassword;
  snprintf(s_join_ssid, sizeof(s_join_ssid), "%s", ssid);
  s_join_secure = secure;
  s_password[0] = '\0';
  s_password_visible = false;
  s_join_requested = false;
  s_keyboard_mode = KeyboardMode::Lower;
  apply_keyboard_mode();
  refresh_join();
  lv_obj_move_foreground(s_join_panel);
  lv_obj_clear_flag(s_join_panel, LV_OBJ_FLAG_HIDDEN);
}

void settings_sheet_type_password(const char* text) {
  snprintf(s_password + strlen(s_password), sizeof(s_password) - strlen(s_password), "%s", text);
  refresh_join();
}

void settings_sheet_press_join() {
  start_join();
}

void settings_sheet_show_entry(SheetEntry entry) {
  s_entry_kind = entry == SheetEntry::EnrolmentLink ? EntryKind::EnrolmentLink
                 : entry == SheetEntry::HaAddress   ? EntryKind::HaAddress
                                                    : EntryKind::HaToken;
  s_entry_error[0] = '\0';
  memset(s_password, 0, sizeof(s_password));
  // An address starts with what every address starts with; the rest start empty.
  if (s_entry_kind == EntryKind::HaAddress) {
    snprintf(s_password, sizeof(s_password), "http://");
  }
  // Long and typed by eye, so shown while typed. Show/Hide still works.
  s_password_visible = true;
  s_join_requested = false;
  s_keyboard_mode = KeyboardMode::Lower;
  apply_keyboard_mode();
  refresh_join();
  lv_obj_move_foreground(s_join_panel);
  lv_obj_clear_flag(s_join_panel, LV_OBJ_FLAG_HIDDEN);
}

void settings_sheet_set_solar(const SheetSolar& solar) {
  plant_set_solar(solar);
}

void settings_sheet_show_devices() {
  plant_open_devices();
}

void settings_sheet_show_array(int index) {
  plant_open_array(index);
}

const SheetActions& sheet_kit::sheet_actions() {
  return s_actions;
}
