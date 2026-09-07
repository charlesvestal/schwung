#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_chain_patch_roundtrip"
mkdir -p "$(dirname "$bin")"

# chain_internal.h includes <malloc.h>, absent on macOS -- stub it so the test
# compiles on the dev host as well as Linux/CI. The temp dir also gives the
# test somewhere to write its patch file that is not the device root FS.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
printf '#include <stdlib.h>\n' > "$work/malloc.h"

# The caps come from the shipped header, never a copy: the point of the test is
# that the patch layer tracks whatever MAX_AUDIO_FX / MAX_MIDI_FX currently are.
# -Wno-sign-compare: chain_params.c has pre-existing int/size_t comparisons that
# are not this test's business to fix, and the noise would bury a real warning.
cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -Wno-sign-compare \
  -I"$work" -Isrc -Isrc/host -Isrc/modules/chain/dsp \
  tests/host/test_chain_patch_roundtrip.c \
  src/modules/chain/dsp/chain_params.c \
  src/modules/chain/dsp/chain_json.c \
  -o "$bin"

# ---- THE PRODUCER HALF ------------------------------------------------------
#
# The C half above feeds the parser hand-written documents. That is exactly how
# a file format with a reader and no writer shipped: every load reset all four
# buses and destroyed a live kit in silence. So one document is written by the
# REAL producer -- busPatchFields in src/shared/bus_model.mjs -- from a
# bus_emit_config-shaped config, and the binary prints what it parsed back out.
# Neither side holds the expectation alone; the diff below is the assertion.
#
# The second document is the same one with "buses" deleted: the negative
# control, which must NOT match. Without it the test would pass on a producer
# that emits nothing.
if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi
WORK="$work" node --input-type=module -e '
import * as M from "./src/shared/bus_model.mjs";
import fs from "node:fs";
const work = process.env.WORK;

/* Shaped exactly as bus_emit_config writes it, holes and all. */
const emitted = JSON.stringify({
  buses: [
    { present: 1, name: "Kick", orphans: 0, voices: ["kick"], sends: [20, 0],
      fx: [{ module: "tapescam", bypassed: 1 }] },
    { present: 0, name: "Bus 2", orphans: 0, voices: [], sends: [0, 0], fx: [] },
    { present: 1, name: "Hats", orphans: 1, voices: ["chh", "ohh"], sends: [0, 15],
      fx: [{ module: "chorus", bypassed: 0 }, { module: "phaser", bypassed: 0 }] },
    { present: 0, name: "Bus 4", orphans: 0, voices: [], sends: [0, 0], fx: [] },
  ],
  main_sends: [5, 30],
  /* A STALE `voice_sends` KEY, as an old document would carry it. The producer
     must not echo it and the parser must not read it: those levels belong to
     the module now and arrive with its own state blob. */
  voice_sends: [{ id: "kick", sends: [64, 0] }],
});
const config = M.parseBusesConfig(emitted);
if (config.unresolved) { console.error("FAIL: fixture config did not parse"); process.exit(1); }

/* One opaque state per occupied position, in both legal forms: an object and
   an already-stringified blob. */
const states = { "0:0": { drive: 0.5 }, "2:0": "{\"rate\":2}" };
const fields = M.busPatchFields(config, (b, k) => states[b + ":" + k]);
if (!fields) { console.error("FAIL: producer answered nothing"); process.exit(1); }

const doc = { custom_name: "Kit", synth: { module: "mrdrums" },
              main_sends: fields.main_sends,
              /* Emitted BEFORE "buses", the order busPatchFields builds:
                 main_sends is found by a whole-document scan. */
              buses: fields.buses };
fs.writeFileSync(work + "/producer.json", JSON.stringify(doc, null, 2));

const omitted = { ...doc };
delete omitted.buses;
fs.writeFileSync(work + "/producer_omitted.json", JSON.stringify(omitted, null, 2));

/* The same flattened summary bus_summary() prints in the C half. */
const lines = ["main_sends=" + fields.main_sends.join(",")];
if ("voice_sends" in fields) {
  console.error("FAIL: the producer still writes voice_sends");
  process.exit(1);
}
fields.buses.forEach((b, i) => {
  if (!b.present) { lines.push(`bus${i} present=0`); return; }
  const fx = b.fx.map((e) => {
    const st = e.state === undefined ? ""
             : (typeof e.state === "string" ? e.state : JSON.stringify(e.state));
    return `${e.module}:${e.bypassed}:${st}`;
  }).join("|");
  lines.push(`bus${i} present=1 name=${b.name} voices=${b.voices.join(",")}` +
             ` sends=${b.sends.join(",")} fx=${fx}`);
});
fs.writeFileSync(work + "/expect.txt", lines.join("\n") + "\n");
'

"$bin" "$work"

# The span: what the producer says it wrote, against what the C parser read.
if ! diff -u "$work/expect.txt" "$work/parsed.txt"; then
  echo "FAIL: the producer and the parser disagree about the buses" >&2
  exit 1
fi
# The negative control. If this ever matches, the diff above is asserting
# nothing -- which is the state the feature shipped in.
if diff -q "$work/expect.txt" "$work/parsed_omitted.txt" >/dev/null; then
  echo "FAIL: omitting \"buses\" from the producer changed nothing" >&2
  exit 1
fi
