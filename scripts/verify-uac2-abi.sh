#!/usr/bin/env bash
# Compare two Module.symvers and fail if any symbol that existed before changed
# its MODVERSIONS CRC.
#
# This is the whole reason the UAC2 modules can be shipped without replacing the
# kernel: enabling SND/SND_PCM/USB_CONFIGFS_F_UAC2 must add exports and change
# none. A changed CRC means a module built here would refuse to load against the
# device's running kernel — or worse, that the built-in ABI moved and nothing
# derived from this tree is trustworthy.
#
# Split out of build-uac2-modules.sh so it can be tested without Docker or a
# kernel tree. Usage: verify-uac2-abi.sh <symvers.stock> <symvers.new>

set -euo pipefail

if [ $# -ne 2 ]; then
  echo "usage: $(basename "$0") <symvers.stock> <symvers.new>" >&2
  exit 2
fi

STOCK="$1"
NEW="$2"

for f in "$STOCK" "$NEW"; do
  [ -f "$f" ] || { echo "error: no such file: $f" >&2; exit 2; }
done

# Module.symvers columns: CRC symbol module export_type [namespace]
changes="$(join -j2 -o 1.1,1.2,2.1 \
             <(sort -k2,2 "$STOCK") \
             <(sort -k2,2 "$NEW") | awk '$1 != $3')"

# A symbol present before and absent now is just as fatal as a changed CRC:
# a module that needs it would fail to resolve.
dropped="$(comm -23 <(awk '{print $2}' "$STOCK" | sort -u) \
                    <(awk '{print $2}' "$NEW" | sort -u))"

before="$(wc -l < "$STOCK" | tr -d ' ')"
after="$(wc -l < "$NEW" | tr -d ' ')"
n_changed="$(printf '%s' "$changes" | grep -c . || true)"
n_dropped="$(printf '%s' "$dropped" | grep -c . || true)"

echo "    symbols: $before -> $after, changed CRCs: $n_changed, dropped: $n_dropped"

if [ "$n_changed" -ne 0 ] || [ "$n_dropped" -ne 0 ]; then
  echo "error: the exported-symbol ABI moved. These modules are not safe to load" >&2
  echo "       against the device's kernel. Refusing to package." >&2
  if [ "$n_changed" -ne 0 ]; then
    echo "  changed (symbol: stock -> new):" >&2
    printf '%s\n' "$changes" | head -n 20 | awk '{print "    " $2 ": " $1 " -> " $3}' >&2
  fi
  if [ "$n_dropped" -ne 0 ]; then
    echo "  dropped:" >&2
    printf '%s\n' "$dropped" | head -n 20 | sed 's/^/    /' >&2
  fi
  exit 1
fi
