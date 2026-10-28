// Checks for this repo's own Arduino-free code. The shared checks are in
// selftest.cpp, which is kept verbatim with SigenStorPuck; these are the panel's.

#include <stdio.h>
#include <string.h>

#include "ha_catalog.h"
#include "kiosk_code.h"
#include "presence_parse.h"

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

int run_panel_selftest() {
  s_checks = 0;
  s_failures = 0;
  test_presence_text();
  test_presence_frames();
  test_kiosk_code();
  test_ha_catalog();
  printf("\n%d panel checks, %d failed\n", s_checks, s_failures);
  return s_failures == 0 ? 0 : 1;
}
