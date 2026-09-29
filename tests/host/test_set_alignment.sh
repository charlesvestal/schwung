#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
# The set-change handshake that gates autosave, run through the real
# shadow_set_pages.c. getxattr has a different signature on macOS, so a shim
# header stands in for <sys/xattr.h>; the functions using it are not called.
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
mkdir -p "$work/sys"
cat > "$work/sys/xattr.h" <<'H'
#include <sys/types.h>
static inline ssize_t getxattr(const char *p, const char *n, void *v, size_t s) { (void)p; (void)n; (void)v; (void)s; return -1; }
static inline int setxattr(const char *p, const char *n, const void *v, size_t s, int f) { (void)p; (void)n; (void)v; (void)s; (void)f; return -1; }
H
bin=build/tests/test_set_alignment
mkdir -p "$(dirname "$bin")"
cc -std=gnu11 -Wall -Wno-unused-function -I"$work" -Isrc -Isrc/host \
  tests/host/test_set_alignment.c src/host/shadow_set_pages.c -o "$bin"
"$bin"

# The UI's half of the set-change disarm: sceneSetEdit(-1) ahead of the first
# save / restore in the SET_CHANGED handler (the shim half is checked above).
ui=src/shadow/shadow_ui.js
start=$(grep -n 'if (flags & SHADOW_UI_FLAG_SET_CHANGED) setChange: {' "$ui" | head -1 | cut -d: -f1)
[ -n "$start" ] || { echo "FAIL: SET_CHANGED handler not found"; exit 1; }
disarm=$(awk -v s="$start" 'NR>s && /sceneSetEdit\(-1\);/ {print NR; exit}' "$ui")
save=$(awk -v s="$start" 'NR>s && /autosaveAllSlots\(\);/ {print NR; exit}' "$ui")
[ -n "$disarm" ] && [ -n "$save" ] && [ "$disarm" -lt "$save" ] \
  || { echo "FAIL: SET_CHANGED must disarm the scene snapshot before it saves or restores"; exit 1; }
echo "test_set_alignment: UI disarms on set change"
