// A one-line notice across the top of whichever screen is showing: no WiFi, the
// data source not set up, a rejected code, a source that cannot be reached.
//
// Every one of them is fixed in the settings sheet, so tapping the banner opens
// the sheet on the page that fixes it. The screens stay visible underneath,
// dashes and all, rather than being replaced by a message: what is missing is
// easier to judge with the rest of the panel in view.
//
// Kept beside ui.h rather than in it: ui.h is shared with SigenStorPuck, which has
// no settings sheet. The full-screen overlay behind ui_set_overlay() is now only
// the boot splash.

#pragma once

#include "settings_sheet.h"

enum class BannerTone : uint8_t {
  Info,       // in progress: connecting, waiting for the clock or the first reading
  Attention,  // needs setting up
  Problem,    // set up, but refused or unreachable
};

// `text` nullptr hides the banner.
void ui_set_banner(const char* text, BannerTone tone, SheetPage page);
