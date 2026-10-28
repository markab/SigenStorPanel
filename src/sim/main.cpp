// Desktop simulator entry point (docs/PLAN.md §B5).
//
// Loads the canned /api/summary fixtures from test/fixtures and lets you step
// through them with the keyboard, so screen work happens in a one-second
// edit-run loop instead of a flash cycle. With --live it polls a real server
// instead (live_source.h).
//
// What it renders right now is a plain dump of the parsed Snapshot. That is
// deliberately not a design — it exists to prove the fixture -> parse -> LVGL
// path end to end, and to make the awkward states (null `today`, null
// registers, unconfigured cost) visible before any screen depends on them.

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "board_config.h"
#include "day_series.h"
#include "enrol_url.h"
#include "forecast_store.h"
#include "history.h"
#include "kiosk_code.h"
#include "live_source.h"
#include "sim_backend.h"
#include "snapshot.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui_panel/overlay_page.h"
#include "ui_panel/settings_sheet.h"

namespace {

constexpr uint32_t COLOUR_BACKGROUND = PUCK_COLOUR_BG;
constexpr uint32_t COLOUR_TEXT = PUCK_COLOUR_TEXT;
constexpr uint32_t COLOUR_MUTED = PUCK_COLOUR_MUTED;
constexpr uint32_t COLOUR_ACCENT = PUCK_COLOUR_BATTERY;
constexpr uint32_t COLOUR_WARN = PUCK_COLOUR_WARN;

std::vector<std::string> s_fixture_paths;
size_t s_current = 0;
int16_t s_startup_tilt = 0;

// --modbus previews the reduced arrangement that data source gets: no cost
// screen and no flows screen (§D1), which is otherwise only visible on a device
// configured for it.
bool s_with_detailed_screens = true;

// --live: readings come from SIGEN_URL instead of the fixtures, and the fixture
// keys do nothing.
bool s_live = false;
std::string s_live_host;
// Set while --shot drives the sheet, whose demo link must not go live.
bool s_shot_pass = false;

// The real screen, plus the raw field dump kept behind the 'd' key. Keeping the
// dump around is worth its few lines: when a screen shows something surprising,
// it answers "is the parse wrong or the layout wrong?" immediately.
lv_obj_t* s_debug_root = nullptr;
bool s_show_debug = false;

lv_obj_t* s_heading = nullptr;
lv_obj_t* s_body = nullptr;

std::vector<std::string> discover_fixtures(const std::string& directory) {
  std::vector<std::string> found;
  DIR* dir = opendir(directory.c_str());
  if (dir == nullptr) {
    return found;
  }
  while (const dirent* entry = readdir(dir)) {
    const std::string name = entry->d_name;
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) {
      found.push_back(directory + "/" + name);
    }
  }
  closedir(dir);
  // Numeric filename prefixes give a stable, meaningful order.
  std::sort(found.begin(), found.end());
  return found;
}

bool read_file(const std::string& path, std::string* out) {
  FILE* file = fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  out->clear();
  char buffer[512];
  size_t read = 0;
  while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    out->append(buffer, read);
  }
  fclose(file);
  return true;
}

