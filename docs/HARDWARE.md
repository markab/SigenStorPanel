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
| Silicon revision | **v1.3** on the first board (ROM `esp32p4-eco2-20240710`), so pioarduino's `board = esp32-p4` is right. Rev 3 parts would need `esp32-p4_r3`; `[chip]` in the boot log prints it |
| Flash | 16 MB. pioarduino's `esp32-p4.json` says 4 MB, so override it |
| PSRAM | **32 MB**, AP hex-mode, 200 MHz (`CONFIG_SPIRAM_MODE_HEX=y` in the core's P4 libs). **Do not carry over the S3's `qio_opi` / `opi` settings** |
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

**The C6 is reset through GPIO32, its EN pin**, not the core's default GPIO54. In the
V1.2 Eagle schematic (`Eagle_SCH&PCB/1.2/*.sch` in Elecrow's repo) the net `C6_EN` joins
the C6's EN, a pull-up (R77), a capacitor (C15) and P4 GPIO32. GPIO54 reaches only pin 12
of the J7 header. Elecrow's example `sdkconfig` keeps the default 54, which is wrong for
the board, and their V1.2 readme says GPIO32.

With the wrong pin the C6 is never reset. It still boots at power-on, so WiFi works, but
any reset of the P4 alone while WiFi is busy (Restart, a crash, a watchdog, flashing)
leaves the C6 mid-session. Every later boot then fails at `WiFi.mode()` with
`sdmmc_send_cmd returned 0x107` and `H_SDIO_DRV: failed to read registers`, and
esp-hosted restarts the P4 every 14 s until the power is cut. Seen on the first board,
2026-10-28.

The core's compiled polarity, `RESET_ACTIVE_HIGH`, is right for an EN pin: esp-hosted
holds it high to run and pulses it low to reset.

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
| BOOT | The P4's boot strap (**assumed GPIO35, still to confirm by pressing it**). Readable as an ordinary button after boot. Held at power-on, it forces download mode: silicon strapping, nothing firmware can change |
| Slide switch | Hard power cut. Firmware cannot see or latch it |
| Battery | Connector with a charger. **No PMIC, no fuel gauge**, but Elecrow's factory app reads a battery voltage through the ADC (`bat_voltage = 4581 mV`, `bat_level = 100 %` with no battery fitted). The pin is not known yet |
| Power draw | **8–10 W with the screen on.** More than a laptop USB port gives; use a 5 V 2 A supply, on the UART3-IN header or the USB 2.0 port |

So the Puck's gestures change:
- the PWR column (day back, auto-cycle, power off) has no button to live on;
- BOOT keeps press / 2 s / 5 s;
- the 5 s hold restarts, since there is no off.

With a full touchscreen this large, day stepping belongs on screen anyway (see
docs/UX.md).

## Serial and flashing

- **UART0 through a WCH CH340K** (USB 1A86:7522) on the board's UART USB-C port, not
  native USB-CDC. Do not set `ARDUINO_USB_CDC_ON_BOOT`. The bridge does the
  auto-reset into download mode, and esptool's reset works through it.
- **macOS needs WCH's driver** for this chip: Apple's built-in one only takes the
  CH340/CH341's 1A86:7523. Install CH34xVCPDriver from
  [WCHSoftGroup/ch34xser_macos](https://github.com/WCHSoftGroup/ch34xser_macos) and
  allow it under System Settings, General, Login Items & Extensions, Driver
  Extensions. The port is then `/dev/cu.wchusbserial*`.
- **Speed:** 921600 drops the stream at once. Uploads work at 460800 (`upload_speed`
  in platformio.ini). Reading the whole 16 MB flash failed at 460800 and needed
  230400, in 1 MB pieces.
- **Opening the port does not reset the board**, with DTR and RTS held low, unlike the
  S3s. A serial logger can attach to a running board.
- **A separate USB 2.0 port** is the P4's own USB. Not needed for flashing. Elecrow's
  factory app enumerates on it as "Advance-P4", 303A:4004, a TinyUSB HID device with no
  serial port.
- **The factory firmware is backed up** (16 MB, from the first board, MAC
  `e8:f6:0a:e1:d5:df`) outside the repo, in `~/GitHub/SigenStorPanel-backups/`, with its
  SHA-256 and boot log. It is Elecrow's, not ours to commit. Writing it back at 0x0
  restores the board.
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
| Connection | the **UART1 socket, J2**, GPIO48 and GPIO47, which the firmware uses as of 2026-10-29. The J7 IO breakout header (IO2 and IO3) also works with a pin change in the profile |
| Wiring | see the tables below. OT2 not needed |

**J2, as the firmware is set up**, labelled UART1, beside the WiFi module: a 4-pin HY2.0
(2.0 mm pitch, Grove-style) socket. Its pins, from the V1.2 Eagle schematic's netlist
(nets RXD1 and TXD1 go nowhere else):

| J2 pin | Net | P4 GPIO | Sensor pin |
|---|---|---|---|
| 1 | RXD1 (panel receives) | 48 | **TX** |
| 2 | TXD1 (panel sends) | 47 | **RX** |
| 3 | VDD_3V3 | | **3V3** |
| 4 | GND | | **GND** |

**J7, the alternative**, the IO breakout header. To use it, set `PUCK_PRESENCE_RX` to 2
and `PUCK_PRESENCE_TX` to 3 in `src/board_crowpanel_101.h`. Four pins in a row down the
header's odd side, so one 4-way Dupont lead does it. Every GPIO on J7 goes to the header
alone in the V1.2 schematic, and the P4 routes its UART to any GPIO:

| J7 pin | Net | Sensor pin |
|---|---|---|
| 5 | VDD_3V3 | **3V3** |
| 7 | GND | **GND** |
| 9 | IO2 on the silkscreen (panel receives) | **TX** |
| 11 | IO3 on the silkscreen (panel sends) | **RX** |

Avoid J7's even pins 2, 4 and 6, which are 5 V, and pin 17, GPIO25, one of the P4's USB
pins. GPIO2 and 3 are also the chip's pin JTAG, unused here. J7's full pinout: 1, 3, 5
3V3; 2, 4, 6 5V; 7, 8, 10, 21-24 GND; 9 GPIO2, 11 GPIO3, 13 GPIO4, 15 GPIO5, 17 GPIO25,
19 GPIO49, 12 GPIO54, 14 GPIO53, 16 GPIO52, 18 GPIO51, 20 GPIO50.

TX and RX cross over on either header: the sensor's TX goes to the panel's receive
pin. Before plugging the sensor in, check with a meter that the pin you take 3V3 from
reads 3.3 V.

**Not UART3 IN (J10).** That 4-pin XH2.54 socket's pin 3 is a 5 V *input* for powering
the board, and its TX and RX do not reach the P4 directly.

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

## Settled on the first board (2026-10-28, V1.2, MAC `e8:f6:0a:e1:d5:df`)

- **Silicon revision v1.3**: `board = esp32-p4` is right.
- **Board version V1.2**, so the 4-bit SDIO table in the profile applies.
- **WiFi through the C6 works**: a scan finds the networks, the panel joins one picked on
  the glass and answers as `sigenstorpanel.local`. Scanning through esp-hosted works as it
  does on an S3. Two `H_SDIO_DRV: Dropping packet(s)` errors appear once at start-up
  and have not recurred.
- **`Arduino_DSI_Display` drives the EK79007** with the Espressif init table; no need for
  `esp_lcd` directly. Backlight on GPIO31 works.
- **GT911 at 0x14**, and swipes and taps land where they should with the profile's
  orientation.
- **PSRAM is 32 MB.** About 390 KB of internal heap is free at boot.
- **Opening the serial port does not reset the board** (see Serial and flashing).
- The I2C scan at boot finds an unrecognised device at **0x2F**, not yet identified. The
  GT911 does not show in that scan because it is still held in reset at that point.

## Open questions to settle on hardware

1. That a reset of the P4 alone (flashing, Restart) now leaves WiFi working, with the C6
   reset through GPIO32.
2. BOOT's GPIO, and whether it reads cleanly after boot.
3. Whether the HMMD sends its text output continuously (every ~100 ms) or only on a
   change. The panel treats 10 s of silence as "no sensor". If the sensor only
   reports changes, that check needs the report-mode frames, or OT2, instead.
4. Whether swiping to and from the flows screen stays smooth. Its anti-aliased
   ribbons (`draw_ribbons()` in `src/ui_panel/screen_flows.cpp`) issue about ten
   times the draw calls of the old 3 px slices, on every frame of a swipe. If it
   drags, render the ribbons once into a PSRAM canvas when the data changes and
   blit that instead.
5. Whether the anti-aliased chart outline (`PUCK_CHART_SMOOTH_EDGE`) keeps a swipe
   between the chart screens smooth. It draws a line segment per chart column and
   the fill a pixel column at a time, roughly twice the work of the column path.
   If it drags, set it to false in `src/board_crowpanel_101.h`.
