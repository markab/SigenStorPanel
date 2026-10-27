# Hardware: Elecrow CrowPanel Advanced 10.1" ESP32-P4 HMI

Everything here was gathered from Elecrow's wiki and example repo before the board
was bought. **None of it has been seen on hardware.** As each fact is confirmed, say so
here and in `src/board_crowpanel_101.h`. That is how SigenStorPuck's `board_2p41.h`
records its V1 facts, and it saved re-deriving them twice.

## Sources

- **Wiki:**
  https://www.elecrow.com/wiki/CrowPanel_Advanced_10.1inch_ESP32-P4_HMI_AI_Display_1024x600_IPS.html
  Its spec table is an image, not text, so the figures below come mostly from the example
  code.
- **Example repo:**
  https://github.com/Elecrow-RD/CrowPanel-Advanced-10.1inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen
  - The **Arduino examples** use Espressif's `ESP32_Display_Panel` library with LVGL 8,
    configured as `ESP_PANEL_BOARD_LCD_CONTROLLER EK79007`, bus `MIPI_DSI`, 2 lanes,
    touch `GT911`. **There is no Arduino WiFi example.**
  - The **ESP-IDF examples** include WiFi (a weather demo). Their `sdkconfig` is the
    best source for anything esp-hosted: SDIO pins, bus width, the slave reset GPIO.
  - The **C6 firmware upgrade guide**:
    `example/CrowPanel_Advance_ESP32-P4 Display_Firmware_Upgrade_Guide/`, which ships
    `Advance_P4_On-Board_ESP32C6_Firmware_V2.12.3_2026_04_08`.
  - **Licence not yet checked.** Treat the code as a reference for facts (pins, timings,
    register values) and do not copy it in until the licence is known.
- **EK79007 init sequence:** Espressif's `esp_lcd_ek79007` component (Apache-2.0).
  GFX Library for Arduino 1.6.8 already carries it as `ek79007_init_operations`, and
  `display_dsi.cpp` uses that, so nothing is copied into this repo.

## The chip

| | |
|---|---|
| SoC | **ESP32-P4**, dual-core RISC-V at up to 400 MHz. **No radio of its own** |
| Silicon revision | **Unknown. Must be below 3.0** for pioarduino's `board = esp32-p4`; rev 3 needs `esp32-p4_r3`. Ask the seller; the first boot log prints it |
| Flash | 16 MB. pioarduino's `esp32-p4.json` says 4 MB, so override it |
| PSRAM | Hex-mode (`CONFIG_SPIRAM_MODE_HEX=y` in the core's P4 libs). Size to confirm. **Do not carry over the S3's `qio_opi` / `opi` settings** |
| Bootloader offset | **0x2000**, not the S3's 0x0 (`CONFIG_BOOTLOADER_OFFSET_IN_FLASH=0x2000`; esp-web-tools' P4 module agrees, `BOOTLOADER_FLASH_OFFSET=8192`) |
| Deep-sleep wake | Only GPIO 0–15 can wake the P4 (LP GPIOs). No `ext0` as on the S3 |

## Display: 1024×600 IPS, EK79007 over MIPI-DSI

| | |
|---|---|
| Bus | MIPI-DSI, **2 data lanes at 1000 Mbps per lane** |
| DPI pixel clock | **51 MHz** |
| Colour | RGB565 |
| HSYNC pulse / back porch / front porch | **70 / 160 / 160** |
| VSYNC pulse / back porch / front porch | **10 / 23 / 21** |
| LCD reset | **GPIO41** |
| Backlight power | **GPIO29** (supply switch) |
| Backlight PWM | **GPIO31, 30 kHz** |
| DSI PHY supply | P4 internal LDO; Espressif's reference designs use channel 3 at 2500 mV. GFX's `Arduino_ESP32DSIPanel` hardcodes exactly that. **Confirm** against Elecrow's example |

**Backlight is real PWM**, unlike the AMOLED boards, where brightness is a controller
command. IPS does not burn in, so none of SigenStorPuck's wear-spreading applies.

**Framebuffer.** A DPI panel scans out continuously from a framebuffer in PSRAM: 1024 ×
600 × 2 B = 1.2 MB, or 2.4 MB double-buffered. LVGL can either render into partial
buffers and copy, or run in direct mode on the framebuffer itself. Measure both in
Phase 1; the answer decides tearing and frame rate.

