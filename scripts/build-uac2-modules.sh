#!/usr/bin/env bash
# Build the USB Audio Class 2.0 gadget modules for Move's stock kernel.
#
# Move's kernel ships with CONFIG_SND unset, so f_uac2 and the ALSA core it
# needs are absent even though their source is in Ableton's GPL drop. This
# builds them as loadable modules against the SAME source and config the device
# runs, so no kernel image is replaced.
#
# The build is only safe because MODVERSIONS symbol CRCs are unchanged by the
# config flips. That is not assumed — the script builds twice (stock, then
# flipped) and fails if any pre-existing exported symbol's CRC moved, or if any
# module depends on a symbol the stock kernel does not provide at that CRC.
#
# Requires Docker. On an arm64 host the kernel builds natively; on x86_64 it
# cross-compiles (slower).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

EXPECTED_RELEASE="5.15.92-rt57-v8"
GPL_ROOT="${SCHWUNG_GPL_SOURCES:-$HOME/Downloads/sources}"
KERNEL_CONFIG="${SCHWUNG_KERNEL_CONFIG:-$GPL_ROOT/config-$EXPECTED_RELEASE}"
OUT_DIR="$REPO_ROOT/dist/uac2-modules"
VOLUME="schwung-uac2-ksrc"
IMAGE="schwung-kbuild"

# The five modules we need. snd-timer is not optional: snd-pcm depends on it.
MODULE_PATHS=(
  sound/core/snd.ko
  sound/core/snd-timer.ko
  sound/core/snd-pcm.ko
  drivers/usb/gadget/function/u_audio.ko
  drivers/usb/gadget/function/usb_f_uac2.ko
)

REUSE_SOURCE=0
for arg in "$@"; do
  case "$arg" in
    --reuse) REUSE_SOURCE=1 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

if [ -z "${SCHWUNG_KERNEL_TARBALL:-}" ]; then
  KERNEL_TARBALL="$(ls "$GPL_ROOT"/aarch64-oe-linux/linux-raspberrypi-*/*.tar.xz 2>/dev/null | head -n 1 || true)"
else
  KERNEL_TARBALL="$SCHWUNG_KERNEL_TARBALL"
fi

if [ -z "$KERNEL_TARBALL" ] || [ ! -f "$KERNEL_TARBALL" ]; then
  cat >&2 <<EOF
error: could not find the kernel source tarball.

This needs Ableton's GPL source drop, which is not in this repo (it is ~130 MB).
Point at it with one of:

  SCHWUNG_GPL_SOURCES=/path/to/sources        (expects aarch64-oe-linux/linux-raspberrypi-*/ and config-$EXPECTED_RELEASE)
  SCHWUNG_KERNEL_TARBALL=/path/to/kernel.tar.xz SCHWUNG_KERNEL_CONFIG=/path/to/config
EOF
  exit 1
fi

if [ ! -f "$KERNEL_CONFIG" ]; then
  echo "error: kernel config not found: $KERNEL_CONFIG" >&2
  exit 1
fi

command -v docker >/dev/null || { echo "error: docker is required" >&2; exit 1; }

echo "=== Building UAC2 gadget modules for $EXPECTED_RELEASE"
echo "    source: $KERNEL_TARBALL"
echo "    config: $KERNEL_CONFIG"

# Native on arm64, cross elsewhere. gcc 11 matches the Yocto toolchain's major.
HOST_ARCH="$(docker info --format '{{.Architecture}}')"
if [ "$HOST_ARCH" = "aarch64" ] || [ "$HOST_ARCH" = "arm64" ]; then
  CROSS_PKGS="build-essential gcc-11-plugin-dev"
  CROSS_MAKE=""
else
  CROSS_PKGS="build-essential gcc-aarch64-linux-gnu gcc-11-plugin-dev-aarch64-linux-gnu"
  CROSS_MAKE="CROSS_COMPILE=aarch64-linux-gnu-"
fi

docker build -q -t "$IMAGE" - <<EOF >/dev/null 2>&1
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      $CROSS_PKGS bc bison flex libssl-dev libelf-dev kmod cpio rsync \
      python3 ca-certificates xz-utils \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
EOF

mkdir -p "$OUT_DIR"
cp "$KERNEL_CONFIG" "$OUT_DIR/config.stock"
cp "$SCRIPT_DIR/verify-uac2-abi.sh" "$OUT_DIR/verify-uac2-abi.sh"

if [ "$REUSE_SOURCE" = "1" ] && docker volume inspect "$VOLUME" >/dev/null 2>&1; then
  echo "--- reusing existing source volume ($VOLUME)"
else
  echo "--- extracting kernel source into a docker volume (this is not a bind mount: bind-mounted"
  echo "    kernel builds on macOS are an order of magnitude slower)"
  docker volume rm -f "$VOLUME" >/dev/null 2>&1 || true
  docker volume create "$VOLUME" >/dev/null
  docker run --rm -v "$KERNEL_TARBALL:/tarball.tar.xz:ro" -v "$VOLUME:/vol" "$IMAGE" \
    bash -c 'tar -xJf /tarball.tar.xz -C /vol --strip-components=1 && make -C /vol mrproper >/dev/null 2>&1 || true'
fi

JOBS="$(docker info --format '{{.NCPU}}')"

