#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

fail() { echo "FAIL: $*" >&2; exit 1; }

resample=src/host/shadow_resample.c
shim=src/schwung_shim.c
ui=src/shadow/shadow_ui.js
schema=src/shared/settings-schema.json
help=src/shared/help_content.json

# A saved pre-hotfix `true` must not survive initialization as an enabled
# runtime switch. Keeping the state file is intentional; consuming its old
# enable flag is not.
grep -Eq 'volatile int usbc_out_persist_enabled = 0;' "$resample" ||
    fail "USB-C persistence is not disabled by default"
if grep -q 'strstr(json, "\\"usbc_out_persist\\"")' "$resample"; then
    fail "saved shadow_config.json can still re-enable USB-C persistence"
fi

# Keep the old host parameter as a compatibility no-op, but no SET request may
# turn the runtime switch back on.
handler=$(sed -n '/strcmp(fx_key, "usbc_out_persist")/,/strcmp(fx_key, "usbc_out_source")/p' "$shim")
grep -q 'usbc_out_persist_enabled = 0;' <<<"$handler" ||
    fail "compatibility parameter does not force USB-C persistence off"
if grep -Eq 'usbc_out_persist_enabled = val|usbc_out_persist_enabled = 1' <<<"$handler"; then
    fail "compatibility parameter can still enable USB-C persistence"
fi

# Retired means absent from both settings surfaces and their active serializer.
# The loader reads the existing JSON object before saving, so not assigning the
# key preserves an old value without manufacturing or acting on one.
if grep -q '"key": "usbc_out_persist"' "$schema"; then
    fail "shared settings schema still exposes USB-C persistence"
fi
if grep -q 'config.usbc_out_persist =' "$ui"; then
    fail "shadow UI still manufactures the retired config key"
fi
if grep -q 'shadow_set_param(0, "master_fx:usbc_out_persist"' "$ui"; then
    fail "shadow UI still pushes the retired setting into the shim"
fi
if grep -q 'restores it a few' "$help"; then
    fail "built-in help still claims Schwung restores USB-C output"
fi

# Retired means the REPLAY is retired, not the repair. The two were armed
# through one variable, so 1.3.2 disabled both at once — fixing the muted
# speaker and shipping a microphone stuck on USB-C until reboot. The repair
# restores what Move's live 37 14 already advertises, so it must reach the wire
# with persistence off. usbc_emit_gate.h owns the arithmetic; these pin the
# wiring that silenced it.
worker=src/host/shim_worker.c
monitor_block=$(sed -n '/usbc_gate_tick_monitor(&usbc_gate/,/^        }$/p' "$worker")
[ -n "$monitor_block" ] || fail "could not locate the monitor-loss block in $worker"
grep -q 'shim_usbc_out_reassert = act.replay_value;' <<<"$monitor_block" ||
    fail "the monitor-loss repair does not arm its own request variable"
# Strip comment lines first: this block DESCRIBES the retired switch by name,
# and a pin that reads prose as code fails on its own explanation.
monitor_code=$(grep -Ev '^[[:space:]]*(/\*|\*)' <<<"$monitor_block")
if grep -q 'usbc_out_persist_enabled' <<<"$monitor_code"; then
    fail "the monitor-loss repair is gated on the retired persistence switch"
fi
grep -q 'usbc_emit_select(shim_usbc_out_replay' "$shim" ||
    fail "the shim no longer selects between the retired replay and the repair"

echo "PASS: USB-C output persistence is disabled, and the monitor-loss repair still reaches the wire"
