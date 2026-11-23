// The parsed form of an EnViOT Display server's sensors, for the Environment
// screen: GET /api/live for the readings, GET /api/stats/day for each sensor's
// last-24-hour range.
//
// SigenStorPanel only. EnViOT Display is a separate server for battery-powered
// environment sensors (temperature, humidity, pressure), built like SigenStor
// Display: a kiosk code sent as a cookie (env_kiosk), read-only, and the panel
// renders what it is given. Its panel guide is docs/panel-api.md in that repo.
//
// Arduino-free, like snapshot.h and events.h, so the simulator compiles it and
// the selftest checks it. Unknown is null, never 0: a reading the server has no
// value for renders as "--".

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "snapshot.h"  // MaybeFloat

// Four cards fill the screen in a 2x2 grid. A fifth sensor is left off.
constexpr size_t ENV_MAX_DEVICES = 4;
// The rows beside the gauge after "Last 24 h": the first readings the server
// marks for the cards (on_card), other than temperature and battery.
constexpr size_t ENV_MAX_ROWS = 2;
constexpr size_t ENV_MAX_BANDS = 6;

// A reading's colour band, as the server names it in `state` and `bands`.
enum class EnvBand : uint8_t {
  None = 0,  // "" or a name this firmware does not know: drawn neutral
  Cold,
  Ok,
  Hot,
  Low,
  Fair,
};

struct EnvBandStop {
  float start = 0.0f;  // the band runs from here to the next stop's start
  EnvBand band = EnvBand::None;
};

// A gauge or bar's scale and bands, from a `readings[]` entry.
struct EnvScale {
  float min = 0.0f;
  float max = 1.0f;
  EnvBandStop bands[ENV_MAX_BANDS];
  uint8_t band_count = 0;
};

struct EnvRow {
  char label[20] = {};
  char unit[8] = {};
  uint8_t decimals = 0;
  MaybeFloat value;
};

struct EnvDevice {
  char id[24] = {};
  char name[24] = {};
  uint32_t colour = 0x8E8E93;  // the device's own colour, from "#rrggbb"
  int64_t last_seen = 0;       // unix seconds; 0 when never seen
  bool stale = false;          // nothing for 45 minutes, as the server judges it

  MaybeFloat temperature;
  EnvBand temperature_band = EnvBand::None;
  // Which /api/stats/day field is this device's temperature: "temperature_1" for
  // a reading with numbered sensors, else the reading's field or key.
  char temperature_field[24] = {};
  MaybeFloat day_min;
  MaybeFloat day_max;

  EnvRow rows[ENV_MAX_ROWS];
  uint8_t row_count = 0;

  MaybeFloat battery;  // volts

  // The first alert that is not "device_stale" (stale already shows). Empty: none.
  char alert[40] = {};
};

// How the last read of the server went, for what the screen says when it has
// nothing to show or what it shows has stopped updating.
enum class EnvStatus : uint8_t {
  Waiting = 0,  // set up, nothing read yet
  Ok,
  Refused,      // 401: no code, a wrong one, or the kiosk device was deleted
  Failed,       // unreachable, a timeout, or an answer that was not readings
};

struct EnvReadings {
  int64_t now = 0;  // the server's clock, unix seconds: "8 min ago" is worked from it
  char temperature_unit[8] = {};
  uint8_t temperature_decimals = 1;
  EnvScale temperature;
  EnvScale battery;
  EnvDevice devices[ENV_MAX_DEVICES];
  uint8_t count = 0;
  // Filled by the fetch, not the parser: whether this is a fresh read, and if
  // not, why. A failed read keeps the last good readings and says so.
  EnvStatus status = EnvStatus::Waiting;
  bool has_readings = false;  // at least one read has succeeded since start
};

// Fills `out` from a /api/live body. False if it is not that (no devices array);
// `out` is then left cleared. Hidden devices are skipped; past four, the rest are.
bool enviot_parse_live(const char* json, size_t length, EnvReadings* out);

// Sets each device's day_min/day_max from a /api/stats/day body, matched by id
// and temperature_field. A device the body does not cover keeps null. False if
// the body is not that.
bool enviot_parse_day(const char* json, size_t length, EnvReadings* readings);

// Copies day_min/day_max from `previous` into `next` for each device that is
// still there with the same field, so the range survives the minute-by-minute
// live reads between the five-minute day reads.
void enviot_carry_day(const EnvReadings& previous, EnvReadings* next);

// The band a value falls in: that of the last stop whose start is at or below
// it, else the first. None when there are no stops.
EnvBand enviot_band_at(const EnvScale& scale, float value);

// "just now" under 90 s, "8 min ago" under an hour, "3 h ago" under two days,
// else "5 days ago". `seconds` is now minus last seen.
void enviot_format_ago(int64_t seconds, char* out, size_t size);

// "No data for 52 min", "No data for 3 h", "No data for 2 days".
void enviot_format_stale(int64_t seconds, char* out, size_t size);

// A value to `decimals` places with thousands grouped: "1,023.1", "-0.8", "80.6".
// "--" when unknown.
void enviot_format_value(const MaybeFloat& value, uint8_t decimals, char* out, size_t size);
