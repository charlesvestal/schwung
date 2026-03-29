#!/bin/sh
# midi_connect.sh — Connect system:midi_capture_ext to all RNBO patcher MIDI inputs.
# Uses jack_midi_connect which appends connections (never replaces existing ones).
# Called periodically from ui.js tick() to catch graph reloads.

TOOL="/data/UserData/schwung/bin/jack_midi_connect"
[ -x "$TOOL" ] || exit 0

LD_LIBRARY_PATH="/data/UserData/rnbo/lib:$LD_LIBRARY_PATH" "$TOOL"