std::string base_name(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// "--" for an unknown value, never "0.00". The whole reason MaybeFloat exists.
std::string number(const MaybeFloat& value, const char* unit, int decimals = 2) {
  if (!value.known) {
    return "--";
  }
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%.*f%s", decimals, value.value, unit);
  return buffer;
}

std::string number(const MaybeInt& value, const char* unit) {
  if (!value.known) {
    return "--";
  }
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%d%s", value.value, unit);
  return buffer;
}

// Signed legs read much more clearly with the direction spelled out than with a
// minus sign the viewer has to interpret against the register conventions.
std::string signed_leg(const MaybeFloat& value, const char* positive, const char* negative) {
  std::string text = number(value, " kW");
  if (!value.known) {
    return text;
  }
  if (value.value > 0.0f) {
    text += "  ";
    text += positive;
  } else if (value.value < 0.0f) {
    text += "  ";
    text += negative;
  }
  return text;
}

std::string describe(const Snapshot& snapshot) {
  std::string text;
  char line[192];

  snprintf(line, sizeof(line), "v%d   %s   age %us   alarms %d\n\n", snapshot.version,
           snapshot.ok ? "ok" : "NOT ok", snapshot.age_s, snapshot.alarms);
  text += line;

  snprintf(line, sizeof(line), "PV     %s\n", number(snapshot.power.pv, " kW").c_str());
  text += line;
  if (snapshot.power.off_grid) {
    text += "GRID   OFF-GRID (leg suppressed)\n";
  } else {
    snprintf(line, sizeof(line), "GRID   %s\n",
             signed_leg(snapshot.power.grid, "import", "export").c_str());
    text += line;
  }
  snprintf(line, sizeof(line), "BATT   %s\n",
           signed_leg(snapshot.power.batt, "charge", "discharge").c_str());
  text += line;
  snprintf(line, sizeof(line), "HOME   %s\n", number(snapshot.power.home, " kW").c_str());
  text += line;
  snprintf(line, sizeof(line), "EV     %s\n\n", number(snapshot.power.ev, " kW").c_str());
  text += line;

  snprintf(line, sizeof(line), "SOC    %s      SOH %s\n",
           number(snapshot.battery.soc_pct, " %", 1).c_str(),
           number(snapshot.battery.soh_pct, " %", 1).c_str());
  text += line;
  snprintf(line, sizeof(line), "TEMP   %s     CAP %s\n",
           number(snapshot.battery.temp_c, " C", 1).c_str(),
           number(snapshot.battery.capacity_kwh, " kWh", 1).c_str());
  text += line;
  snprintf(line, sizeof(line), "ETA    %s\n\n", number(snapshot.battery.eta_min, " min").c_str());
  text += line;

  if (!snapshot.today.present) {
    text += "TODAY  null - day unreadable\n\n";
  } else {
    snprintf(line, sizeof(line), "TODAY  solar %s  imp %s  exp %s\n",
             number(snapshot.today.solar, "", 1).c_str(),
             number(snapshot.today.imported, "", 1).c_str(),
             number(snapshot.today.exported, "", 1).c_str());
    text += line;
    snprintf(line, sizeof(line), "       load %s  chg %s  dis %s\n\n",
             number(snapshot.today.load, "", 1).c_str(),
             number(snapshot.today.charge, "", 1).c_str(),
             number(snapshot.today.discharge, "", 1).c_str());
    text += line;
  }

  if (!snapshot.solar.configured) {
    text += "SOLAR  no forecast configured\n\n";
  } else {
    snprintf(line, sizeof(line), "SOLAR  fc %s  left %s  vs %s  peak %s\n\n",
             number(snapshot.solar.forecast_kwh, "", 1).c_str(),
             number(snapshot.solar.remaining_kwh, "", 1).c_str(),
             number(snapshot.solar.vs_forecast_pct, "%", 0).c_str(),
             number(snapshot.solar.peak_kw, "", 1).c_str());
    text += line;
  }

  if (!snapshot.cost.configured) {
    text += "COST   no tariff configured\n";
  } else {
    snprintf(line, sizeof(line), "COST   save %s GBP    now %s\n",
             number(snapshot.cost.saving_gbp, "").c_str(),
             number(snapshot.cost.rate_p, "p", 1).c_str());
    text += line;
    text += "NEXT   ";
    for (size_t i = 0; i < snapshot.cost.next_count; ++i) {
      snprintf(line, sizeof(line), "%.1fp  ", snapshot.cost.next[i].pence);
      text += line;
    }
    text += "\n";
  }

  return text;
}

// --------------------------------------------------- synthetic day history ---
//
// The fixtures are single snapshots, so there is nothing for the charts of §D3
// to draw. Rather than grow the fixture format, each one is expanded into a
// plausible day that *ends* at its own instantaneous values.
//
// The point of this is the awkward case, not the pretty one: a solar curve
// broken up by cloud is what the min/max envelope exists to render, and a smooth
// bell would let a bug in the reduction pass unnoticed.

float hash01(uint32_t value) {
  uint32_t x = value * 2654435761u;
  x ^= x >> 13;
  x *= 1274126177u;
  x ^= x >> 16;
  return static_cast<float>(x & 0xFFFFu) / 65535.0f;
}

// Normalised generation, 0 at night and peaking near the middle of the day. The
// exponent narrows the bell slightly, which is closer to a real array than a
// plain sine.
float solar_shape(int local_minute) {
  constexpr int SUNRISE = 330;   // 05:30
  constexpr int SUNSET = 1230;   // 20:30
  if (local_minute <= SUNRISE || local_minute >= SUNSET) {
    return 0.0f;
  }
  const float t = static_cast<float>(local_minute - SUNRISE) /
                  static_cast<float>(SUNSET - SUNRISE);
  return powf(sinf(t * 3.14159265f), 1.6f);
}

// Cloud, in bands rather than uniformly, so part of the day stays clean and the
// two cases can be compared in one screenshot.
float cloud_factor(uint32_t minute, int local_minute) {
  const float season = sinf(static_cast<float>(local_minute) * 0.011f);
  if (season < 0.25f) {
    return 1.0f;  // clear spell
  }
  const float roll = hash01(minute);
  if (roll > 0.45f) {
    return 1.0f;
  }
  // A dip to somewhere between a fifth and most of clear-sky output.
  return 0.2f + 0.7f * hash01(minute + 7777u);
}

// A day's worth of battery: drained overnight, charged through the middle of the
// day, drawn down again in the evening.
float soc_shape(int local_minute) {
  const float t = static_cast<float>(local_minute) / 1440.0f * 2.0f * 3.14159265f;
  return -cosf(t - 1.2f);  // -1 around 06:00, +1 around 18:00
}

void synthesise_history(const Snapshot& snapshot) {
  history_reset(HistoryBank::Live);
  if (!snapshot.valid || snapshot.ts == 0) {
    return;
  }

  // UTC, so the fixtures' own 13:00 timestamp is the time of day. Setting it at
  // all is what puts the charts on their anchored-to-midnight path rather than
  // the rolling fallback, which is the arrangement worth looking at.
  history_set_timezone(HistoryBank::Live, 0);

  const uint32_t end = snapshot.ts / 60;
  const int end_local = static_cast<int>(end % 1440);

  // Scale the bell so it passes exactly through the live reading. A fixture
  // reporting nothing at midday gets an overcast day instead of a bright one it
  // would contradict.
  const float shape_now = solar_shape(end_local);
  const bool generating = snapshot.power.pv.known && snapshot.power.pv.value > 0.05f;
  const bool overcast = snapshot.power.pv.known && !generating;
  float peak = 4.2f;
  if (generating && shape_now > 0.05f) {
    peak = snapshot.power.pv.value / shape_now;
  } else if (overcast) {
    peak = 0.9f;
  }

  // State of charge: build the shape, then shift the whole series so its last
  // sample is the reading actually on screen.
  constexpr float SOC_AMPLITUDE = 28.0f;
  const float soc_offset = snapshot.battery.soc_pct.known
                               ? snapshot.battery.soc_pct.value -
                                     SOC_AMPLITUDE * soc_shape(end_local)
                               : 0.0f;

  // From local midnight to now, not a full 24 h back: the anchored window covers
  // the whole day, so filling only the part that has happened is what makes the
  // curve grow into an empty right-hand side the way it will on a real device.
  for (int local = 0; local <= end_local; ++local) {
    const uint32_t minute = end - static_cast<uint32_t>(end_local - local);

    // A null-register fixture must leave the band genuinely empty — that is the
    // case the "nothing recorded yet" path exists for.
    if (snapshot.power.pv.known) {
      const float cloud = overcast ? 0.35f + 0.5f * hash01(minute)
                                   : cloud_factor(minute, local);
      history_put(HistoryBank::Live, HistorySeries::Pv, minute, peak * solar_shape(local) * cloud);
    }

    // House load: a base draw with a morning and an evening hump, plus the odd
    // kettle. Spiky on purpose — a flat line would not exercise the smoothing the
    // load chart needs, which is the whole reason its window is wider.
    if (snapshot.power.home.known) {
      const float morning = expf(-powf((local - 450) / 90.0f, 2.0f));
      const float evening = expf(-powf((local - 1140) / 120.0f, 2.0f));
      float load = 0.35f + 1.4f * morning + 2.2f * evening;
      if (hash01(minute + 31337u) > 0.94f) {
        load += 2.5f;  // kettle
      }
      history_put(HistoryBank::Live, HistorySeries::Load, minute, load);
    }

    if (snapshot.battery.soc_pct.known) {
      float soc = soc_offset + SOC_AMPLITUDE * soc_shape(local);
      if (soc < 2.0f) {
        soc = 2.0f;
      }
      if (soc > 100.0f) {
        soc = 100.0f;
      }
      history_put(HistoryBank::Live, HistorySeries::Soc, minute, soc);
    }

    // Grid: load minus generation, signed. Import (positive) morning and evening,
    // export (negative) through the solar middle of the day — a bipolar curve for
    // the grid screen's chart.
    if (snapshot.power.grid.known) {
      const float cloud =
          overcast ? 0.35f + 0.5f * hash01(minute) : cloud_factor(minute, local);
      const float pv_now = snapshot.power.pv.known ? peak * solar_shape(local) * cloud : 0.0f;
      const float morning = expf(-powf((local - 450) / 90.0f, 2.0f));
      const float evening = expf(-powf((local - 1140) / 120.0f, 2.0f));
      const float load_now = 0.35f + 1.4f * morning + 2.2f * evening;
      history_put(HistoryBank::Live, HistorySeries::Grid, minute, load_now - pv_now);
    }
  }

  // A clear-sky forecast for the whole day, for the solar screen's "forecast
  // ahead". Same bell as the actual but without the cloud, so it sits a little
  // above the cloud-broken generation and runs on past now to dusk.
  if (snapshot.power.pv.known && peak > 0.05f) {
    const uint32_t day_start = end - static_cast<uint32_t>(end_local);
    float slot_kw[48];
    for (int i = 0; i < 48; ++i) {
      slot_kw[i] = peak * solar_shape(i * 30 + 15);  // average kW in the slot
    }
    forecast_store_set(slot_kw, 48, day_start);
  } else {
    forecast_store_clear();
  }
}

void show_current_fixture() {
  const std::string& path = s_fixture_paths[s_current];
  const std::string name = base_name(path);

  char heading[128];
  snprintf(heading, sizeof(heading), "%s   [%zu/%zu]", name.c_str(), s_current + 1,
           s_fixture_paths.size());
  lv_label_set_text(s_heading, heading);

  std::string json;
  if (!read_file(path, &json)) {
    lv_label_set_text(s_body, "could not read the fixture");
    return;
  }

  Snapshot snapshot;
  if (!snapshot_parse(json.c_str(), json.size(), &snapshot)) {
    lv_label_set_text(s_body, "fixture did not parse");
    lv_obj_set_style_text_color(s_body, lv_color_hex(COLOUR_WARN), LV_PART_MAIN);
    ui_update(Snapshot{});
    return;
  }

  lv_obj_set_style_text_color(s_body, lv_color_hex(COLOUR_TEXT), LV_PART_MAIN);
  lv_label_set_text(s_body, describe(snapshot).c_str());
  // Rebuilt per fixture, so stepping between them does not leave the previous
  // fixture's day behind the new one's numbers.
  synthesise_history(snapshot);
  ui_update(snapshot);
  if (ui_day_offset() != 0) {
    ui_update_day(snapshot);
  }

  const std::string title = "SigenStorPanel sim - " + name;
  sim_backend_set_title(title.c_str());
  printf("[sim] %s (%zu bytes)\n", name.c_str(), json.size());
}


// ---------------------------------------------------------------- --live ---
//
// The device's poll-to-screen path in miniature: the day series goes into the
// history ring as it arrives, then the summary goes to ui_update(), which files
// it into the ring as well. The overlays are the device's, worded the same.

bool s_live_ever_ok = false;
bool s_live_day_ok = false;

void show_live(const LiveUpdate& update) {
  if (!update.day_series.empty()) {
    const uint32_t now_minute = static_cast<uint32_t>(time(nullptr) / 60);
    if (day_series_parse(HistoryBank::Live, update.day_series.c_str(),
                         update.day_series.size(), now_minute)) {
      s_live_day_ok = true;
      printf("[live] day series (%zu bytes)\n", update.day_series.size());
    } else {
      fprintf(stderr, "[live] day series did not parse\n");
    }
  }

  if (!update.summary.empty()) {
    Snapshot snapshot;
    if (snapshot_parse(update.summary.c_str(), update.summary.size(), &snapshot)) {
      s_live_ever_ok = true;
      ui_set_overlay(nullptr, nullptr, nullptr);
      lv_obj_set_style_text_color(s_body, lv_color_hex(COLOUR_TEXT), LV_PART_MAIN);
      lv_label_set_text(s_body, describe(snapshot).c_str());
      ui_update(snapshot);

      char title[160];
      const time_t now = time(nullptr);
      struct tm local;
      localtime_r(&now, &local);
      snprintf(title, sizeof(title), "SigenStorPanel sim - live %s - %02d:%02d:%02d",
               s_live_host.c_str(), local.tm_hour, local.tm_min, local.tm_sec);
      sim_backend_set_title(title);
    } else {
      fprintf(stderr, "[live] summary did not parse (%zu bytes)\n", update.summary.size());
    }
  }

  if (!update.error.empty()) {
    const std::string title = "SigenStorPanel sim - live - " + update.error;
    sim_backend_set_title(title.c_str());
    // A rejected token never fixes itself, so it is said whatever came before.
    if (update.error == "http 401" || update.error == "http 403") {
      ui_set_overlay_page(SheetPage::Data);
      ui_set_overlay("Re-enrol needed", s_live_host.c_str(),
                     "The server no longer accepts this panel's code.\nTap to enter a new one, "
                     "or scan this",
                     true);
    } else if (!s_live_ever_ok) {
      ui_set_overlay_page(SheetPage::Data);
      const std::string detail =
          "Cannot reach the server\n" + update.error + "\nTap to check the settings";
      ui_set_overlay("No data", nullptr, detail.c_str());
    }
  }
}

// The link pasted into the settings sheet, kept between runs the way the device
// keeps it in NVS. In the home directory, never the tree, and readable only by
// you: it is a working credential for your server.
std::string live_link_path() {
  const char* home = getenv("HOME");
  return std::string(home != nullptr ? home : ".") + "/.config/sigenstorpanel/sim-server";
}

std::string live_link_load() {
  std::ifstream file(live_link_path());
  std::string link;
  std::getline(file, link);
  return link;
}

void live_link_save(const std::string& link) {
  const std::string path = live_link_path();
  const std::string directory = path.substr(0, path.rfind('/'));
  mkdir((directory.substr(0, directory.rfind('/'))).c_str(), 0700);
  mkdir(directory.c_str(), 0700);
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    fprintf(stderr, "[live] could not save the link to %s\n", path.c_str());
    return;
  }
  const std::string line = link + "\n";
  if (write(fd, line.data(), line.size()) != static_cast<ssize_t>(line.size())) {
    fprintf(stderr, "[live] could not save the link to %s\n", path.c_str());
  }
  close(fd);
}

