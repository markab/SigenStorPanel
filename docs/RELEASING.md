# Releasing

The same pipeline as SigenStorPuck's, in `.github/workflows/build.yml`:

| Event | What happens |
|---|---|
| Any push or pull request | The firmware and the simulator are built, and `--selftest` runs |
| A push to `main` | The firmware is also packaged, and the installer is published to Pages with it, once Pages is switched on (below) |
| A `v*` tag | A GitHub Release is also cut, with `firmware-panel-10.1.bin` and `merged-panel-10.1.bin` attached |

The installer and the panel's own updater read the same file,
`https://markab.github.io/SigenStorPanel/panel-10.1/manifest.json`. Whatever `main`
advertises there is what new installs get and what panels in use are offered.

## Switching it on (once)

On github.com, in the repository's **Settings**:

1. **Pages → Build and deployment → Source: GitHub Actions.** If this is left on
   "Deploy from a branch", Pages serves the README through Jekyll and the installer
   never appears.
2. **Secrets and variables → Actions → Variables → New repository variable:**
   `PAGES_ENABLED` = `true`. This is the switch for the workflow's Pages job, which is
   skipped until it is set, so pushes to `main` don't fail before Pages exists.
3. Re-run the latest `build` workflow on `main` (Actions tab, or push anything). The
   installer is then at <https://markab.github.io/SigenStorPanel/>.

**The repository is public**, as SigenStorPuck is. Pages on a private repository needs a
paid GitHub plan, and the published site is public either way, and has to be: the panel
fetches its updates from it without a token. Being public, nothing gitignored may ever be
committed: `CLAUDE.md`, `docs/PLAN.md` and the two `docs/PUCK_` files document
SigenStorDisplay's API and must stay out.

With the GitHub CLI signed in, steps 1 and 2 are:

```bash
gh api -X POST repos/markab/SigenStorPanel/pages -f build_type=workflow
gh variable set PAGES_ENABLED --body true --repo markab/SigenStorPanel
```

## Cutting a release

1. Bump `PUCK_FW_VERSION` in `src/board_config.h`. **This is the gate.** A push to
   `main` without a bump republishes the same version, which no panel will install.
2. Run the checks:
   - `pio run -e panel`;
   - `pio run -e sim && .pio/build/sim/program --selftest`;
   - `tools/upstream_drift.sh`;
   - the provenance grep in CLAUDE.md, which must return nothing.
3. Commit and push to `main`. Pages now offers the new version to installs and updates.
4. Tag it to match, and push the tag:

   ```bash
   git tag v0.1.0
   git push origin v0.1.0
   ```

   Packaging refuses a tag that differs from `PUCK_FW_VERSION`, so the Release never
   advertises a version the firmware doesn't report.

## Checking a build locally

```bash
pio run -e panel && tools/package_release.sh
```

This writes `dist/panel-10.1/` with `firmware.bin`, `merged.bin` and `manifest.json`.
`merged.bin` is the whole flash from offset 0, with the bootloader at 0x2000. It should
be byte-identical to PlatformIO's own `.pio/build/panel/firmware.factory.bin`.

To try the installer page, serve `web/installer/` with `dist/`'s contents copied in,
from `localhost` (WebSerial needs HTTPS or localhost), in Chrome or Edge.
