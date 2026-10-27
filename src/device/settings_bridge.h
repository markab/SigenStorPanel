// Connects the on-device settings sheet (src/ui_panel/settings_sheet.h) to the
// settings store, WiFi, the poll task and the updater. The sheet only draws and
// reports; everything it changes is saved here.

#pragma once

// Builds the sheet over the active screen and fills it. After ui_create() and
// settings_begin().
void settings_bridge_begin();

// Refreshes the live parts (WiFi, memory, the last reading, the updater) once a
// second. Call from loop(), on the LVGL thread.
void settings_bridge_loop();
