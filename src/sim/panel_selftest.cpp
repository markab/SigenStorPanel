// Checks for this repo's own Arduino-free code. The shared checks are in
// selftest.cpp, which is kept verbatim with SigenStorPuck; these are the panel's.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enviot.h"
#include "ha_catalog.h"
#include "kiosk_code.h"
#include "presence_parse.h"
#include "sound_events.h"

namespace {

int s_checks = 0;
int s_failures = 0;

void check(bool ok, const char* what) {
  ++s_checks;
  if (!ok) {
    ++s_failures;
    printf("  FAIL: %s\n", what);
  }
}

// Feeds a whole buffer; returns how many readings came out and the last one.
int feed(PresenceParser& parser, const uint8_t* bytes, size_t length, PresenceReading* last) {
  int readings = 0;
  for (size_t i = 0; i < length; ++i) {
    PresenceReading reading;
    if (parser.feed(bytes[i], &reading)) {
      *last = reading;
      ++readings;
    }
  }
  return readings;
}

int feed_text(PresenceParser& parser, const char* text, PresenceReading* last) {
  return feed(parser, reinterpret_cast<const uint8_t*>(text), strlen(text), last);
}

void test_presence_text() {
  printf("presence sensor, text mode\n");
  PresenceParser parser;
  PresenceReading reading;
  check(feed_text(parser, "ON\r\n", &reading) == 1 && reading.present && !reading.has_distance,
        "ON is presence");
  check(feed_text(parser, "Range 145\r\n", &reading) == 1 && reading.present &&
            reading.has_distance && reading.distance == 145,
        "Range carries a distance");
  check(feed_text(parser, "OFF\r\n", &reading) == 1 && !reading.present && !reading.has_distance,
        "OFF is absence and drops the distance");
  check(feed_text(parser, "Range \r\nnonsense\r\n", &reading) == 0, "malformed lines are ignored");
  check(feed_text(parser, "ON\n", &reading) == 1 && reading.present, "a bare LF ends a line");
  check(feed_text(parser, "THIS LINE IS FAR TOO LONG TO BE ANYTHING\r\nOFF\r\n", &reading) == 1 &&
            !reading.present,
        "an overlong line is dropped, the next one still read");
}

void test_presence_frames() {
  printf("presence sensor, report frames\n");
  PresenceParser parser;
  PresenceReading reading;
  // Presence, 230 cm, sixteen gate energies, tail.
  uint8_t frame[4 + 2 + 3 + 32 + 4] = {0xF4, 0xF3, 0xF2, 0xF1, 35, 0, 1, 230, 0};
  memcpy(frame + sizeof(frame) - 4, "\xF8\xF7\xF6\xF5", 4);
  check(feed(parser, frame, sizeof(frame), &reading) == 1 && reading.present &&
            reading.distance == 230,
        "a report frame is read");
  frame[6] = 0;
  check(feed(parser, frame, sizeof(frame), &reading) == 1 && !reading.present,
        "presence 0 is absence");
  frame[sizeof(frame) - 1] = 0x00;
  check(feed(parser, frame, sizeof(frame), &reading) == 0, "a frame with a bad tail is dropped");
  frame[sizeof(frame) - 1] = 0xF5;
  // Text either side of a frame, as when the mode changes mid-stream.
  check(feed_text(parser, "ON\r\n", &reading) == 1 && reading.present,
        "text after a frame is still read");
}

// True when `typed` tidies to `expected`, or is refused when that is null.
bool tidies_to(const char* typed, const char* expected) {
  char out[KIOSK_CODE_SIZE] = "junk";
  const bool ok = kiosk_code_tidy(typed, out, sizeof(out));
  if (expected == nullptr) {
    return !ok && out[0] == '\0';
  }
  return ok && strcmp(out, expected) == 0;
}

void test_kiosk_code() {
  printf("kiosk code\n");
  const char* const tidy = "k7mq4xrp9tbzh2wdc8nf";
  check(tidies_to("k7mq-4xrp-9tbz-h2wd-c8nf", tidy), "the dashes come out");
  check(tidies_to(tidy, tidy), "an undashed code is kept");
  check(tidies_to("  K7MQ 4XRP\t9TBZ-H2WD c8nf\n", tidy), "case and whitespace go");
  check(tidies_to("k7mq-4xrp-9tbz-h2wd-c8nO", "k7mq4xrp9tbzh2wdc8n0"), "o is read as 0");
  check(tidies_to("k7mq-4xrp-9tbz-h2wd-cIlf", "k7mq4xrp9tbzh2wdc11f"), "i and l are read as 1");
  check(tidies_to("k7mq-4xrp-9tbz-h2wd-c8n", nullptr), "19 characters is refused");
  check(tidies_to("k7mq-4xrp-9tbz-h2wd-c8nfx", nullptr), "21 characters is refused");
  check(tidies_to("k7mq-4xrp-9tbz-h2wd-c8nu", nullptr), "u is in no code");
  check(tidies_to("k7mq-4xrp-9tbz-h2wd-c8n=", nullptr), "punctuation is refused");
  check(tidies_to("", nullptr) && tidies_to(" \t", nullptr) && tidies_to(nullptr, nullptr),
        "nothing is refused");

  // Before v0.38: 43 characters of base64url, matched exactly as issued.
  const char* const old = "Qm9vdHN0cmFwVG9rZW4x_Ym9vdHN0cmFwLXRva2Vu-x";
  check(strlen(old) == 43 && tidies_to(old, old), "an older token keeps its case");
  check(tidies_to(" Qm9vdHN0cmFwVG9rZW4x_Ym9vdHN0cmFwLXRva2Vu-x\r\n", old),
        "an older token loses its whitespace");
  check(tidies_to("Qm9vdHN0cmFwVG9rZW4x_Ym9vdHN0cmFwLXRva2Vu-", nullptr),
        "42 characters is refused");
  check(tidies_to("Qm9vdHN0cmFwVG9rZW4x+Ym9vdHN0cmFwLXRva2Vu-x", nullptr),
        "plain base64 is refused");

  char small[20];
  check(!kiosk_code_tidy(tidy, small, sizeof(small)) && small[0] == '\0',
        "a buffer too small is refused");
}

}  // namespace

