# Setup and settings: the model

This repository exists, rather than being a third board profile in SigenStorPuck,
largely because of how **claude-gadget** handles setup. That flow feels finished; the
Puck's does not.

## What claude-gadget does that the Puck does not

- **The captive portal asks for almost nothing:** WiFi, and the one credential the device
  needs to reach its service (claude-gadget: an API token and a timezone).
- **The portal is skinned.**
  - A single `<style>` block injected with `wm.setCustomHeadElement()` gives WiFiManager
    the product's palette, type and a small animated mark.
  - `wm.setTitle()` names it.
  - Everything that would confuse the user is turned off (`setShowInfoUpdate(false)`, a
    trimmed `setMenu`).
  - It looks like part of the product, not a router page.
- **Everyday settings are on the device's own screens.** Brightness and dim level are
  sliders that preview live on the panel while dragged, and write flash only on release.
  The auto-dim timeout is a slider too. The WiFi tile lists saved networks, each with a
  forget button (tap twice), plus "hold BOOT 3 s to add a network".
- **It remembers several networks** and joins whichever saved one is strongest in range.
  WiFiManager on its own remembers only the last.

The Puck does the reverse. Its portal asks only for a device name, and everything else
lives on a long web settings page at `http://<name>.local/`. That was right for a 1.75"
round screen with no room for controls. A 10.1" touchscreen has room for all of them.

## The model for SigenStorPanel

**1. Portal: WiFi plus one link, and nothing else.**
- WiFi network and password.
- **The server enrolment link**: the single pasteable URL SigenStorDisplay hands out,
  which carries both server address and kiosk token. `src/enrol_url.cpp` already parses
  it. This is "the key to connect to the service".
- Skinned in the SigenStor palette (`src/ui/theme.h`) with the same technique as
  claude-gadget's `portal_theme.h`.
- Non-blocking, so the panel keeps drawing a "join this network to set up" screen with a
  QR code for the AP while the portal runs.
- *Open question:* Modbus and Home Assistant need more than one field (a plant IP; an HA
  URL plus token plus entity mapping). Recommended:
  - leaving the link blank finishes setup without a source;
  - the panel then offers a source picker on screen;
  - the HA and Modbus details go on the web page.
  Confirm with Mark in Phase 3.

**2. On the device: everything someone might change on an ordinary day.**

*Built in the sim 2026-10-27* (`src/ui_panel/settings_sheet.*`, wired on the device by
`src/device/settings_bridge.*`):
- **Getting there:** a sheet that slides up over any data screen. Swipe up opens it,
  left and right moves between its pages, and swipe down or Done closes it. It
  closes by itself after 60 s untouched. The old settings screen (s8) is gone from
  the data row; its QR code and address are on the Device page.
- **Pages:** Display, Screens, WiFi, Data, Device.
- **Restarts:** changes that need one (screens shown, data source, upside down) put
  up a "Restart to apply" bar.
- **Not wired on the device yet:** "Add a network" and a multi-network list. Both
  need the saved-network store ported from claude-gadget's `net.cpp`. Until then
  the WiFi page lists only the current network, and forgetting it forgets WiFi.

The original plan, kept for the reasoning:
A settings screen, last in the swipe order as on the Puck, with tiles for:
- **Display:** brightness, dim level (live preview), dim timeout, overnight
  backlight-off window.
- **Screens:**
  - which screens are shown and which the auto-cycle visits (the Puck's two masks);
  - auto-cycle on/off and its interval, stored separately, as on the Puck.
- **WiFi:** saved networks with forget; "add a network" reopens the portal.
- **Data source:** server / Modbus / Home Assistant, and the server link (replace or
  forget).
- **Updates:** current version, check now, install. The check runs on the poll task,
  never on the LVGL thread (see CLAUDE.md).
- **About:** IP, `.local` name, a QR code to the web page, free heap and the largest
  free block.
- **Day stepping** moves onto the glass. On a screen this size, back/forward arrows by the
  date chip beat a button gesture, and there is no PWR key to put "back" on anyway.

**3. Web settings page: only what is painful to type on glass.**
- Home Assistant entity mapping.
- Solar forecast location and the four array slots.
- Modbus plant address and inverter list.
- Hostname.
- Everything on the device screen also shows here, read-only or editable, so nothing is
  reachable *only* from the glass.

## claude-gadget files to study (under ~/GitHub/claude-gadget)

| File | For |
|---|---|
| `src/portal_theme.h` | The portal skin: one `PORTAL_HEAD` string. Note "Generated; edit the palette in the `:root` block" |
| `src/net.cpp` | Portal setup (`setCustomHeadElement`, `setTitle`, `setMenu`, `setConfigPortalBlocking(false)`), the saved-network list (`MAX_KNOWN = 5`), roaming to the strongest known network, reopening the portal on demand with a 300 s idle timeout, closing the LAN web portal once a token is saved |
| `src/ui.cpp` | `build_settings()`: the sliders, live dim preview, flash-on-release. The WiFi tile and `forget_cb`. Screenshot mode, which renders every page with demo data |
| `src/settings.cpp` | A small NVS settings struct. Compare with SigenStorPuck's much larger `src/device/settings.cpp`, which this repo carries |
| `src/ota.cpp` | The update check and install, for comparison with `src/device/updater.cpp` |
| `tools/screenshots.py` | Serial screenshot capture. Fine at 466×466, far too slow at 1024×600 (see docs/HARDWARE.md). Port the *idea* to an HTTP endpoint |

claude-gadget is Mark's own code, so porting from it is fine. Still port deliberately:
take the technique, written against this repo's settings model and names. A
copied-in second settings system would give the device two ideas of what is saved.
