#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

if ! command -v node >/dev/null 2>&1; then
  echo "node is required to run this test" >&2
  exit 1
fi

node -e '
import("./src/shared/param_format.mjs").then((m) => {
  const { formatParamValue, formatParamForSet, precisionForStep } = m;

  // Step-derived precision
  if (precisionForStep(1)    !== 0) { console.log("FAIL prec(1)");    process.exit(1); }
  if (precisionForStep(0.5)  !== 1) { console.log("FAIL prec(0.5)");  process.exit(1); }
  if (precisionForStep(0.01) !== 2) { console.log("FAIL prec(0.01)"); process.exit(1); }
  if (precisionForStep(0.001)!== 3) { console.log("FAIL prec(0.001)");process.exit(1); }

  // dB unit
  if (formatParamValue(-6.0,  { type:"float", unit:"dB",  step:0.1 }) !== "-6.0 dB")  { console.log("FAIL dB"); process.exit(1); }
  // Hz unit
  if (formatParamValue(440,   { type:"float", unit:"Hz",  step:1   }) !== "440 Hz")   { console.log("FAIL Hz"); process.exit(1); }
  // ms unit
  if (formatParamValue(12.5,  { type:"float", unit:"ms",  step:0.1 }) !== "12.5 ms")  { console.log("FAIL ms"); process.exit(1); }
  // % unit on 0..1 range scales x100
  if (formatParamValue(0.5,   { type:"float", unit:"%",   step:0.01, max:1 }) !== "50%") { console.log("FAIL % scaled"); process.exit(1); }
  // % unit on 0..100 range does not scale
  if (formatParamValue(50,    { type:"float", unit:"%",   step:1, max:100 }) !== "50%") { console.log("FAIL % unscaled"); process.exit(1); }
  // st (semitones) signed
  if (formatParamValue(7,     { type:"int",   unit:"st"  }) !== "+7 st") { console.log("FAIL st pos"); process.exit(1); }
  if (formatParamValue(-3,    { type:"int",   unit:"st"  }) !== "-3 st") { console.log("FAIL st neg"); process.exit(1); }
  if (formatParamValue(0,     { type:"int",   unit:"st"  }) !== "0 st")  { console.log("FAIL st zero"); process.exit(1); }
  // sec
  if (formatParamValue(1.234, { type:"float", unit:"sec", step:0.001}) !== "1.234 sec") { console.log("FAIL sec"); process.exit(1); }
  // No unit — uses step precision
  if (formatParamValue(0.5,   { type:"float", step:0.01 }) !== "0.50") { console.log("FAIL nounit float"); process.exit(1); }
  if (formatParamValue(42,    { type:"int" }) !== "42") { console.log("FAIL nounit int"); process.exit(1); }
  // Enum returns option string by index
  if (formatParamValue(2, { type:"enum", options:["A","B","C","D"] }) !== "C") { console.log("FAIL enum"); process.exit(1); }
  // display_format override (printf-style)
  if (formatParamValue(0.123, { type:"float", display_format:"%.4f" }) !== "0.1230") { console.log("FAIL display_format"); process.exit(1); }

  // formatParamForSet — for set_param wire format (no unit suffix)
  if (formatParamForSet(0.5,  { type:"float", step:0.01 }) !== "0.500") { console.log("FAIL set float"); process.exit(1); }
  if (formatParamForSet(7,    { type:"int" }) !== "7") { console.log("FAIL set int"); process.exit(1); }
  // Enum sends index by default
  if (formatParamForSet(2, { type:"enum", options:["A","B","C"] }) !== "2") { console.log("FAIL set enum index"); process.exit(1); }
  // Enum with options_as_string=true sends the option string
  if (formatParamForSet(2, { type:"enum", options:["A","B","C"], options_as_string:true }) !== "C") { console.log("FAIL set enum str"); process.exit(1); }

  console.log("PASS param_format");
}).catch((e) => { console.log("FAIL import:", e); process.exit(1); });
'
