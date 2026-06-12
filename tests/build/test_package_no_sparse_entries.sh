#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# BusyBox tar on Move cannot decode GNU-sparse pax entries: a sparse build
# artifact (e.g. dsp.so with a hole left by Docker virtiofs) gets extracted
# to a literal GNUSparseFile.0/<name> path and the real file is silently
# left stale on device. bsdtar (macOS) auto-detects holes and emits these
# entries in its default pax format; its ustar writer cannot encode sparse
# and stores the file dense. GNU tar only sparses with an explicit -S.
#
# Pins:
# 1. package.sh's non-GNU (bsdtar) branch must write ustar format.
# 2. install.sh must refuse to extract a tarball containing sparse entries
#    (device-side raw listing shows the mangled names; host-side listing
#    un-mangles them, so the check has to be the raw grep).

# 1. bsdtar branch forces ustar
if ! awk '/if tar --version.*GNU/,0' scripts/package.sh | \
     awk '/^else$/,/^fi$/' | rg -q -- '--format ustar'; then
  echo "FAIL: package.sh bsdtar branch does not force --format ustar (sparse-safe)" >&2
  exit 1
fi

# 2. install.sh rejects sparse entries before extraction
if ! rg -q 'GNUSparseFile' scripts/install.sh; then
  echo "FAIL: install.sh does not validate the tarball against GNUSparseFile entries" >&2
  exit 1
fi

# 3. If a local tarball exists, its raw stream must not contain sparse
#    entries (bsdtar -t un-mangles names, so grep the uncompressed bytes).
if [ -f schwung.tar.gz ]; then
  if gzcat schwung.tar.gz 2>/dev/null | LC_ALL=C grep -qa 'GNUSparseFile'; then
    echo "FAIL: schwung.tar.gz contains GNU sparse entries (BusyBox-unextractable)" >&2
    exit 1
  fi
fi

echo "PASS: packaging is sparse-entry safe for BusyBox tar"