SheetData s_sheet_data;
// The sim's settings_set_server(): the server and its code, as last saved.
std::string s_sheet_base;
std::string s_sheet_code;

// As settings_token_masked() shows a code: its last four characters.
void show_sheet_server() {
  const size_t at = s_sheet_base.find("://");
  snprintf(s_sheet_data.server_host, sizeof(s_sheet_data.server_host), "%s",
           at == std::string::npos ? s_sheet_base.c_str() : s_sheet_base.c_str() + at + 3);
  const size_t length = s_sheet_code.size();
  if (length == 0) {
    s_sheet_data.server_code[0] = '\0';
  } else if (length <= 4) {
    snprintf(s_sheet_data.server_code, sizeof(s_sheet_data.server_code), "****");
  } else {
    snprintf(s_sheet_data.server_code, sizeof(s_sheet_data.server_code), "...%s",
             s_sheet_code.c_str() + length - 4);
  }
  settings_sheet_set_data(s_sheet_data);
}

// Switches the screens from the fixtures to `base`. The fixture's made-up day
// goes first, so the real day series is not drawn on top of it.
bool start_live(const std::string& base, const std::string& token) {
  s_live_host = base.substr(base.find("://") + 3);
  const size_t shown = token.size() > 4 ? 4 : token.size();
  printf("[live] polling %s with token ...%s\n", base.c_str(),
         token.substr(token.size() - shown).c_str());
  if (!live_source_begin(base, token, 5)) {
    fprintf(stderr, "[sim] curl did not start\n");
    return false;
  }
  s_live = true;
  s_live_ever_ok = false;
  s_live_day_ok = false;
  history_reset(HistoryBank::Live);
  forecast_store_clear();
  ui_clear_day();
  ui_set_day_offset(0);
  ui_set_overlay("Waiting for data", nullptr, nullptr);
  sim_backend_set_title("SigenStorPanel sim - live - connecting");

  s_sheet_data.source = DataSource::Server;
  s_sheet_base = base;
  s_sheet_code = token;
  show_sheet_server();
  return true;
}

void show_current_fixture();

void stop_live() {
  live_source_end();
  s_live = false;
  ui_set_overlay(nullptr, nullptr, nullptr);
  show_current_fixture();
}

// The bridge's save_server(): a blank code keeps the one saved, and a code is
// tidied as the device tidies it. Live as soon as there are both.
bool save_sheet_server(const char* base, const char* code, char* error, size_t size) {
  char tidy[KIOSK_CODE_SIZE] = {};
  if (code[0] != '\0' && !kiosk_code_tidy(code, tidy, sizeof(tidy))) {
    snprintf(error, size, "Not a kiosk code. A code is 20 letters and digits");
    return false;
  }
  s_sheet_base = base;
  if (tidy[0] != '\0') {
    s_sheet_code = tidy;
  }
  printf("[sim] server %s%s\n", base, s_sheet_code.empty() ? ", no code yet" : "");
  show_sheet_server();
  // The screen-shot pass types made-up details; only a person's go live.
  if (s_shot_pass || s_sheet_code.empty()) {
    return true;
  }
  live_link_save(s_sheet_base + "/kiosk-enroll?token=" + s_sheet_code);
  return start_live(s_sheet_base, s_sheet_code);
}

// ---------------------------------------------------------- settings sheet ---
//
// Stand-in for the device's settings store, WiFi and poll task. Every action
// writes back into this and pushes it to the sheet again, the way main.cpp does
// from NVS, so the sheet can be clicked through here. The Device page's memory
// figures are made up: the sim has no ESP32 heap to report.

SheetDisplay s_sheet_display;
SheetScreens s_sheet_screens;
SheetWifi s_sheet_wifi;
SheetDevice s_sheet_device;
SheetSolar s_sheet_solar;
SheetScan s_sheet_scan;
SheetJoinStatus s_sheet_join;
char s_sheet_joining[33] = {};

// What a scan "finds" in the sim.
void demo_scan() {
  struct Demo {
    const char* ssid;
    int8_t rssi;
    bool secure;
  };
  static const Demo FOUND[] = {
      {"Home WiFi", -58, true},          {"Home WiFi Garage", -77, true},
      {"BT-7HQ2KX", -66, true},          {"Neighbour 5G", -71, true},
      {"Cafe Guest", -80, false},        {"SKY4A1F2", -84, true},
      {"DIRECT-printer", -86, true},
  };
  s_sheet_scan = SheetScan{};
  for (const Demo& demo : FOUND) {
    SheetScanResult& result = s_sheet_scan.results[s_sheet_scan.count++];
    snprintf(result.ssid, sizeof(result.ssid), "%s", demo.ssid);
    result.rssi = demo.rssi;
    result.secure = demo.secure;
    for (int i = 0; i < s_sheet_wifi.saved_count; ++i) {
      result.saved = result.saved || strcmp(s_sheet_wifi.saved[i].ssid, demo.ssid) == 0;
    }
  }
  settings_sheet_set_scan(s_sheet_scan);
}

void demo_network(SheetNetwork* network, const char* ssid, int8_t rssi, bool in_range) {
  snprintf(network->ssid, sizeof(network->ssid), "%s", ssid);
  network->rssi = rssi;
  network->in_range = in_range;
}

void push_sheet_device() {
  s_sheet_device.uptime_s = lv_tick_get() / 1000 + 3 * 3600 + 12 * 60;
  settings_sheet_set_device(s_sheet_device);
}

