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
#include "updater_progress.h"

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
  }
  const int saved = net_saved_count();
  for (int i = 0; i < saved && i < SHEET_MAX_NETWORKS; ++i) {
    SheetNetwork& network = wifi.saved[i];
    const String ssid = net_saved_ssid(i);
    snprintf(network.ssid, sizeof(network.ssid), "%s", ssid.c_str());
    // The one in use is in range by definition; the rest as of the last scan.
    if (wifi.connected && ssid == WiFi.SSID()) {
      network.rssi = wifi.rssi;
      network.in_range = true;
    } else {
      network.in_range = net_scan_rssi(ssid, &network.rssi);
    }
    wifi.saved_count = static_cast<uint8_t>(i + 1);
  }
  settings_sheet_set_wifi(wifi);
}

bool is_saved(const String& ssid) {
  for (int i = 0; i < net_saved_count(); ++i) {
    if (net_saved_ssid(i) == ssid) {
      return true;
    }
  }
  return false;
}

// Pushed only when something changed, so a picker being scrolled is not rebuilt
// under the finger once a second.
void push_scan() {
  static bool last_running = false;
  static int last_count = -1;
  NetScanResult found[SHEET_MAX_SCAN];
  const int count = net_scan_results(found, SHEET_MAX_SCAN);
  const bool running = net_scan_running();
  // A finished scan always passes through running, so the change catches it.
  if (running == last_running && count == last_count) {
    return;
  }
  last_running = running;
  last_count = count;
  SheetScan scan;
  scan.running = running;
  for (int i = 0; i < count; ++i) {
    SheetScanResult& result = scan.results[i];
    snprintf(result.ssid, sizeof(result.ssid), "%s", found[i].ssid.c_str());
    result.rssi = found[i].rssi;
    result.secure = found[i].secure;
    result.saved = is_saved(found[i].ssid);
  }
  scan.count = static_cast<uint8_t>(count);
  settings_sheet_set_scan(scan);
}

void push_join() {
  static NetJoin last = NetJoin::Idle;
  const NetJoin state = net_join_state();
  if (state == last) {
    return;
  }
  last = state;
  SheetJoinStatus join;
  join.state = state == NetJoin::Joining  ? SheetJoin::Joining
               : state == NetJoin::Joined ? SheetJoin::Joined
               : state == NetJoin::Failed ? SheetJoin::Failed
                                          : SheetJoin::Idle;
  snprintf(join.message, sizeof(join.message), "%s", net_join_error().c_str());
  settings_sheet_set_join(join);
  if (state == NetJoin::Joined) {
    push_wifi();
  }
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
  device.update_percent = updater_progress_percent();
  device.auto_check = settings_get().check_updates;
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
    net_forget(String(ssid));
    push_wifi();
  };
  a.start_scan = []() { net_scan_start(); };
  a.join_network = [](const char* ssid, const char* password) {
    net_join(String(ssid), String(password));
  };
  a.set_auto_check = [](bool enabled) { settings_set_check_updates(enabled); };
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
  // Scan and join progress at loop rate: someone is watching for it.
  push_scan();
  push_join();

  // Nothing saved: put the network picker up, once per time it happens, so a new
  // panel shows the way to get online without being asked.
  static bool picker_offered = false;
  if (net_state() == NetState::NoNetworks) {
    if (!picker_offered && !settings_sheet_is_open()) {
      picker_offered = true;
      settings_sheet_show_picker();
    }
  } else {
    picker_offered = false;
  }

  // Install progress moves quickly enough to be worth more than once a second.
  static int8_t last_percent = -1;
  if (updater_progress_percent() != last_percent) {
    last_percent = updater_progress_percent();
    push_device();
  }

  static uint32_t last = 0;
  if (millis() - last < 1000) {
    return;
  }
  last = millis();
  push_wifi();
  push_device();
}
