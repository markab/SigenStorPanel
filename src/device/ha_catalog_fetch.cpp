#include "ha_catalog_fetch.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <freertos/semphr.h>
#include <string.h>

#include "board_config.h"
#include "psram_alloc.h"
#include "settings.h"

extern "C" const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern "C" const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

namespace {

constexpr const char* TEMPLATE_PATH = "/api/template";
constexpr const char* USER_AGENT = "SigenStorPanel/" PUCK_FW_VERSION " (ESP32-P4)";
constexpr uint16_t CONNECT_TIMEOUT_MS = 4000;
// Rendering a few hundred sensors is quick for Home Assistant, but a busy one on
// a Pi can take a while to start answering.
constexpr uint16_t TOTAL_TIMEOUT_MS = 15000;
constexpr uint32_t TASK_STACK = 12288;

// A mutex, not a spinlock: ha_catalog_copy() moves up to 50 KB under it, which
// must not be done with interrupts off.
SemaphoreHandle_t s_lock = nullptr;
HaCatalogState s_state = HaCatalogState::Idle;
char s_message[48] = {};
uint32_t s_generation = 0;
size_t s_count = 0;
// The list itself, in PSRAM: 300 sensors is about 50 KB.
HaCandidate* s_list = nullptr;
bool s_running = false;

// Created on first use, which is on the UI task, before any fetch task exists.
void lock() {
  if (s_lock == nullptr) {
    s_lock = xSemaphoreCreateMutex();
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
}

void unlock() {
  xSemaphoreGive(s_lock);
}

void finish(HaCatalogState state, const char* message, HaCandidate* list, size_t count) {
  lock();
  s_state = state;
  snprintf(s_message, sizeof(s_message), "%s", message);
  if (list != nullptr) {
    // Swapped in whole, so a reader never sees half a list.
    HaCandidate* old = s_list;
    s_list = list;
    s_count = count;
    list = old;
  }
  ++s_generation;
  s_running = false;
  unlock();
  free(list);
}

void fetch_task(void*) {
  const Settings& settings = settings_get();
  const String base = settings.ha_base_url;
  const String token = settings.ha_token;
  if (base.isEmpty() || token.isEmpty()) {
    finish(HaCatalogState::Failed, "Set the address and token first", nullptr, 0);
    vTaskDelete(nullptr);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    finish(HaCatalogState::Failed, "No WiFi", nullptr, 0);
    vTaskDelete(nullptr);
    return;
  }

  String body;
  {
    JsonDocument request;
    request["template"] = HA_CATALOG_TEMPLATE;
    serializeJson(request, body);
  }

  const bool secure = base.startsWith("https://");
  WiFiClient plain;
  WiFiClientSecure tls;
  WiFiClient* client = &plain;
  if (secure) {
    // The bearer token gets the same certificate validation as every other
    // request. Never setInsecure().
    tls.setCACertBundle(rootca_crt_bundle_start,
                        static_cast<size_t>(rootca_crt_bundle_end - rootca_crt_bundle_start));
    client = &tls;
  }
  HTTPClient http;
  http.setConnectTimeout(CONNECT_TIMEOUT_MS);
  http.setTimeout(TOTAL_TIMEOUT_MS);
  http.setUserAgent(USER_AGENT);
  http.setReuse(false);
  // A bearer token is not followed across a redirect.
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(*client, base + TEMPLATE_PATH)) {
    finish(HaCatalogState::Failed, "Could not reach Home Assistant", nullptr, 0);
    vTaskDelete(nullptr);
    return;
  }
  http.addHeader("Authorization", String("Bearer ") + token);
  http.addHeader("Content-Type", "application/json");
  const int status = http.POST(body);
  if (status <= 0) {
    http.end();
    finish(HaCatalogState::Failed, "Could not reach Home Assistant", nullptr, 0);
    vTaskDelete(nullptr);
    return;
  }
  if (status == 401 || status == 403) {
    http.end();
    finish(HaCatalogState::Failed, "Home Assistant rejected the token", nullptr, 0);
    vTaskDelete(nullptr);
    return;
  }
  if (status != 200) {
    http.end();
    char message[48];
    snprintf(message, sizeof(message), "Home Assistant answered HTTP %d", status);
    finish(HaCatalogState::Failed, message, nullptr, 0);
    vTaskDelete(nullptr);
    return;
  }
  const String text = http.getString();
  http.end();

  HaCandidate* list =
      static_cast<HaCandidate*>(psram_alloc(HA_CATALOG_MAX * sizeof(HaCandidate)));
  if (list == nullptr) {
    finish(HaCatalogState::Failed, "Out of memory", nullptr, 0);
    vTaskDelete(nullptr);
    return;
  }
  const size_t count = ha_catalog_parse(text.c_str(), text.length(), list, HA_CATALOG_MAX);
  Serial.printf("[ha] %u sensors listed for the picker\n", static_cast<unsigned>(count));
  finish(HaCatalogState::Ready, count == 0 ? "Home Assistant listed no sensors" : "", list,
         count);
  vTaskDelete(nullptr);
}

}  // namespace

void ha_catalog_request() {
  lock();
  const bool running = s_running;
  if (!running) {
    s_running = true;
    s_state = HaCatalogState::Loading;
    s_message[0] = '\0';
  }
  unlock();
  if (running) {
    return;
  }
  if (xTaskCreatePinnedToCore(fetch_task, "ha_catalog", TASK_STACK, nullptr, 1, nullptr, 0) !=
      pdPASS) {
    finish(HaCatalogState::Failed, "Could not start the request", nullptr, 0);
  }
}

HaCatalogState ha_catalog_state(char* message, size_t size) {
  lock();
  const HaCatalogState state = s_state;
  if (message != nullptr && size > 0) {
    snprintf(message, size, "%s", s_message);
  }
  unlock();
  return state;
}

uint32_t ha_catalog_generation() {
  lock();
  const uint32_t generation = s_generation;
  unlock();
  return generation;
}

size_t ha_catalog_copy(HaCandidate* out, size_t max) {
  lock();
  size_t count = s_count < max ? s_count : max;
  if (s_list != nullptr && out != nullptr) {
    memcpy(out, s_list, count * sizeof(HaCandidate));
  } else {
    count = 0;
  }
  unlock();
  return count;
}