docker run --rm -i \
  -v "$VOLUME:/src" -v "$OUT_DIR:/out" \
  -e "EXPECTED_RELEASE=$EXPECTED_RELEASE" -e "JOBS=$JOBS" -e "CROSS_MAKE=$CROSS_MAKE" \
  -e "MODULE_PATHS=${MODULE_PATHS[*]}" \
  "$IMAGE" bash -s <<'INNER'
set -euo pipefail
cd /src
MAKE="make ARCH=arm64 $CROSS_MAKE"

echo "--- configuring (stock)"
cp /out/config.stock .config
$MAKE olddefconfig >/dev/null

RELEASE="$($MAKE -s kernelrelease)"
if [ "$RELEASE" != "$EXPECTED_RELEASE" ]; then
  echo "error: kernel release is '$RELEASE', expected '$EXPECTED_RELEASE'." >&2
  echo "       The modules would carry the wrong vermagic and refuse to load." >&2
  exit 1
fi
echo "    kernelrelease: $RELEASE"

echo "--- build 1/2: stock config (establishes the reference ABI)"
$MAKE -j"$JOBS" vmlinux modules >/out/build-stock.log 2>&1
cp Module.symvers /out/symvers.stock

echo "--- enabling SND, SND_PCM and USB_CONFIGFS_F_UAC2"
./scripts/config --file .config --module SND --module SND_PCM --enable USB_CONFIGFS_F_UAC2
$MAKE olddefconfig >/dev/null
cp .config /out/config.uac2

echo "--- build 2/2: with UAC2"
$MAKE -j"$JOBS" vmlinux modules >/out/build-uac2.log 2>&1
cp Module.symvers /out/symvers.uac2

# ---- Gate 1: no pre-existing exported symbol may change CRC. -----------------
# If one did, every already-loaded module on the device would be at a different
# ABI than the kernel we derived these from, and nothing here is safe to ship.
# The comparison lives in its own script so tests/host/test_uac2_abi_gate.sh can
# prove it actually rejects a moved CRC.
echo "--- verifying: exported symbol CRCs unchanged"
bash /out/verify-uac2-abi.sh /out/symvers.stock /out/symvers.uac2

# ---- Gate 2: collect the modules, check vermagic and every dependency. -------
rm -rf /out/ko && mkdir -p /out/ko
for m in $MODULE_PATHS; do
  [ -f "$m" ] || { echo "error: expected module not built: $m" >&2; exit 1; }
  cp "$m" /out/ko/
done

echo "--- verifying: vermagic"
for f in /out/ko/*.ko; do
  vm="$(modinfo -F vermagic "$f")"
  case "$vm" in
    "$EXPECTED_RELEASE "*) ;;
    *) echo "error: $(basename "$f") has vermagic '$vm'" >&2; exit 1 ;;
  esac
done
echo "    all modules: $(modinfo -F vermagic /out/ko/snd.ko)"

echo "--- verifying: every module symbol resolves against the STOCK kernel"
awk '{print $2}' /out/symvers.stock | sort > /tmp/stock.syms
awk '{print $2, $1}' /out/symvers.stock | sort > /tmp/stock.crc
awk '{print $2, $3}' /out/symvers.uac2 | sort > /tmp/uac2.prov
ours=$(for m in $MODULE_PATHS; do echo "${m%.ko}"; done)

from_kernel=0; from_set=0; bad=0
for f in /out/ko/*.ko; do
  while read -r crc sym; do
    if grep -qx "$sym" /tmp/stock.syms; then
      have="$(grep -m1 "^$sym " /tmp/stock.crc | cut -d' ' -f2)"
      if [ "$have" != "$crc" ]; then
        echo "  CRC MISMATCH: $(basename "$f") needs $sym at $crc, kernel has $have" >&2
        bad=$((bad + 1))
      else
        from_kernel=$((from_kernel + 1))
      fi
    else
      prov="$(grep -m1 "^$sym " /tmp/uac2.prov | cut -d' ' -f2 || true)"
      if echo "$ours" | grep -qx "$prov"; then
        from_set=$((from_set + 1))
      else
        echo "  UNSATISFIED: $(basename "$f") needs $sym <- ${prov:-nothing}" >&2
        bad=$((bad + 1))
      fi
    fi
  done < <(modprobe --dump-modversions "$f")
done
echo "    resolved by running kernel: $from_kernel, by this module set: $from_set, unsatisfied: $bad"
[ "$bad" -eq 0 ] || { echo "error: $bad unsatisfied dependencies. Refusing to package." >&2; exit 1; }

# Only vmlinux's build stamp may differ between the two builds.
echo "--- built-in objects rebuilt by the config change:"
grep '^  CC' /out/build-uac2.log | grep -v '\[M\]' | awk '{print "    " $2}' | sort -u || true

{
  echo "# Schwung UAC2 gadget modules"
  echo "# kernelrelease: $EXPECTED_RELEASE"
  echo "# vermagic:      $(modinfo -F vermagic /out/ko/snd.ko)"
  echo "# exported symbol ABI vs stock: unchanged (verified)"
  echo
  cd /out/ko && sha256sum *.ko
} > /out/manifest.txt
INNER

echo
echo "=== Modules built and verified"
cat "$OUT_DIR/manifest.txt"
echo
echo "Output: $OUT_DIR/ko/"
