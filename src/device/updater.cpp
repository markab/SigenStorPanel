#include "updater.h"
#include "updater_progress.h"

#include <algorithm>
#include <memory>

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "board_config.h"
#include "display.h"
#include "display_dsi.h"
#include "net.h"
#include "settings.h"

namespace {

// GitHub Pages, not the release assets.
//
// This repository is private, and `releases/latest/download/...` on a private
// repository needs an authenticated request — an anonymous device gets a flat 404,
// which is exactly what a fielded Puck reported. The alternative would be putting
// a GitHub token on every device, which is a real credential guarding far more
// than firmware, to read a file that is already published.
//
// Pages is public and already serves both files, because the installer needs them
// on one origin anyway (GitHub's release assets send no CORS header, so the web
// flasher could not fetch them cross-origin either). The device and the installer
// now read the same manifest from the same place.
//
// One consequence worth knowing: Pages deploys from main, so what a device sees as
// "the latest release" is whatever version main advertises, not the newest tag.
// The version bump is the gate — pushing code to main without bumping
// PUCK_FW_VERSION publishes nothing a device will act on.
//
// Redirects are still followed: Pages serves these through a CDN.
//
// Per-board: each variant fetches its own slug's manifest and image
// (SigenStorPuck/<slug>/...), so a 2.41 never pulls a 1.75 build. The two share a
// version, so the URL — built from the compile-time PUCK_BOARD_SLUG — is the
// discriminator, backed up by the `board` field check below.
constexpr const char* MANIFEST_URL =
    "https://markab.github.io/SigenStorPanel/" PUCK_BOARD_SLUG "/manifest.json";
constexpr const char* FIRMWARE_URL =
    "https://markab.github.io/SigenStorPanel/" PUCK_BOARD_SLUG "/firmware.bin";

// Opening the settings page starts a check, and saving any form re-renders it, so
// without a floor a few minutes of fiddling with settings would be a few dozen
// requests to GitHub. The "Check now" button bypasses this.
constexpr uint32_t CHECK_MIN_INTERVAL_MS = 10 * 60 * 1000;

// With "check automatically" on, how often. An update is not urgent, and the
// check is the device reaching a third party unasked.
constexpr uint32_t AUTO_CHECK_EVERY_MS = 12 * 60 * 60 * 1000;

// Long enough for the install screen to show 100% before the restart.
constexpr uint32_t RESTART_AFTER_MS = 1500;

// How much is taken off the connection at a time (one TLS record), and how much
// is written to flash at a time (one sector, copied into internal RAM first).
constexpr size_t READ_CHUNK_BYTES = 16384;
constexpr size_t WRITE_CHUNK_BYTES = 4096;

// The progress bar's split: the download fills it to here, the flash write the
// rest. Writing is a few seconds; the download is most of the wait.
constexpr int DOWNLOAD_SHARE_PERCENT = 70;

// A download that has delivered nothing for this long has stalled.
constexpr uint32_t DOWNLOAD_STALL_MS = 20000;

// How long "Writing" stays readable before the screen goes dark for the write.
constexpr uint32_t WRITE_NOTICE_MS = 1500;

// How long to wait for the UI thread to move the draw buffer out of internal RAM.
constexpr uint32_t BUFFER_MOVE_WAIT_MS = 2000;


// Guards s_status, which the background check writes and the settings page reads.
// Without it the page could copy a String mid-assignment, which is a use-after-free
// rather than merely a stale reading.
SemaphoreHandle_t s_lock = nullptr;
UpdateStatus s_status;

// Set by the settings page, consumed by the poll task. Nothing else coordinates
// them: a stale read either runs one extra check or defers it by one poll, and
// neither matters.
volatile bool s_check_pending = false;
volatile bool s_apply_pending = false;
uint32_t s_last_check_ms = 0;
bool s_ever_checked = false;
volatile int8_t s_progress = -1;
volatile bool s_wants_internal_ram = false;
volatile bool s_writing = false;

extern "C" const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern "C" const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

void attach_bundle(WiFiClientSecure& client) {
  // The same validated-TLS rule as the API client: the cert bundle covers GitHub's
  // CDN as well as your own server, which is the reason for using a bundle rather
  // than pinning a root.
  client.setCACertBundle(rootca_crt_bundle_start,
                         static_cast<size_t>(rootca_crt_bundle_end - rootca_crt_bundle_start));
}

// Compares dotted versions numerically. A string compare would call "0.10.0" older
// than "0.9.0", which is exactly the point at which it would matter.
bool is_newer(const String& candidate, const String& current) {
  int ci = 0;
  int ni = 0;
  for (int part = 0; part < 3; ++part) {
    int c = 0;
    int n = 0;
    while (ci < static_cast<int>(candidate.length()) && isdigit(candidate[ci])) {
      c = c * 10 + (candidate[ci++] - '0');
    }
    while (ni < static_cast<int>(current.length()) && isdigit(current[ni])) {
      n = n * 10 + (current[ni++] - '0');
    }
    if (c != n) {
      return c > n;
    }
    if (ci < static_cast<int>(candidate.length())) ++ci;  // skip the dot
    if (ni < static_cast<int>(current.length())) ++ni;
  }
  return false;
}

// HTTPClient returns its own negative codes alongside real HTTP statuses, so a
// bare number is not enough to report. "manifest HTTP -1" told a user nothing
// except that something had gone wrong — and -1 is not an HTTP status at all, it
// is a connection that never opened.
//
// The free heap goes in the connection-failure cases on purpose. A TLS handshake
// against this cert bundle needs tens of kilobytes in one piece, and running out
// is a leading cause of a refused connection that looks like a network fault.
String describe_http_error(int code) {
  switch (code) {
    case HTTPC_ERROR_CONNECTION_REFUSED:
      return String("could not open a TLS connection (free heap ") + ESP.getFreeHeap() + " B)";
    case HTTPC_ERROR_CONNECTION_LOST:
      return "the connection dropped mid-request";
    case HTTPC_ERROR_NOT_CONNECTED:
      return "not connected";
    case HTTPC_ERROR_SEND_HEADER_FAILED:
    case HTTPC_ERROR_SEND_PAYLOAD_FAILED:
      return "could not send the request";
    case HTTPC_ERROR_NO_HTTP_SERVER:
      return "no HTTP server answered";
    case HTTPC_ERROR_TOO_LESS_RAM:
      return String("not enough memory (free heap ") + ESP.getFreeHeap() + " B)";
    case HTTPC_ERROR_READ_TIMEOUT:
      return "GitHub did not answer in time";
    case HTTPC_ERROR_ENCODING:
      return "the reply used an encoding we cannot read";
    default:
      break;
  }
  if (code < 0) {
    return String("connection failed (") + code + ")";
  }
  return String("GitHub answered HTTP ") + code;
}

void publish(const UpdateStatus& status) {
  if (s_lock == nullptr) {
    s_status = status;
    return;
  }
  if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
    s_status = status;
    xSemaphoreGive(s_lock);
  }
}

