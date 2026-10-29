// The panel's additions to display.h, which is kept verbatim with SigenStorPuck.

#pragma once

// Moves LVGL's draw buffer into internal RAM or out to PSRAM. Call it on the LVGL
// thread, between lv_timer_handler() calls. Returns false, leaving the buffer
// where it was, if the other kind of RAM has no block big enough.
//
// Internal RAM renders faster and is where the buffer normally lives. An install
// moves it out for as long as it runs: at 120 KB it is the largest thing in
// internal RAM, which the download needs (updater.cpp, fetch_image).
bool display_use_internal_buffer(bool internal);
