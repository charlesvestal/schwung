#!/usr/bin/env bash
#
# THE MOD-ROUTE SAVE DOCUMENT: writer, carrier and reader must agree.
#
# The span is three files and nothing else covers it end to end:
#
#   chain_host.c      the "mod_config" getter WRITES the document
#   shadow_ui.js      reads that key and stores it as patch.mod_routes
#   chain_patch.c     READS the "mod_routes" section back
#
# test_buses_from_producer exists because a feature with a reader and no writer
# passed every test it had: chain_patch.c had read "buses" since the feature
# landed and nothing emitted them, so every load silently reset all four. This
# is the same shape of gap for mod routes, checked by NAME -- a field written as
# "srcType" and read as "src" round-trips as a default with no error anywhere,
# which is precisely how a saved route loses its source.
set -euo pipefail

cd "$(dirname "$0")/../.."
HOST="src/modules/chain/dsp/chain_host.c"
PATCHC="src/modules/chain/dsp/chain_patch.c"
UI="src/shadow/shadow_ui.js"
for f in "$HOST" "$PATCHC" "$UI"; do
  [ -f "$f" ] || { echo "FAIL: missing $f"; exit 1; }
done

fail=0
say_fail() { echo "FAIL: $1"; fail=1; }
say_ok()   { echo "  ok  $1"; }

# ---- the carrier: one key out, one section in ---------------------------
if /usr/bin/grep -q 'getSlotParam(slotIndex, "mod_config")' "$UI"; then
  say_ok "the UI reads mod_config from the DSP"
else
  say_fail "the UI does not read mod_config; the document is never produced"
fi

if /usr/bin/grep -q 'patch.mod_routes = modRoutes;' "$UI"; then
  say_ok "the UI stores it as patch.mod_routes"
else
  say_fail "the UI does not store the document as patch.mod_routes"
fi

# The legacy section must no longer be WRITTEN. Two documents describing the
# same eight routes can disagree, and the reader prefers the new one anyway.
if /usr/bin/grep -q 'patch.lfos = ' "$UI"; then
  say_fail "the UI still writes patch.lfos; the legacy section is read-only now"
else
  say_ok "the UI no longer writes the legacy lfos section"
fi

if /usr/bin/grep -q '"\\"mod_routes\\""' "$PATCHC" || \
   /usr/bin/grep -q 'mod_routes\\"' "$PATCHC"; then
  say_ok "the parser looks for a mod_routes section"
else
  say_fail "the parser never looks for mod_routes"
fi

# ---- writer keys vs reader keys -----------------------------------------
# The writer is one snprintf format string; the reader is a run of json_get_*
# calls. Both are lifted from source rather than restated here, so this cannot
# drift into agreeing with itself.
writer=$(/usr/bin/sed -n '/strcmp(key, "mod_config")/,/^    }$/p' "$HOST" \
         | /usr/bin/grep -oE '\\"[a-z_]+\\":' \
         | /usr/bin/sed 's/\\"//g; s/://' | /usr/bin/sort -u)
reader=$(/usr/bin/sed -n '/MOD ROUTES . two section names on disk/,/^    }$/p' "$PATCHC" \
         | /usr/bin/grep -oE 'json_get_(int|float|string)\(obj, "[a-z_]+"' \
         | /usr/bin/grep -oE '"[a-z_]+"' | /usr/bin/tr -d '"' | /usr/bin/sort -u)

if [ -z "$writer" ]; then say_fail "could not lift the writer keys"; fi
if [ -z "$reader" ]; then say_fail "could not lift the reader keys"; fi

# Every field the READER wants must be WRITTEN, or a saved route silently loses
# it and loads as a default.
missing=$(/usr/bin/comm -13 <(printf '%s\n' "$writer") <(printf '%s\n' "$reader") || true)
if [ -n "$missing" ]; then
  echo "    read but never written:"; printf '%s\n' "$missing" | /usr/bin/sed 's/^/      /'
  say_fail "the parser reads fields the save document does not contain"
else
  say_ok "every field the parser reads is written by mod_config ($(printf '%s\n' "$reader" | /usr/bin/wc -l | /usr/bin/tr -d ' ') fields)"
fi

# The reverse is not a failure -- "mod%d" and division_table_version are
# structural -- but a field written and never read is dead weight worth naming.
extra=$(/usr/bin/comm -23 <(printf '%s\n' "$writer") <(printf '%s\n' "$reader") || true)
if [ -n "$extra" ]; then
  echo "    written but not read (informational):"
  printf '%s\n' "$extra" | /usr/bin/sed 's/^/      /'
fi

# ---- the three fields the sources added must all be in the document -----
for k in src cc_num slew; do
  if printf '%s\n' "$writer" | /usr/bin/grep -qx "$k"; then
    say_ok "mod_config writes $k"
  else
    say_fail "mod_config does not write $k -- it would not survive a save"
  fi
  if printf '%s\n' "$reader" | /usr/bin/grep -qx "$k"; then
    say_ok "the parser reads $k"
  else
    say_fail "the parser does not read $k"
  fi
done

# ---- runtime-only fields must NOT be persisted --------------------------
# Restoring a slew position would replay a transient on every load.
for k in slewed slew_primed; do
  if printf '%s\n' "$writer" | /usr/bin/grep -qx "$k"; then
    say_fail "mod_config writes $k, which is runtime state"
  fi
done
say_ok "the runtime slew fields are not persisted"

# ---- src is written as a NAME, not an index -----------------------------
# A stored index would silently mean a different source the moment
# mod_src_names is reordered.
if /usr/bin/sed -n '/strcmp(key, "mod_config")/,/^    }$/p' "$HOST" \
   | /usr/bin/grep -q 'mod_src_name(lfo->src)'; then
  say_ok "src is written as its wire name, not as an index"
else
  say_fail "src is not written via mod_src_name; an index would break on reorder"
fi

[ "$fail" = "0" ] && echo "ALL PASS" || { echo "FAIL"; exit 1; }