// Builds a status rather than mutating the shared one, so the whole result becomes
// visible in a single assignment and the page never sees a half-updated check.
UpdateStatus perform_check() {
  UpdateStatus result;

  if (WiFi.status() != WL_CONNECTED) {
    result.state = UpdateState::Failed;
    result.message = "no network";
    return result;
  }

  // The same gate sigen_api.cpp has, and for the same reason: a certificate
  // cannot be validated against a clock that has not been set, so the handshake
  // fails and reports itself as a refused connection rather than as the clock
  // problem it is.
  //
  // This became much easier to hit when checks started firing as the settings
  // page opens. A deliberate button press happened minutes after boot; a page
  // load happens seconds after it, which is a race against NTP.
  if (!net_time_synced()) {
    result.state = UpdateState::Failed;
    result.message = "waiting for the clock — HTTPS cannot be validated yet";
    return result;
  }

  WiFiClientSecure client;
  attach_bundle(client);

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, MANIFEST_URL)) {
    result.state = UpdateState::Failed;
    result.message = "could not parse the update URL";
    return result;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    result.state = UpdateState::Failed;
    result.message = describe_http_error(code);
    return result;
  }

  const String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    result.state = UpdateState::Failed;
    result.message = "manifest did not parse";
    return result;
  }

  result.latest_version = doc["version"] | "";
  if (result.latest_version.isEmpty()) {
    result.state = UpdateState::Failed;
    result.message = "manifest has no version";
    return result;
  }

  // Cross-flash guard: never act on a manifest built for another board. The
  // per-slug URL already keeps them apart; this catches a misplaced manifest,
  // which matters because the two variants share a version and so cannot be told
  // apart by version alone. An absent board field is allowed (older manifests).
  const String board = doc["board"] | "";
  if (!board.isEmpty() && board != PUCK_BOARD_SLUG) {
    result.state = UpdateState::Failed;
    result.message = "the release is for a different board";
    return result;
  }

  const bool newer = is_newer(result.latest_version, String(PUCK_FW_VERSION));
  result.state = newer ? UpdateState::Available : UpdateState::UpToDate;
  result.message = newer ? "" : "This is the latest release.";
  Serial.printf("[update] latest %s, running %s: %s\n", result.latest_version.c_str(),
                PUCK_FW_VERSION, newer ? "newer available" : "up to date");
  return result;
}


