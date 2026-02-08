#!/usr/bin/env bash
set -euo pipefail

SCRIPT="scripts/install.sh"

if [ ! -f "$SCRIPT" ]; then
  echo "FAIL: Missing $SCRIPT" >&2
  exit 1
fi

if ! grep -q -- "--skip-confirmation" "$SCRIPT"; then
  echo "FAIL: install.sh missing --skip-confirmation option" >&2
  exit 1
fi

if ! grep -qi "unsupported by ableton" "$SCRIPT"; then
  echo "FAIL: install.sh missing unsupported disclaimer text" >&2
  exit 1
fi

if ! grep -qi "accept no liability" "$SCRIPT"; then
  echo "FAIL: install.sh missing liability disclaimer text" >&2
  exit 1
fi

if ! grep -q "install Move Everything" "$SCRIPT"; then
  echo "FAIL: install.sh disclaimer should refer to Move Everything" >&2
  exit 1
fi

if ! grep -q "Type 'yes' to proceed" "$SCRIPT"; then
  echo "FAIL: install.sh missing explicit yes confirmation prompt" >&2
  exit 1
fi

if ! grep -q 'if \[ "\$response" != "yes" \]' "$SCRIPT"; then
  echo "FAIL: install.sh missing exact 'yes' gate check" >&2
  exit 1
fi

echo "PASS: install.sh includes disclaimer prompt and skip confirmation flag"
