// The on-device settings: a sheet that slides up over the data screens, with
// its pages side by side. Swipe up on any data screen to open it, left and right
// between its pages, down (or Done) to close it. See docs/UX.md.
//
// This repo's own, not shared with SigenStorPuck. It knows nothing about NVS,
// WiFi or the poll task: the owner (main.cpp on the device, src/sim/main.cpp in
// the simulator) pushes state in with the settings_sheet_set_*() calls and acts
// on the SheetActions it is handed. That keeps every page renderable in the sim,
// and keeps network work off the LVGL thread: an action only asks for it.

#pragma once

#include <lvgl.h>
#include <stdint.h>

#include "data_source.h"
#include "ha_catalog.h"
#include "solar_source.h"

static constexpr int SHEET_MAX_NETWORKS = 5;
static constexpr int SHEET_MAX_SCAN = 16;
static constexpr int SHEET_MAX_MODBUS = 4;
static constexpr int SHEET_MAX_ARRAYS = 4;
static constexpr int SHEET_TEXT = 64;

enum class SheetPage : uint8_t {
  Display = 0,
  Screens,
  Wifi,
  Data,
  Solar,
  Device,
  Count,
};

struct SheetDisplay {
  uint8_t brightness = 120;
  uint8_t dim_brightness = 60;
  uint32_t dim_after_s = 30;  // 0: never dim
  bool screen_off_set = false;
  uint16_t screen_off_start_min = 22 * 60 + 30;
  uint16_t screen_off_end_min = 7 * 60;
  bool flipped = false;  // upside down; applies on the next boot
  // With a presence sensor: backlight off after nobody is seen for this long.
  uint32_t presence_off_after_s = 300;  // 0: never
};

// The optional mmWave room sensor, as last heard.
struct SheetPresence {
  bool connected = false;
  bool present = false;
  uint32_t empty_for_s = 0;  // how long nobody has been seen
  bool has_distance = false;
  uint16_t distance_cm = 0;
};

struct SheetScreens {
  uint8_t visible = 0xFF;  // PuckScreen bits; applies on the next boot
  uint8_t rotate = 0xFF;   // PuckScreen bits the auto-cycle visits
  // Screens this data source can draw at all. The rest are shown greyed out.
  uint8_t available = 0xFF;
  bool rotate_enabled = false;
  uint32_t rotate_s = 15;
};

struct SheetNetwork {
  char ssid[33] = {};
  int8_t rssi = 0;  // meaningful only when in_range
  bool in_range = false;
};

struct SheetWifi {
  bool connected = false;
  char ssid[33] = {};
  int8_t rssi = 0;
  uint8_t channel = 0;
  char ip[16] = {};
  SheetNetwork saved[SHEET_MAX_NETWORKS];
  uint8_t saved_count = 0;
};

enum class SheetDeviceKind : uint8_t {
  Inverter,
  InverterWithDc,  // an inverter with a DC (vehicle) charger
  AcCharger,
};

// One device in a Modbus plant, as addressed in the Sigen app. 0 is an empty slot.
struct SheetModbusDevice {
  uint8_t slave_id = 0;
  SheetDeviceKind kind = SheetDeviceKind::Inverter;
};

struct SheetData {
  DataSource source = DataSource::Server;
  // Set separately, so either can be there without the other. Empty: not set.
  char server_host[SHEET_TEXT] = {};
  char server_code[12] = {};  // masked, "...c8nf": never the whole code
  char modbus_host[40] = {};
  uint8_t modbus_plant_address = 247;
  SheetModbusDevice modbus_devices[SHEET_MAX_MODBUS];
  bool ha_set = false;
  char ha_host[SHEET_TEXT] = {};
  uint32_t poll_s = 5;
};

// What a scan found, for the network picker.
struct SheetScanResult {
  char ssid[33] = {};
  int8_t rssi = 0;
  bool secure = false;
  bool saved = false;  // joined without asking for the password again
};

struct SheetScan {
  bool running = false;
  SheetScanResult results[SHEET_MAX_SCAN];
  uint8_t count = 0;
};

enum class SheetJoin : uint8_t {
  Idle,
  Joining,
  Joined,
  Failed,
};

