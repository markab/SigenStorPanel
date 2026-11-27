// Why the panel last started, and the crash the core dump left in flash, for the
// settings sheet's Device page. Panel only.
//
// A panel on the wall has no serial port to read, and the startup chime is the
// only sign it restarted. The core dump (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH, in
// the coredump partition) survives the restart and every later one until the
// next crash, so the sheet can show it whenever someone looks.

#pragma once

#include "ui_panel/settings_sheet.h"

// Reads both, once, at start-up: a flash read of about a kilobyte. Also prints
// them to serial.
void crash_report_read(SheetCrash* out);
