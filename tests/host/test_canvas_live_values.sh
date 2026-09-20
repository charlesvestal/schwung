#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { readFileSync } from "node:fs";
let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };
const src = readFileSync("src/shadow/shadow_ui.js", "utf8");
ok(/extra_keys\) \? meta\.extra_keys\.slice\(0, 4\)/.test(src), "fullscreen live keys are capped at four");
ok(/Math\.max\(50, Number\(meta\.fullscreen_live_ms\)\)/.test(src), "fullscreen polling is capped at 20 Hz");
ok(/invokeCanvasOverlayHook\("onValues", \{ values, nowMs: now \}\)/.test(src), "live values use an event hook");
ok(/const DRAW_PATH_HOOKS = new Set\(\["draw", "tick"\]\)/.test(src), "draw and tick still lose param accessors");
process.exit(fail ? 1 : 0);
'