void log_heap(const char* when) {
  Serial.printf("[update] %s: internal %u B free, largest %u B\n", when,
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

// Frees what heap_caps_malloc gave, for a unique_ptr.
struct HeapCapsFree {
  void operator()(uint8_t* block) const { heap_caps_free(block); }
};
using PsramImage = std::unique_ptr<uint8_t, HeapCapsFree>;

// Downloads firmware.bin whole into PSRAM. Returns an empty string on success, or
// why it failed.
//
// Not HTTPUpdate, which crashed the panel a few percent into every install, and
// not streaming to flash either, which crashed it too. On the P4, WiFi runs
// through esp-hosted, and every packet the C6 hands over waits in internal RAM:
// in esp-hosted's receive buffers, then in lwIP until it is read. A flash write
// stalls both cores, so a download that writes as it goes reads slower than the
// network delivers, and up to a full TCP window (64 KB) piles up in internal RAM,
// in pieces. Then esp-hosted needs one contiguous buffer for its next read
// (28 KB, in the second core dump), finds none, and esp-hosted 2.12.3 asserts
// rather than dropping it (`sdio_rx_get_buffer`, sdio_drv.c:896).
//
// So the download touches no flash: it lands in PSRAM, of which there are 32 MB,
// and is drained as fast as it arrives. The flash is written once the connection
// is closed (write_image), when there is no traffic left to fall behind on.
String fetch_image(PsramImage& image, size_t& size) {
  WiFiClientSecure client;
  attach_bundle(client);
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setReuse(false);
  if (!http.begin(client, FIRMWARE_URL)) {
    return "could not parse the update URL";
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return describe_http_error(code);
  }
  const int length = http.getSize();
  const esp_partition_t* slot = esp_ota_get_next_update_partition(nullptr);
  if (length <= 0) {
    http.end();
    return "GitHub did not say how big the update is";
  }
  if (slot == nullptr || static_cast<uint32_t>(length) > slot->size) {
    http.end();
    return "the update is too big for this panel's app slot";
  }
  size = static_cast<size_t>(length);
  image.reset(static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
  if (!image) {
    http.end();
    return "not enough PSRAM to hold the update";
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t done = 0;
  int logged_quarter = 0;
  uint32_t last_data_ms = millis();
  s_progress = 0;
  while (done < size) {
    const int ready = stream->available();
    if (ready <= 0) {
      if (!stream->connected() || millis() - last_data_ms > DOWNLOAD_STALL_MS) {
        break;
      }
      delay(2);
      continue;
    }
    const size_t want = std::min({static_cast<size_t>(ready), READ_CHUNK_BYTES, size - done});
    const int got = stream->read(image.get() + done, want);
    if (got <= 0) {
      continue;
    }
    done += static_cast<size_t>(got);
    last_data_ms = millis();
    const int percent = static_cast<int>(static_cast<uint64_t>(done) * 100 / size);
    s_progress = static_cast<int8_t>(percent * DOWNLOAD_SHARE_PERCENT / 100);
    if (percent / 25 > logged_quarter) {
      logged_quarter = percent / 25;
      log_heap((String("downloaded ") + percent + "%").c_str());
    }
  }
  http.end();
  if (done != size) {
    return String("the download stopped at ") +
           static_cast<int>(static_cast<uint64_t>(done) * 100 / size) + "%";
  }
  return "";
}

// Writes a downloaded image into whichever OTA slot is not running, and makes it
// the one to boot. Returns an empty string on success, or why it failed.
String write_image(const uint8_t* image, size_t size) {
  const esp_partition_t* slot = esp_ota_get_next_update_partition(nullptr);
  if (slot == nullptr) {
    return "there is no spare app slot to install into";
  }
  // Erases `size` bytes of the slot up front, yielding between blocks.
  Serial.printf("[update] writing %u bytes to %s\n", static_cast<unsigned>(size), slot->label);
  esp_ota_handle_t ota = 0;
  esp_err_t err = esp_ota_begin(slot, size, &ota);
  if (err != ESP_OK) {
    return String("could not prepare the app slot (") + esp_err_to_name(err) + ")";
  }

  // Through internal RAM a sector at a time: flash is written with the cache off,
  // when PSRAM cannot be read.
  std::unique_ptr<uint8_t[]> sector(new (std::nothrow) uint8_t[WRITE_CHUNK_BYTES]);
  if (!sector) {
    esp_ota_abort(ota);
    return String("not enough memory (free heap ") + ESP.getFreeHeap() + " B)";
  }
  for (size_t done = 0; done < size;) {
    const size_t n = std::min(WRITE_CHUNK_BYTES, size - done);
    memcpy(sector.get(), image + done, n);
    // The first write also checks the image's magic byte, so an HTML error page
    // served with a 200 is refused here rather than flashed.
    err = esp_ota_write(ota, sector.get(), n);
    if (err != ESP_OK) {
      esp_ota_abort(ota);
      return String("could not write the update (") + esp_err_to_name(err) + ")";
    }
    done += n;
    s_progress = static_cast<int8_t>(
        DOWNLOAD_SHARE_PERCENT +
        static_cast<uint64_t>(done) * (100 - DOWNLOAD_SHARE_PERCENT) / size);
  }
  // Checks the image's checksum and hash before anything will boot it.
  err = esp_ota_end(ota);
  if (err != ESP_OK) {
    return err == ESP_ERR_OTA_VALIDATE_FAILED
               ? String("the downloaded image did not validate")
               : String("could not finish the update (") + esp_err_to_name(err) + ")";
  }
  err = esp_ota_set_boot_partition(slot);
  if (err != ESP_OK) {
    return String("could not switch to the update (") + esp_err_to_name(err) + ")";
  }
  return "";
}

String download_image() {
  PsramImage image;
  size_t size = 0;
  const String failure = fetch_image(image, size);
  if (!failure.isEmpty()) {
    return failure;
  }
  // The screen flickers while flash is written (display_hold_dark), so it says
  // so, then goes dark until the restart, or until the write fails.
  s_progress = DOWNLOAD_SHARE_PERCENT;
  s_writing = true;
  delay(WRITE_NOTICE_MS);
  display_hold_dark(true);
  const String written = write_image(image.get(), size);
  if (!written.isEmpty()) {
    display_hold_dark(false);
    s_writing = false;
  }
  return written;
}

void perform_apply() {
  if (updater_status().state != UpdateState::Available) {
    return;
  }
  UpdateStatus status = updater_status();
  Serial.printf("[update] downloading %s\n", status.latest_version.c_str());
  status.state = UpdateState::Downloading;
  publish(status);

  // Even drained as fast as TLS can decrypt it, the download keeps up to a TCP
  // window (64 KB, about 80 KB with esp-hosted's and lwIP's overheads) waiting in
  // internal RAM, and esp-hosted needs blocks of up to 64 KB beside that. With the
  // UI and a TLS session in place there were 167 KB free in pieces of at most
  // 70 KB, and the third attempt died at 25%. The display's draw buffer is 120 KB
  // of internal RAM that an install can do without, so it goes to PSRAM until
  // the restart, or back again if the install fails.
  log_heap("before");
  s_wants_internal_ram = true;
  const uint32_t asked_ms = millis();
  while (display_buffer_is_internal() && millis() - asked_ms < BUFFER_MOVE_WAIT_MS) {
    delay(10);
  }
  log_heap("draw buffer in PSRAM");

  // If the new image will not boot, the bootloader falls back to this one —
  // which is the reason for the stock two-slot 16 MB partition table.
  s_progress = -1;
  const String failure = download_image();
  if (failure.isEmpty()) {
    // The restart is ours, after the install screen has shown it finished.
    s_progress = 100;
    Serial.println("[update] written, restarting into it");
    delay(RESTART_AFTER_MS);
    net_restart("firmware updated");
    return;
  }
  status.state = UpdateState::Failed;
  status.message = failure;
  Serial.printf("[update] failed: %s\n", status.message.c_str());
  s_progress = -1;
  s_wants_internal_ram = false;
  publish(status);
}

}  // namespace

void updater_begin() {
  if (s_lock == nullptr) {
    s_lock = xSemaphoreCreateMutex();
  }
  UpdateStatus initial;
  initial.state = UpdateState::Idle;
  publish(initial);
}

bool updater_enabled() {
  return settings_get().check_updates;
}

void updater_request_check(bool force) {
  // The automatic check (page open) is opt-in; the button is not, because
  // pressing it is the asking. It used to bypass this by checking synchronously
  // from the page handler instead — on the LVGL thread, and possibly alongside the
  // poll task's own TLS session — which froze the screen for the round trip and
  // could fail the handshake for want of a second session's heap.
  if ((!updater_enabled() && !force) || s_check_pending) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (!force && s_ever_checked && (millis() - s_last_check_ms) < CHECK_MIN_INTERVAL_MS) {
    return;
  }

  s_check_pending = true;
  UpdateStatus checking = updater_status();
  checking.state = UpdateState::Checking;
  checking.message = "";
  publish(checking);
}

void updater_request_apply() {
  if (updater_status().state == UpdateState::Available) {
    s_apply_pending = true;
  }
}

void updater_service() {
  // Apply first. It ends in a reboot, so anything queued behind it is moot.
  if (s_apply_pending) {
    s_apply_pending = false;
    perform_apply();
    return;
  }
  // "Check automatically": once the clock is good after boot, then twice a day.
  if (!s_check_pending && updater_enabled() && net_time_synced() &&
      (!s_ever_checked || millis() - s_last_check_ms >= AUTO_CHECK_EVERY_MS)) {
    updater_request_check(false);
  }
  if (!s_check_pending) {
    return;
  }
  s_check_pending = false;
  publish(perform_check());
  s_last_check_ms = millis();
  s_ever_checked = true;
}

UpdateStatus updater_status() {
  UpdateStatus copy;
  if (s_lock != nullptr && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
    copy = s_status;
    xSemaphoreGive(s_lock);
  }
  return copy;
}

int8_t updater_progress_percent() {
  return s_progress;
}

bool updater_wants_internal_ram() {
  return s_wants_internal_ram;
}

bool updater_writing_flash() {
  return s_writing;
}
