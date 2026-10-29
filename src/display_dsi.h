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

// Keeps the backlight off whatever brightness and sleep say, until released.
// Safe from any task.
//
// For writing flash: PSRAM pauses while flash is written, so the DPI peripheral
// cannot read the framebuffer and the panel flashes blue for every write, a few
// dozen times in an install. Dark for those seconds reads better than flicker.
void display_hold_dark(bool dark);