void seed_sheet() {
  s_sheet_display.brightness = 204;
  s_sheet_display.dim_brightness = 51;
  s_sheet_display.dim_after_s = 60;
  s_sheet_display.screen_off_set = true;
  s_sheet_display.screen_off_start_min = 22 * 60 + 30;
  s_sheet_display.screen_off_end_min = 6 * 60 + 30;
  settings_sheet_set_display(s_sheet_display);

  // As if a room sensor were fitted and someone were standing in front of it.
  SheetPresence presence;
  presence.connected = true;
  presence.present = true;
  presence.has_distance = true;
  presence.distance_cm = 140;
  settings_sheet_set_presence(presence);

  s_sheet_screens.available =
      s_with_detailed_screens ? 0xFF : static_cast<uint8_t>(~PUCK_DETAILED_SCREENS);
  s_sheet_screens.visible = 0xFF;
  s_sheet_screens.rotate = static_cast<uint8_t>(~(1u << PUCK_SCREEN_COST));
  s_sheet_screens.rotate_enabled = true;
  s_sheet_screens.rotate_s = 20;
  settings_sheet_set_screens(s_sheet_screens);

  s_sheet_wifi.connected = true;
  snprintf(s_sheet_wifi.ssid, sizeof(s_sheet_wifi.ssid), "Home WiFi");
  s_sheet_wifi.rssi = -58;
  s_sheet_wifi.channel = 6;
  snprintf(s_sheet_wifi.ip, sizeof(s_sheet_wifi.ip), "192.168.1.100");
  demo_network(&s_sheet_wifi.saved[0], "Home WiFi", -58, true);
  demo_network(&s_sheet_wifi.saved[1], "Home WiFi Garage", -77, true);
  demo_network(&s_sheet_wifi.saved[2], "Holiday cottage", 0, false);
  s_sheet_wifi.saved_count = 3;
  settings_sheet_set_wifi(s_sheet_wifi);

  s_sheet_data.source = s_with_detailed_screens ? DataSource::Server : DataSource::Modbus;
  s_sheet_base = "https://sigen.example.com";
  s_sheet_code = "k7mq4xrp9tbzh2wdc8nf";
  show_sheet_server();
  snprintf(s_sheet_data.modbus_host, sizeof(s_sheet_data.modbus_host), "192.168.1.20");
  s_sheet_data.modbus_devices[0] = {1, SheetDeviceKind::InverterWithDc};
  s_sheet_data.modbus_devices[1] = {2, SheetDeviceKind::Inverter};
  s_sheet_data.modbus_devices[2] = {3, SheetDeviceKind::AcCharger};
  snprintf(s_sheet_data.ha_host, sizeof(s_sheet_data.ha_host), "homeassistant.local:8123");
  s_sheet_data.poll_s = 5;
  settings_sheet_set_data(s_sheet_data);

  s_sheet_solar.location_set = true;
  s_sheet_solar.latitude = 51.5074f;
  s_sheet_solar.longitude = -0.1278f;
  s_sheet_solar.system_loss = 0.85f;
  s_sheet_solar.inverter_cap_kw = 8.0f;
  s_sheet_solar.arrays[0] = {6.4f, 35.0f, 180.0f};
  s_sheet_solar.arrays[1] = {3.2f, 30.0f, 270.0f};
  s_sheet_solar.ha_source = SolarForecastSource::Puck;
  settings_sheet_set_solar(s_sheet_solar);

  snprintf(s_sheet_device.firmware, sizeof(s_sheet_device.firmware), "%s", PUCK_FW_VERSION);
  snprintf(s_sheet_device.host, sizeof(s_sheet_device.host), "sigenstorpanel");
  snprintf(s_sheet_device.ip, sizeof(s_sheet_device.ip), "192.168.1.100");
  s_sheet_device.internal_free = 412 * 1024;
  s_sheet_device.internal_largest = 192 * 1024;
  s_sheet_device.psram_free = 29 * 1024 * 1024 + 300 * 1024;
  s_sheet_device.psram_total = 32 * 1024 * 1024;
  s_sheet_device.wifi_connected = true;
  s_sheet_device.rssi = -58;
  s_sheet_device.ever_read = true;
  s_sheet_device.last_read_age_s = 3;
  s_sheet_device.update = SheetUpdate::UpToDate;
  push_sheet_device();
}

