// Checks for this repo's own Arduino-free code. The shared checks are in
// selftest.cpp, which is kept verbatim with SigenStorPuck; these are the panel's.

#include <stdio.h>
#include <string.h>

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

}  // namespace

int run_panel_selftest() {
  s_checks = 0;
  s_failures = 0;
  test_presence_text();
  test_presence_frames();
  printf("\n%d panel checks, %d failed\n", s_checks, s_failures);
  return s_failures == 0 ? 0 : 1;
}
