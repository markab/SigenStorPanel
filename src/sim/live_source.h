// The sim's --live mode: polls a real SigenStorDisplay server instead of
// reading fixtures, so the 1024x600 layouts can be judged against your own
// house's numbers and today's real curves.
//
// Server source only. Modbus and Home Assistant go through the Arduino network
// classes, which the sim does not have.
//
// The fetching runs on its own thread, the way the poll task does on the device,
// so a slow server never stalls drawing. The bodies are handed to the UI loop
// unparsed: parsing and filing them is the shared code's job, and doing it on
// the UI thread keeps the history ring single-writer here.

#pragma once

#include <stdint.h>

#include <string>

// What arrived since the last take. Either body may be empty.
struct LiveUpdate {
  std::string summary;
  std::string day_series;
  // Set when the last summary fetch failed: an HTTP status, or a curl error.
  std::string error;
};

// Starts polling `base_url` ("https://host" or "http://192.168.1.10:8000", no
// trailing slash) with the kiosk token, every `poll_s` seconds for the summary
// and every five minutes for the day series. Restarts the thread if it was
// already polling. False if curl could not start.
bool live_source_begin(const std::string& base_url, const std::string& token, uint32_t poll_s);

// Moves anything new into `out`. False when there was nothing.
bool live_source_take(LiveUpdate* out);

// Asks the thread to stop and waits for it. Safe to call when not running.
void live_source_end();
