#!/usr/bin/env bash
set -euo pipefail

# provision_assets.sh — push module assets from your machine to the Move, so a
# contract dump captures each module LOADED rather than cold.
#
#   tools/param-pages/provision_assets.sh --dry-run     # default: show, do nothing
#   tools/param-pages/provision_assets.sh --go
#   tools/param-pages/provision_assets.sh --go sf2      # one module
#
# Reads tools/param-pages/assets.local.json (gitignored — copy assets.example.json).
#
# WHY. An asset-driven module declares a different contract depending on what it
# has loaded. sf2 with an empty soundfonts/ directory declares three parameters,
# and the audit sheet draws exactly that — indistinguishable, in a picture, from
# a module whose UI is broken. The fix is not a harness change: the Node harness
# never runs the DSP, it renders a captured contract. The assets have to be on
# the DEVICE before dump_contracts_device.js reads it.
#
# The assets NEVER pass through the repo. They go from wherever they live on
# your machine to the device, and tests/host/test_no_bundled_assets.sh fails the
# build if one ever appears in the tree.
#
# DRY RUN IS THE DEFAULT, deliberately. This writes into a live device's module
# tree and some of these files are hundreds of megabytes over scp; seeing the
# plan before it runs costs one command and has saved this repo from worse.

cd "$(dirname "$0")/../.."

MAP="tools/param-pages/assets.local.json"
DEVICE="${SCHWUNG_DEVICE:-ableton@move.local}"
GO=0
ONLY=""

for a in "$@"; do
  case "$a" in
    --go) GO=1 ;;
    --dry-run) GO=0 ;;
    --*) echo "unknown option: $a" >&2; exit 2 ;;
    *) ONLY="$a" ;;
  esac
done

if [ ! -f "$MAP" ]; then
  echo "no $MAP" >&2
  echo "  copy tools/param-pages/assets.example.json to it and fill in your paths." >&2
  echo "  it is gitignored: the assets themselves never enter this repo." >&2
  exit 2
fi

# Parsed with node rather than jq: node is already required by every other tool
# in this directory, jq is not, and a missing jq would fail here as "no such
# module" rather than as "install jq".
plan=$(node -e '
const fs = require("fs");
const map = JSON.parse(fs.readFileSync(process.argv[1], "utf8"));
const only = process.argv[2] || "";
const root = map.device_module_root;
for (const [id, m] of Object.entries(map.modules || {})) {
  if (only && id !== only) continue;
  for (const f of (m.files || [])) {
    if (!f || f.startsWith("/path/to/")) continue;   /* untouched template row */
    console.log([id, root + "/" + m.device_dir, f].join("\t"));
  }
}
' "$MAP" "$ONLY")

if [ -z "$plan" ]; then
  echo "nothing to push${ONLY:+ for $ONLY} — every files[] entry is empty or still a template path"
  exit 0
fi

missing=0
total=0
echo "device: $DEVICE"
echo
printf '%-10s %-46s %10s  %s\n' MODULE DEST SIZE SOURCE
while IFS=$'\t' read -r id dest src; do
  if [ ! -e "$src" ]; then
    printf '%-10s %-46s %10s  %s\n' "$id" "$dest" "MISSING" "$src"
    missing=1
    continue
  fi
  bytes=$(du -sk "$src" | cut -f1)
  total=$((total + bytes))
  printf '%-10s %-46s %9sM  %s\n' "$id" "$dest" "$((bytes / 1024))" "$(basename "$src")"
done <<< "$plan"
echo
echo "total: $((total / 1024)) MB"

if [ "$missing" -ne 0 ]; then
  echo "FAIL: a source path does not exist — fix $MAP before pushing" >&2
  exit 1
fi

if [ "$GO" -ne 1 ]; then
  echo
  echo "dry run. re-run with --go to push."
  exit 0
fi

# Device free space is checked before writing, not after failing. The root FS is
# usually 100% full; /data is the only place with room, and these files are big
# enough to fill even that if nobody looks.
avail=$(ssh "$DEVICE" "df -k /data/UserData | tail -n 1 | awk '{print \$4}'")
if [ -n "$avail" ] && [ "$avail" -lt "$((total + 262144))" ]; then
  echo "FAIL: device has $((avail / 1024)) MB free on /data, pushing $((total / 1024)) MB" >&2
  exit 1
fi

while IFS=$'\t' read -r id dest src; do
  echo "==> $id: $(basename "$src")"
  ssh "$DEVICE" "mkdir -p '$dest'"
  scp -r "$src" "$DEVICE:$dest/"
done <<< "$plan"

echo
echo "pushed. now re-capture the contracts so the audit sees the loaded state:"
echo "  see tools/param-pages/dump_contracts_device.js"
