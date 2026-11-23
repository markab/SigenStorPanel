#include "enviot_fetch.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <freertos/semphr.h>
#include <time.h>

#include <new>

#include "board_config.h"
#include "psram_alloc.h"
#include "settings.h"

extern "C" const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern "C" const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

namespace {

constexpr const char* LIVE_PATH = "/api/live";
constexpr const char* DAY_PATH = "/api/stats/day";
constexpr const char* USER_AGENT = "SigenStorPanel/" PUCK_FW_VERSION " (ESP32-P4)";
// The sensors report about every 15 minutes, so a minute is plenty.
constexpr uint32_t LIVE_EVERY_MS = 60 * 1000;
constexpr uint32_t DAY_EVERY_MS = 5 * 60 * 1000;
// A refused code will not start working by itself: try again now and then, not
// every minute (the server's guide asks the same).
constexpr uint32_t REFUSED_EVERY_MS = 10 * 60 * 1000;
// Nothing set, or no WiFi yet: look again in a while.
constexpr uint32_t IDLE_EVERY_MS = 30 * 1000;
constexpr uint16_t CONNECT_TIMEOUT_MS = 4000;
// InfluxDB behind the day stats can take a few seconds.
constexpr uint16_t TOTAL_TIMEOUT_MS = 15000;
// /api/live is about 1 KB a sensor plus 3 KB of definitions.
constexpr size_t BODY_MAX = 48 * 1024;
// WiFiClientSecure lives on this stack, as on the poll task's.
constexpr uint32_t TASK_STACK = 12288;

SemaphoreHandle_t s_lock = nullptr;
TaskHandle_t s_task = nullptr;
// The readings handed to the UI, and the generation that says they changed. In
// PSRAM, with the body buffer and the work copy: all CPU-only.
EnvReadings* s_shared = nullptr;
uint32_t s_generation = 0;
EnvReadings* s_work = nullptr;
char* s_body = nullptr;

// Collects a response body into s_body. writeToStream() is used rather than
// getString() so the body goes to PSRAM, not internal RAM, and so a chunked
// reply is decoded the same as one with a length.
// A Stream only because writeToStream() asks for one; nothing reads it back.
class BodyWriter : public Stream {
 public:
  size_t length = 0;
  bool overflowed = false;

  int available() override {
    return 0;
  }
  int read() override {
    return -1;
  }
  int peek() override {
    return -1;
  }

  size_t write(uint8_t byte) override {
    return write(&byte, 1);
  }
  size_t write(const uint8_t* data, size_t size) override {
    if (length + size >= BODY_MAX) {
      overflowed = true;
      return 0;
    }
    memcpy(s_body + length, data, size);
    length += size;
    s_body[length] = '\0';
    return size;
  }
};

bool clock_is_plausible() {
  return time(nullptr) > 1704067200;  // 1 Jan 2024: NTP has landed
}

// GET base+path with the kiosk code. Returns the HTTP status, or a negative
// HTTPClient code when there was no answer. On 200 the body is in s_body.
int get(const String& base, const String& code, const char* path, size_t* length) {
  *length = 0;
  const bool secure = base.startsWith("https://");
  if (secure && !clock_is_plausible()) {
    return HTTPC_ERROR_CONNECTION_REFUSED;
  }
  WiFiClient plain;
  WiFiClientSecure tls;
  WiFiClient* client = &plain;
  if (secure) {
    // Validated, never setInsecure(): the code is a long-lived credential.
    tls.setCACertBundle(rootca_crt_bundle_start,
                        static_cast<size_t>(rootca_crt_bundle_end - rootca_crt_bundle_start));
    client = &tls;
  }
  HTTPClient http;
  http.setConnectTimeout(CONNECT_TIMEOUT_MS);
  http.setTimeout(TOTAL_TIMEOUT_MS);
  http.setUserAgent(USER_AGENT);
  http.setReuse(false);
  // The code is not followed to another host.
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(*client, base + path)) {
    return HTTPC_ERROR_CONNECTION_REFUSED;
  }
  http.addHeader("Cookie", "env_kiosk=" + code);
  http.addHeader("Accept", "application/json");
  const int status = http.GET();
  if (status == HTTP_CODE_OK) {
    BodyWriter writer;
    const int written = http.writeToStream(&writer);
    if (written < 0 || writer.overflowed) {
      http.end();
      return HTTPC_ERROR_READ_TIMEOUT;
    }
    *length = writer.length;
  }
  http.end();
  return status;
}

