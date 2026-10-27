#include "live_source.h"

#include <curl/curl.h>
#include <stdio.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {

// The same paths and cadence as src/device/sigen_api.cpp and poller.cpp.
constexpr const char* SUMMARY_PATH = "/api/summary";
constexpr const char* DAY_SERIES_PATH = "/api/day/series?slot_minutes=5";
constexpr auto DAY_REFRESH = std::chrono::minutes(5);
constexpr long TIMEOUT_S = 15;

std::string s_base_url;
std::string s_cookie;
std::chrono::seconds s_poll_interval(5);

std::thread s_thread;
std::mutex s_lock;
std::condition_variable s_wake;
bool s_stop = false;
bool s_fresh = false;
LiveUpdate s_pending;

size_t append_body(char* data, size_t size, size_t count, void* user) {
  static_cast<std::string*>(user)->append(data, size * count);
  return size * count;
}

// GET base + path. Certificates are verified (curl's default, against the
// system's trust store), as the device does with setCACertBundle().
bool fetch(CURL* curl, const char* path, std::string* body, std::string* error) {
  body->clear();
  const std::string url = s_base_url + path;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_COOKIE, s_cookie.c_str());
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "SigenStorPanel-sim");
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, TIMEOUT_S);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);

  const CURLcode result = curl_easy_perform(curl);
  if (result != CURLE_OK) {
    *error = curl_easy_strerror(result);
    return false;
  }
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  if (status != 200) {
    char text[48];
    snprintf(text, sizeof(text), "http %ld", status);
    *error = text;
    return false;
  }
  return true;
}

void run() {
  CURL* curl = curl_easy_init();
  auto last_day = std::chrono::steady_clock::time_point();
  bool have_day = false;

  for (;;) {
    std::string summary;
    std::string error;
    const bool ok = fetch(curl, SUMMARY_PATH, &summary, &error);
    if (!ok) {
      fprintf(stderr, "[live] %s%s: %s\n", s_base_url.c_str(), SUMMARY_PATH, error.c_str());
    }

    // After a good summary only, as the poller does: a server that is not
    // answering gets one request per poll, not two.
    std::string day;
    const auto now = std::chrono::steady_clock::now();
    if (ok && (!have_day || now - last_day >= DAY_REFRESH)) {
      last_day = now;
      std::string day_error;
      if (fetch(curl, DAY_SERIES_PATH, &day, &day_error)) {
        have_day = true;
      } else {
        day.clear();
        fprintf(stderr, "[live] day series unavailable: %s — charts fill from polls only\n",
                day_error.c_str());
      }
    }

    {
      std::unique_lock<std::mutex> lock(s_lock);
      if (ok) {
        s_pending.summary = std::move(summary);
        s_pending.error.clear();
      } else {
        s_pending.error = error;
      }
      if (!day.empty()) {
        s_pending.day_series = std::move(day);
      }
      s_fresh = true;
      if (s_wake.wait_for(lock, s_poll_interval, [] { return s_stop; })) {
        break;
      }
    }
  }
  curl_easy_cleanup(curl);
}

}  // namespace

bool live_source_begin(const std::string& base_url, const std::string& token, uint32_t poll_s) {
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    return false;
  }
  s_base_url = base_url;
  while (!s_base_url.empty() && s_base_url.back() == '/') {
    s_base_url.pop_back();
  }
  // Both names, as the device sends them: __Host- for HTTPS, the plain one for
  // a LAN server on HTTP.
  s_cookie = "sig_kiosk=" + token + "; __Host-sig_kiosk=" + token;
  s_poll_interval = std::chrono::seconds(poll_s == 0 ? 5 : poll_s);
  s_thread = std::thread(run);
  return true;
}

bool live_source_take(LiveUpdate* out) {
  std::lock_guard<std::mutex> lock(s_lock);
  if (!s_fresh) {
    return false;
  }
  *out = std::move(s_pending);
  s_pending = LiveUpdate();
  s_fresh = false;
  return true;
}

void live_source_end() {
  if (!s_thread.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(s_lock);
    s_stop = true;
  }
  s_wake.notify_all();
  s_thread.join();
  curl_global_cleanup();
}