struct SheetJoinStatus {
  SheetJoin state = SheetJoin::Idle;
  char message[SHEET_TEXT] = {};  // why it failed
};

// One roof face. kwp 0 is an empty slot.
struct SheetArray {
  float kwp = 0.0f;
  float tilt_deg = 35.0f;
  float azimuth_deg = 180.0f;  // clockwise from north; 180 is due south
};

// The panel's own solar forecast, for the sources with no server to supply one.
struct SheetSolar {
  bool location_set = false;
  float latitude = 0.0f;
  float longitude = 0.0f;
  float system_loss = 0.85f;
  float inverter_cap_kw = 0.0f;  // 0: no limit
  SheetArray arrays[SHEET_MAX_ARRAYS];
  // Home Assistant only: whether the forecast is off, the panel's, or HA's own.
  SolarForecastSource ha_source = SolarForecastSource::Disabled;
};

enum class SheetEntry : uint8_t {
  EnrolmentLink,
  ServerAddress,
  KioskCode,
  HaAddress,
  HaToken,
  DeviceName,
};

// Which Home Assistant sensor supplies each role, indexed like HA_ENTITIES in
// home_assistant.h. Empty: that role is not read.
struct SheetHaSensors {
  char entity[HA_ENTITY_COUNT][HA_ENTITY_ID_MAX] = {};
};

enum class SheetHaListState : uint8_t {
  Idle,
  Loading,
  Ready,
  Failed,
};

// The sensors Home Assistant offers, for the picker. `items` belongs to the
// caller and must stay valid until the next settings_sheet_set_ha_list().
struct SheetHaList {
  SheetHaListState state = SheetHaListState::Idle;
  char message[48] = {};  // why it failed, or that it listed nothing
  const HaCandidate* items = nullptr;
  size_t count = 0;
};

enum class SheetUpdate : uint8_t {
  Idle,
  Checking,
  UpToDate,
  Available,
  Installing,
  Failed,
};

struct SheetDevice {
  char firmware[16] = {};
  char host[40] = {};  // mDNS name, without ".local"
  // A new name is saved but not yet in use: it is registered once, at boot.
  bool host_pending = false;
  char ip[16] = {};
  char mac[18] = {};  // the WiFi MAC, for a router's DHCP reservation or allow list
  uint32_t uptime_s = 0;
  uint32_t internal_free = 0;
  uint32_t internal_largest = 0;  // the number TLS cares about
  uint32_t psram_free = 0;
  uint32_t psram_total = 0;
  int8_t rssi = 0;
  bool wifi_connected = false;
  bool ever_read = false;
  uint32_t last_read_age_s = 0;
  uint32_t failures_in_a_row = 0;
  char last_result[32] = {};
  SheetUpdate update = SheetUpdate::Idle;
  bool auto_check = false;       // check for updates without being asked
  int8_t update_percent = -1;    // while installing; -1 when not known yet
  char update_version[16] = {};  // the newer version, when there is one
  char update_detail[SHEET_TEXT] = {};
};

