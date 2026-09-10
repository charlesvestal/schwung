#!/usr/bin/env bash
#
# A CANVAS TAKEOVER'S ONLY WAY TO ASK AGAIN.
#
# `draw` and `tick` are DRAW_PATH_HOOKS and get a ctx with getParam/setParam
# REMOVED, deliberately: one param read is ~2.8ms against a 1.68ms whole page
# render, so an overlay reading per frame halves its own frame rate and
# everything drawn with it.
#
# That leaves a takeover able to read only when the user touches something --
# and a takeover owns the screen for minutes while things change under it that
# its own input did not cause: a worker thread finishing, a Remote UI panel in
# a browser editing the same module. The screen then tells the truth only when
# poked, which reads as the device having missed the change.
#
# `onPoll` is the event on a metronome. What must stay true:
#   * it is NOT a draw-path hook, or it loses the accessors that are its point
#   * it is throttled by a named constant, not called per frame
#   * it is opt-in, so an overlay without one costs nothing
#   * it goes through invokeCanvasOverlayHook, so a throw disables it once
#     rather than throwing at 60Hz forever
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
F="$ROOT/src/shadow/shadow_ui.js"
fail=0
note(){ echo "FAIL: $1"; fail=1; }

grep -q 'DRAW_PATH_HOOKS = new Set(\["draw", "tick"\])' "$F" \
  || note "DRAW_PATH_HOOKS changed -- onPoll must NOT be in it or it loses getParam"
grep -q 'const CANVAS_POLL_MS' "$F" || note "CANVAS_POLL_MS is gone -- the poll is unthrottled"
grep -q 'typeof canvasRuntime.overlay.onPoll !== "function"' "$F" \
  || note "onPoll is no longer opt-in -- every overlay pays for it"
grep -q 'invokeCanvasOverlayHook("onPoll"' "$F" \
  || note "onPoll is not called through invokeCanvasOverlayHook -- a throw would not disable it"
grep -q 'canvasRuntime.pollMs = Date.now();' "$F" \
  || note "the metronome is not seeded at onOpen -- the first tick re-reads what onOpen just read"

node --check "$F" 2>/dev/null || note "shadow_ui.js does not parse"

[ "$fail" = 0 ] || exit 1
echo "PASS: $(basename "$0")  (a takeover can ask again, on a metronome)"