## Touch: GT911

| | |
|---|---|
| I2C | **SDA 45, SCL 46**, 400 kHz |
| Reset | **GPIO40** |
| Interrupt | **GPIO42** |
| Address | **0x5D**, or 0x14, depending on INT's level as reset is released. Elecrow's example expects 0x5D. Probe both, as `touch.cpp` did on the Puck |

## WiFi: ESP32-C6 co-processor over SDIO (esp-hosted)

The P4 has no radio. WiFi 6 comes from an on-board **ESP32-C6** running Espressif's
**esp-hosted** slave firmware, factory-flashed at **V2.12.3** (the esp-hosted 2.x line).
The P4 talks to it over SDIO, and `WiFi.h` works on top of that through
`esp_wifi_remote`.

**SDIO slot 1: CLK = GPIO18, CMD = GPIO19.** The data lines **differ by board version,
and the versions are not compatible:**

| Board | Bus width | D0 | D1 | D2 | D3 |
|---|---|---|---|---|---|
| **V1.1 / V1.2** | 4-bit | **17** | **16** | **15** | **14** |
| V1.0 | 1-bit | 14 | 15 | — | — |

V1.1/V1.2's data lines are **the reverse of Espressif's default** (14, 15, 16, 17).
Arduino-ESP32 **3.1.x** has esp-hosted 0.0.25 with the default pins compiled in and no
way to change them, so WiFi cannot work on it. **3.3.x** has `hostedSetPins(clk, cmd,
d0, d1, d2, d3, rst)`, which must be called before `WiFi.begin()`, and esp-hosted 2.x to
match the C6's firmware.

**Unknown: the GPIO the P4 resets the C6 with.** The core's default is 54. Find it in
Elecrow's ESP-IDF example `sdkconfig`
(`CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE`, or similar) before Phase 2. If the reset pin
is wrong, the C6 never starts and `WiFi.begin()` times out with nothing obvious in the
log.

**C6 firmware updates.** esp-hosted supports the host flashing the slave over the
existing link (host-performs-slave-OTA). Elecrow documents it. Not needed unless the
core's esp-hosted version and V2.12.3 turn out to disagree on the protocol.

**What the 3.3.8 core actually does** (read from the installed package, 2026-10-27):
- Its P4 libraries are built with `CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH=4`, default data pins
  14/15/16/17 and reset GPIO54.
- `hostedSetPins()` changes the pins only. The core's `hostedInit()` starts from
  `INIT_DEFAULT_HOST_SDIO_CONFIG()`, so the bus is always 4-bit through the Arduino API.
- esp-hosted's own `struct esp_hosted_sdio_config` has a `bus_width` field, set before
  `esp_hosted_init()`. So **V1.0 is probably reachable** by bringing esp-hosted up
  ourselves (set the config with `bus_width = 1`, init, connect) before the first WiFi
  call, instead of letting the core do it. Not tried. The C6 on a V1.0 must also run
  slave firmware built for 1-bit.

So: **V1.1/V1.2 is supported by the firmware as built. V1.0 needs that extra work**,
done in Phase 2 only if a V1.0 board turns up. If it does, detect the version at boot
(try 4-bit, fall back to 1-bit, remember the answer in NVS) rather than shipping two
builds.

**Buy V1.1 or V1.2.** Ask the seller which version they ship before ordering.

## Buttons and power

| | |
|---|---|
| RST | The chip's EN line. Not readable |
| BOOT | The P4's boot strap (**assumed GPIO35, confirm**). Readable as an ordinary button after boot. Held at power-on, it forces download mode: silicon strapping, nothing firmware can change |
| Slide switch | Hard power cut. Firmware cannot see or latch it |
| Battery | Connector with a charger. **No PMIC, no fuel gauge** |
| Power draw | **8–10 W with the screen on.** More than a laptop USB port gives; use a 5 V 2 A supply, on the UART3-IN header or the USB 2.0 port |

So the Puck's gestures change:
- the PWR column (day back, auto-cycle, power off) has no button to live on;
- BOOT keeps press / 2 s / 5 s;
- the 5 s hold restarts, since there is no off.

With a full touchscreen this large, day stepping belongs on screen anyway (see
docs/UX.md).

