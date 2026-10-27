#!/usr/bin/env bash
#
# Reports how the files this repo shares verbatim with SigenStorPuck have drifted.
# Read-only: it changes nothing in either repository.
#
#   tools/upstream_drift.sh            compare against SigenStorPuck's main
#   tools/upstream_drift.sh <ref>      compare against any ref there (tag, SHA)
#
# Each file in tools/upstream_verbatim.txt is compared three ways: against the base
# it was copied at, against upstream's <ref>, and as it stands here.
#
#   same       identical everywhere
#   upstream   changed in SigenStorPuck since the base: worth pulling
#   LOCAL      changed here although it is listed as verbatim: either revert it or
#              move it to UPSTREAM.md's adapted table and out of the list
#   BOTH       changed on both sides: merge by hand
#   gone       no longer exists upstream at <ref>
#
# After pulling upstream changes, bump the `base` line in tools/upstream_verbatim.txt
# to the ref they came from, and note the pull in UPSTREAM.md.
#
# SIGENSTORPUCK can point at the upstream clone; the default is the sibling checkout.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
upstream="${SIGENSTORPUCK:-${repo_root}/../SigenStorPuck}"
list="${repo_root}/tools/upstream_verbatim.txt"
ref="${1:-main}"

if ! git -C "${upstream}" rev-parse --git-dir >/dev/null 2>&1; then
	echo "no SigenStorPuck clone at ${upstream} (set SIGENSTORPUCK)" >&2
	exit 1
fi

base="$(sed -n 's/^base \([0-9a-f]*\)$/\1/p' "${list}")"
if [[ -z "${base}" ]]; then
	echo "no 'base <sha>' line in ${list}" >&2
	exit 1
fi

blob() { git -C "${upstream}" rev-parse --verify --quiet "$1:$2" || true; }

counts=(0 0 0 0 0)
while IFS= read -r path; do
	[[ -z "${path}" || "${path}" == \#* || "${path}" == base\ * ]] && continue

	base_blob="$(blob "${base}" "${path}")"
	up_blob="$(blob "${ref}" "${path}")"
	here_blob="$(git hash-object "${repo_root}/${path}" 2>/dev/null || true)"

	if [[ -z "${up_blob}" ]]; then
		state=gone; counts[4]=$((counts[4] + 1))
	elif [[ "${here_blob}" == "${base_blob}" && "${up_blob}" == "${base_blob}" ]]; then
		state=same; counts[0]=$((counts[0] + 1))
	elif [[ "${here_blob}" == "${base_blob}" ]]; then
		state=upstream; counts[1]=$((counts[1] + 1))
	elif [[ "${up_blob}" == "${base_blob}" || "${up_blob}" == "${here_blob}" ]]; then
		state=LOCAL; counts[2]=$((counts[2] + 1))
	else
		state=BOTH; counts[3]=$((counts[3] + 1))
	fi

	[[ "${state}" != same ]] && printf '%-9s %s\n' "${state}" "${path}"
done <"${list}"

echo "---"
echo "base ${base}, upstream ${ref} ($(git -C "${upstream}" rev-parse --short "${ref}"))"
echo "same ${counts[0]}  upstream ${counts[1]}  LOCAL ${counts[2]}  BOTH ${counts[3]}  gone ${counts[4]}"
