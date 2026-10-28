// WiFi, time and mDNS.
//
// Adapted from SigenStorPuck (see UPSTREAM.md). There is no captive portal: the
// panel has a touchscreen, so networks are chosen and their passwords typed on
// the glass (the settings sheet's WiFi page). The long credentials (the server's
// enrolment link, a Home Assistant token) are pasted into the web settings page,
// whose QR code is on the glass too. See docs/UX.md.
//
// Up to NET_MAX_SAVED networks are remembered, most recently used first, and the
// panel joins whichever saved one is strongest in range: at boot, and again
// whenever the connection has been gone for a while. Ported from claude-gadget's
// net.cpp, against this repo's names.
//
// Everything here is non-blocking and runs on the loop thread: scans are
// asynchronous and polled from net_loop(), joins are WiFi.begin() and a timeout.

#pragma once

#include <Arduino.h>

static constexpr int NET_MAX_SAVED = 5;
static constexpr int NET_MAX_SCAN = 16;

enum class NetState {
  Starting,
  // Nothing saved. The panel shows the network picker.
  NoNetworks,
  // Saved networks exist; scanning for them or joining one.
  Connecting,
  Connected,
};

struct NetScanResult {
  String ssid;
  int8_t rssi = 0;
  bool secure = false;
};

enum class NetJoin {
  Idle,
  Joining,
  Joined,
  Failed,
};

void net_begin();

// Must be called from loop(): drives scanning, joining and roaming.
void net_loop();

NetState net_state();

// The panel's address once on the network: "sigenstorpanel.local"
// and the IP. Empty until connected.
String net_hostname();

// The name registered this boot, without ".local", whether or not mDNS is up yet.
// What a newly saved name is compared against: a new one waits for a restart.
String net_name();
String net_ip();

// True once the clock has been set from NTP. TLS certificate validation needs it,
// so it is surfaced rather than assumed.
bool net_time_synced();

// The saved networks, most recently used first.
int net_saved_count();
String net_saved_ssid(int index);

// Forgets one saved network. If it is the one in use, the panel drops it and
// looks for another saved network.
void net_forget(const String& ssid);

// Forgets every saved network and restarts, which brings up the network picker.
void net_forget_wifi();

// Restarts the panel, shutting the C6 down first. Every deliberate restart goes
// through here rather than calling ESP.restart() directly.
//
// A restart of the P4 alone can leave the C6 mid-session, and the next boot then
// cannot reach it over SDIO: esp-hosted restarts the P4 every 14 s until the power
// is cut (docs/HARDWARE.md). Turning WiFi off first ends the session cleanly. A
// C6 that is already unresponsive could hang that, so the restart is forced after
// a few seconds regardless.
[[noreturn]] void net_restart(const char* why);

// A scan for the picker. Results replace the previous scan's, strongest first,
// one entry per name.
void net_scan_start();
bool net_scan_running();
int net_scan_results(NetScanResult* out, int max);
// Signal for a saved network from the last scan, or false if it was not seen.
bool net_scan_rssi(const String& ssid, int8_t* rssi);

// Joins a network typed in on the glass. Saved only once it actually connects;
// if it fails, the panel goes back to whichever saved network it can find.
void net_join(const String& ssid, const String& password);
NetJoin net_join_state();
// Why the last join failed, in words for the screen.
String net_join_error();
