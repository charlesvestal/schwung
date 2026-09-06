#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A v1.2 payload has no boot-targets directory.  Reading the selector's
# optional default must therefore be successful even when head exits 1: with
# the installer's pipefail setting, an unguarded command substitution aborts
# the installer immediately after "Rebooting Move...".
if awk '
  /^boot_default=\$/ {
    found = 1
    if (index($0, "|| true")) guarded = 1
  }
  END { exit !(found && guarded) }
' scripts/install.sh; then
  echo "PASS: optional boot-target default read is safe for pre-selector payloads"
else
  echo "FAIL: boot_default read can abort a pre-selector installation under pipefail" >&2
  exit 1
fi