SheetActions sim_sheet_actions() {
  SheetActions actions;
  actions.preview_brightness = [](uint8_t level) {
    printf("[sim] backlight preview %u\n", static_cast<unsigned>(level));
  };
  actions.set_display = [](const SheetDisplay& display) {
    s_sheet_display = display;
    printf("[sim] display: %u/%u after %us, off %s %02u:%02u-%02u:%02u, flipped %s, "
           "empty room off after %us\n",
           display.brightness, display.dim_brightness, static_cast<unsigned>(display.dim_after_s),
           display.screen_off_set ? "on" : "off", display.screen_off_start_min / 60,
           display.screen_off_start_min % 60, display.screen_off_end_min / 60,
           display.screen_off_end_min % 60, display.flipped ? "yes" : "no",
           static_cast<unsigned>(display.presence_off_after_s));
    settings_sheet_set_display(s_sheet_display);
  };
  actions.set_screens = [](const SheetScreens& screens) {
    s_sheet_screens = screens;
    printf("[sim] screens: visible 0x%02X cycle 0x%02X, cycle %s every %us\n", screens.visible,
           screens.rotate, screens.rotate_enabled ? "on" : "off",
           static_cast<unsigned>(screens.rotate_s));
    ui_set_rotate_enabled(screens.rotate_enabled);
    ui_set_rotate_interval(screens.rotate_s);
    settings_sheet_set_screens(s_sheet_screens);
  };
  actions.forget_network = [](const char* ssid) {
    printf("[sim] forget network %s\n", ssid);
    int kept = 0;
    for (int i = 0; i < s_sheet_wifi.saved_count; ++i) {
      if (strcmp(s_sheet_wifi.saved[i].ssid, ssid) != 0) {
        s_sheet_wifi.saved[kept++] = s_sheet_wifi.saved[i];
      }
    }
    s_sheet_wifi.saved_count = static_cast<uint8_t>(kept);
    settings_sheet_set_wifi(s_sheet_wifi);
  };
  actions.start_scan = []() {
    printf("[sim] scan\n");
    s_sheet_scan.running = true;
    settings_sheet_set_scan(s_sheet_scan);
    lv_timer_t* done = lv_timer_create([](lv_timer_t*) { demo_scan(); }, 1200, nullptr);
    lv_timer_set_repeat_count(done, 1);
  };
  // Any password works except "wrongpass", which the sim refuses the way a real
  // access point would.
  actions.join_network = [](const char* ssid, const char* password) {
    printf("[sim] join %s\n", ssid);
    snprintf(s_sheet_joining, sizeof(s_sheet_joining), "%s", ssid);
    const bool refuse = strcmp(password, "wrongpass") == 0;
    s_sheet_join = SheetJoinStatus{};
    s_sheet_join.state = SheetJoin::Joining;
    settings_sheet_set_join(s_sheet_join);
    lv_timer_t* done = lv_timer_create(
        [](lv_timer_t* timer) {
          const bool refused = timer->user_data != nullptr;
          s_sheet_join.state = refused ? SheetJoin::Failed : SheetJoin::Joined;
          if (refused) {
            snprintf(s_sheet_join.message, sizeof(s_sheet_join.message), "Wrong password?");
          } else {
            // Front of the saved list, as the device's store does it.
            SheetNetwork joined;
            snprintf(joined.ssid, sizeof(joined.ssid), "%s", s_sheet_joining);
            joined.rssi = -60;
            joined.in_range = true;
            int kept = 1;
            SheetNetwork list[SHEET_MAX_NETWORKS];
            list[0] = joined;
            for (int i = 0; i < s_sheet_wifi.saved_count && kept < SHEET_MAX_NETWORKS; ++i) {
              if (strcmp(s_sheet_wifi.saved[i].ssid, s_sheet_joining) != 0) {
                list[kept++] = s_sheet_wifi.saved[i];
              }
            }
            memcpy(s_sheet_wifi.saved, list, sizeof(list));
            s_sheet_wifi.saved_count = static_cast<uint8_t>(kept);
            snprintf(s_sheet_wifi.ssid, sizeof(s_sheet_wifi.ssid), "%s", s_sheet_joining);
            s_sheet_wifi.connected = true;
            settings_sheet_set_wifi(s_sheet_wifi);
          }
          settings_sheet_set_join(s_sheet_join);
        },
        1500, refuse ? reinterpret_cast<void*>(1) : nullptr);
    lv_timer_set_repeat_count(done, 1);
  };
  actions.set_auto_check = [](bool enabled) {
    printf("[sim] check for updates automatically: %s\n", enabled ? "on" : "off");
    s_sheet_device.auto_check = enabled;
  };
  actions.set_source = [](DataSource source) {
    s_sheet_data.source = source;
    printf("[sim] source %s\n", data_source_name(source));
    settings_sheet_set_data(s_sheet_data);
  };
  actions.forget_server = []() {
    printf("[sim] forget server link\n");
    unlink(live_link_path().c_str());
    if (s_live) {
      stop_live();
    }
    s_sheet_base.clear();
    s_sheet_code.clear();
    show_sheet_server();
  };
  actions.set_modbus_host = [](const char* host) {
    printf("[sim] modbus host %s\n", host);
    snprintf(s_sheet_data.modbus_host, sizeof(s_sheet_data.modbus_host), "%s", host);
    settings_sheet_set_data(s_sheet_data);
  };
  // The same parser the device uses, so a bad link is refused here as it is there.
  actions.set_enrolment_link = [](const char* link, char* error, size_t size) {
    const EnrolUrl parsed = enrol_url_parse(link);
    if (!parsed.ok) {
      snprintf(error, size, "Not a link. It should start https://");
      return false;
    }
    if (parsed.token[0] == '\0') {
      snprintf(error, size, "That link has no token in it");
      return false;
    }
    return save_sheet_server(parsed.base, parsed.token, error, size);
  };
  actions.set_server_address = [](const char* url, char* error, size_t size) {
    const EnrolUrl parsed = enrol_url_parse(url);
    if (!parsed.ok) {
      snprintf(error, size, "It should start https:// or http://");
      return false;
    }
    return save_sheet_server(parsed.base, parsed.token, error, size);
  };
  actions.set_kiosk_code = [](const char* code, char* error, size_t size) {
    if (s_sheet_base.empty()) {
      snprintf(error, size, "Set the server first");
      return false;
    }
    if (code[0] == '\0') {
      snprintf(error, size, "Type the code first");
      return false;
    }
    // A copy: save_sheet_server() assigns s_sheet_base from what it is given.
    return save_sheet_server(std::string(s_sheet_base).c_str(), code, error, size);
  };
  actions.set_ha_address = [](const char* url, char* error, size_t size) {
    const EnrolUrl parsed = enrol_url_parse(url);
    if (!parsed.ok) {
      snprintf(error, size, "It should start http:// or https://");
      return false;
    }
    printf("[sim] home assistant at %s\n", parsed.base);
    const char* host = strstr(parsed.base, "://");
    snprintf(s_sheet_data.ha_host, sizeof(s_sheet_data.ha_host), "%s",
             host != nullptr ? host + 3 : parsed.base);
    settings_sheet_set_data(s_sheet_data);
    return true;
  };
  actions.set_ha_token = [](const char* token, char* error, size_t size) {
    if (strlen(token) < 20) {
      snprintf(error, size, "Too short for a Home Assistant token");
      return false;
    }
    printf("[sim] home assistant token (%zu characters)\n", strlen(token));
    s_sheet_data.ha_set = true;
    settings_sheet_set_data(s_sheet_data);
    return true;
  };
  actions.set_modbus_devices = [](uint8_t plant, const SheetModbusDevice* devices, int count) {
    printf("[sim] modbus plant %u:", static_cast<unsigned>(plant));
    for (int i = 0; i < count; ++i) {
      printf(" %u", static_cast<unsigned>(devices[i].slave_id));
    }
    printf("\n");
    s_sheet_data.modbus_plant_address = plant;
    memcpy(s_sheet_data.modbus_devices, devices, sizeof(SheetModbusDevice) * count);
    settings_sheet_set_data(s_sheet_data);
  };
  actions.set_solar = [](const SheetSolar& solar) {
    printf("[sim] solar: %.4f, %.4f, efficiency %.2f, limit %.1f kW\n", solar.latitude,
           solar.longitude, solar.system_loss, solar.inverter_cap_kw);
    s_sheet_solar = solar;
    settings_sheet_set_solar(s_sheet_solar);
  };
  actions.set_ha_forecast_source = [](SolarForecastSource source) {
    printf("[sim] home assistant forecast: %s\n", solar_forecast_source_name(source));
    s_sheet_solar.ha_source = source;
  };
  actions.set_poll_interval = [](uint32_t seconds) {
    printf("[sim] poll every %us\n", static_cast<unsigned>(seconds));
    s_sheet_data.poll_s = seconds;
    settings_sheet_set_data(s_sheet_data);
  };
  actions.check_update = []() {
    printf("[sim] update check requested\n");
    s_sheet_device.update = SheetUpdate::Checking;
    push_sheet_device();
    lv_timer_t* done = lv_timer_create(
        [](lv_timer_t*) {
          s_sheet_device.update = SheetUpdate::UpToDate;
          push_sheet_device();
        },
        1500, nullptr);
    lv_timer_set_repeat_count(done, 1);
  };
  actions.install_update = []() { printf("[sim] install requested\n"); };
  actions.restart = []() { printf("[sim] restart requested\n"); };
  return actions;
}

