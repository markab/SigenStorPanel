// No PMIC, no PWR key, no battery gauge on the CrowPanel: the slide switch cuts
// power outright and firmware can neither see nor latch it. The interface is kept
// so main.cpp and buttons.cpp read the same as SigenStorPuck's; every call here
// reports "nothing to report". Backlight control lives in display_dsi.cpp,
// behind display_set_brightness() and display_set_sleep().

#include "power.h"

#include "board_config.h"

void power_hold() {
  // No battery latch (PUCK_POWER_LATCH is -1).
}

void power_begin() {
  Serial.println("[power] no PMIC: mains or the board's own charger, slide switch is a hard cut");
}

PowerStatus power_status() {
  // pmic_ok false keeps the device-battery indicator hidden.
  return PowerStatus{};
}

void power_loop() {}

bool power_key_down() {
  return false;
}

void power_shutdown() {
  // There is no off: the P4 cannot deep-sleep waiting on an arbitrary GPIO, and
  // nothing can cut the rails but the switch. The nearest thing is a restart.
  Serial.println("[power] no soft off on this board, restarting instead");
  ESP.restart();
}
