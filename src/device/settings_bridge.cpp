#include "settings_bridge.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "board_config.h"
#include "data_source.h"
#include "display.h"
#include "modbus_api.h"
#include "net.h"
#include "poller.h"
#include "settings.h"
#include "ui/ui.h"
#include "ui_panel/settings_sheet.h"
#include "updater.h"

namespace {

// Hosts are shown, never whole URLs: a URL can carry a path or a query, and the
// sheet is on a wall where anyone can read it.
void copy_host(char* out, size_t size, const String& url) {
  int start = url.indexOf("://");
  start = start < 0 ? 0 : start + 3;
  int end = url.indexOf('/', start);
  if (end < 0) {
    end = url.length();
  }
  snprintf(out, size, "%s", url.substring(start, end).c_str());
}

void push_display() {
  const Settings& s = settings_get();
  SheetDisplay display;
  display.brightness = s.brightness;
  display.dim_brightness = s.dim_brightness;
  display.dim_after_s = s.dim_after_s;
  display.screen_off_set = s.screen_off_set;
  if (s.screen_off_set) {
    display.screen_off_start_min = s.screen_off_start_min;
    display.screen_off_end_min = s.screen_off_end_min;
  }
  display.flipped = s.orientation == 2;
  settings_sheet_set_display(display);
}

void push_screens() {
  const Settings& s = settings_get();
  SheetScreens screens;
  screens.visible = s.screens_visible;
  screens.rotate = s.screens_rotate;
  screens.available = data_source_capabilities(s.source).detailed_flows
                          ? 0xFF
                          : static_cast<uint8_t>(~PUCK_DETAILED_SCREENS);
  screens.rotate_enabled = s.rotate_enabled;
  screens.rotate_s = s.rotate_s;
  settings_sheet_set_screens(screens);
}

void push_data() {
  const Settings& s = settings_get();
  SheetData data;
  data.source = s.source;
  data.server_set = !s.base_url.isEmpty() && !s.token.isEmpty();
  copy_host(data.server_host, sizeof(data.server_host), s.base_url);
  snprintf(data.modbus_host, sizeof(data.modbus_host), "%s", s.modbus_host.c_str());
  data.ha_set = settings_home_assistant_is_configured();
  copy_host(data.ha_host, sizeof(data.ha_host), s.ha_base_url);
  data.poll_s = s.poll_interval_s;
  settings_sheet_set_data(data);
}

void push_wifi() {
  SheetWifi wifi;
  wifi.connected = WiFi.status() == WL_CONNECTED;
  if (wifi.connected) {
    snprintf(wifi.ssid, sizeof(wifi.ssid), "%s", WiFi.SSID().c_str());
    wifi.rssi = static_cast<int8_t>(WiFi.RSSI());
    wifi.channel = static_cast<uint8_t>(WiFi.channel());
    snprintf(wifi.ip, sizeof(wifi.ip), "%s", WiFi.localIP().toString().c_str());
    // WiFiManager remembers one network, so the saved list is the current one
    // until the multi-network store lands (docs/UX.md, ported from
    // claude-gadget's net.cpp).
    snprintf(wifi.saved[0].ssid, sizeof(wifi.saved[0].ssid), "%s", wifi.ssid);
    wifi.saved[0].rssi = wifi.rssi;
    wifi.saved[0].in_range = true;
    wifi.saved_count = 1;
  }
  settings_sheet_set_wifi(wifi);
}

void push_device() {
  SheetDevice device;
  snprintf(device.firmware, sizeof(device.firmware), "%s", PUCK_FW_VERSION);
  snprintf(device.host, sizeof(device.host), "%s", net_hostname().c_str());
  snprintf(device.ip, sizeof(device.ip), "%s", net_ip().c_str());
  device.uptime_s = millis() / 1000;
  device.internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  device.internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  device.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  device.psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  device.wifi_connected = WiFi.status() == WL_CONNECTED;
  device.rssi = device.wifi_connected ? static_cast<int8_t>(WiFi.RSSI()) : 0;

  const PollStatus poll = poller_status();
  device.ever_read = poll.ever_succeeded;
  device.last_read_age_s = poll.last_ok_ms > 0 ? (millis() - poll.last_ok_ms) / 1000 : 0;
  device.failures_in_a_row = poll.consecutive_failures;
  snprintf(device.last_result, sizeof(device.last_result), "%s",
           fetch_result_name(poll.last_result));

  const UpdateStatus update = updater_status();
  switch (update.state) {
    case UpdateState::Idle:
      device.update = SheetUpdate::Idle;
      break;
    case UpdateState::Checking:
      device.update = SheetUpdate::Checking;
      break;
    case UpdateState::UpToDate:
      device.update = SheetUpdate::UpToDate;
      break;
    case UpdateState::Available:
      device.update = SheetUpdate::Available;
      break;
    case UpdateState::Downloading:
      device.update = SheetUpdate::Installing;
      break;
    case UpdateState::Failed:
      device.update = SheetUpdate::Failed;
      break;
  }
  snprintf(device.update_version, sizeof(device.update_version), "%s",
           update.latest_version.c_str());
  snprintf(device.update_detail, sizeof(device.update_detail), "%s", update.message.c_str());
  settings_sheet_set_device(device);
}

SheetActions actions() {
  SheetActions a;
  a.preview_brightness = [](uint8_t level) { display_set_brightness(level); };
  a.set_display = [](const SheetDisplay& display) {
    settings_set_display(display.brightness, display.dim_after_s, display.dim_brightness);
    settings_set_screen_off(display.screen_off_set, display.screen_off_start_min,
                            display.screen_off_end_min);
    settings_set_orientation(display.flipped ? 2 : 0);
    display_set_brightness(display.brightness);
    push_display();
  };
  a.set_screens = [](const SheetScreens& screens) {
    settings_set_screens(screens.visible, screens.rotate);
    settings_set_screensaver(screens.rotate_s, settings_get().sweep_min);
    settings_set_rotate_enabled(screens.rotate_enabled);
    ui_set_rotate_interval(screens.rotate_s);
    ui_set_rotate_enabled(screens.rotate_enabled);
    push_screens();
  };
  a.forget_network = [](const char* ssid) {
    // With one stored network, forgetting it is forgetting WiFi: this restarts
    // into the setup portal.
    Serial.printf("[settings] forgetting WiFi network %s\n", ssid);
    net_forget_wifi();
  };
  a.add_network = []() {
    // Needs the saved-network list, which is not ported yet.
    Serial.println("[settings] add network: not available until the saved-network list lands");
    ui_toast("NOT AVAILABLE YET");
  };
  a.set_source = [](DataSource source) {
    settings_set_source(source);
    push_data();
  };
  a.forget_server = []() {
    settings_forget_server();
    poller_wake();
    push_data();
  };
  a.set_modbus_host = [](const char* host) {
    const Settings& s = settings_get();
    settings_set_modbus(String(host), s.modbus_port, s.modbus_plant_address, s.modbus_devices,
                        SETTINGS_MAX_MODBUS_DEVICES);
    // A new plant must not inherit the previous one's cached readings.
    modbus_api_reset();
    poller_wake();
    push_data();
  };
  a.set_poll_interval = [](uint32_t seconds) {
    settings_set_poll_interval(seconds);
    poller_wake();
    push_data();
  };
  // Both only ask: the poll task does the HTTPS (updater_service()).
  a.check_update = []() { updater_request_check(true); };
  a.install_update = []() { updater_request_apply(); };
  a.restart = []() {
    Serial.println("[settings] restart from the settings sheet");
    ui_toast("RESTARTING");
    delay(300);
    ESP.restart();
  };
  return a;
}

}  // namespace

void settings_bridge_begin() {
  settings_sheet_create(lv_scr_act(), actions());
  push_display();
  push_screens();
  push_data();
  push_wifi();
  push_device();
}

void settings_bridge_loop() {
  static uint32_t last = 0;
  if (millis() - last < 1000) {
    return;
  }
  last = millis();
  push_wifi();
  push_device();
}