// What the owner does when someone changes something. Any of these may be null.
// None of them may block: anything that touches the network is a request for
// the poll task to pick up (CLAUDE.md, "Never do network work on the LVGL thread").
struct SheetActions {
  // While a brightness slider is being dragged: set the backlight, save nothing.
  void (*preview_brightness)(uint8_t level) = nullptr;
  // On release, and on every other Display-page change: save.
  void (*set_display)(const SheetDisplay& display) = nullptr;
  void (*set_screens)(const SheetScreens& screens) = nullptr;
  void (*forget_network)(const char* ssid) = nullptr;
  // The picker asks for a fresh list; results arrive by settings_sheet_set_scan().
  void (*start_scan)() = nullptr;
  // An empty password means "the saved one" for a saved network, or none for an
  // open one. Progress arrives by settings_sheet_set_join().
  void (*join_network)(const char* ssid, const char* password) = nullptr;
  void (*set_source)(DataSource source) = nullptr;
  void (*forget_server)() = nullptr;
  // Typed on the panel's keyboard. Return false, with a reason in `error`, to
  // keep the entry screen up (a link with no token in it, say).
  bool (*set_enrolment_link)(const char* link, char* error, size_t error_size) = nullptr;
  // The link's two halves, as the Admin page also shows them. Each keeps the other.
  bool (*set_server_address)(const char* url, char* error, size_t error_size) = nullptr;
  bool (*set_kiosk_code)(const char* code, char* error, size_t error_size) = nullptr;
  // Asks for Home Assistant's sensor list; it arrives through
  // settings_sheet_set_ha_list(). Must not block.
  void (*load_ha_list)() = nullptr;
  // One role's sensor, by HA_ENTITIES index; an empty entity_id stops reading it.
  void (*set_ha_sensor)(int role, const char* entity_id) = nullptr;
  // The panel's name on the network. False, with a reason, if nothing usable is
  // left once it is cleaned up for DNS.
  bool (*set_device_name)(const char* name, char* error, size_t error_size) = nullptr;
  bool (*set_ha_address)(const char* url, char* error, size_t error_size) = nullptr;
  bool (*set_ha_token)(const char* token, char* error, size_t error_size) = nullptr;
  void (*set_modbus_devices)(uint8_t plant_address, const SheetModbusDevice* devices,
                             int count) = nullptr;
  void (*set_solar)(const SheetSolar& solar) = nullptr;
  void (*set_ha_forecast_source)(SolarForecastSource source) = nullptr;
  void (*set_modbus_host)(const char* host) = nullptr;
  void (*set_poll_interval)(uint32_t seconds) = nullptr;
  void (*set_auto_check)(bool enabled) = nullptr;
  void (*check_update)() = nullptr;
  void (*install_update)() = nullptr;
  void (*restart)() = nullptr;
};

// Builds the sheet, hidden, as the topmost child of `parent` (the active
// screen), and listens there for the swipe up that opens it. Call after
// ui_create() so it sits above the data screens and their overlays.
void settings_sheet_create(lv_obj_t* parent, const SheetActions& actions);

void settings_sheet_set_display(const SheetDisplay& display);
void settings_sheet_set_presence(const SheetPresence& presence);
void settings_sheet_set_screens(const SheetScreens& screens);
void settings_sheet_set_wifi(const SheetWifi& wifi);
void settings_sheet_set_data(const SheetData& data);
void settings_sheet_set_device(const SheetDevice& device);
void settings_sheet_set_scan(const SheetScan& scan);
void settings_sheet_set_solar(const SheetSolar& solar);
void settings_sheet_set_ha_sensors(const SheetHaSensors& sensors);
void settings_sheet_set_ha_list(const SheetHaList& list);
void settings_sheet_set_join(const SheetJoinStatus& join);

void settings_sheet_open(SheetPage page, bool animate);
void settings_sheet_close(bool animate);
bool settings_sheet_is_open();
SheetPage settings_sheet_page();

// The Modbus address keypad, opened as if the field had been tapped. For the
// sim's screenshot pass.
void settings_sheet_show_keypad(bool show);

// Opens the sheet on the WiFi page with the network picker up and a scan
// started. What the panel shows when it has no saved network to join, and what
// "Add a network" does.
void settings_sheet_show_picker();

// The password screen for a network, as if it had been picked. For the sim.
void settings_sheet_show_password(const char* ssid, bool secure);

// Opens the keyboard for a link, address, code or token, as its button would. For the sim.
void settings_sheet_show_entry(SheetEntry entry);
// The Modbus device editor, and one solar array's editor. For the sim.
void settings_sheet_show_devices();
// The Home Assistant sensor list, and the picker for one role. For the sim.
void settings_sheet_show_ha_sensors();
void settings_sheet_show_ha_picker(int role);
void settings_sheet_show_array(int index);

// Types into the password field and presses Join, as fingers would. For the sim.
void settings_sheet_type_password(const char* text);
void settings_sheet_press_join();

// The sim's physical keyboard: whether a password, link, code or token is being
// typed, and one key into it. Printable ASCII appends, '\b' deletes, '\r' presses
// Join or Save.
bool settings_sheet_typing();
void settings_sheet_type_key(int key);
