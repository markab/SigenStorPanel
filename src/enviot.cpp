#include "enviot.h"

#include <ArduinoJson.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ARDUINO)
#include "esp_heap_caps.h"
#endif

namespace {

// /api/live for four sensors parses to a document of 15-20 KB. That belongs in
// PSRAM: internal RAM is what TLS needs, and this runs straight after a handshake.
class PsramAllocator : public ArduinoJson::Allocator {
 public:
  void* allocate(size_t size) override {
#if defined(ARDUINO)
    void* block = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return block != nullptr ? block : malloc(size);
#else
    return malloc(size);
#endif
  }
  void deallocate(void* block) override {
    free(block);
  }
  void* reallocate(void* block, size_t size) override {
#if defined(ARDUINO)
    void* moved = heap_caps_realloc(block, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return moved != nullptr ? moved : realloc(block, size);
#else
    return realloc(block, size);
#endif
  }
};

PsramAllocator s_allocator;

void copy_text(char* out, size_t size, JsonVariantConst value) {
  const char* text = value.as<const char*>();
  snprintf(out, size, "%s", text != nullptr ? text : "");
}

MaybeFloat maybe(JsonVariantConst value) {
  MaybeFloat out;
  if (value.is<float>() || value.is<int>()) {
    const float v = value.as<float>();
    if (isfinite(v)) {
      out.known = true;
      out.value = v;
    }
  }
  return out;
}

EnvBand band_named(const char* name) {
  if (name == nullptr) return EnvBand::None;
  if (strcmp(name, "cold") == 0) return EnvBand::Cold;
  if (strcmp(name, "ok") == 0) return EnvBand::Ok;
  if (strcmp(name, "hot") == 0) return EnvBand::Hot;
  if (strcmp(name, "low") == 0) return EnvBand::Low;
  if (strcmp(name, "fair") == 0) return EnvBand::Fair;
  return EnvBand::None;
}

uint32_t parse_colour(const char* hex) {
  if (hex == nullptr || hex[0] != '#' || strlen(hex) != 7) {
    return 0x8E8E93;
  }
  char* end = nullptr;
  const unsigned long value = strtoul(hex + 1, &end, 16);
  return end != nullptr && *end == '\0' ? static_cast<uint32_t>(value) : 0x8E8E93;
}

void parse_scale(JsonObjectConst entry, EnvScale* scale) {
  scale->min = entry["gauge_min"] | 0.0f;
  scale->max = entry["gauge_max"] | 1.0f;
  if (!(scale->max > scale->min)) {
    scale->max = scale->min + 1.0f;
  }
  scale->band_count = 0;
  for (JsonVariantConst stop : entry["bands"].as<JsonArrayConst>()) {
    if (scale->band_count >= ENV_MAX_BANDS) {
      break;
    }
    JsonArrayConst pair = stop.as<JsonArrayConst>();
    if (pair.size() < 2 || !(pair[0].is<float>() || pair[0].is<int>())) {
      continue;
    }
    EnvBandStop& out = scale->bands[scale->band_count++];
    out.start = pair[0].as<float>();
    out.band = band_named(pair[1].as<const char*>());
  }
}

// What a reading definition says about where its value is stored, for matching
// /api/stats/day: numbered sensors are stored per sensor ("temperature_1"), the
// rest under their field, or their key when they have no field of their own.
void stats_field(JsonObjectConst entry, JsonObjectConst device_reading, char* out, size_t size) {
  const char* key = entry["key"] | "";
  if (entry["sensors"] | false) {
    int main = device_reading["main"] | 0;
    if (main <= 0) {
      main = entry["main_sensor"] | 1;
    }
    snprintf(out, size, "%s_%d", key, main);
    return;
  }
  const char* field = entry["field"] | "";
  snprintf(out, size, "%s", field[0] != '\0' ? field : key);
}

int64_t read_seconds(JsonVariantConst value) {
  if (value.is<float>() || value.is<double>() || value.is<int64_t>()) {
    const double v = value.as<double>();
    return isfinite(v) && v > 0 ? static_cast<int64_t>(v) : 0;
  }
  return 0;
}

}  // namespace

bool enviot_parse_live(const char* json, size_t length, EnvReadings* out) {
  if (json == nullptr || out == nullptr) {
    return false;
  }
  *out = EnvReadings();

  // Only what the screen draws. The reading definitions carry a dozen fields the
  // web app uses and the panel does not; the alert list repeats per device.
  JsonDocument filter;
  filter["now"] = true;
  filter["signal"] = true;
  JsonObject reading = filter["readings"].add<JsonObject>();
  for (const char* field : {"key", "label", "unit", "decimals", "gauge_min", "gauge_max", "bands",
                            "on_card", "sensors", "main_sensor", "field"}) {
    reading[field] = true;
  }
  JsonObject device = filter["devices"].add<JsonObject>();
  for (const char* field : {"id", "name", "color", "hidden", "last_seen", "stale", "readings"}) {
    device[field] = true;
  }
  JsonObject alert = device["alerts"].add<JsonObject>();
  alert["kind"] = true;
  alert["title"] = true;

  JsonDocument doc(&s_allocator);
  if (deserializeJson(doc, json, length, DeserializationOption::Filter(filter)) !=
      DeserializationError::Ok) {
    return false;
  }
  JsonArrayConst devices = doc["devices"].as<JsonArrayConst>();
  JsonArrayConst readings = doc["readings"].as<JsonArrayConst>();
  if (devices.isNull() || readings.isNull()) {
    return false;
  }

  out->now = read_seconds(doc["now"]);
  const char* signal = doc["signal"] | "";

  JsonObjectConst temperature_entry;
  for (JsonObjectConst entry : readings) {
    const char* key = entry["key"] | "";
    if (strcmp(key, "temperature") == 0) {
      temperature_entry = entry;
      parse_scale(entry, &out->temperature);
      copy_text(out->temperature_unit, sizeof(out->temperature_unit), entry["unit"]);
      out->temperature_decimals = entry["decimals"] | 1;
    } else if (strcmp(key, "battery") == 0) {
      parse_scale(entry, &out->battery);
    }
  }

  for (JsonObjectConst source : devices) {
    if (out->count >= ENV_MAX_DEVICES) {
      break;
    }
    if (source["hidden"] | false) {
      continue;
    }
    EnvDevice& d = out->devices[out->count++];
    copy_text(d.id, sizeof(d.id), source["id"]);
    copy_text(d.name, sizeof(d.name), source["name"]);
    d.colour = parse_colour(source["color"].as<const char*>());
    d.last_seen = read_seconds(source["last_seen"]);
    d.stale = source["stale"] | false;

    JsonObjectConst values = source["readings"].as<JsonObjectConst>();
    JsonObjectConst temperature = values["temperature"].as<JsonObjectConst>();
    d.temperature = maybe(temperature["value"]);
    d.temperature_band = band_named(temperature["state"].as<const char*>());
    if (!temperature_entry.isNull()) {
      stats_field(temperature_entry, temperature, d.temperature_field,
                  sizeof(d.temperature_field));
    }
    d.battery = maybe(values["battery"]["value"]);

    // The rows: readings the server puts on its cards, in its order, that this
    // device has. Temperature has the gauge, battery the bar, and signal strength
    // is not shown on the panel at all.
    for (JsonObjectConst entry : readings) {
      if (d.row_count >= ENV_MAX_ROWS) {
        break;
      }
      const char* key = entry["key"] | "";
      if (!(entry["on_card"] | false) || strcmp(key, "temperature") == 0 ||
          strcmp(key, "battery") == 0 || (signal[0] != '\0' && strcmp(key, signal) == 0)) {
        continue;
      }
      JsonObjectConst value = values[key].as<JsonObjectConst>();
      if (value.isNull()) {
        continue;
      }
      EnvRow& row = d.rows[d.row_count++];
      copy_text(row.label, sizeof(row.label), entry["label"]);
      copy_text(row.unit, sizeof(row.unit), entry["unit"]);
      row.decimals = entry["decimals"] | 0;
      row.value = maybe(value["value"]);
    }

    for (JsonObjectConst a : source["alerts"].as<JsonArrayConst>()) {
      const char* kind = a["kind"] | "";
      if (strcmp(kind, "device_stale") == 0) {
        continue;
      }
      copy_text(d.alert, sizeof(d.alert), a["title"]);
      if (d.alert[0] != '\0') {
        break;
      }
    }
  }
  return true;
}

bool enviot_parse_day(const char* json, size_t length, EnvReadings* readings) {
  if (json == nullptr || readings == nullptr) {
    return false;
  }
  JsonDocument doc(&s_allocator);
  if (deserializeJson(doc, json, length) != DeserializationError::Ok) {
    return false;
  }
  JsonObjectConst devices = doc["devices"].as<JsonObjectConst>();
  if (devices.isNull()) {
    return false;
  }
  for (uint8_t i = 0; i < readings->count; ++i) {
    EnvDevice& d = readings->devices[i];
    d.day_min = MaybeFloat();
    d.day_max = MaybeFloat();
    if (d.temperature_field[0] == '\0') {
      continue;
    }
    JsonObjectConst range = devices[d.id][d.temperature_field].as<JsonObjectConst>();
    d.day_min = maybe(range["min"]);
    d.day_max = maybe(range["max"]);
  }
  return true;
}

void enviot_carry_day(const EnvReadings& previous, EnvReadings* next) {
  if (next == nullptr) {
    return;
  }
  for (uint8_t i = 0; i < next->count; ++i) {
    EnvDevice& d = next->devices[i];
    for (uint8_t j = 0; j < previous.count; ++j) {
      const EnvDevice& p = previous.devices[j];
      if (strcmp(p.id, d.id) == 0 && strcmp(p.temperature_field, d.temperature_field) == 0) {
        d.day_min = p.day_min;
        d.day_max = p.day_max;
        break;
      }
    }
  }
}

EnvBand enviot_band_at(const EnvScale& scale, float value) {
  if (scale.band_count == 0) {
    return EnvBand::None;
  }
  EnvBand band = scale.bands[0].band;
  for (uint8_t i = 0; i < scale.band_count; ++i) {
    if (value >= scale.bands[i].start) {
      band = scale.bands[i].band;
    }
  }
  return band;
}

void enviot_format_ago(int64_t seconds, char* out, size_t size) {
  if (seconds < 90) {
    snprintf(out, size, "just now");
  } else if (seconds < 3600) {
    snprintf(out, size, "%d min ago", static_cast<int>(seconds / 60));
  } else if (seconds < 48 * 3600) {
    snprintf(out, size, "%d h ago", static_cast<int>(seconds / 3600));
  } else {
    snprintf(out, size, "%d days ago", static_cast<int>(seconds / 86400));
  }
}

void enviot_format_stale(int64_t seconds, char* out, size_t size) {
  if (seconds < 3600) {
    snprintf(out, size, "No data for %d min", static_cast<int>(seconds / 60));
  } else if (seconds < 48 * 3600) {
    snprintf(out, size, "No data for %d h", static_cast<int>(seconds / 3600));
  } else {
    snprintf(out, size, "No data for %d days", static_cast<int>(seconds / 86400));
  }
}

void enviot_format_value(const MaybeFloat& value, uint8_t decimals, char* out, size_t size) {
  if (!value.known) {
    snprintf(out, size, "--");
    return;
  }
  if (decimals > 3) {
    decimals = 3;
  }
  char plain[24];
  snprintf(plain, sizeof(plain), "%.*f", decimals, static_cast<double>(value.value));
  // "-0.0" reads as a sign error on a thermometer.
  if (plain[0] == '-' && strspn(plain + 1, "0.") == strlen(plain + 1)) {
    memmove(plain, plain + 1, strlen(plain));
  }
  const char* digits = plain[0] == '-' ? plain + 1 : plain;
  const char* point = strchr(digits, '.');
  const size_t whole = point != nullptr ? static_cast<size_t>(point - digits) : strlen(digits);
  char grouped[32];
  size_t n = 0;
  if (plain[0] == '-') {
    grouped[n++] = '-';
  }
  for (size_t i = 0; i < whole && n + 2 < sizeof(grouped); ++i) {
    if (i > 0 && (whole - i) % 3 == 0) {
      grouped[n++] = ',';
    }
    grouped[n++] = digits[i];
  }
  grouped[n] = '\0';
  snprintf(out, size, "%s%s", grouped, point != nullptr ? point : "");
}
