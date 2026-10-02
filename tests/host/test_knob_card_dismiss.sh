#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
node --test tests/host/test_knob_card_dismiss.mjs
