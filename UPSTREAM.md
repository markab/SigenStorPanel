# Upstream

SigenStorPanel was started on 2026-10-27 from two of Mark's own repositories.

| Source | Ref | What it gave |
|---|---|---|
| [SigenStorPuck](https://github.com/markab/SigenStorPuck) | `de3b9f5` (v0.15.2) | Almost all of `src/`, the simulator, fixtures, tools and the web installer |
| claude-gadget (~/GitHub/claude-gadget) | `00bf7fc` | `scripts/version.py`. Its setup flow and on-device settings are the model in docs/UX.md, ported by hand, not copied |

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
| `src/main.cpp` | No `power_hold()`, no AMOLED sweep, P4 bring-up order |
| `src/device/power.{h,cpp}` | No AXP2101, no battery latch, no `ext0` wake on the P4; becomes backlight control |
| `src/device/buttons.{h,cpp}` | BOOT only; no PWR key |
| `src/device/net.{h,cpp}` | `hostedSetPins()` before WiFi; skinned portal with WiFi plus enrolment link (docs/UX.md); saved-network list |
| `src/device/settings.{h,cpp}` | New NVS namespace; settings that move onto the device |
| `src/device/settings_server.{h,cpp}` | Trimmed to the deep config (docs/UX.md) |
| `src/device/updater.cpp` | Pages URL points at SigenStorPanel (already changed) |
| `src/ui/theme.h` | Font roles and spacing for 1024×600 |
| `src/ui/fonts/`, `tools/generate_fonts.sh` | Larger Montserrat subsets |
| `src/sim/main.cpp`, `src/sim/sim_backend.{h,cpp}` | 1024×600 window; panel screen set |
| `include/lv_conf.h` | `LV_MEM_SIZE` and fonts for the panel |
| `tools/package_release.sh` | ESP32-P4, bootloader at 0x2000, one variant, no flat-URL alias |
| `.github/workflows/build.yml` | `panel` and `sim`; own PlatformIO core dir. Manual-only until Phase 0c |
| `web/installer/index.html` | One board |
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

## Pull log

| Date | Upstream ref | What came over |
|---|---|---|
| 2026-10-27 | `de3b9f5` | Initial copy |
