// SigenStorPanel — device entry point.
//
// Adapted from SigenStorPuck (see UPSTREAM.md). Brings up the hardware, joins
// WiFi, and polls the data source on a task pinned to core 0 while this loop runs
// LVGL on core 1. Everything is set on the panel's own settings sheet: there is
// no web interface on the device.
// Screens never learn where a reading came from: everything arrives through one
// ui_update() call (PLAN.md §B3).

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#include "esp_heap_caps.h"

#include "board_config.h"
#include "device/buttons.h"
#include "device/net.h"
#include "device/poller.h"
#include "device/power.h"
#include "device/presence.h"
#include "device/settings.h"
#include "device/settings_bridge.h"
#include "device/updater.h"
#include "display.h"
#include "screen_window.h"
#include "snapshot.h"
#include "touch.h"
#include "ui_panel/banner.h"
#include "ui_panel/ui_live.h"
#include "ui/theme.h"
#include "ui/ui.h"

namespace {

// Poller/network state is observed at a humanly prompt cadence. Snapshot and day
// generations below keep this from becoming a 4 Hz rewrite of unchanged LVGL
// objects; lv_timer_handler() still runs every loop for touch and animation.
constexpr uint32_t UI_REFRESH_MS = 250;

// The PMIC is read over I2C, so at a human rate rather than per frame.
constexpr uint32_t POWER_POLL_MS = 5000;

// The banner last shown, so one that has not changed is not rewritten into LVGL
// four times a second, and the log only speaks on a change.
char s_banner_text[112] = {};
BannerTone s_banner_tone = BannerTone::Info;

void set_banner(const char* text, BannerTone tone = BannerTone::Info,
                SheetPage page = SheetPage::Data) {
  const char* shown = text == nullptr ? "" : text;
  if (strncmp(s_banner_text, shown, sizeof(s_banner_text)) == 0 && tone == s_banner_tone) {
    return;
  }
  snprintf(s_banner_text, sizeof(s_banner_text), "%s", shown);
  s_banner_tone = tone;
  ui_set_banner(text, tone, page);

  // Logged because it is the one part of the UI you cannot ask the device about,
  // and exactly what you want to know when someone says the panel shows a warning.
  if (text == nullptr) {
    Serial.println("[ui] banner cleared");
  } else {
    Serial.printf("[ui] banner: %s\n", s_banner_text);
  }
}

// Decides what, if anything, the banner says. Ordered by what the viewer can
// actually do about it: no network, then nothing configured, then a rejected
// credential, then simply waiting. Every one of them is fixed in the settings
// sheet, and the banner opens the page that does it.
void refresh_banner(bool have_snapshot, const PollStatus& status) {
  // The boot splash goes the first time there is anything to say.
  static bool splash_up = true;
  if (splash_up) {
    splash_up = false;
    ui_set_overlay(nullptr, nullptr, nullptr);
  }

  // Still fed to the UI: the settings screen shows the address whether or not a
  // banner is up, and both are empty until WiFi is joined.
  ui_set_address(net_hostname().c_str(), net_ip().c_str());

  switch (net_state()) {
    case NetState::NoNetworks:
      // The network picker opens over this by itself (settings_bridge.cpp); this
      // is what is left if someone closes it.
      set_banner("No WiFi. Tap to choose a network", BannerTone::Attention, SheetPage::Wifi);
      return;
    case NetState::Starting:
    case NetState::Connecting:
      set_banner("Connecting to WiFi...", BannerTone::Info, SheetPage::Wifi);
      return;
    case NetState::Connected:
      break;
  }

  const DataSource source = settings_get().source;

  // Different words per source: there is no server code to enter when the panel
  // talks to the plant directly, and telling someone to find one would send them
  // looking for something that does not exist.
  if (!settings_is_provisioned()) {
    if (source == DataSource::Modbus) {
      set_banner("Plant not set up. Tap to enter its IP address", BannerTone::Attention);
    } else if (source == DataSource::HomeAssistant) {
      set_banner("Home Assistant not set up. Tap to enter its address and token",
                 BannerTone::Attention);
    } else {
      set_banner("Server not set up. Tap to enter the server and code", BannerTone::Attention);
    }
    return;
  }

  // A rejected credential is not a network fault and will never fix itself, so it
  // says so rather than sitting on "waiting for data" forever.
  if (status.last_result == FetchResult::Unauthorised) {
    set_banner(source == DataSource::HomeAssistant
                   ? "Home Assistant rejected the token. Tap to enter a new one"
                   : "The server rejected this panel's code. Tap to enter a new one",
               BannerTone::Problem);
    return;
  }

  if (!have_snapshot) {
    if (status.last_result == FetchResult::ClockUnset) {
      set_banner("Setting the clock...", BannerTone::Info, SheetPage::Wifi);
    } else if (status.last_result == FetchResult::NoNetwork) {
      // The poll that ran before WiFi came up. WiFi is connected by the time this
      // is reached, so the next poll will say more; red here was a false alarm
      // on every boot.
      set_banner("Waiting for data...", BannerTone::Info);
    } else if (status.consecutive_failures > 0) {
      // Naming what is unreachable, because on the Modbus path "the server" is
      // not a thing that exists and the first place to look is the Sigen app's
      // Modbus whitelist.
      static char text[112];
      snprintf(text, sizeof(text), "Cannot reach %s (%s). Tap to check the settings",
               source == DataSource::Modbus          ? "the plant"
               : source == DataSource::HomeAssistant ? "Home Assistant"
                                                     : "the server",
               fetch_result_name(status.last_result));
      set_banner(text, BannerTone::Problem);
    } else {
      set_banner("Waiting for data...", BannerTone::Info);
    }
    return;
  }

  // Data is flowing: get out of the way. A stale reading is reported by the plant
  // node on screen 1, not by a banner.
  set_banner(nullptr);
}

// Dims after a period with no touch. LVGL's own inactivity timer is used rather
// than tracking touch here, because it already accounts for every input the
// indev driver reports — so a tap wakes the screen for free.
//
// Dimmed, never blanked: a status display you have to wake up before you can read
// it has stopped being a status display. It also matters on AMOLED, where a static
// image at full brightness for months is what causes burn-in.
void apply_idle_dim() {
  const Settings& settings = settings_get();
  if (settings.dim_after_s == 0) {
    return;
  }
  const bool should_dim = lv_disp_get_inactive_time(nullptr) > settings.dim_after_s * 1000;

  static bool dimmed = false;
  if (should_dim != dimmed) {
    dimmed = should_dim;
    display_set_brightness(dimmed ? settings.dim_brightness : settings.brightness);
  }
}

// Blanks the panel outright between two local times, and brings it back for a
// button (buttons.cpp swallows the gesture that does the waking) or for the end
// of the window.
//
// Local, not UTC. The device's own clock is UTC — net.cpp asks NTP for no
// timezone — so the offset has to come from the reading, which is the same one
// the day charts are anchored to. Until one arrives the window is read against
// UTC, which in Britain is right for half the year and an hour out for the other
// half.
void apply_screen_schedule() {
  const Settings& settings = settings_get();
  const time_t now = time(nullptr);

  // Without a real clock every time of day is a guess, and the guess is 1970.
  // Blanking on it would leave a device that has lost NTP dark with no way to
  // tell why. The same 2024 floor sigen_api.cpp uses to judge certificates.
  const bool can_tell_the_time = now > 1704067200;

  bool overnight = false;
  if (settings.screen_off_set && can_tell_the_time) {
    const uint16_t minute =
        screen_local_minute(static_cast<uint32_t>(now), poller_tz_offset_min());
    overnight = screen_window_contains(settings.screen_off_start_min, settings.screen_off_end_min,
                                       minute);
  }

  // The room sensor, when there is one answering: nobody seen for the set time.
  // A sensor that has gone quiet is not believed, so a fault cannot leave the
  // panel dark.
  const PresenceStatus room = presence_status();
  const uint32_t empty_after_ms = settings.presence_off_after_s * 1000;
  const bool empty_room = settings.presence_off_after_s > 0 && room.connected && !room.present &&
                          millis() - room.last_present_ms >= empty_after_ms;

  if (!overnight && !empty_room) {
    display_set_sleep(false);
    return;
  }
  if (display_asleep()) {
    return;
  }
  // Recently touched or pressed: leave it up. LVGL's inactivity timer is the same
  // one the idle dim reads, so a swipe through the screens holds it awake as
  // naturally as it holds off the dim. For the room sensor the hold is the whole
  // timeout: someone tapping at the panel is there, whatever the radar thinks.
  const uint32_t hold_ms = overnight ? PUCK_SCREEN_WAKE_S * 1000 : empty_after_ms;
  if (lv_disp_get_inactive_time(nullptr) < hold_ms) {
    return;
  }
  display_set_sleep(true);
}

void refresh_ui() {
  static bool first = true;
  static uint32_t seen_snapshot_generation = 0;
  static uint32_t seen_day_generation = 0;
  static int seen_day_offset = 0;

  Snapshot snapshot;
  uint32_t snapshot_generation = 0;
  const bool have = poller_snapshot(&snapshot, &snapshot_generation);
  const PollStatus status = poller_status();
  const bool snapshot_changed =
      have && (first || snapshot_generation != seen_snapshot_generation);

  // The viewed day before the live reading, so one pass never draws the new day's
  // chart against the old day's figures.
  Snapshot day;
  uint32_t day_generation = 0;
  const bool have_day = poller_day_snapshot(&day, &day_generation);
  const int day_offset = ui_day_offset();
  if (first || day_generation != seen_day_generation ||
      day_offset != seen_day_offset) {
    if (have_day) {
      ui_update_day(day);
    } else if (day_offset != 0) {
      // Asked for but not here yet. Dashes and LOADING, not the live reading:
      // falling back to live flashed today's figures under a past date on every
      // step between days.
      ui_set_day_loading();
    } else {
      ui_clear_day();
    }
  }
  if (snapshot_changed) {
    ui_update(snapshot);
  }

  seen_snapshot_generation = snapshot_generation;
  seen_day_generation = day_generation;
  seen_day_offset = day_offset;
  first = false;
  refresh_banner(have, status);
}

// The Puck's own battery, shown only when it is actually running on one. Normally
// this is a mains-powered display and the indicator would be permanent clutter
// telling you nothing.
void refresh_device_battery() {
  const PowerStatus power = power_status();
  const bool on_battery = power.pmic_ok && power.battery_present && !power.usb_present;
  ui_set_device_battery(on_battery, power.percent, power.charging);
}

// One line per boot stage: time since start, free internal RAM, its largest
// block, and free DMA-capable RAM. esp-hosted takes its SDIO buffers from the
// last of those, so a stage that eats into it shows up here before WiFi fails.
void log_stage(const char* stage) {
  Serial.printf("[boot] %-12s %6lu ms  internal %u B (largest %u)  dma %u B\n", stage,
                static_cast<unsigned long>(millis()),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)));
}