## Serial and flashing

- **UART0 through a CH341** USB-serial bridge, not native USB-CDC. Do not set
  `ARDUINO_USB_CDC_ON_BOOT`. The bridge does the auto-reset into download mode.
- **A separate USB 2.0 port** is the P4's own USB. Not needed for flashing.
- `esptool.py --chip esp32p4 read_mac` identifies the board. Do it before every flash:
  more than one ESP32 board shares this desk.
- **Serial screenshots are impractical.** 1024×600×2 B is 1.2 MB, about 105 s per screen
  at 115200 baud. Serve them over HTTP from the device instead (`GET /screenshot`).

## Optional: mmWave presence sensor

A **Waveshare HMMD mmWave sensor** ([wiki](https://www.waveshare.com/wiki/HMMD_mmWave_Sensor))
can switch the backlight off when nobody is in the room. Without one, nothing changes.

| | |
|---|---|
| Sensor | 3.0-3.6 V, about 50 mA; UART 115200 8N1 at 3.3 V; pins 3V3, GND, TX, RX, OT2 |
| Range | wall-mounted: 8 m for movement, 6 m for micro-movement (breathing, typing) |
| Connection | the board's **UART1 header** ("UART"): 3.3 V out at up to 1 A, RX **GPIO48**, TX **GPIO47** |
| Wiring | sensor 3V3 to 3V3, GND to GND, **TX to GPIO48**, **RX to GPIO47**. OT2 not needed |

Why the UART and not OT2:
- OT2 is a single high/low line, but the only free header with power is the UART one,
  and its two signal pins are RX and TX.
- The UART also gives distance and, more usefully, proves the sensor is alive. A
  silent sensor is ignored rather than trusted, so a loose wire cannot leave the
  panel dark.

The panel sends the sensor no commands. It reads the default text output ("ON",
"OFF", "Range n") and also understands report-mode frames, in case the sensor has
been reconfigured with Waveshare's tool. Waveshare does not document the commands
that set range or sensitivity, so changing those means their tool for now.

Mounting: on the wall at 1.5-2 m, pins at the top, facing the room. Behind the
panel's case works only if the case material is radar-transparent: plastic yes,
metal no. Waveshare has a radome design guide.

## Also on the board (unused for now)

- **Speaker and amp:** I2S LRCLK 21, BCLK 22, SDATA 23; amp enable GPIO30.
- **PDM microphone.**
- **microSD slot.**
- **Wireless module socket** (LoRa / nRF).

The speaker could give alerts later, as claude-gadget's `sound.cpp` does. Not planned.

## Open questions to settle on hardware

1. Silicon revision (decides `esp32-p4` vs `esp32-p4_r3`).
2. Board version printed on the PCB (decides the SDIO table).
3. C6 reset GPIO.
4. BOOT's GPIO, and whether it reads cleanly after boot.
5. GT911 address (0x5D or 0x14).
6. Touch orientation (mirror X / Y).
7. PSRAM size.
8. Whether `Arduino_DSI_Display` drives the EK79007 with the Espressif init table, or
   whether `esp_lcd` directly is needed.
9. Whether opening the CH341 port resets the board on macOS, as it does on the S3s.
10. Whether scanning (`WiFi.scanNetworks(true)`) works through esp-hosted as it does on an
    S3. The network picker and roaming both depend on it.
11. The UART1 header's pin order, and that GPIO48 is free for the presence sensor.
12. Whether the HMMD sends its text output continuously (every ~100 ms) or only on a
    change. The panel treats 10 s of silence as "no sensor". If the sensor only
    reports changes, that check needs the report-mode frames, or OT2, instead.
13. Whether swiping to and from the flows screen stays smooth. Its anti-aliased
    ribbons (`draw_ribbons()` in `src/ui_panel/screen_flows.cpp`) issue about ten
    times the draw calls of the old 3 px slices, on every frame of a swipe. If it
    drags, render the ribbons once into a PSRAM canvas when the data changes and
    blit that instead.
14. Whether the anti-aliased chart outline (`PUCK_CHART_SMOOTH_EDGE`) keeps a swipe
    between the chart screens smooth. It draws a line segment per chart column and
    the fill a pixel column at a time, roughly twice the work of the column path.
    If it drags, set it to false in `src/board_crowpanel_101.h`.
