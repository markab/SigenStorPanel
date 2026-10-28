#include "net.h"

#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>

#include "esp_system.h"
#include "esp_timer.h"

#include <algorithm>
#include <vector>

#include "board_config.h"
#include "settings.h"

namespace {

constexpr const char* NTP_PRIMARY = "pool.ntp.org";
constexpr const char* NTP_SECONDARY = "time.nist.gov";

// The saved networks get their own namespace, apart from the settings store, so
// "forget WiFi" and "reset settings" stay separate acts.
constexpr const char* SAVED_NAMESPACE = "panelwifi";

// Joining: long enough for a slow DHCP server, short enough that a wrong password
// is reported while the person who typed it is still looking.
constexpr uint32_t JOIN_TIMEOUT_MS = 20000;
constexpr uint32_t SCAN_TIMEOUT_MS = 15000;
// Lost the connection: let the stack's own reconnect try first, then look for
// any saved network, and keep looking at this interval.
constexpr uint32_t ROAM_AFTER_MS = 15000;
constexpr uint32_t ROAM_EVERY_MS = 30000;

struct Saved {
  String ssid;
  String password;
};

// One scanned network, with what a targeted join needs.
struct Seen {
  String ssid;
  int8_t rssi = 0;
  bool secure = false;
  uint8_t bssid[6] = {};
  int32_t channel = 0;
};

std::vector<Saved> s_saved;
std::vector<Seen> s_seen;

NetState s_state = NetState::Starting;
bool s_was_connected = false;
bool s_time_requested = false;
bool s_mdns_up = false;
// The name actually registered this boot. Changing the hostname takes effect on
// the next boot, and until then the screen should not claim the new name.
String s_active_hostname;

bool s_scanning = false;
uint32_t s_scan_started = 0;
// A scan was asked for while a join was in flight; it runs once the join ends.
bool s_scan_queued = false;
// The scan in flight should end in joining the strongest saved network.
bool s_roam_after_scan = false;

enum class Joining { None, Roam, User };
Joining s_joining = Joining::None;
uint32_t s_join_started = 0;
String s_join_ssid;
String s_join_password;
NetJoin s_join_state = NetJoin::Idle;
String s_join_error;
// From the last disconnect event: why a join failed.
volatile uint8_t s_disconnect_reason = 0;

uint32_t s_disconnected_since = 0;
uint32_t s_last_roam = 0;

void load_saved() {
  s_saved.clear();
  Preferences store;
  if (!store.begin(SAVED_NAMESPACE, true)) {
    return;  // never written: nothing saved yet
  }
  const uint8_t count = store.getUChar("n", 0);
  for (uint8_t i = 0; i < count && i < NET_MAX_SAVED; ++i) {
    const String key = String(i);
    Saved saved{store.getString(("s" + key).c_str(), ""), store.getString(("p" + key).c_str(), "")};
    if (!saved.ssid.isEmpty()) {
      s_saved.push_back(saved);
    }
  }
  store.end();
}

void store_saved() {
  Preferences store;
  if (!store.begin(SAVED_NAMESPACE, false)) {
    Serial.println("[net] could not open the saved-network store");
    return;
  }
  store.clear();
  store.putUChar("n", static_cast<uint8_t>(s_saved.size()));
  for (size_t i = 0; i < s_saved.size(); ++i) {
    const String key = String(i);
    store.putString(("s" + key).c_str(), s_saved[i].ssid);
    store.putString(("p" + key).c_str(), s_saved[i].password);
  }
  store.end();
}

// Moves a network to the front of the list, adding it if new and dropping the
// least recently used if that makes too many.
void remember(const String& ssid, const String& password) {
  if (ssid.isEmpty()) {
    return;
  }
  if (!s_saved.empty() && s_saved[0].ssid == ssid && s_saved[0].password == password) {
    return;
  }
  s_saved.erase(std::remove_if(s_saved.begin(), s_saved.end(),
                               [&ssid](const Saved& saved) { return saved.ssid == ssid; }),
                s_saved.end());
  s_saved.insert(s_saved.begin(), Saved{ssid, password});
  if (s_saved.size() > static_cast<size_t>(NET_MAX_SAVED)) {
    s_saved.resize(NET_MAX_SAVED);
  }
  store_saved();
  Serial.printf("[net] remembered %s (%u saved)\n", ssid.c_str(),
                static_cast<unsigned>(s_saved.size()));
}

const Saved* find_saved(const String& ssid) {
  for (const Saved& saved : s_saved) {
    if (saved.ssid == ssid) {
      return &saved;
    }
  }
  return nullptr;
}

void on_wifi_event(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    s_disconnect_reason = info.wifi_sta_disconnected.reason;
  }
}

