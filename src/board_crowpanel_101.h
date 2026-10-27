// Every pin and tunable for the Elecrow CrowPanel Advanced 10.1" ESP32-P4 HMI.
//
// One board profile — board_config.h includes it.
// The PUCK_ prefix is kept on purpose: the data, device and sim code is shared
// with SigenStorPuck (see UPSTREAM.md) and reads these exact names, so renaming
// them would turn every drift diff into noise.
//
// 1024x600 IPS, EK79007 over 2-lane MIPI-DSI, GT911 capacitive touch, ESP32-P4
// with 16 MB flash and hex PSRAM, WiFi through an ESP32-C6 co-processor over
// SDIO (esp-hosted). No PMIC: power is a battery charger and a slide switch.
//
// NOTHING HERE HAS BEEN SEEN ON HARDWARE YET. Every value comes from Elecrow's
// wiki and example code (sources in docs/HARDWARE.md). Values with a
// TODO(confirm on hardware) are the ones most likely to be wrong; replace the
// TODO with a "confirmed on hardware" note as each one is proved, the way
// SigenStorPuck's board_2p41.h records its V1 facts.

#pragma once

#include <lvgl.h>
#include <stdint.h>

// --------------------------------------------------------- board identity ---

// Names the OTA manifest directory on Pages and is checked against the
// manifest's "board" field before an update is installed (updater.cpp).
#define PUCK_BOARD_SLUG "panel-10.1"
#define PUCK_BOARD_NAME "Elecrow CrowPanel Advanced 10.1 ESP32-P4"

static constexpr bool PUCK_DISPLAY_ROUND = false;

// ---------------------------------------------------------------- display ---

// Landscape, natively — no software rotation, unlike the 2.41.
static constexpr int16_t PUCK_LCD_WIDTH = 1024;
static constexpr int16_t PUCK_LCD_HEIGHT = 600;

// MIPI-DSI link. Elecrow's ESP-IDF example: 2 data lanes at 1000 Mbps each,
// DPI pixel clock 51 MHz, RGB565.
static constexpr uint8_t PUCK_DSI_LANES = 2;
static constexpr uint32_t PUCK_DSI_LANE_MBPS = 1000;
static constexpr uint32_t PUCK_DPI_CLOCK_MHZ = 51;

// EK79007 video timings, in pixels / lines.
static constexpr uint16_t PUCK_DPI_HSYNC_PULSE = 70;
static constexpr uint16_t PUCK_DPI_HSYNC_BACK_PORCH = 160;
static constexpr uint16_t PUCK_DPI_HSYNC_FRONT_PORCH = 160;
static constexpr uint16_t PUCK_DPI_VSYNC_PULSE = 10;
static constexpr uint16_t PUCK_DPI_VSYNC_BACK_PORCH = 23;
static constexpr uint16_t PUCK_DPI_VSYNC_FRONT_PORCH = 21;

static constexpr int8_t PUCK_LCD_RST = 41;

// The P4's DSI PHY is fed from its internal LDO. Espressif's P4 reference designs
// use LDO channel 3 at 2500 mV. TODO(confirm on hardware): check against Elecrow's
// example, and whether the Arduino core or Arduino_DSI_Display does this for us.
static constexpr int8_t PUCK_DSI_PHY_LDO_CHAN = 3;
static constexpr uint16_t PUCK_DSI_PHY_LDO_MV = 2500;

// IPS with a real backlight, so brightness is PWM — unlike the AMOLED boards'
// controller command. GPIO29 switches the backlight supply; GPIO31 dims it.
static constexpr int8_t PUCK_BACKLIGHT_POWER = 29;
static constexpr int8_t PUCK_BACKLIGHT_PWM = 31;
static constexpr uint32_t PUCK_BACKLIGHT_PWM_HZ = 30000;
static constexpr uint8_t PUCK_LCD_BRIGHTNESS = 200;  // 0..255, default before settings load

// LVGL partial draw buffer, in whole display lines. 60 x 1024 x 2 B = 120 KB.
// TODO(confirm on hardware): DSI wants a full framebuffer in PSRAM anyway, so
// the right shape here may be direct mode into that rather than a partial buffer.
static constexpr uint16_t PUCK_LVGL_BUFFER_LINES = 60;

// ---------------------------------------------------------------- buttons ---

