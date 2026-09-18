#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The ABI gate that lets the UAC2 gadget modules ship without a kernel replacement.
#
# scripts/build-uac2-modules.sh builds the kernel twice — stock, then with
# SND/SND_PCM/USB_CONFIGFS_F_UAC2 flipped on — and refuses to package if any
# already-exported symbol changed its MODVERSIONS CRC. That check is the only
# thing standing between "five modules that load" and "five modules that fail to
# resolve against the device's running kernel".
#
# A gate that cannot fail is not a gate, so this exercises the failure paths
# explicitly: an unchanged pair must pass, and a CRC change, a dropped symbol,
# and both together must each be rejected. Fixtures are synthetic, so this needs
# no Docker, no kernel tree and no GPL drop — it runs in CI.

VERIFY="scripts/verify-uac2-abi.sh"
[ -x "$VERIFY" ] || { echo "FAIL: $VERIFY missing or not executable"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Module.symvers columns: CRC symbol module export_type
cat > "$TMP/stock" <<'EOF'
0x1a2b3c4d	snd_card_new	vmlinux	EXPORT_SYMBOL
0x5e6f7a8b	usb_ep_queue	vmlinux	EXPORT_SYMBOL_GPL
0x9c0d1e2f	kmalloc_caches	vmlinux	EXPORT_SYMBOL
0xdeadbeef	config_ep_by_speed	vmlinux	EXPORT_SYMBOL_GPL
EOF

# The real shape of a good build: same symbols at the same CRCs, plus new ones.
cat "$TMP/stock" > "$TMP/good"
cat >> "$TMP/good" <<'EOF'
0xaaaa0001	u_audio_start_playback	drivers/usb/gadget/function/u_audio	EXPORT_SYMBOL_GPL
0xaaaa0002	snd_pcm_new	sound/core/snd-pcm	EXPORT_SYMBOL
EOF

# One CRC moved.
sed 's/^0x5e6f7a8b\tusb_ep_queue/0xffff0000\tusb_ep_queue/' "$TMP/good" > "$TMP/crc_changed"

# One symbol vanished.
grep -v 'config_ep_by_speed' "$TMP/good" > "$TMP/dropped"

# Both at once.
grep -v 'config_ep_by_speed' "$TMP/crc_changed" > "$TMP/both"

fail=0

expect_pass() {
  local name="$1" file="$2"
  if out="$("$VERIFY" "$TMP/stock" "$file" 2>&1)"; then
    echo "ok: $name accepted"
  else
    echo "FAIL: $name should have been accepted, but the gate rejected it"
    echo "$out" | sed 's/^/    /'
    fail=1
  fi
}

expect_reject() {
  local name="$1" file="$2" needle="$3"
  if out="$("$VERIFY" "$TMP/stock" "$file" 2>&1)"; then
    echo "FAIL: $name was ACCEPTED — the gate cannot detect this and is useless"
    echo "$out" | sed 's/^/    /'
    fail=1
  elif ! printf '%s' "$out" | grep -q "$needle"; then
    echo "FAIL: $name was rejected, but the message never mentions '$needle'"
    echo "$out" | sed 's/^/    /'
    fail=1
  else
    echo "ok: $name rejected"
  fi
}

expect_pass   "unchanged ABI plus new exports" "$TMP/good"
expect_reject "a changed symbol CRC"           "$TMP/crc_changed" "usb_ep_queue"
expect_reject "a dropped symbol"               "$TMP/dropped"     "config_ep_by_speed"
expect_reject "a changed CRC and a drop"       "$TMP/both"        "ABI moved"

# The build script must actually USE the gate, and must not package past it.
BUILD="scripts/build-uac2-modules.sh"
[ -f "$BUILD" ] || { echo "FAIL: $BUILD missing"; exit 1; }

if ! grep -q 'changed CRCs' "$BUILD" && ! grep -q 'verify-uac2-abi' "$BUILD"; then
  echo "FAIL: $BUILD no longer performs the symbol-CRC comparison"
  fail=1
else
  echo "ok: build script performs the CRC comparison"
fi

# The five modules are a set: snd-pcm needs snd-timer, and shipping four of
# them produces a module that cannot resolve at insmod time on the device.
for m in snd.ko snd-timer.ko snd-pcm.ko u_audio.ko usb_f_uac2.ko; do
  if ! grep -q "$m" "$BUILD"; then
    echo "FAIL: $BUILD no longer builds $m"
    fail=1
  fi
done
[ "$fail" = 0 ] && echo "ok: all five modules still in the build set"

if [ "$fail" != 0 ]; then
  echo "FAILED"
  exit 1
fi
echo "PASS"
