#!/usr/bin/env bash
set -euo pipefail

# launch_shadow_ui() must reap BEFORE its started/pid early-out.
#
# The early-out used to come first, so when shadow_ui died the stale
# shadow_ui_started/shadow_ui_pid pair made it return before the reap that
# would have cleared them — the child stayed a zombie and was never respawned
# until MoveOriginal restarted (i.e. the shadow UI was gone for the session).
#
# The ordering also matters for realtime safety: launch_shadow_ui() is called
# from shadow_swap_display() on the SPI path, where file I/O is banned.
# waitpid() is a bare syscall and safe to run every call; shadow_ui_refresh_pid()
# reads /proc and must stay BEHIND an early-out so it is off the steady path.

src="src/host/shadow_process.c"

if [[ ! -f "$src" ]]; then
  echo "FAIL: $src not found (run from repo root)" >&2
  exit 1
fi

body="$(sed -n '/^void launch_shadow_ui(void) {/,/^}/p' "$src")"

if [[ -z "$body" ]]; then
  echo "FAIL: could not extract launch_shadow_ui() body" >&2
  exit 1
fi

line_of() { grep -n "$1" <<<"$body" | head -n 1 | cut -d: -f1; }

reap_line="$(line_of 'shadow_ui_reap();' || true)"
guard_line="$(line_of 'if (shadow_ui_started && shadow_ui_pid > 0) return;' || true)"
refresh_line="$(line_of 'shadow_ui_refresh_pid();' || true)"

for pair in "reap:$reap_line" "guard:$guard_line" "refresh:$refresh_line"; do
  if [[ -z "${pair#*:}" ]]; then
    echo "FAIL: ${pair%%:*} call not found in launch_shadow_ui()" >&2
    exit 1
  fi
done

if (( reap_line >= guard_line )); then
  echo "FAIL: shadow_ui_reap() must come BEFORE the started/pid early-out," >&2
  echo "      otherwise a dead shadow_ui is never respawned" >&2
  exit 1
fi

if (( refresh_line <= guard_line )); then
  echo "FAIL: shadow_ui_refresh_pid() reads /proc and must stay BEHIND an" >&2
  echo "      early-out — launch_shadow_ui() runs on the SPI path" >&2
  exit 1
fi

# A child reaped by someone else reports ECHILD, not our pid; treating only the
# pid case as death leaves shadow_ui_started stuck at 1 forever.
if ! rg -q 'res < 0 && errno == ECHILD' "$src"; then
  echo "FAIL: shadow_ui_reap() should treat ECHILD as 'child is gone'" >&2
  exit 1
fi

# The SPI path forbids file I/O; the reap helper itself must stay syscall-only.
reap_body="$(sed -n '/^static void shadow_ui_reap(void) {/,/^}/p' "$src")"
if rg -q 'fopen|fprintf|unified_log' <<<"$reap_body"; then
  echo "FAIL: shadow_ui_reap() runs on the SPI path and must not do file I/O" >&2
  exit 1
fi

# The watchdog must sit ABOVE the shadow-mode gate in shadow_swap_display().
# Below it, the periodic relaunch only ran while the shadow UI was on screen —
# so a shadow_ui that died while hidden could never be recovered.
shim="src/schwung_shim.c"
swap="$(sed -n '/^static void shadow_swap_display(void)/,/^}/p' "$shim")"

watchdog_line="$(grep -n 'launch_shadow_ui();' <<<"$swap" | head -n 1 | cut -d: -f1 || true)"
gate_line="$(grep -n 'if (!shadow_display_mode) {' <<<"$swap" | head -n 1 | cut -d: -f1 || true)"

if [[ -z "$watchdog_line" || -z "$gate_line" ]]; then
  echo "FAIL: could not locate the watchdog or the shadow-mode gate" >&2
  exit 1
fi

if (( watchdog_line >= gate_line )); then
  echo "FAIL: the launch_shadow_ui() watchdog must run BEFORE the" >&2
  echo "      !shadow_display_mode early-return, or a shadow_ui that dies" >&2
  echo "      while the UI is hidden is never recovered" >&2
  exit 1
fi

# An always-on watchdog can fork-bomb a shadow_ui that dies on startup.
if ! rg -q 'SHADOW_UI_MAX_RAPID_RELAUNCH' "$src"; then
  echo "FAIL: an always-on watchdog needs a relaunch backoff" >&2
  exit 1
fi

# The give-up state must cost only the waitpid, not a /proc read per call.
launch_body="$(sed -n '/^void launch_shadow_ui(void) {/,/^}/p' "$src")"
backoff_line="$(grep -n 'if (shadow_ui_backoff_active) return;' <<<"$launch_body" | head -n 1 | cut -d: -f1 || true)"
refresh_line2="$(grep -n 'shadow_ui_refresh_pid();' <<<"$launch_body" | head -n 1 | cut -d: -f1 || true)"
if [[ -z "$backoff_line" ]] || (( backoff_line >= refresh_line2 )); then
  echo "FAIL: the backoff early-out must precede shadow_ui_refresh_pid()," >&2
  echo "      or the give-up state reads /proc on every SPI-path call" >&2
  exit 1
fi

# An explicit user request must clear the backoff, or one crash loop makes the
# shadow UI unreachable until reboot.
if ! rg -q 'launch_shadow_ui_reset_backoff\(\);' "$shim"; then
  echo "FAIL: explicit shadow-UI shortcuts should clear the relaunch backoff" >&2
  exit 1
fi

echo "PASS: launch_shadow_ui() reaps before its early-out, the watchdog runs ungated, and backoff stays off the hot path"