void publish(const EnvReadings& readings) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *s_shared = readings;
  ++s_generation;
  xSemaphoreGive(s_lock);
}

void fetch_task(void*) {
  uint32_t last_day_ms = 0;
  bool have_day = false;
  String last_base;
  String last_code;
  for (;;) {
    uint32_t wait_ms = IDLE_EVERY_MS;
    const Settings& settings = settings_get();
    const String base = settings.enviot_base_url;
    const String code = settings.enviot_code;
    if (!base.isEmpty() && !code.isEmpty() && WiFi.status() == WL_CONNECTED) {
      if (base != last_base || code != last_code) {
        // A different server's ranges are not this one's.
        have_day = false;
        last_base = base;
        last_code = code;
      }
      size_t length = 0;
      const uint32_t started = millis();
      const int status = get(base, code, LIVE_PATH, &length);
      // The work copy starts from what was published, so a failed read keeps the
      // last good readings and only its status changes.
      xSemaphoreTake(s_lock, portMAX_DELAY);
      *s_work = *s_shared;
      xSemaphoreGive(s_lock);
      const bool had_readings = s_work->has_readings;
      if (status == HTTP_CODE_OK) {
        static EnvReadings* previous = nullptr;
        if (previous == nullptr) {
          void* block = psram_alloc(sizeof(EnvReadings));
          previous = block != nullptr ? new (block) EnvReadings() : nullptr;
        }
        if (previous != nullptr) {
          *previous = *s_work;
        }
        if (enviot_parse_live(s_body, length, s_work)) {
          s_work->status = EnvStatus::Ok;
          s_work->has_readings = true;
          if (previous != nullptr && have_day) {
            enviot_carry_day(*previous, s_work);
          }
          if (!have_day || millis() - last_day_ms >= DAY_EVERY_MS) {
            size_t day_length = 0;
            if (get(base, code, DAY_PATH, &day_length) == HTTP_CODE_OK &&
                enviot_parse_day(s_body, day_length, s_work)) {
              have_day = true;
              last_day_ms = millis();
            }
          }
          Serial.printf("[enviot] %u sensors in %lu ms\n", static_cast<unsigned>(s_work->count),
                        static_cast<unsigned long>(millis() - started));
        } else {
          // Not readings: keep what there was.
          if (previous != nullptr) {
            *s_work = *previous;
          }
          s_work->status = EnvStatus::Failed;
          s_work->has_readings = had_readings;
          Serial.println("[enviot] /api/live answered with something that is not readings");
        }
        wait_ms = LIVE_EVERY_MS;
      } else if (status == HTTP_CODE_UNAUTHORIZED || status == HTTP_CODE_FORBIDDEN) {
        s_work->status = EnvStatus::Refused;
        Serial.printf("[enviot] code refused (HTTP %d)\n", status);
        wait_ms = REFUSED_EVERY_MS;
      } else {
        s_work->status = EnvStatus::Failed;
        Serial.printf("[enviot] read failed (%d)\n", status);
        wait_ms = LIVE_EVERY_MS;
      }
      publish(*s_work);
    }
    // A wake (a new server or code) cuts the wait short.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
  }
}

}  // namespace

void enviot_fetch_begin() {
  if (s_task != nullptr) {
    return;
  }
  s_lock = xSemaphoreCreateMutex();
  void* shared = psram_alloc(sizeof(EnvReadings));
  void* work = psram_alloc(sizeof(EnvReadings));
  s_body = static_cast<char*>(psram_alloc(BODY_MAX));
  if (s_lock == nullptr || shared == nullptr || work == nullptr || s_body == nullptr) {
    Serial.println("[enviot] out of memory; the Environment screen will wait");
    return;
  }
  s_shared = new (shared) EnvReadings();
  s_work = new (work) EnvReadings();
  // Core 0 with the poller, below it: the plant's readings come first.
  if (xTaskCreatePinnedToCore(fetch_task, "enviot", TASK_STACK, nullptr, 1, &s_task, 0) !=
      pdPASS) {
    s_task = nullptr;
    Serial.println("[enviot] could not start the task");
  }
}

bool enviot_fetch_take(EnvReadings* out, uint32_t* generation) {
  if (s_lock == nullptr || out == nullptr || generation == nullptr) {
    return false;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const bool changed = s_generation != *generation;
  if (changed) {
    *out = *s_shared;
    *generation = s_generation;
  }
  xSemaphoreGive(s_lock);
  return changed;
}

void enviot_fetch_wake() {
  if (s_task != nullptr) {
    xTaskNotifyGive(s_task);
  }
}