// TODO(confirm on hardware): the P4's boot strapping pin is GPIO35, and the
// board's BOOT key is assumed to be on it. Whether it is free to read after boot
// is the thing to check. RST is the chip's EN line, not a GPIO.
static constexpr int8_t PUCK_BUTTON_BOOT = 35;

// No PWR key and no PMIC. The slide switch is a hard power cut, so there is
// nothing for firmware to latch or read.
static constexpr int8_t PUCK_BUTTON_PWR = -1;
static constexpr int8_t PUCK_POWER_LATCH = -1;
static constexpr uint32_t PUCK_PWR_DEBOUNCE_MS = 0;

// ------------------------------------------------------------------ touch ---

static constexpr int8_t PUCK_TOUCH_INT = 42;
static constexpr int8_t PUCK_TOUCH_RST = 40;

// GT911's address depends on INT's level at the end of reset. 0x5D is what
// Elecrow's example expects. TODO(confirm on hardware) with the boot bus scan.
static constexpr uint8_t PUCK_TOUCH_ADDR_PRIMARY = 0x5D;
static constexpr uint8_t PUCK_TOUCH_ADDR_ALT = 0x14;

static constexpr uint8_t PUCK_TOUCH_MAX_POINTS = 5;

static constexpr bool PUCK_TOUCH_MIRROR_X = false;  // TODO(confirm on hardware)
static constexpr bool PUCK_TOUCH_MIRROR_Y = false;  // TODO(confirm on hardware)

// -------------------------------------------------------------------- i2c ---

static constexpr int8_t PUCK_I2C_SDA = 45;
static constexpr int8_t PUCK_I2C_SCL = 46;
static constexpr uint32_t PUCK_I2C_HZ = 400000;

// ------------------------------------------------------- wifi co-processor ---
//
// The ESP32-C6 sits on SDIO slot 1. Arduino-ESP32 3.3.x's hostedSetPins() takes
// these at runtime; 3.1.x compiled in a different 4-bit wiring and cannot be told
// otherwise, which is why this project needs the newer core (see CLAUDE.md).
//
// The data lines depend on the board version, and the two are not compatible:
//   V1.1 / V1.2   4-bit, D0..D3 = 17, 16, 15, 14   (reversed from the default)
//   V1.0          1-bit, D0 = 14, D1 = 15
// This profile is V1.1/V1.2. A V1.0 would need its own profile, the way the
// 2.41's V1 and V2 do in SigenStorPuck.
static constexpr int8_t PUCK_SDIO_CLK = 18;
static constexpr int8_t PUCK_SDIO_CMD = 19;
static constexpr int8_t PUCK_SDIO_D0 = 17;
static constexpr int8_t PUCK_SDIO_D1 = 16;
static constexpr int8_t PUCK_SDIO_D2 = 15;
static constexpr int8_t PUCK_SDIO_D3 = 14;

// TODO(confirm on hardware): the pin the P4 resets the C6 with. Not in Elecrow's
// pin table; the core's default is GPIO54. Find it from their ESP-IDF example's
// sdkconfig (CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE) before Phase 2.
static constexpr int8_t PUCK_SDIO_C6_RESET = 54;

// ----------------------------------------------------------------- serial ---

// UART0 through the CH341 bridge, not native USB-CDC.
static constexpr uint32_t PUCK_SERIAL_BAUD = 115200;

// --------------------------------------------------------------------- ui ---

// The short dimension; the bound for any shared overlay written for a square.
static constexpr int16_t PUCK_SAFE_SQUARE = 600;

static constexpr uint32_t PUCK_STALE_AFTER_S = 30;
static constexpr int PUCK_KW_DECIMALS = 2;
static constexpr int PUCK_MAX_DAYS_BACK = 7;
static constexpr uint32_t PUCK_DAY_RETURN_S = 120;
static constexpr uint32_t PUCK_SCREEN_WAKE_S = 30;
static constexpr float PUCK_SOC_LOW_PCT = 15.0f;

// ------------------------------------------------------- panel longevity ---
//
// IPS does not burn in the way AMOLED does, so there is no sweep band. These stay
// defined only because shared code still reads them; Phase 0 removes the sweep
// and these with it.
static constexpr uint32_t PUCK_SWEEP_DURATION_MS = 0;
static constexpr lv_coord_t PUCK_SWEEP_BAND_PX = 0;
static constexpr lv_opa_t PUCK_SWEEP_OPACITY = 0;
