#!/usr/bin/env bash
#
# PAD BLOCK MUST NOT SURVIVE THE SCREEN THAT ASKED FOR IT.
#
# host_pad_block(1) stops pad notes reaching Move and routes them to whoever is
# on screen -- for EVERY module, not just the one that asked. Its callers (a
# canvas overlay, the text-entry keyboard) release it in their close hooks,
# which is correct and is not enough: leave by any path that does not run that
# hook and the flag is stranded ON, and every module afterwards silently has no
# pads until a reboot.
#
# The reconciler mirrors the CC claims -- derived from what is on screen, never
# bookkept -- and is ONE-DIRECTIONAL: the host only ever clears. Setting
# belongs to whoever wants the pads; a component that has gone cannot clear
# anything.
set -uo pipefail
cd "$(dirname "$0")/../.."
if node tests/host/test_pad_block_released.mjs; then
    echo "PASS: $(basename "$0")  (pad_block cannot outlive its screen)"
else
    echo "FAIL: pad_block can be stranded on"; exit 1
fi
