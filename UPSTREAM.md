# Upstream

SigenStorPanel was started on 2026-10-27 from two of Mark's own repositories.

| Source | Ref | What it gave |
|---|---|---|
| [SigenStorPuck](https://github.com/markab/SigenStorPuck) | `de3b9f5` (v0.15.2) | Almost all of `src/`, the simulator, fixtures, tools and the web installer |
| claude-gadget (~/GitHub/claude-gadget) | `00bf7fc` | Its setup flow and on-device settings are the model in docs/UX.md, ported by hand, not copied. Its `scripts/version.py` (version from the tag) came over too and was removed on 2026-10-29: the panel keeps the Puck's `PUCK_FW_VERSION` model |

## The plan: copy and track now, a shared library later

Agreed 2026-10-27. **Do not redevelop the shared layer.** It is about 9,500 lines:
- about 7,200 lines of Arduino-free C++ that the sim compiles (the payload parser, day
  series, history rings, all three sources' decoding, the solar model, the screen-off
  window), covered by the selftest;
- about 2,300 lines of network clients and the poll task.

All of it has been proven against the real server and plant. What *should* be redone is
already separate: the screens (`src/ui_panel/`), setup and settings, power and buttons.

1. **Until the panel runs on hardware (end of Phase 1): copy and track, as below.** The
   panel will probably force changes to "shared" code (memory sizes, the User-Agent name,
   chart widths at 1024 px). A library designed before those surface would be designed
   against guesses.
2. **Then extract the verbatim set into a library repo, `SigenStorCore`.**
   - It holds the code, the selftest and the fixtures, with its own CI.
   - Both projects take it through `lib_deps`, pinned to a tag
     (`https://github.com/markab/SigenStorCore.git#vX.Y.Z`).
   - Use `symlink://../SigenStorCore` while working on both at once.
   - This file and the drift script then retire.
3. **Before the network clients go into that library, move SigenStorPuck to Arduino
   3.3.8 too.** Plain C++ doesn't care which core it runs on; the network clients might.
   claude-gadget already runs 3.3.8 on the same S3 boards. Do it on a branch, with a soak
   test on the 1.75 and the 2.41.

Ruled out:
- **Folding the panel into SigenStorPuck as a third board:** two Arduino cores in one
  PlatformIO project, and a different setup model.
- **Git submodules:** `lib_deps` does the same job with versioning built in.

## How the copied files are treated

Every copied file is in exactly one of three groups.

### Verbatim: kept identical to SigenStorPuck

Listed one per line in **`tools/upstream_verbatim.txt`**:
- the data layer (snapshot, day series, history, forecast, Modbus registers, Home
  Assistant, the solar model);
- the network clients and the poll task;
- the shared UI helpers (`chart_band`, `format`, `qr_block`);
- the screen ids in `ui.h`;
- the selftest and the fixtures;
- the vendored esp-web-tools.

**A fix to one of these is made in SigenStorPuck first**, then pulled here. Then both
devices get it and the copies cannot quietly diverge.

If a verbatim file needs to differ here (a product name in a User-Agent string, say),
prefer a small upstream change that takes the difference from `board_config.h`. That
beats editing the copy. If that's not sensible, move the file to the table below and
take it out of the list.

```bash
tools/upstream_drift.sh          # against SigenStorPuck's main
tools/upstream_drift.sh v0.16.0  # against any upstream ref
```

`upstream` lines are fixes worth pulling. `LOCAL` lines are verbatim files that have
been edited here by mistake. After pulling, bump the `base` line in the list and add a
row to the log at the bottom.

### Adapted: started from SigenStorPuck, now this repo's own

Not drift-tracked. When upstream changes one of these, read the diff and port the idea
if it applies.

| File | Why it differs |
|---|---|
| `src/board_config.h` | Includes the one board profile; own version number |
| `src/board_crowpanel_101.h` | New. Same symbol names as SigenStorPuck's profiles |
| `src/main.cpp` | No `power_hold()`, no USB-CDC wait, no AMOLED sweep |
| `src/display_dsi.{h,cpp}`, `src/touch_gt911.cpp` | New. This board's backends behind the verbatim `display.h` / `touch.h` contracts. Upstream's `display.cpp` / `touch.cpp` are the equivalents. `display_dsi.h` adds moving the draw buffer between internal RAM and PSRAM, for installs |
| `src/device/power.{h,cpp}` | No AXP2101, no battery latch, no `ext0` wake on the P4. Same interface, every call a no-op; the backlight is in `display_dsi.cpp` |
| `src/device/buttons.{h,cpp}` | BOOT only; no PWR key |
| `src/device/net.{h,cpp}` | Rewritten. `WiFi.setPins()` before WiFi; no WiFiManager and no captive portal; up to five saved networks, joining the strongest in range (ported from claude-gadget); scans and joins driven from the on-glass picker |
| `src/device/settings.{h,cpp}` | New NVS namespace; settings that move onto the device. The screen masks are 16 bits from 0.1.3, stored the same way as SigenStorPuck 0.16.0 (`scr_vis16`/`scr_rot16`, low byte still in the old keys) |
| `src/presence_parse.{h,cpp}`, `src/device/presence.{h,cpp}`, `src/sim/panel_selftest.cpp` | New. The optional mmWave room sensor, and checks for the panel's own Arduino-free code. `selftest.cpp` stays verbatim; the sim's `--selftest` runs both |
| `src/device/settings_bridge.{h,cpp}` | New. Connects the on-device settings sheet (`src/ui_panel/settings_sheet.*`) to the settings store, WiFi, poller and updater. No upstream equivalent |
| `src/device/settings_server.{h,cpp}` | **Removed** 2026-10-28. Everything is set on the panel's settings sheet; the panel has no web interface |
| `src/ha_catalog.{h,cpp}`, `src/device/ha_catalog_fetch.{h,cpp}`, `src/ui_panel/settings_sheet_ha.cpp` | New. Home Assistant's sensor list, rendered by its `/api/template` and picked from on the panel in place of typing entity IDs on the web page. The parse is Arduino-free and checked by `panel_selftest.cpp`; the fetch runs on its own task, since `poller.cpp` is verbatim |
| `src/ui_panel/screen_events.cpp` | New, against the shared `ui/screen_events.h`: the live or next event in a card with the battery's run, and the earlier events in rows. Upstream's round and landscape versions show one event |
| `src/sound_events.{h,cpp}`, `src/device/sound.{h,cpp}`, `src/ui_panel/settings_sheet_sound.cpp` | New. The speaker and its Sound page: which readings call for a chime (battery full, battery at its reserve, a car starting or stopping charging; Arduino-free and checked by `panel_selftest.cpp`), and tones synthesised as claude-gadget's `sound.cpp` does, for this board's codec-less I2S amp |
| `src/kiosk_code.{h,cpp}` | New. Tidies a kiosk code typed on the glass into the one spelling SigenStor Display (v0.38 on) hashes. Arduino-free beside the verbatim `enrol_url`, checked by `panel_selftest.cpp` |
| `src/device/updater.cpp`, `src/device/updater_progress.h` | Pages URL points at SigenStorPanel. Install progress for the on-screen bar, restarts itself after showing 100%, and with "check automatically" on checks after boot and every 12 h. **Installs without HTTPUpdate** (0.1.1): the image is downloaded whole into PSRAM, then written with `esp_ota_*`, and the display's draw buffer moves to PSRAM for the install. Streaming to flash crashed esp-hosted on the P4 by running internal RAM out (see the comment on `fetch_image`). The Puck's native WiFi does not need this |
| `src/ui/theme.h` | Font roles and spacing for 1024×600 |
| `src/ui/fonts/`, `tools/generate_fonts.sh` | Larger Montserrat subsets |
| `src/sim/main.cpp`, `src/sim/sim_backend.{h,cpp}` | 1024×600 window; panel screen set |
| `src/sim/live_source.{h,cpp}` | New. The sim's `--live` mode: polls a real server through libcurl. A candidate to offer upstream, since nothing in it is panel-specific |
| `include/lv_conf.h` | `LV_MEM_SIZE` and fonts for the panel |
| `tools/package_release.sh` | ESP32-P4, bootloader at 0x2000, one variant, no flat-URL alias; esptool 5 (`merge-bin`) from this repo's own PlatformIO core |
| `.github/workflows/build.yml` | `panel`, and `sim` with `--selftest` as a job of its own; own PlatformIO core dir; the Pages job waits for the `PAGES_ENABLED` variable |
| `web/installer/index.html` | One board; the Mac CH340 driver note; setup on the glass |
| `.gitignore` | Also ignores the two `docs/PUCK_*.md` reference copies |

### Forked: deliberately separate

| Here | Came from | |
|---|---|---|
| `src/ui_panel/` | SigenStorPuck `src/ui_landscape/` (the 2.41's 600×450 screens) | To be re-laid out for 1024×600. Upstream landscape changes are layout for a different screen and are not pulled |

### Reference copies (gitignored, local only)

| Here | Came from |
|---|---|
| `docs/PUCK_CLAUDE.md` | SigenStorPuck's `CLAUDE.md` at `de3b9f5` |
| `docs/PUCK_PLAN.md` | SigenStorPuck's `docs/PLAN.md` at `de3b9f5`. Every `PLAN.md §x` reference in copied source means this file |

Both are gitignored upstream too, because they document SigenStorDisplay's unreleased
API. They are snapshots; for current versions read them in ../SigenStorPuck.

## Not copied

- The round UI (`src/ui/screen_*.cpp`, `src/ui/ui.cpp`).
- Both AMOLED display backends and both touch backends.
- `board_1p75.h` and `board_2p41.h`.
- XPowersLib and the AXP2101 code paths.
- SigenStorPuck's README and STL files.

## Waiting on upstream

These are changes this repo needs in verbatim files. Each one is made in SigenStorPuck
first, then pulled here; none has been made yet. Add a row whenever work here turns
one up, and delete the row once the pull lands and is in the pull log.

| Found | Files (verbatim) | What upstream should change | Until then |
|---|---|---|---|
| 2026-10-27, Phase 0a | `src/device/sigen_api.cpp`, `solar_api.cpp`, `home_assistant_api.cpp` | The User-Agent is hardcoded as `"SigenStorPuck/" PUCK_FW_VERSION " (ESP32-S3)"`. Build it from two new `board_config.h` macros: a product name and a chip name | The panel identifies itself to the server and Home Assistant as a SigenStorPuck on an S3 |
| 2026-10-27, Phase 0a | `src/device/modbus_api.cpp` | Calls `NetworkClient::flush()`, which Arduino 3.3 deprecates in favour of `clear()`. Fix it when SigenStorPuck moves to 3.3.8 (step 3 of the plan above). The panel's own copy in `settings_server.cpp` went with the web interface | A deprecation warning on every panel build. It still works |
| 2026-10-27, settings sheet | `src/device/updater.h` | Optional. The panel shows install progress, through its own `updater_progress.h` beside the verbatim header. If the Puck wants a progress ring too, add `updater_progress_percent()` to `updater.h` upstream and delete the panel's extra header | Nothing breaks: the panel's addition sits beside the shared header |
| 2026-10-28, settings sheet | `src/enrol_url.{h,cpp}` | Optional. Take in `kiosk_code_tidy()` from the panel's `src/kiosk_code.{h,cpp}` (beside `enrol_url_parse()`, or as its own shared file) and use it on the web page's token field. The header also predates v0.38: it says the device never asks for the server and token apart, and calls the token 43 characters. Codes are now 20 | The panel keeps its own `kiosk_code.{h,cpp}`. The Puck's web page saves a typed code as given, spaces and capitals included; the server still matches it |
| 2026-11-26, losses | `src/home_assistant_history.{h,cpp}` | The day's load curve restored from Home Assistant's history is still `PV − battery + grid` when no home sensor is chosen, so it counts the system's losses as house. Since 0.17.1 the live figure is AC-side. Fetch the plant sensor's history too, and use `grid + plant + DC charger` (house plus EV, as the Load series holds), with `ha_ev_is_dc()` saying which kind the EV sensor is | On Home Assistant with no home sensor, the restored part of today's Home use curve sits higher than the live part by the losses, about 0.4 kW on a grid-charging night |
| 2026-10-27, Phase 0a | `src/display.h`, `src/touch.h` | Their comments describe the CO5300 panel and CST9217 touch. The contract is generic, so describe it generically | Comments name the wrong chips. The functions are right |

## Pull log

| Date | Upstream ref | What came over |
|---|---|---|
| 2026-10-27 | `de3b9f5` | Initial copy |
| 2026-10-27 | `18c8451` | `src/ui/chart_band.cpp`: the column cap follows `PUCK_LCD_WIDTH`, so the panel's charts can run the full 1024 px. Made upstream for this repo |
| 2026-10-27 | `f414186` | `src/ui/chart_band.cpp`: an optional anti-aliased chart outline, chosen by `PUCK_CHART_SMOOTH_EDGE` in the board profile; on here, off on both Puck boards. Made upstream for this repo |
| 2026-11-14 | `debfd7c` (v0.16.0) | The Events screen's shared half, made upstream for this repo: `src/events.{h,cpp}` (the /api/events model, parser and wording), the fetch in `sigen_api` and `poller` (server source, every 5 min), the `events` capability in `data_source`, `history_timezone()`, `PUCK_SCREEN_EVENTS` and 16-bit `PuckScreenMask` in `ui/ui.h`, `ui/screen_events.h`, the selftest cases and `test/fixtures/events.json`. Fonts regenerated with the en dash (U+2013); the panel's subsets were identical to the Puck's, so the files were copied |
| 2026-11-26 | `63a6d9a` (v0.17.2) | The loss under PLANT as the system's total, as SigenStorDisplay 0.47.6 sends it: strings (V × I from each inverter's 31025 count, PV1–4 at 31027, PV5–16 at 31042) − battery − plant − DC charger in `power_balance`, read in `modbus_regs` and `modbus_api` (an inverter that refuses them is left alone for ten minutes), and optional PV1–4 voltage and current mappings in `home_assistant`, with the selftest's string cases. The panel's own half: V, mV, A and mA sensors with "pv" in the ID in `ha_catalog`'s list, the note on the sensors list, and "Off" on the grid leg when the plant is off grid (`52c7cbb`, made by hand in `src/ui_panel/screen_power.cpp`) |
| 2026-11-26 | `9b790a9` (v0.17.1) | House load on the AC side and the system's losses, matching SigenStorDisplay 0.47.3: the new `src/power_balance.{h,cpp}`, used by `modbus_regs` (AC and DC chargers read apart) and `home_assistant` (`HaEvKind`, `ha_ev_is_dc()`, `HaParseOptions`); `power.losses`, `today.losses` and the `*_losses` flows in `snapshot`; `home_assistant_api` passing the EV kind; the selftest's five reference cases; fixtures 02 and 04 with losses, and the new 10 and 11, grid-charging nights. The panel's own half: `ha_ev_kind` in `settings`, the EV charger row on the sensors list, `PUCK_COLOUR_LOSSES`, and the Flows and Power screens |
| 2026-11-26 | `ab85000` (v0.17.3) | Solar as what the panels delivered, as in SigenStorDisplay 0.47.7: `power_balance` returns `max(strings, pv)` where the strings are known, else `pv`, and `modbus_regs` and `home_assistant` write it to `power.pv`. The losses use the same figure, so Solar + battery out = plant + loss. HOME is unchanged. Five files: `power_balance.{h,cpp}`, `modbus_regs.cpp`, `home_assistant.cpp`, the selftest with the addendum's cases. Nothing panel-only needed changing: every screen reads `power.pv` |
| 2026-11-23 | `846660e` | `src/ui/ui.h`: `PUCK_SCREEN_ENVIRONMENT` (id 9) for the panel's EnViOT screen, `PUCK_PANEL_ONLY_SCREENS`, which every Puck UI drops, and `UiConfig::with_environment_screen` (false unless the panel sets it). Made upstream for this repo |