// The rendered template's lines, and which roles each sensor can fill.
void test_ha_catalog() {
  const char text[] =
      "sensor.sigen_plant_pv_power\tkW\tSigen Plant PV Power\t3.42\n"
      "sensor.sigen_plant_battery_state_of_charge\t%\tBattery SOC\t78\r\n"
      "sensor.daily_import\tkWh\tDaily import\t6.8\n"
      "sensor.cell_temp\t\xC2\xB0" "C\tCell temperature\t24.5\n"
      "binary_sensor.off_grid\t\tOff grid\toff\n"
      "not an entity\tkW\tBroken\t1\n"
      "sensor.only_three\tkW\tfields\n"
      "sensor.load\tW\tLoad\t1200";  // the last line need not end in a newline
  HaCandidate list[8];
  const size_t count = ha_catalog_parse(text, strlen(text), list, 8);
  check(count == 6, "catalog: six good lines out of eight");
  check(strcmp(list[0].entity_id, "sensor.sigen_plant_pv_power") == 0 &&
            strcmp(list[0].unit, "kW") == 0 && strcmp(list[0].state, "3.42") == 0,
        "catalog: fields split on tabs");
  check(strcmp(list[1].state, "78") == 0, "catalog: a CR before the LF is dropped");
  check(strcmp(list[5].entity_id, "sensor.load") == 0 && strcmp(list[5].state, "1200") == 0,
        "catalog: a last line without a newline is kept");
  check(ha_catalog_suits(HaValueKind::Power, list[0]) &&
            ha_catalog_suits(HaValueKind::Power, list[5]) &&
            !ha_catalog_suits(HaValueKind::Energy, list[0]),
        "catalog: W and kW suit power, not energy");
  check(ha_catalog_suits(HaValueKind::Percent, list[1]) &&
            ha_catalog_suits(HaValueKind::Energy, list[2]) &&
            ha_catalog_suits(HaValueKind::Temperature, list[3]),
        "catalog: percent, energy and temperature by unit");
  check(ha_catalog_suits(HaValueKind::Boolean, list[4]) &&
            !ha_catalog_suits(HaValueKind::Boolean, list[0]),
        "catalog: only a unitless sensor suits an on/off role");
  check(ha_catalog_parse(text, strlen(text), list, 2) == 2, "catalog: stops at the limit");
}

