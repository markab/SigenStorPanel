#include "presence.h"

#include <Arduino.h>

#include "board_config.h"
#include "presence_parse.h"

namespace {

PresenceParser s_parser;
PresenceStatus s_status;
uint32_t s_last_reading_ms = 0;
bool s_ever = false;

}  // namespace

void presence_begin() {
  Serial1.begin(PUCK_PRESENCE_BAUD, SERIAL_8N1, PUCK_PRESENCE_RX, PUCK_PRESENCE_TX);
  s_status.last_present_ms = millis();
  Serial.printf("[presence] listening on GPIO%d for an mmWave sensor\n", PUCK_PRESENCE_RX);
}

void presence_loop() {
  // Bounded, so a sensor stuck spewing cannot hold up the frame.
  for (int i = 0; i < 256 && Serial1.available() > 0; ++i) {
    PresenceReading reading;
    if (!s_parser.feed(static_cast<uint8_t>(Serial1.read()), &reading)) {
      continue;
    }
    const uint32_t now = millis();
    if (!s_ever) {
      s_ever = true;
      Serial.println("[presence] sensor found");
    }
    if (reading.present != s_status.present) {
      Serial.printf("[presence] %s\n", reading.present ? "someone here" : "nobody here");
    }
    s_last_reading_ms = now;
    s_status.present = reading.present;
    s_status.has_distance = reading.has_distance;
    s_status.distance = reading.distance;
    if (reading.present) {
      s_status.last_present_ms = now;
    }
  }
  const bool connected = s_ever && millis() - s_last_reading_ms < PUCK_PRESENCE_LOST_MS;
  if (connected != s_status.connected && s_ever) {
    Serial.printf("[presence] sensor %s\n", connected ? "back" : "silent, ignoring it");
  }
  s_status.connected = connected;
}

PresenceStatus presence_status() {
  return s_status;
}
