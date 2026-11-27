#!/usr/bin/env bash
# Turns the Device page's Crash details into functions and source lines.
#
#   tools/decode_crash.sh <build> <address>...
#   tools/decode_crash.sh 3f9a1c07b 4004b2a6 4004b1f0 40051e2c 4005204a
#
# <build> is the "build" the crash view shows: the first hex digits of the
# crashing firmware's ELF SHA-256. Give the PC, RA and the stack addresses as
# shown. The ELF is found among the CI runs' firmware-elf artifacts (kept 90
# days), or pass one with ELF=path. A local build of the same commit does not do:
# it lays the code out differently.

set -euo pipefail

if [[ $# -lt 2 ]]; then
  sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
fi

build=$(echo "$1" | tr 'A-F' 'a-f')
shift
cache="${XDG_CACHE_HOME:-$HOME/.cache}/sigenstorpanel-elf"
mkdir -p "$cache"

find_elf() {
  local f
  for f in "$cache"/*.elf; do
    [[ -e "$f" ]] || continue
    if shasum -a 256 "$f" | grep -q "^$build"; then
      echo "$f"
      return 0
    fi
  done
  return 1
}

elf="${ELF:-}"
if [[ -z "$elf" ]]; then
  if ! elf=$(find_elf); then
    echo "Looking through CI runs for an ELF whose SHA-256 starts $build..." >&2
    repo=$(gh repo view --json nameWithOwner -q .nameWithOwner)
    for run in $(gh run list --workflow build.yml --limit 40 --json databaseId -q '.[].databaseId'); do
      if [[ -e "$cache/run-$run.elf" ]]; then
        continue
      fi
      tmp=$(mktemp -d)
      if gh run download "$run" -R "$repo" -n firmware-elf -D "$tmp" >/dev/null 2>&1; then
        mv "$tmp/firmware.elf" "$cache/run-$run.elf"
      else
        # No ELF on that run (a pull request, or older than this script): note it.
        : > "$cache/run-$run.elf"
      fi
      rm -rf "$tmp"
      if elf=$(find_elf); then
        echo "Found it: CI run $run" >&2
        break
      fi
    done
  fi
fi
if [[ -z "$elf" || ! -s "$elf" ]]; then
  echo "No ELF found for build $build" >&2
  exit 1
fi

addr2line=$(ls ~/.platformio-sigenstorpanel/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-addr2line 2>/dev/null ||
  command -v riscv32-esp-elf-addr2line)
# The chip's ROM (0x4fc00000-0x4fc20000) is in no firmware ELF: Espressif's ROM
# ELF has its symbols. The P4 on this board is a pre-rev-3 part (rev 0/1).
rom_elf=$(ls ~/.platformio-sigenstorpanel/packages/tool-esp-rom-elfs/esp32p4_rev0_rom.elf 2>/dev/null || true)
for address in "$@"; do
  a=$((16#${address#0x}))
  if (( a >= 0x4fc00000 && a < 0x4fc20000 )) && [[ -n "$rom_elf" ]]; then
    printf 'ROM '
    "$addr2line" -pfiaC -e "$rom_elf" "0x${address#0x}"
  else
    "$addr2line" -pfiaC -e "$elf" "0x${address#0x}"
  fi
done
