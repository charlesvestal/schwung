#!/usr/bin/env bash
set -euo pipefail

file="src/modules/tools/song-mode/ui.js"

if ! rg -F -q "const CONTINUE_PAD = -1;" "$file"; then
  echo "FAIL: missing continue-pad sentinel constant" >&2
  exit 1
fi

if ! rg -F -q "entry.pads[grid.track] = CONTINUE_PAD;" "$file"; then
  echo "FAIL: missing Shift+Pad continue assignment" >&2
  exit 1
fi

if ! rg -F -q "if (shiftHeld && selectedEntry > 0) {" "$file"; then
  echo "FAIL: continue assignment is not guarded to non-first steps" >&2
  exit 1
fi

if ! rg -F -q "const BAR_LENGTH_MODES = [\"longest\", \"shortest\", \"custom\"];" "$file"; then
  echo "FAIL: missing bar-length mode options" >&2
  exit 1
fi

if ! rg -F -q "barLengthMode: \"longest\"" "$file"; then
  echo "FAIL: missing default bar-length mode for steps" >&2
  exit 1
fi

if ! rg -F -q "customBars: 1" "$file"; then
  echo "FAIL: missing default custom bars value for steps" >&2
  exit 1
fi

if ! rg -F -q "print(2, 45, \"Length:\", 1);" "$file"; then
  echo "FAIL: step params does not render length override" >&2
  exit 1
fi

if ! rg -F -q "if (mode === \"custom\") {" "$file" || ! rg -F -q "print(2, 55, \"Custom:\", 1);" "$file"; then
  echo "FAIL: custom bar length parameter is not conditionally rendered" >&2
  exit 1
fi

if ! rg -F -q "const stepBars = getEntryDurationBars(entry);" "$file" || ! rg -F -q "print(2, 25, \"Steps: \" + stepBars" "$file"; then
  echo "FAIL: step params should display effective step length at top (including custom mode)" >&2
  exit 1
fi

if rg -F -q "print(2, 55, \"Step: " "$file"; then
  echo "FAIL: redundant Step duration row should not be rendered at bottom" >&2
  exit 1
fi

if ! rg -F -q "let stepParamsEditing = false;" "$file"; then
  echo "FAIL: missing step params edit/select mode state" >&2
  exit 1
fi

if ! rg -F -q "stepParamsEditing = !stepParamsEditing;" "$file"; then
  echo "FAIL: jog click does not toggle step params edit mode" >&2
  exit 1
fi

if rg -F -q "if (status === MidiCC && (d1 === MoveUp || d1 === MoveDown) && d2 > 0)" "$file"; then
  echo "FAIL: step params should not use Up/Down navigation anymore" >&2
  exit 1
fi

if ! rg -F -q "parts.push((t + 1) + \"\\\"\");" "$file"; then
  echo "FAIL: continue tracks should render as track number plus quote (e.g. 2\")" >&2
  exit 1
fi

if ! rg -F -q "const barsStr = entry.repeats > 1 ? (stepBars + \"x\" + entry.repeats) : String(stepBars);" "$file"; then
  echo "FAIL: list row should display duration as '4' or '4x2'" >&2
  exit 1
fi

if ! rg -F -q "const totalBars = getEntryDurationBars(entry) * entry.repeats;" "$file"; then
  echo "FAIL: playback is not using step duration helper" >&2
  exit 1
fi

echo "PASS: Song Mode continue pad + length override wiring present"
