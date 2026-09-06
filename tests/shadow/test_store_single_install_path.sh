#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

f="src/shadow/shadow_ui.js"

# THE ON-DEVICE STORE IS GONE, IN THREE STAGES, AND THIS PINS THE LAST ONE.
#
# 1. The interactive flows went first — the store browser, UPDATE_PROMPT with
#    per-module install and Update All, staged core updates. They were already
#    unreachable, every entry point having been redirected to a pointer screen,
#    because the privileged writes silently no-opped for `ableton`.
# 2. Then the pointer screens: [Module Store] printed "move.local:7700" and
#    nothing else, which is a signpost wearing a shop's name.
# 3. Then detection. [Check Updates] scanned the catalog, listed what was
#    outdated, and told you to open the web manager to install any of it —
#    which shows that same list next to the button that acts on it.
#
# schwung-manager is the single install/update path, and the device now answers
# "where is it" with the Connect screen: its own IP, and a QR that opens it.

for dead in 'sharedInstallModule' 'processAllUpdates' 'performCoreUpdate' \
            'STORE_PICKER_CATEGORIES' 'STORE_PICKER_LIST' 'STORE_PICKER_DETAIL' \
            'STORE_PICKER_RESULT' 'UPDATE_PROMPT' 'UPDATE_RESTART' \
            'buildStoreCategoryItems' 'getHostUpdateModule' \
            'showUpdatesAvailableScreen' 'showModuleStorePointer' \
            'checkForUpdatesInBackground' 'buildNoUpdatesMessage' \
            'pendingUpdates' 'enterStorePicker' 'storePickerCategory'; do
  # Comments explaining what was removed are allowed and wanted; CODE is not.
  # Stripping block comments and line comments is what makes the difference
  # visible to a grep, and without it this file would have to choose between
  # pinning the removal and letting the removal be explained.
  if perl -0pe 's{/\*.*?\*/}{}gs; s{^\s*//.*$}{}gm; s{\s//.*$}{}gm' "$f" | rg -q "$dead"; then
    echo "FAIL: retired store/update symbol still reachable in code: $dead" >&2
    exit 1
  fi
done

# What replaced them.
for live in 'VIEWS.CONNECT' 'enterConnect' 'drawConnectBody' 'host_get_device_ip' \
            'VIEWS.NOTICE'; do
  if ! rg -q "$live" "$f"; then
    echo "FAIL: the Connect / notice surface is missing: $live" >&2
    exit 1
  fi
done

# The store view module is now the notice screen, and every browser draw is
# gone from it.
if [ -e src/shadow/shadow_ui_store.mjs ]; then
  echo "FAIL: src/shadow/shadow_ui_store.mjs is back; it is shadow_ui_notice.mjs now" >&2
  exit 1
fi
for dead in 'drawStorePickerCategories' 'drawStorePickerList' 'drawStorePickerDetail' \
            'drawStorePickerResult' 'buildReleaseNoteLines'; do
  if perl -0pe 's{/\*.*?\*/}{}gs; s{^\s*//.*$}{}gm; s{\s//.*$}{}gm' \
       src/shadow/shadow_ui_notice.mjs "$f" | rg -q "$dead"; then
    echo "FAIL: dead store-browser draw still present: $dead" >&2
    exit 1
  fi
done

# THE ADDRESS IS NOT HARD-CODED ANY MORE. `move.local:7700` was printed by five
# call sites as the primary instruction; it survives in exactly one place, as
# the fallback connect_screen.mjs offers a device that has no address to give.
hits=$(perl -0pe 's{/\*.*?\*/}{}gs; s{^\s*//.*$}{}gm; s{\s//.*$}{}gm' "$f" | rg -c 'move\.local:7700' || true)
if [ -n "$hits" ]; then
  echo "FAIL: shadow_ui.js still hard-codes move.local:7700 ($hits site(s)); the device knows its own IP" >&2
  exit 1
fi

echo "PASS: schwung-manager is the single install/update path, and the device points at it by IP"