void log_boot_banner() {
  Serial.printf("\nSigenStorPanel %s\n", PUCK_FW_VERSION);
  Serial.printf("[chip] %s rev %d, %d MHz, flash %u MB, PSRAM %u MB, free heap %u B\n",
                ESP.getChipModel(), ESP.getChipRevision(), getCpuFrequencyMhz(),
                static_cast<unsigned>(ESP.getFlashChipSize() / (1024 * 1024)),
                static_cast<unsigned>(ESP.getPsramSize() / (1024 * 1024)),
                static_cast<unsigned>(ESP.getFreeHeap()));
}

}  // namespace

void setup() {
  Serial.begin(PUCK_SERIAL_BAUD);
  // UART0 through the CH341, so there is no USB-CDC enumeration to wait for.
  // A short pause still lets a monitor that reset the board on open catch the
  // banner.
  delay(200);
  log_boot_banner();

  // main() owns the shared I2C bus. Only the GT911 is on it today.
  Wire.begin(PUCK_I2C_SDA, PUCK_I2C_SCL, PUCK_I2C_HZ);
  touch_scan_i2c();

  // Before the panel: the rotation is applied as it is brought up.
  settings_begin();
  log_stage("settings");

  lv_init();
  if (!display_begin(settings_get().orientation)) {
    Serial.println("[boot] display bring-up failed — halting");
    while (true) {
      delay(1000);
    }
  }
  touch_begin();
  touch_set_orientation(settings_get().orientation);
  log_stage("display");

  power_begin();
  buttons_begin();
  presence_begin();

  // Which screens exist is settled here and not revisited: the tileview's tiles
  // are built once, so the masks and the data source both apply on the next boot
  // — same as `orientation`.
  UiConfig ui_config;
  const SourceCapabilities source_capabilities =
      data_source_capabilities(settings_get().source);
  ui_config.with_detailed_screens = source_capabilities.detailed_flows;
  ui_config.visible = settings_get().screens_visible;
  ui_config.rotate = settings_get().screens_rotate;
  {
    const Settings& s = settings_get();
    const auto mapped = [&s](HaEntity entity) {
      return !s.ha_entities[static_cast<size_t>(entity)].isEmpty();
    };
    ui_config.solar_figures = solar_figures_supplied(
        s.source, s.ha_solar_forecast_source, mapped(HaEntity::ForecastRemaining),
        mapped(HaEntity::ForecastPercentage), mapped(HaEntity::ForecastPeak));
  }
  ui_create(lv_scr_act(), ui_config);
  // The on-screen day arrows do what the buttons do.
  ui_set_day_step(buttons_step_day);
  log_stage("ui");
  // Settings slide up over the screens, so built after them.
  settings_bridge_begin();
  // Sources without a dated API have no history to step into, so the buttons
  // refuse rather than moving an indicator over figures that will never change.
  ui_set_day_stepping(source_capabilities.historical_days);
  display_set_brightness(settings_get().brightness);
  // A named boot screen rather than a bare word: on a device that takes a couple
  // of seconds to find WiFi, this is the only proof it is alive.
  ui_set_overlay("SigenStorPanel", nullptr, PUCK_FW_VERSION);
  Serial.println("[ui] splash: SigenStorPanel " PUCK_FW_VERSION);
  // Draw the first frame before WiFi so the screen is alive while the radio comes
  // up, rather than black for a couple of seconds.
  lv_timer_handler();
  log_stage("first frame");

  // Neither applied while the drawing side is blocked: skewing touch to match a
  // rotation that is not on screen would put every tap 2.5 degrees out for no
  // visible reason.
  ui_set_rotate_interval(settings_get().rotate_s);
  ui_set_rotate_enabled(settings_get().rotate_enabled);
  // No sweep band: IPS does not burn in (CLAUDE.md). The setting is still
  // stored, but nothing here acts on it.
  ui_set_day_return(PUCK_DAY_RETURN_S);

  net_begin();
  log_stage("net");
  poller_begin();
  log_stage("poller");
  updater_begin();

  Serial.println("[boot] ready");
}