String describe_failure(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_EXPIRE:
      return "Wrong password?";
    case WIFI_REASON_NO_AP_FOUND:
      return "Network not found. Is it in range?";
    case 0:
      return "No answer from the network";
    default:
      return "Could not connect (reason " + String(reason) + ")";
  }
}

void on_connected() {
  Serial.printf("[net] connected to %s, ip %s\n", WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str());

  // The name only: there is no web interface to advertise (docs/UX.md).
  if (!s_mdns_up && MDNS.begin(s_active_hostname.c_str())) {
    s_mdns_up = true;
    Serial.printf("[net] answering as %s.local (%s)\n", s_active_hostname.c_str(),
                  WiFi.localIP().toString().c_str());
  }

  if (!s_time_requested) {
    // UTC. The device shows relative times, not clock times, so no timezone is
    // needed — the clock exists to make TLS certificate validation possible.
    configTime(0, 0, NTP_PRIMARY, NTP_SECONDARY);
    s_time_requested = true;
    Serial.println("[net] NTP requested");
  }
}

void start_scan(bool then_roam) {
  s_roam_after_scan = s_roam_after_scan || then_roam;
  if (s_scanning) {
    return;
  }
  if (s_joining != Joining::None) {
    s_scan_queued = true;
    return;
  }
  if (WiFi.scanNetworks(true) == WIFI_SCAN_FAILED) {
    Serial.println("[net] scan would not start");
    return;
  }
  s_scanning = true;
  s_scan_started = millis();
}

void begin_join(Joining kind, const String& ssid, const String& password, const Seen* seen) {
  s_joining = kind;
  s_join_started = millis();
  s_join_ssid = ssid;
  s_join_password = password;
  s_disconnect_reason = 0;
  if (seen != nullptr) {
    // Straight to the strongest access point of that name rather than whichever
    // the stack finds first.
    WiFi.begin(ssid.c_str(), password.c_str(), seen->channel, seen->bssid);
  } else {
    WiFi.begin(ssid.c_str(), password.c_str());
  }
}

void start_roam() {
  s_last_roam = millis();
  if (s_saved.empty()) {
    return;
  }
  Serial.println("[net] looking for saved networks");
  start_scan(true);
}

void finish_scan() {
  const int16_t count = WiFi.scanComplete();
  if (count == WIFI_SCAN_RUNNING && millis() - s_scan_started < SCAN_TIMEOUT_MS) {
    return;
  }
  s_scanning = false;
  s_seen.clear();
  for (int16_t i = 0; i < count; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) {
      continue;  // hidden networks have no name to show or match
    }
    const int8_t rssi = static_cast<int8_t>(WiFi.RSSI(i));
    auto existing = std::find_if(s_seen.begin(), s_seen.end(),
                                 [&ssid](const Seen& seen) { return seen.ssid == ssid; });
    if (existing != s_seen.end()) {
      if (existing->rssi >= rssi) {
        continue;
      }
      s_seen.erase(existing);
    }
    Seen seen;
    seen.ssid = ssid;
    seen.rssi = rssi;
    seen.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    memcpy(seen.bssid, WiFi.BSSID(i), sizeof(seen.bssid));
    seen.channel = WiFi.channel(i);
    s_seen.push_back(seen);
  }
  WiFi.scanDelete();
  std::sort(s_seen.begin(), s_seen.end(),
            [](const Seen& a, const Seen& b) { return a.rssi > b.rssi; });
  if (s_seen.size() > static_cast<size_t>(NET_MAX_SCAN)) {
    s_seen.resize(NET_MAX_SCAN);
  }
  Serial.printf("[net] scan found %u networks\n", static_cast<unsigned>(s_seen.size()));

  if (!s_roam_after_scan) {
    return;
  }
  s_roam_after_scan = false;
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }
  // s_seen is strongest first, so the first saved name found is the one to join.
  for (const Seen& seen : s_seen) {
    if (const Saved* saved = find_saved(seen.ssid)) {
      Serial.printf("[net] joining %s (%d dBm)\n", saved->ssid.c_str(), seen.rssi);
      begin_join(Joining::Roam, saved->ssid, saved->password, &seen);
      return;
    }
  }
  Serial.println("[net] no saved network in range");
}

