// Which settings page tapping the full-screen message opens.
//
// Kept beside ui.h rather than in it: ui.h is shared with SigenStorPuck, which has
// no settings sheet. main.cpp sets this with each message, so a message about
// the data source opens Data and one about WiFi opens WiFi.

#pragma once

#include "settings_sheet.h"

void ui_set_overlay_page(SheetPage page);