void build_debug_view() {
  lv_obj_t* screen = s_debug_root;

  // Same bezel guide as the device's bring-up screen, so it stays obvious that
  // the usable area is a circle and not a 466 px square.
  lv_obj_t* bezel = lv_obj_create(screen);
  lv_obj_set_size(bezel, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_center(bezel);
  lv_obj_set_style_radius(bezel, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bezel, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(bezel, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(bezel, lv_color_hex(COLOUR_MUTED), LV_PART_MAIN);
  lv_obj_clear_flag(bezel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(bezel, LV_OBJ_FLAG_CLICKABLE);

  s_heading = lv_label_create(screen);
  lv_obj_set_style_text_font(s_heading, PUCK_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_heading, lv_color_hex(COLOUR_ACCENT), LV_PART_MAIN);
  lv_obj_align(s_heading, LV_ALIGN_TOP_MID, 0, 44);

  s_body = lv_label_create(screen);
  lv_obj_set_style_text_font(s_body, PUCK_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_body, lv_color_hex(COLOUR_TEXT), LV_PART_MAIN);
  lv_obj_set_width(s_body, PUCK_SAFE_SQUARE);
  lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
  lv_obj_align(s_body, LV_ALIGN_TOP_MID, 0, 70);

  lv_obj_t* hint = lv_label_create(screen);
  lv_obj_set_style_text_font(hint, PUCK_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(hint, lv_color_hex(COLOUR_MUTED), LV_PART_MAIN);
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_text(hint,
                    "n/p fixture   [ ] screen   , . tilt   - = day\n"
                    "r auto-cycle   o overlay   s settings   d dump   esc quit");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -34);
}

// Builds both views once and swaps visibility, rather than tearing down and
// rebuilding a widget tree on every keypress.
void build_views() {
  lv_obj_t* screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_hex(COLOUR_BACKGROUND), LV_PART_MAIN);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  UiConfig config;
  config.with_detailed_screens = s_with_detailed_screens;
  ui_create(screen, config);

  // A stand-in for what net_hostname()/net_ip() return on the device, so the
  // settings screen and its QR code render here rather than only on hardware.
  ui_set_address("sigenstorpanel.local", "192.168.1.100");

  settings_sheet_create(screen, sim_sheet_actions());
  seed_sheet();
  lv_timer_create([](lv_timer_t*) { push_sheet_device(); }, 1000, nullptr);

  s_debug_root = lv_obj_create(screen);
  lv_obj_remove_style_all(s_debug_root);
  lv_obj_set_size(s_debug_root, PUCK_LCD_WIDTH, PUCK_LCD_HEIGHT);
  lv_obj_center(s_debug_root);
  lv_obj_set_style_bg_color(s_debug_root, lv_color_hex(COLOUR_BACKGROUND), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_debug_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_debug_root, LV_OBJ_FLAG_SCROLLABLE);

  build_debug_view();
  lv_obj_add_flag(s_debug_root, LV_OBJ_FLAG_HIDDEN);
}

void apply_view_visibility() {
  if (s_show_debug) {
    lv_obj_clear_flag(s_debug_root, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_debug_root, LV_OBJ_FLAG_HIDDEN);
  }
}

// The overlays are driven by device state — WiFi, provisioning, a revoked token
// — none of which a fixture can express. Cycling them from the keyboard is the
// only way to lay them out without flashing a deliberately broken device.
// The same words and tap targets as refresh_overlay() in src/main.cpp.
constexpr int OVERLAY_STATES = 4;  // three messages, then none

void cycle_overlay() {
  static int which = 0;
  which = (which + 1) % OVERLAY_STATES;
  switch (which) {
    case 1:
      ui_set_overlay_page(SheetPage::Data);
      ui_set_overlay("Not configured",
                     "http://sigenstorpanel.local/\n"
                     "http://192.168.1.100/",
                     "Tap to enter the server and code, or scan this", true);
      break;
    case 2:
      ui_set_overlay_page(SheetPage::Data);
      ui_set_overlay("Re-enrol needed",
                     "http://sigenstorpanel.local/\n"
                     "http://192.168.1.100/",
                     "The server no longer accepts this panel's code.\nTap to enter a new one, "
                     "or scan this",
                     true);
      break;
    case 3:
      ui_set_overlay_page(SheetPage::Wifi);
      ui_set_overlay("Set up WiFi", nullptr, "Tap or swipe up to choose a network");
      break;
    default:
      ui_set_overlay(nullptr, nullptr, nullptr);
      break;
  }
}

void handle_key(int key) {
  const size_t count = s_fixture_paths.size();
  if (key == 'o') {
    cycle_overlay();
    return;
  }
  if (key == 's') {
    if (settings_sheet_is_open()) {
      settings_sheet_close(true);
    } else {
      settings_sheet_open(settings_sheet_page(), true);
    }
    return;
  }
  if (key == 'd') {
    s_show_debug = !s_show_debug;
    apply_view_visibility();
    return;
  }
  // Fine rotation, for judging the resampling cost before it reaches hardware.
  if (key == ',' || key == '.') {
    static int16_t tenths = 0;
    tenths += (key == '.') ? 5 : -5;
    if (tenths > 300) tenths = 300;
    if (tenths < -300) tenths = -300;
    ui_set_fine_rotation(tenths);
    printf("[sim] fine rotation %.1f degrees\n", tenths / 10.0);
    return;
  }
  // The day indicator and the toast, which the buttons drive on hardware and no
  // fixture can express.
  if (key == '-' || key == '=') {
    if (s_live) {
      // Past days need the dated fetches, which --live does not make.
      printf("[sim] --live shows today only\n");
      return;
    }
    ui_set_day_offset(ui_day_offset() + (key == '=' ? 1 : -1));
    // The device fetches a dated payload for a past day; here the current fixture
    // stands in for it, so the loaded state is what gets drawn. Shift-stepping
    // shows the state before it arrives instead.
    if (ui_day_offset() == 0) {
      ui_clear_day();
    } else {
      show_current_fixture();
    }
    return;
  }
  if (key == '_' || key == '+') {
    ui_set_day_offset(ui_day_offset() + (key == '+' ? 1 : -1));
    if (ui_day_offset() == 0) {
      ui_clear_day();
    } else {
      ui_set_day_loading();
    }
    return;
  }
  if (key == 'r') {
    const bool on = !ui_rotate_enabled();
    ui_set_rotate_enabled(on);
    ui_toast(on ? "CYCLE ON" : "CYCLE OFF");
    return;
  }
  if (key == ']' || key == '[') {
    const int count = ui_screen_count();
    const int step = key == ']' ? 1 : count - 1;
    ui_show_screen((ui_current_screen() + step) % count);
    return;
  }
  if (s_live) {
    return;
  }
  if (key == 'n' || key == ' ') {
    s_current = (s_current + 1) % count;
  } else if (key == 'p') {
    s_current = (s_current + count - 1) % count;
  } else if (key >= '1' && key <= '9') {
    const size_t requested = static_cast<size_t>(key - '1');
    if (requested >= count) {
      return;
    }
    s_current = requested;
  } else {
    return;
  }
  show_current_fixture();
}

void run_frames(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 16) {
    sim_backend_delay(16);
    lv_timer_handler();
  }
}

// A finger drag from one point to another through the real input path, so the
// gesture handling is what gets tested, not just the functions it calls.
void drag(int x0, int y0, int x1, int y1) {
  constexpr int STEPS = 12;
  sim_backend_set_pointer(x0, y0, true);
  run_frames(40);
  for (int i = 1; i <= STEPS; ++i) {
    sim_backend_set_pointer(x0 + (x1 - x0) * i / STEPS, y0 + (y1 - y0) * i / STEPS, true);
    run_frames(16);
  }
  sim_backend_set_pointer(x1, y1, false);
  run_frames(500);
}

bool save_shot(const std::string& directory, const char* name) {
  lv_refr_now(nullptr);
  const std::string path = directory + "/" + name + ".bmp";
  if (!sim_backend_save_bmp(path.c_str())) {
    return false;
  }
  printf("[sim] wrote %s\n", path.c_str());
  return true;
}

// --live --shot: waits for the server's first reading and day series, then
// writes every screen once. The same pass as the fixtures', against real data.
int run_live_shots(const std::string& directory) {
  for (int waited = 0; waited < 30000 && !(s_live_ever_ok && s_live_day_ok); waited += 100) {
    LiveUpdate update;
    if (live_source_take(&update)) {
      show_live(update);
    }
    run_frames(100);
  }
  if (!s_live_ever_ok) {
    fprintf(stderr, "[sim] no reading from the server within 30 s\n");
    return 1;
  }
  int failures = 0;
  for (int screen = 0; screen < ui_screen_count(); ++screen) {
    ui_show_screen(screen);
    run_frames(600);
    char name[32];
    snprintf(name, sizeof(name), "live_%d", screen + 1);
    failures += save_shot(directory, name) ? 0 : 1;
  }
  return failures;
}

// The settings sheet: the swipes that open, page and close it, then every page.
int run_sheet_shots(const std::string& directory) {
  int failures = 0;
  const auto check = [&failures](bool ok, const char* what) {
    printf("[sim] sheet: %s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
      ++failures;
    }
  };

  settings_sheet_close(false);
  ui_show_screen(0);
  run_frames(100);
  drag(512, 460, 512, 160);
  check(settings_sheet_is_open(), "swipe up opens it");
  drag(820, 330, 200, 330);
  check(settings_sheet_page() == SheetPage::Screens, "swipe left goes to the next page");
  drag(512, 140, 512, 460);
  check(!settings_sheet_is_open(), "swipe down closes it");
  ui_show_screen(2);
  run_frames(100);
  drag(300, 480, 300, 180);
  check(settings_sheet_is_open() && ui_current_screen() == 2,
        "opens from another screen and leaves it there");
  settings_sheet_close(false);

  // A message covering the screens is a thing to fix in settings: a tap on it
  // opens the page that fixes it, and a swipe up still works through it.
  ui_set_overlay_page(SheetPage::Data);
  ui_set_overlay("Re-enrol needed", nullptr, "Tap to enter a new one");
  run_frames(100);
  drag(512, 300, 512, 300);
  check(settings_sheet_is_open() && settings_sheet_page() == SheetPage::Data,
        "tapping a data message opens Data");
  settings_sheet_close(false);
  ui_set_overlay_page(SheetPage::Wifi);
  ui_set_overlay("Set up WiFi", nullptr, "Tap or swipe up to choose a network");
  run_frames(100);
  drag(512, 300, 512, 300);
  check(settings_sheet_is_open() && settings_sheet_page() == SheetPage::Wifi,
        "tapping a WiFi message opens WiFi");
  settings_sheet_close(false);
  run_frames(100);
  drag(512, 460, 512, 160);
  check(settings_sheet_is_open(), "swipe up opens it through a message");
  settings_sheet_close(false);
  ui_set_overlay(nullptr, nullptr, nullptr);
  run_frames(100);

  static const char* const NAMES[] = {"sheet_1_display", "sheet_2_screens", "sheet_3_wifi",
                                      "sheet_4_data",    "sheet_5_solar",   "sheet_6_device"};
  for (int page = 0; page < static_cast<int>(SheetPage::Count); ++page) {
    settings_sheet_open(static_cast<SheetPage>(page), false);
    run_frames(100);
    failures += save_shot(directory, NAMES[page]) ? 0 : 1;
  }

  // The states a fresh seed does not show: a changed source with its restart
  // bar, the address keypad, and an update on offer.
  const SheetData seeded = s_sheet_data;
  s_sheet_data.source =
      seeded.source == DataSource::Modbus ? DataSource::Server : DataSource::Modbus;
  settings_sheet_set_data(s_sheet_data);
  settings_sheet_open(SheetPage::Data, false);
  run_frames(100);
  failures += save_shot(directory, "sheet_4b_data_other_source") ? 0 : 1;
  if (s_sheet_data.source == DataSource::Modbus) {
    settings_sheet_show_keypad(true);
    run_frames(50);
    failures += save_shot(directory, "sheet_4c_keypad") ? 0 : 1;
    settings_sheet_show_keypad(false);
  }
  s_sheet_data = seeded;
  settings_sheet_set_data(s_sheet_data);

  // Typing credentials on the glass, and the plant editors.
  // Through the physical-keyboard path, as a ⌘V paste arrives: a character at a
  // time, a typo and its backspace, then return to save.
  settings_sheet_show_entry(SheetEntry::EnrolmentLink);
  check(settings_sheet_typing(), "link entry takes the keyboard");
  for (const char* c = "https://sigen.example.com/kiosk-enroll?token=k7mq-4xrp-9tbz-h2wd-c8nfx";
       *c; ++c) {
    settings_sheet_type_key(*c);
  }
  settings_sheet_type_key('\b');
  run_frames(50);
  failures += save_shot(directory, "sheet_4d_enter_link") ? 0 : 1;
  const auto forget = []() {
    s_sheet_base.clear();
    s_sheet_code.clear();
    show_sheet_server();
  };
  forget();
  settings_sheet_type_key('\r');
  run_frames(50);
  check(!settings_sheet_typing() && strcmp(s_sheet_data.server_host, "sigen.example.com") == 0 &&
            s_sheet_code == "k7mq4xrp9tbzh2wdc8nf",
        "return saves the typed link");

  // The link's two halves instead: the server, then its code read off the Admin
  // page in capitals and spaced groups, one character short at first.
  forget();
  settings_sheet_open(SheetPage::Data, false);
  settings_sheet_show_entry(SheetEntry::ServerAddress);
  for (const char* c = "sigen.example.com"; *c; ++c) {
    settings_sheet_type_key(*c);
  }
  settings_sheet_type_key('\r');
  run_frames(50);
  check(!settings_sheet_typing() && s_sheet_base == "https://sigen.example.com" &&
            s_sheet_data.server_code[0] == '\0',
        "the server saves without a code");
  failures += save_shot(directory, "sheet_4g_data_no_code") ? 0 : 1;
  settings_sheet_show_entry(SheetEntry::KioskCode);
  for (const char* c = "K7MQ 4XRP 9TBZ H2WD C8N"; *c; ++c) {
    settings_sheet_type_key(*c);
  }
  settings_sheet_type_key('\r');
  check(settings_sheet_typing() && s_sheet_code.empty(), "a short code is refused");
  settings_sheet_type_key('F');
  run_frames(50);
  failures += save_shot(directory, "sheet_4h_enter_code") ? 0 : 1;
  settings_sheet_type_key('\r');
  run_frames(50);
  check(!settings_sheet_typing() && s_sheet_code == "k7mq4xrp9tbzh2wdc8nf" &&
            strcmp(s_sheet_data.server_code, "...c8nf") == 0,
        "a code saves tidied, and shows masked");
  settings_sheet_close(false);

  const SheetData before_plant = s_sheet_data;
  s_sheet_data.source = DataSource::Modbus;
  settings_sheet_set_data(s_sheet_data);
  settings_sheet_open(SheetPage::Data, false);
  settings_sheet_show_devices();
  run_frames(50);
  failures += save_shot(directory, "sheet_4e_modbus_devices") ? 0 : 1;
  settings_sheet_close(false);
  settings_sheet_open(SheetPage::Solar, false);
  run_frames(50);
  failures += save_shot(directory, "sheet_5b_solar_modbus") ? 0 : 1;
  settings_sheet_show_array(0);
  run_frames(50);
  failures += save_shot(directory, "sheet_5c_array") ? 0 : 1;
  settings_sheet_close(false);
  s_sheet_data.source = DataSource::HomeAssistant;
  settings_sheet_set_data(s_sheet_data);
  settings_sheet_open(SheetPage::Data, false);
  run_frames(50);
  failures += save_shot(directory, "sheet_4f_data_home_assistant") ? 0 : 1;
  settings_sheet_open(SheetPage::Solar, false);
  run_frames(50);
  failures += save_shot(directory, "sheet_5d_solar_home_assistant") ? 0 : 1;
  settings_sheet_close(false);
  s_sheet_data = before_plant;
  settings_sheet_set_data(s_sheet_data);

  s_sheet_device.update = SheetUpdate::Available;
  snprintf(s_sheet_device.update_version, sizeof(s_sheet_device.update_version), "0.1.0");
  settings_sheet_open(SheetPage::Device, false);
  push_sheet_device();
  run_frames(100);
  failures += save_shot(directory, "sheet_6b_update") ? 0 : 1;
  s_sheet_device.update = SheetUpdate::UpToDate;
  push_sheet_device();

  // Adding a network on the glass: the picker, the password, a refusal, success.
  settings_sheet_show_picker();
  run_frames(1500);
  failures += save_shot(directory, "sheet_3b_picker") ? 0 : 1;
  settings_sheet_show_password("BT-7HQ2KX", true);
  settings_sheet_type_password("wrongpass");
  run_frames(50);
  failures += save_shot(directory, "sheet_3c_password") ? 0 : 1;
  settings_sheet_press_join();
  run_frames(1800);
  failures += save_shot(directory, "sheet_3d_wrong_password") ? 0 : 1;
  settings_sheet_show_password("BT-7HQ2KX", true);
  settings_sheet_type_password("correct horse");
  settings_sheet_press_join();
  run_frames(1800);
  check(s_sheet_wifi.saved_count > 0 && strcmp(s_sheet_wifi.saved[0].ssid, "BT-7HQ2KX") == 0,
        "a joined network is saved first");
  failures += save_shot(directory, "sheet_3e_joined") ? 0 : 1;

  settings_sheet_close(false);

  // The install screen covers everything, sheet or no sheet.
  s_sheet_device.update = SheetUpdate::Installing;
  s_sheet_device.update_percent = 42;
  snprintf(s_sheet_device.update_version, sizeof(s_sheet_device.update_version), "0.1.0");
  push_sheet_device();
  run_frames(50);
  failures += save_shot(directory, "update_installing") ? 0 : 1;
  s_sheet_device.update = SheetUpdate::UpToDate;
  s_sheet_device.update_percent = -1;
  push_sheet_device();
  run_frames(50);

  return failures;
}

}  // namespace

// Renders every fixture once and writes each to a BMP, then exits. Non
// interactive, so a screen can be checked against all the awkward states in one
// command with nobody watching the window.
int run_screenshot_pass(const std::string& output_directory) {
  int failures = 0;
  for (int screen = 0; screen < ui_screen_count(); ++screen) {
    ui_show_screen(screen);
    for (s_current = 0; s_current < s_fixture_paths.size(); ++s_current) {
      show_current_fixture();

      // Let the flow animation run briefly before capturing, so the shot shows
      // dots mid-travel rather than all bunched at the start of their path.
      for (int frame = 0; frame < 12; ++frame) {
        sim_backend_delay(20);
        lv_timer_handler();
      }
      lv_refr_now(nullptr);

      const std::string name = base_name(s_fixture_paths[s_current]);
      const std::string stem = name.substr(0, name.find_last_of('.'));
      char prefix[16];
      snprintf(prefix, sizeof(prefix), "s%d_", screen + 1);
      const std::string path = output_directory + "/" + prefix + stem + ".bmp";
      if (sim_backend_save_bmp(path.c_str())) {
        printf("[sim] wrote %s\n", path.c_str());
      } else {
        ++failures;
      }
    }
  }

  // The overlays are not screens, but they are full-screen layouts with a QR
  // code and three lines of text to fit on a circle — exactly the thing this
  // pass exists to check. Once each, over whichever fixture is loaded.
  for (int i = 1; i < OVERLAY_STATES; ++i) {
    cycle_overlay();
    lv_refr_now(nullptr);
    char path[512];
    snprintf(path, sizeof(path), "%s/overlay_%d.bmp", output_directory.c_str(), i);
    if (sim_backend_save_bmp(path)) {
      printf("[sim] wrote %s\n", path);
    } else {
      ++failures;
    }
  }
  cycle_overlay();

  // The day indicator and the toast, which the buttons drive on hardware and no
  // fixture can express — same reasoning as the overlays above.
  ui_show_screen(1);
  // Back to a real fixture first: the loop above leaves s_current one past the
  // end, and show_current_fixture() would index out of bounds, fail to read and
  // return before feeding the UI anything — leaving these captures showing
  // whatever was last drawn.
  s_current = 0;
  // Offset before the fixture, so show_current_fixture() feeds the day as well
  // as the live reading and the screens draw the loaded state, not the pending
  // one.
  ui_set_day_offset(-2);
  show_current_fixture();
  lv_refr_now(nullptr);
  {
    const std::string path = output_directory + "/day_back.bmp";
    if (sim_backend_save_bmp(path.c_str())) {
      printf("[sim] wrote %s\n", path.c_str());
    } else {
      ++failures;
    }
  }
  ui_set_day_loading();
  lv_refr_now(nullptr);
  {
    const std::string path = output_directory + "/day_loading.bmp";
    if (sim_backend_save_bmp(path.c_str())) {
      printf("[sim] wrote %s\n", path.c_str());
    } else {
      ++failures;
    }
  }

  // Back to a loaded day, then a message over it: a transient toast owns the top
  // slot while it lasts, so this has to come after the loading capture or it
  // would be the thing on screen for it.
  show_current_fixture();
  ui_toast("CYCLE OFF");
  lv_refr_now(nullptr);
  {
    const std::string path = output_directory + "/toast.bmp";
    if (sim_backend_save_bmp(path.c_str())) {
      printf("[sim] wrote %s\n", path.c_str());
    } else {
      ++failures;
    }
  }

  // The return to today, which runs on LVGL's inactivity clock — real time in the
  // sim, so the timeout is shortened to two seconds for the check rather than
  // waited out. Both halves are checked, because a return that fired at once would
  // pass the second on its own: still on the past day a second after the press
  // that put it there, and back on today once the timeout has passed.
  {
    ui_set_day_return(2);
    ui_set_day_offset(-1);
    lv_disp_trig_activity(nullptr);  // what the press that stepped back does
    const auto run_for = [](uint32_t ms) {
      for (uint32_t t = 0; t < ms; t += 20) {
        sim_backend_delay(20);
        lv_timer_handler();
      }
    };
    run_for(1000);
    const bool held = ui_day_offset() == -1;
    run_for(2500);
    const bool returned = ui_day_offset() == 0;
    printf("[sim] day return: %s while recent, %s after the timeout\n",
           held ? "held" : "RETURNED EARLY", returned ? "back to today" : "STILL ON A PAST DAY");
    if (!held || !returned) {
      ++failures;
    }
    ui_set_day_return(0);
  }

  ui_set_day_offset(0);
  ui_clear_day();

  failures += run_sheet_shots(output_directory);

  // How close LV_MEM_SIZE is to its limit, after a pass that has built every
  // screen and drawn every fixture through them — which is as hard as the pool
  // is ever worked. Reported here because the failure mode is not a clear "pool
  // full": it surfaces as whatever allocates next, and the sixth screen turned
  // it into an out-of-memory assert inside the anti-aliased corner mask.
  lv_mem_monitor_t mem;
  lv_mem_monitor(&mem);
  printf("[sim] LVGL pool: %u%% of %u KB used, %u%% fragmented, largest free block %u B\n",
         mem.used_pct, static_cast<unsigned>(mem.total_size / 1024), mem.frag_pct,
         static_cast<unsigned>(mem.free_biggest_size));

  return failures;
}

// src/sim/selftest.cpp — checks for the shared Modbus and history code, which
// needs no display and so runs before any of the LVGL setup below.
int run_selftest();
// src/sim/panel_selftest.cpp — the same for this repo's own code.
int run_panel_selftest();

int main(int argc, char** argv) {
  // sim [fixture-dir] [--shot output-dir] [--selftest] [--modbus] [--live] [--fixtures]
  //
  // Live data from a real server, in order of preference:
  //   - SIGEN_URL, the enrolment link (or a base URL plus SIGEN_TOKEN);
  //   - the link last pasted into the settings sheet (Data, Server);
  //   - nothing: the fixtures, as always.
  // --live insists on one of the first two; --fixtures ignores them. --shot
  // without --live is always the fixtures, so the screenshot pass stays the same
  // whatever is saved.
  std::string directory = "test/fixtures";
  bool want_live = false;
  bool no_live = false;
  std::string shot_directory;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--selftest") {
      const int shared = run_selftest();
      const int panel = run_panel_selftest();
      return shared != 0 || panel != 0 ? 1 : 0;
    } else if (argument == "--live") {
      want_live = true;
    } else if (argument == "--fixtures") {
      no_live = true;
    } else if (argument == "--modbus") {
      s_with_detailed_screens = false;
    } else if (argument == "--shot" && i + 1 < argc) {
      shot_directory = argv[++i];
    } else if (argument == "--tilt" && i + 1 < argc) {
      s_startup_tilt = static_cast<int16_t>(atoi(argv[++i]));
    } else {
      directory = argument;
    }
  }

  // Where live data would come from, if anywhere. Resolved before the window
  // opens, so a bad SIGEN_URL fails at once rather than behind a blank screen.
  std::string live_url;
  std::string live_token;
  if (!no_live && (want_live || shot_directory.empty())) {
    const char* url = getenv("SIGEN_URL");
    const char* token = getenv("SIGEN_TOKEN");
    const bool from_env = url != nullptr && *url != '\0';
    const std::string link = from_env ? url : live_link_load();
    const EnrolUrl parsed = enrol_url_parse(link.c_str());
    live_url = parsed.ok ? parsed.base : "";
    live_token = token != nullptr && *token != '\0' ? token : parsed.token;
    if (from_env && (!parsed.ok || live_token.empty())) {
      fprintf(stderr,
              "[sim] SIGEN_URL should be the kiosk enrolment link from the server's Admin\n"
              "      page, in quotes, or a base URL plus SIGEN_TOKEN:\n"
              "        SIGEN_URL='https://host/kiosk-enroll?token=...' "
              ".pio/build/sim/program\n");
      return 1;
    }
    if (live_url.empty() || live_token.empty()) {
      live_url.clear();
      if (want_live) {
        printf("[sim] no server saved: paste the enrolment link into the settings sheet\n"
               "      (Data, Server) and the sim switches to it\n");
      }
    } else if (!from_env) {
      printf("[sim] using the server saved in %s (--fixtures to ignore it)\n",
             live_link_path().c_str());
    }
  }

  s_fixture_paths = discover_fixtures(directory);
  if (s_fixture_paths.empty()) {
    fprintf(stderr,
            "[sim] no .json fixtures in '%s'\n"
            "      run from the repo root, or pass a directory as the first argument\n",
            directory.c_str());
    return 1;
  }
  printf("[sim] %zu fixtures in %s\n", s_fixture_paths.size(), directory.c_str());

  lv_init();
  if (!sim_backend_begin()) {
    return 1;
  }

  build_views();
  if (s_startup_tilt != 0) {
    ui_set_fine_rotation(s_startup_tilt);
  }

  if (!live_url.empty() && !start_live(live_url, live_token)) {
    return 1;
  }
  if (want_live && !s_live && shot_directory.empty()) {
    settings_sheet_open(SheetPage::Data, false);
  }

  if (!shot_directory.empty()) {
    if (want_live && !s_live) {
      fprintf(stderr, "[sim] --live --shot needs SIGEN_URL or a saved server\n");
      return 1;
    }
    s_shot_pass = !s_live;
    const int failures =
        s_live ? run_live_shots(shot_directory) : run_screenshot_pass(shot_directory);
    live_source_end();
    sim_backend_end();
    return failures == 0 ? 0 : 1;
  }

  if (!s_live) {
    show_current_fixture();
  }

  while (sim_backend_pump()) {
    // The keyboard types while a password, link or token is open, so a link can
    // be pasted with ⌘V; otherwise it drives the harness.
    const bool typing = settings_sheet_typing();
    sim_backend_set_text_mode(typing);
    while (const int key = sim_backend_take_key()) {
      if (typing) {
        settings_sheet_type_key(key);
      } else {
        handle_key(key);
      }
    }
    LiveUpdate update;
    if (s_live && live_source_take(&update)) {
      show_live(update);
    }
    lv_timer_handler();
    // Roughly LV_DISP_DEF_REFR_PERIOD; the renderer's vsync does the real pacing.
    sim_backend_delay(5);
  }

  live_source_end();
  sim_backend_end();
  return 0;
}