void finish_join() {
  const bool connected = WiFi.status() == WL_CONNECTED && WiFi.SSID() == s_join_ssid;
  const bool timed_out = millis() - s_join_started > JOIN_TIMEOUT_MS;
  const bool refused = WiFi.status() == WL_CONNECT_FAILED;
  if (!connected && !timed_out && !refused) {
    return;
  }

  const Joining kind = s_joining;
  s_joining = Joining::None;
  if (connected) {
    remember(s_join_ssid, s_join_password);
    if (kind == Joining::User) {
      s_join_state = NetJoin::Joined;
    }
  } else {
    const String why = describe_failure(s_disconnect_reason);
    Serial.printf("[net] could not join %s: %s\n", s_join_ssid.c_str(), why.c_str());
    if (kind == Joining::User) {
      s_join_state = NetJoin::Failed;
      s_join_error = why;
      // Back to whatever saved network is around, rather than left with none.
      WiFi.disconnect(false);
      start_roam();
    }
  }
  s_join_password = "";
  if (s_scan_queued) {
    s_scan_queued = false;
    start_scan(false);
  }
}

}  // namespace

void net_begin() {
  // The P4 has no radio: WiFi is the C6 over SDIO (esp-hosted), and this board's
  // data lines are not the core's compiled-in default. Must come before the
  // first WiFi call, which is what brings esp-hosted up. See docs/HARDWARE.md.
  if (!WiFi.setPins(PUCK_SDIO_CLK, PUCK_SDIO_CMD, PUCK_SDIO_D0, PUCK_SDIO_D1, PUCK_SDIO_D2,
                    PUCK_SDIO_D3, PUCK_SDIO_C6_RESET)) {
    Serial.println("[net] could not set the C6's SDIO pins; WiFi will use the core's defaults");
  }
  WiFi.onEvent(on_wifi_event);
  // The saved list is the one store of credentials. Left on, the stack keeps its
  // own copy of the last network, which "forget" would not touch.
  WiFi.persistent(false);
  // esp-hosted resets the C6 and brings up SDIO here, so this is where a broken
  // link to the radio stops the boot. Timed so the log says how long it took.
  const uint32_t radio_start = millis();
  Serial.println("[net] starting the C6 (esp-hosted)");
  WiFi.mode(WIFI_STA);
  Serial.printf("[net] C6 up in %lu ms\n", static_cast<unsigned long>(millis() - radio_start));

  // Read once and held: mDNS and the DHCP hostname are both registered a single
  // time, when the network comes up, so a change made later cannot apply until
  // the next boot however it is stored.
  s_active_hostname = settings_get().hostname;
  WiFi.setHostname(s_active_hostname.c_str());
  // Keep the radio out of power save: at a 5 s poll the latency it adds is not
  // worth the milliamps on a mains-powered display.
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  load_saved();
  Serial.printf("[net] hostname %s, %u saved networks\n", s_active_hostname.c_str(),
                static_cast<unsigned>(s_saved.size()));
  s_disconnected_since = millis();
  if (s_saved.empty()) {
    s_state = NetState::NoNetworks;
    // So the picker has a list the moment it opens.
    start_scan(false);
  } else {
    s_state = NetState::Connecting;
    start_roam();
  }
}