namespace {

Snapshot battery_at(float soc) {
  Snapshot s;
  s.valid = true;
  s.battery.soc_pct.known = true;
  s.battery.soc_pct.value = soc;
  return s;
}

Snapshot ev_at(float kw) {
  Snapshot s;
  s.valid = true;
  s.power.ev.known = true;
  s.power.ev.value = kw;
  return s;
}

constexpr uint8_t FULL = sound_bit(SoundCue::BatteryFull);
constexpr uint8_t RESERVE = sound_bit(SoundCue::BatteryReserve);
constexpr uint8_t EV_START = sound_bit(SoundCue::EvStart);
constexpr uint8_t EV_STOP = sound_bit(SoundCue::EvStop);

void test_sound_battery() {
  printf("sounds, battery\n");
  SoundEvents events;
  check(events.update(battery_at(100), 10, 0) == 0, "booting beside a full battery is quiet");
  check(events.update(battery_at(100), 10, 5000) == 0, "and stays quiet while it is full");
  check(events.update(battery_at(98), 10, 10000) == 0 && events.update(battery_at(100), 10, 15000) == 0,
        "a dip to 98% does not re-arm full");
  check(events.update(battery_at(96), 10, 20000) == 0, "below 97% re-arms full");
  check(events.update(battery_at(100), 10, 25000) == FULL, "full chimes on reaching 100%");
  check(events.update(battery_at(100), 10, 30000) == 0, "once");

  SoundEvents low;
  check(low.update(battery_at(40), 10, 0) == 0, "first reading only learns");
  check(low.update(battery_at(11), 10, 5000) == 0, "above the reserve is quiet");
  check(low.update(battery_at(10), 10, 10000) == RESERVE, "reaching the reserve chimes");
  check(low.update(battery_at(9), 10, 15000) == 0 && low.update(battery_at(11), 10, 20000) == 0 &&
            low.update(battery_at(10), 10, 25000) == 0,
        "hovering around the reserve does not chime again");
  check(low.update(battery_at(13), 10, 30000) == 0 && low.update(battery_at(10), 10, 35000) == RESERVE,
        "3% above re-arms it");
  check(low.update(battery_at(8), 0, 40000) == 0, "a reserve of 0 never chimes");

  SoundEvents unknown;
  Snapshot blank;
  blank.valid = true;
  check(unknown.update(battery_at(50), 10, 0) == 0 && unknown.update(blank, 10, 5000) == 0 &&
            unknown.update(battery_at(10), 10, 10000) == RESERVE,
        "an unknown reading changes nothing");
}

void test_sound_ev() {
  printf("sounds, car charging\n");
  SoundEvents events;
  check(events.update(ev_at(0), 10, 0) == 0, "first reading only learns");
  check(events.update(ev_at(7.2f), 10, 5000) == 0, "one reading is not a start");
  check(events.update(ev_at(0.1f), 10, 10000) == 0 && events.update(ev_at(7.2f), 10, 15000) == 0,
        "a blip resets the wait");
  check(events.update(ev_at(7.2f), 10, 40000) == 0, "29 s in is not yet a start");
  check(events.update(ev_at(7.1f), 10, 45000) == EV_START, "30 s of charging is a start");
  check(events.update(ev_at(0.0f), 10, 50000) == 0 && events.update(ev_at(0.0f), 10, 100000) == 0,
        "a pause under a minute is not a stop");
  check(events.update(ev_at(0.0f), 10, 110000) == EV_STOP, "a minute without charging is a stop");

  SoundEvents charging;
  check(charging.update(ev_at(7.0f), 10, 0) == 0 && charging.update(ev_at(0.0f), 10, 5000) == 0 &&
            charging.update(ev_at(0.0f), 10, 65000) == EV_STOP,
        "booting mid-charge: no start, but the stop is heard");
}


// A fixture file, whole, or nullptr. The caller frees it.
char* read_fixture(const char* path, size_t* length) {
  FILE* file = fopen(path, "rb");
  if (file == nullptr) {
    return nullptr;
  }
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  char* text = static_cast<char*>(malloc(static_cast<size_t>(size) + 1));
  *length = fread(text, 1, static_cast<size_t>(size), file);
  text[*length] = '\0';
  fclose(file);
  return text;
}

bool near(const MaybeFloat& value, float expected) {
  return value.known && value.value > expected - 0.001f && value.value < expected + 0.001f;
}

bool parse_live_text(const char* text, EnvReadings* out) {
  return enviot_parse_live(text, strlen(text), out);
}

void test_enviot_live() {
  size_t length = 0;
  char* live = read_fixture("test/fixtures/enviot_live.json", &length);
  check(live != nullptr, "enviot_live.json is there (run from the repo root)");
  if (live == nullptr) {
    return;
  }
  static EnvReadings r;
  check(enviot_parse_live(live, length, &r), "the live fixture parses");
  free(live);
  check(r.count == 4, "four shown, the hidden fifth left off");
  check(r.now == 1795458600, "the server's clock");
  check(r.temperature.min == -10.0f && r.temperature.max == 40.0f &&
            r.temperature.band_count == 3,
        "the temperature scale and its three bands");
  check(near({true, r.battery.min}, 3.2f) && near({true, r.battery.max}, 4.2f) &&
            r.battery.band_count == 3,
        "the battery scale");
  check(strcmp(r.temperature_unit, "\xC2\xB0" "C") == 0 && r.temperature_decimals == 1,
        "the temperature unit");
  const EnvDevice& gh = r.devices[0];
  check(strcmp(gh.id, "10000001") == 0 && strcmp(gh.name, "Greenhouse") == 0 &&
            gh.colour == 0x199E70,
        "the first device's id, name and colour");
  check(near(gh.temperature, 1.5f) && gh.temperature_band == EnvBand::Cold,
        "its temperature and the server's band for it");
  check(strcmp(gh.temperature_field, "temperature_1") == 0, "numbered sensors: the main one's field");
  check(near(gh.battery, 3.78f), "its battery voltage");
  check(gh.last_seen == 1795458600 - 480 && !gh.stale, "when it was last seen");
  check(gh.row_count == 2 && strcmp(gh.rows[0].label, "Humidity") == 0 &&
            strcmp(gh.rows[1].label, "Pressure") == 0,
        "two rows, in the server's order: Light is the third card reading and is left off");
  check(near(gh.rows[0].value, 80.6f) && strcmp(gh.rows[0].unit, "%RH") == 0 &&
            gh.rows[0].decimals == 1,
        "a row's value, unit and places");
  check(!gh.day_min.known && !gh.day_max.known, "no range until the day stats are read");
  check(gh.alert[0] == '\0', "no alert");
  check(strcmp(r.devices[3].name, "Upstairs") == 0, "devices in the server's order");

  char* day = read_fixture("test/fixtures/enviot_day.json", &length);
  check(day != nullptr && enviot_parse_day(day, length, &r), "the day fixture parses");
  free(day);
  check(near(gh.day_min, -0.8f) && near(gh.day_max, 9.4f),
        "the range is the main sensor's, not sensor 2's");
  check(near(r.devices[2].day_min, 19.6f) && near(r.devices[2].day_max, 22.9f),
        "every device gets its own range");

  static EnvReadings next;
  check(parse_live_text(R"({"now":10,"readings":[{"key":"temperature","sensors":true,"main_sensor":1}],
      "devices":[{"id":"10000003","readings":{"temperature":{"value":20}}},
                 {"id":"10000001","readings":{"temperature":{"value":2}}}]})", &next) &&
            next.count == 2,
        "a minimal body parses");
  enviot_carry_day(r, &next);
  check(near(next.devices[0].day_max, 22.9f) && near(next.devices[1].day_min, -0.8f),
        "the range carries over by id between day reads");
  check(!next.devices[0].battery.known && next.devices[0].row_count == 0 &&
            next.devices[0].temperature_band == EnvBand::None,
        "what is missing is unknown, not zero");
}