void loop() {
  // The PMIC's key edges before the gesture recogniser reads them, so a press
  // is seen on the pass it arrives rather than the next one.
  power_loop();
  buttons_loop();
  presence_loop();

  // Someone walking in counts as activity, the same as a touch: it lifts the
  // dim, and the screen they see is at full brightness. Only the overnight
  // window ignores them, by design: that is for the hours the room is not used.
  static bool was_present = false;
  const PresenceStatus room = presence_status();
  if (room.connected && room.present && !was_present) {
    lv_disp_trig_activity(nullptr);
  }
  was_present = room.connected && room.present;

  // Any button press wakes a dimmed screen. buttons_loop() has already told LVGL
  // the device is active; this is the brightness half of the same thought.
  if (lv_disp_get_inactive_time(nullptr) < 100) {
    display_set_brightness(settings_get().brightness);
  }

  net_loop();

  settings_bridge_loop();

  static uint32_t last_refresh = 0;
  if (millis() - last_refresh >= UI_REFRESH_MS) {
    last_refresh = millis();
    refresh_ui();
  }

  static uint32_t last_power = 0;
  if (millis() - last_power >= POWER_POLL_MS) {
    last_power = millis();
    refresh_device_battery();
  }

  apply_idle_dim();
  apply_screen_schedule();
  lv_timer_handler();
  delay(5);
}