void net_loop() {
  if (s_scanning) {
    finish_scan();
  }
  if (s_joining != Joining::None) {
    finish_join();
  }

  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !s_was_connected) {
    s_state = NetState::Connected;
    s_disconnected_since = 0;
    on_connected();
  } else if (!connected && s_was_connected) {
    Serial.println("[net] connection lost");
    s_disconnected_since = millis();
  }
  s_was_connected = connected;

  if (!connected) {
    s_state = s_saved.empty() ? NetState::NoNetworks : NetState::Connecting;
    const uint32_t now = millis();
    if (s_joining == Joining::None && !s_scanning && !s_saved.empty() &&
        now - s_disconnected_since > ROAM_AFTER_MS && now - s_last_roam > ROAM_EVERY_MS) {
      start_roam();
    }
  }
}

NetState net_state() {
  return s_state;
}

String net_hostname() {
  return s_mdns_up ? s_active_hostname + ".local" : String();
}

String net_name() {
  return s_active_hostname;
}

String net_ip() {
  return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String();
}

bool net_time_synced() {
  // 1 Jan 2024: anything earlier means NTP has not answered yet.
  return time(nullptr) > 1704067200;
}

int net_saved_count() {
  return static_cast<int>(s_saved.size());
}

String net_saved_ssid(int index) {
  return index >= 0 && index < net_saved_count() ? s_saved[index].ssid : String();
}

void net_forget(const String& ssid) {
  const size_t before = s_saved.size();
  s_saved.erase(std::remove_if(s_saved.begin(), s_saved.end(),
                               [&ssid](const Saved& saved) { return saved.ssid == ssid; }),
                s_saved.end());
  if (s_saved.size() == before) {
    return;
  }
  store_saved();
  Serial.printf("[net] forgot %s\n", ssid.c_str());
  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid) {
    WiFi.disconnect(false);
    // Look for another saved network straight away.
    s_disconnected_since = millis() - ROAM_AFTER_MS;
    s_last_roam = millis() - ROAM_EVERY_MS;
  }
}

void net_forget_wifi() {
  Serial.println("[net] forgetting every saved network");
  s_saved.clear();
  store_saved();
  WiFi.disconnect(false, true);
  net_restart("networks forgotten");
}

void net_restart(const char* why) {
  Serial.printf("[net] restarting: %s. Shutting the C6 down first\n", why);
  Serial.flush();

  // The backstop, armed before anything that could block on the C6.
  constexpr uint64_t FORCE_AFTER_US = 3000 * 1000;
  esp_timer_create_args_t args = {};
  args.callback = [](void*) { esp_restart(); };
  args.name = "force_restart";
  esp_timer_handle_t timer = nullptr;
  if (esp_timer_create(&args, &timer) == ESP_OK) {
    esp_timer_start_once(timer, FORCE_AFTER_US);
  }

  WiFi.disconnect(true);
  // Deinitialises WiFi and then esp-hosted, which ends the SDIO session.
  WiFi.mode(WIFI_OFF);
  Serial.println("[net] C6 shut down");
  Serial.flush();
  delay(100);
  ESP.restart();
  for (;;) {
  }
}

void net_scan_start() {
  start_scan(false);
}

bool net_scan_running() {
  return s_scanning || s_scan_queued;
}

int net_scan_results(NetScanResult* out, int max) {
  int count = 0;
  for (const Seen& seen : s_seen) {
    if (count >= max) {
      break;
    }
    out[count].ssid = seen.ssid;
    out[count].rssi = seen.rssi;
    out[count].secure = seen.secure;
    ++count;
  }
  return count;
}

bool net_scan_rssi(const String& ssid, int8_t* rssi) {
  for (const Seen& seen : s_seen) {
    if (seen.ssid == ssid) {
      *rssi = seen.rssi;
      return true;
    }
  }
  return false;
}

void net_join(const String& ssid, const String& password) {
  if (s_scanning) {
    WiFi.scanDelete();
    s_scanning = false;
  }
  s_join_state = NetJoin::Joining;
  s_join_error = "";
  // No password typed for a saved network: it was picked to switch to, and its
  // password is already here.
  String use = password;
  if (use.isEmpty()) {
    if (const Saved* saved = find_saved(ssid)) {
      use = saved->password;
    }
  }
  Serial.printf("[net] joining %s, picked on the panel\n", ssid.c_str());
  WiFi.disconnect(false);
  begin_join(Joining::User, ssid, use, nullptr);
}

NetJoin net_join_state() {
  return s_join_state;
}

String net_join_error() {
  return s_join_error;
}