void test_enviot_edges() {
  static EnvReadings r;
  check(!parse_live_text("{\"detail\":\"Not authenticated\"}", &r) && r.count == 0,
        "a 401 body is not readings");
  check(!parse_live_text("not json", &r), "nor is junk");
  check(parse_live_text(R"({"now":100,"readings":[],"devices":[
      {"id":"a","name":"Shed","color":"red","stale":true,"last_seen":1.5,
       "readings":{"temperature":{"value":null,"state":"warm"}},
       "alerts":[{"kind":"device_stale","title":"Offline"},{"kind":"rule","title":"Frost risk"}]}]})", &r),
        "a stale device with alerts parses");
  const EnvDevice& d = r.devices[0];
  check(d.stale && d.last_seen == 1, "stale, and when it was last seen");
  check(!d.temperature.known, "a null temperature is unknown");
  check(d.temperature_band == EnvBand::None, "a band this firmware does not know is neutral");
  check(d.colour == 0x8E8E93, "a colour that is not #rrggbb is grey");
  check(strcmp(d.alert, "Frost risk") == 0, "the stale alert is skipped for the next one");
  check(d.temperature_field[0] == '\0', "no temperature definition, no field to match");

  EnvScale scale;
  scale.band_count = 3;
  scale.bands[0] = {-10.0f, EnvBand::Cold};
  scale.bands[1] = {3.0f, EnvBand::Ok};
  scale.bands[2] = {28.0f, EnvBand::Hot};
  check(enviot_band_at(scale, -20.0f) == EnvBand::Cold, "below the first stop: the first band");
  check(enviot_band_at(scale, 2.99f) == EnvBand::Cold, "just under a stop");
  check(enviot_band_at(scale, 3.0f) == EnvBand::Ok, "on a stop: that stop's band");
  check(enviot_band_at(scale, 50.0f) == EnvBand::Hot, "above the last");
  check(enviot_band_at(EnvScale(), 1.0f) == EnvBand::None, "no bands");

  char text[32];
  enviot_format_ago(89, text, sizeof(text));
  check(strcmp(text, "just now") == 0, "under 90 s is just now");
  enviot_format_ago(480, text, sizeof(text));
  check(strcmp(text, "8 min ago") == 0, "minutes");
  enviot_format_ago(3 * 3600 + 59, text, sizeof(text));
  check(strcmp(text, "3 h ago") == 0, "hours");
  enviot_format_ago(5 * 86400, text, sizeof(text));
  check(strcmp(text, "5 days ago") == 0, "days");
  enviot_format_stale(52 * 60, text, sizeof(text));
  check(strcmp(text, "No data for 52 min") == 0, "stale in minutes");
  enviot_format_stale(2 * 3600, text, sizeof(text));
  check(strcmp(text, "No data for 2 h") == 0, "stale in hours");

  enviot_format_value({true, 1023.1f}, 1, text, sizeof(text));
  check(strcmp(text, "1,023.1") == 0, "thousands grouped");
  enviot_format_value({true, -0.8f}, 1, text, sizeof(text));
  check(strcmp(text, "-0.8") == 0, "below zero");
  enviot_format_value({true, -0.04f}, 1, text, sizeof(text));
  check(strcmp(text, "0.0") == 0, "no minus on a rounded zero");
  enviot_format_value({true, -1234567.0f}, 0, text, sizeof(text));
  check(strcmp(text, "-1,234,567") == 0, "millions, below zero");
  enviot_format_value({true, 80.6f}, 1, text, sizeof(text));
  check(strcmp(text, "80.6") == 0, "no comma under a thousand");
  enviot_format_value(MaybeFloat(), 1, text, sizeof(text));
  check(strcmp(text, "--") == 0, "unknown");
}

}  // namespace

int run_panel_selftest() {
  s_checks = 0;
  s_failures = 0;
  test_presence_text();
  test_presence_frames();
  test_kiosk_code();
  test_ha_catalog();
  test_sound_battery();
  test_sound_ev();
  test_enviot_live();
  test_enviot_edges();
  printf("\n%d panel checks, %d failed\n", s_checks, s_failures);
  return s_failures == 0 ? 0 : 1;
}
