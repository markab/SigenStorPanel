#!/usr/bin/env bash
#
# Builds the release artifacts into dist/:
#
#   dist/<slug>/firmware.bin   the OTA image, for the panel updating itself
#   dist/<slug>/merged.bin     the whole flash in one file, for the web installer
#   dist/<slug>/manifest.json  ESP Web Tools manifest, and what the device reads
#                              to find out whether there is a newer version
#   dist/firmware-<slug>.bin   distinctly-named copies for the GitHub Release,
#   dist/merged-<slug>.bin     whose asset namespace is flat (no subdirectories)
#
# One manifest serves both consumers: the installer uses `builds` to flash, and
# the device reads `version`. The layout is SigenStorPuck's, per board slug, so a
# second board can be added as another entry in `variants` without moving
# anything a device in the field already polls (see updater.cpp).
#
# Differences from SigenStorPuck's packager, all from the chip:
# - the ESP32-P4's bootloader sits at 0x2000, not the S3's 0x0;
# - esptool comes from this repo's own PlatformIO core (core_dir in
#   platformio.ini), whose esptool 5 spells the command merge-bin;
# - boot_app0.bin is looked up in that core too.
#
# Usage: pio run -e panel && tools/package_release.sh

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dist="${repo_root}/dist"

# env : slug : human-readable name. The slug names the release directory on Pages
# and the OTA manifest, and must match PUCK_BOARD_SLUG in the board profile.
variants=(
	"panel:panel-10.1:Elecrow CrowPanel Advanced 10.1″ ESP32-P4"
)

# The single source of truth for the version is the firmware's own string. Taking
# it from the tag instead would let a release be cut whose manifest advertises a
# version the binary does not report, and the device compares against what the
# binary reports, so it would either update in a loop or never update at all.
version="$(sed -n 's/^#define PUCK_FW_VERSION "\(.*\)"$/\1/p' "${repo_root}/src/board_config.h")"
if [[ -z "${version}" ]]; then
	echo "could not read PUCK_FW_VERSION from src/board_config.h" >&2
	exit 1
fi

# When run from a tag, the two must agree.
if [[ -n "${GITHUB_REF_NAME:-}" && "${GITHUB_REF_NAME}" == v* ]]; then
	tag_version="${GITHUB_REF_NAME#v}"
	if [[ "${tag_version}" != "${version}" ]]; then
		echo "tag ${GITHUB_REF_NAME} does not match PUCK_FW_VERSION ${version}" >&2
		echo "bump src/board_config.h or retag; a release must not advertise a version" >&2
		echo "different from the one the firmware reports" >&2
		exit 1
	fi
fi

# This repo's PlatformIO core, as platformio.ini names it, with ~ expanded.
core_dir="$(cd "${repo_root}" && pio system info --json-output |
	python3 -c 'import sys,json;print(json.load(sys.stdin)["core_dir"]["value"])')"

# pioarduino installs esptool into the core's own Python environment. Fall back to
# one on PATH, as CI's pip install provides.
esptool="${core_dir}/penv/bin/esptool"
if [[ ! -x "${esptool}" ]]; then
	esptool="$(command -v esptool || command -v esptool.py || true)"
fi
if [[ -z "${esptool}" ]]; then
	echo "no esptool in ${core_dir}/penv or on PATH" >&2
	exit 1
fi

# boot_app0 lives in the framework, not the build directory.
boot_app0="$(find "${core_dir}/packages" -name boot_app0.bin -path '*partitions*' | head -1)"
if [[ -z "${boot_app0}" ]]; then
	echo "could not find boot_app0.bin under ${core_dir}/packages" >&2
	exit 1
fi

rm -rf "${dist}"
mkdir -p "${dist}"

for entry in "${variants[@]}"; do
	IFS=: read -r env slug name <<<"${entry}"
	build_dir="${repo_root}/.pio/build/${env}"
	out="${dist}/${slug}"

	if [[ ! -f "${build_dir}/firmware.bin" ]]; then
		echo "no build for ${env} at ${build_dir}: run 'pio run -e ${env}' first" >&2
		exit 1
	fi

	mkdir -p "${out}"
	cp "${build_dir}/firmware.bin" "${out}/firmware.bin"

	# --flash-mode keep: the bootloader header PlatformIO produced is already right
	# for this board's flash. Offsets are the ones `pio project metadata` reports
	# for the panel env: bootloader 0x2000, partition table 0x8000, boot_app0
	# 0xe000, app 0x10000 (default_16MB.csv).
	"${esptool}" --chip esp32p4 merge-bin \
		-o "${out}/merged.bin" \
		--flash-mode keep \
		--flash-size 16MB \
		0x2000 "${build_dir}/bootloader.bin" \
		0x8000 "${build_dir}/partitions.bin" \
		0xe000 "${boot_app0}" \
		0x10000 "${build_dir}/firmware.bin"

	# `path` is relative, and that is the whole trick: release assets send no CORS
	# header, so the installer page must fetch the binary from its own origin. The
	# Pages job publishes this manifest and merged.bin together under <slug>/, so
	# "merged.bin" resolves next to the manifest. `board` is the cross-flash guard.
	cat >"${out}/manifest.json" <<JSON
{
  "name": "SigenStorPanel — ${name}",
  "version": "${version}",
  "board": "${slug}",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-P4",
      "parts": [
        {
          "path": "merged.bin",
          "offset": 0
        }
      ]
    }
  ]
}
JSON

	# Distinctly-named copies for the GitHub Release (flat asset namespace).
	cp "${out}/firmware.bin" "${dist}/firmware-${slug}.bin"
	cp "${out}/merged.bin" "${dist}/merged-${slug}.bin"

	echo "packaged ${slug} (${env}) ${version}"
done

echo "---"
find "${dist}" -type f | sort
