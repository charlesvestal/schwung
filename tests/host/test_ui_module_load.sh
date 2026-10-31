#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
node --test tests/host/test_ui_module_load.mjs
